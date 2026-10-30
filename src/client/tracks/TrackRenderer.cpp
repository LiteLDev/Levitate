// Client: draws curved tracks (Bedrock blocks can't be curved) and tilted tracks.
//
// The server sends every curve over the companion protocol (channel "tracks.curves", see
// src/feature/tracks/TrackSync.cpp); this file keeps them per dimension and draws ties and rails
// along each curve every frame, after the block entities of the opaque pass.
// A track tilted between two sloped curves (Create's TrackBlockEntityTilt / TrackModel, op "tilt")
// is left out of the chunk mesh and drawn every frame from its own block model turned onto the
// ramp (core/tracks/Tilt.h tiltModelPoint); its chunk is meshed again when the tilt changes.

#ifdef LV_CLIENT

#include <array>
#include <cmath>
#include <map>
#include <set>
#include <mutex>
#include <tuple>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/BlockChangedEventTarget.h"

#include "glm/gtc/matrix_transform.hpp"

#include "client/Client.h"
#include "client/building/BlockQuads.h"
#include "client/tracks/TrackRenderer.h"
#include "core/tracks/Curve.h"
#include "core/tracks/Tilt.h"
#include "feature/tracks/Tracks.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::tracks {

namespace ct = core::tracks;

namespace {

struct Quad {
    std::array<ct::Vec3d, 4> v;
    float                     r, g, b;
};

struct RenderCurve {
    std::vector<Quad> quads;
    ct::Vec3d         centre;
    double            reach = 0;
};

using CurveKey = std::tuple<int, ct::BlockPos3, ct::BlockPos3>;

std::mutex                         gMutex;
std::map<CurveKey, RenderCurve>    gCurves;
bool                               gLoggedFirstFrame = false;

// tilted tracks: (dim, pos) -> smoothing angle, positions to mesh again (both gMutex)
std::map<std::pair<int, ct::BlockPos3>, double>                      gTilts;
std::vector<std::pair<int, ct::BlockPos3>>                           gRemesh;
std::set<ct::BlockPos3>                                              gTiltedHere; // the player's dimension (gMutex)
std::map<std::pair<void const*, double>, std::unique_ptr<mce::Mesh>> gTiltMeshes; // client thread
bool                                                                 gLoggedTilt = false;

// Model sizes in blocks (our own track model: ties 2 px high, rails 2 px on top of them)
constexpr double kPx         = 1 / 16.0;
constexpr double kTieHalfW   = 15 * kPx;
constexpr double kTieHalfL   = 2 * kPx;
constexpr double kTieH       = 2 * kPx;
constexpr double kRailOffset = 9 * kPx;
constexpr double kRailHalfW  = 1 * kPx;
constexpr double kRailH      = 2 * kPx;

/// Adds the visible faces of a box given by its bottom corners (b0..b3, counter-clockwise from
/// above) and height `h` along `up`.
void box(std::vector<Quad>& out, std::array<ct::Vec3d, 4> const& b, ct::Vec3d const& up, double h, float r, float g, float bl) {
    std::array<ct::Vec3d, 4> t;
    for (int i = 0; i < 4; ++i) t[i] = b[i] + up * h;
    out.push_back({{t[0], t[1], t[2], t[3]}, r, g, bl});
    for (int i = 0; i < 4; ++i) {
        int   j     = (i + 1) & 3;
        float shade = (i & 1) ? 0.72f : 0.86f;
        out.push_back({{b[i], b[j], t[j], t[i]}, r * shade, g * shade, bl * shade});
    }
}

RenderCurve build(ct::Curve const& curve) {
    RenderCurve rc;
    auto        segs = curve.segments();
    rc.centre        = (curve.bounds().min + curve.bounds().max) * 0.5;
    rc.reach         = (curve.bounds().max - curve.bounds().min).length() / 2;
    // colours of our track texture (tracks.ps1: ties #5b4330, rails #a7a89f), converted from sRGB
    // to the linear vertex colours the colour material expects
    auto        lin  = [](int c) { return float(std::pow(c / 255.0, 2.2)); };
    float const tieR = lin(0x5b), tieG = lin(0x43), tieB = lin(0x30);
    float const railR = lin(0xa7), railG = lin(0xa8), railB = lin(0x9f);

    for (size_t i = 0; i < segs.size(); ++i) {
        auto const& s    = segs[i];
        ct::Vec3d   up   = s.faceNormal;
        ct::Vec3d   side = s.normal;
        ct::Vec3d   fwd  = s.derivative;
        // a tie on every segment (every half block, as the block model), not on the curve ends
        if (i > 0 && i + 1 < segs.size()) {
            ct::Vec3d c = s.position;
            box(rc.quads,
                {c - side * kTieHalfW - fwd * kTieHalfL,
                 c - side * kTieHalfW + fwd * kTieHalfL,
                 c + side * kTieHalfW + fwd * kTieHalfL,
                 c + side * kTieHalfW - fwd * kTieHalfL},
                up,
                kTieH,
                tieR,
                tieG,
                tieB);
        }
        if (i == 0) continue;
        auto const& p = segs[i - 1];
        for (int railSide : {1, -1}) {
            auto railBase = [&](ct::CurveSegment const& q, double w) {
                return q.position + q.normal * (railSide * kRailOffset + w) + q.faceNormal * kTieH;
            };
            box(rc.quads,
                {railBase(p, -kRailHalfW), railBase(s, -kRailHalfW), railBase(s, kRailHalfW), railBase(p, kRailHalfW)},
                up,
                kRailH,
                railR,
                railG,
                railB);
        }
    }
    return rc;
}

ct::BlockPos3 posOf(nlohmann::json const& j) { return {j.at(0).get<int>(), j.at(1).get<int>(), j.at(2).get<int>()}; }

void onMessage(nlohmann::json const& msg) {
    if (!msg.is_object()) return;
    std::string op  = msg.value("op", "");
    int         dim = msg.value("dim", 0);
    std::lock_guard lock(gMutex);
    if (op == "reset") {
        gCurves.clear();
        for (auto const& [key, a] : gTilts) gRemesh.push_back(key);
        gTilts.clear();
        return;
    }
    if (op == "tilt") {
        try {
            auto key = std::pair{dim, posOf(msg.at("pos"))};
            auto a   = msg.find("angle");
            if (a != msg.end() && a->is_number()) gTilts[key] = a->get<double>();
            else gTilts.erase(key);
            gRemesh.push_back(key);
        } catch (...) {
        }
        return;
    }
    for (auto const& jc : msg.value("curves", nlohmann::json::array())) {
        try {
            if (op == "add") {
                auto c = ct::Curve::fromJson(jc);
                if (!c) continue;
                gCurves[{dim, c->block(0), c->block(1)}] = build(*c);
            } else if (op == "remove") {
                auto a = posOf(jc.at(0)), b = posOf(jc.at(1));
                gCurves.erase({dim, a, b});
                gCurves.erase({dim, b, a});
            }
        } catch (...) {
        }
    }
}

mce::MaterialPtr& material() {
    // Position + Color, depth tested, no culling (vanilla "wireframe" material)
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

/// Tilted tracks: the block's own quads turned onto the ramp, drawn as a block entity.
void renderTilts(BaseActorRenderContext& ctx, int dim, BlockSource& region) {
    std::vector<std::pair<int, ct::BlockPos3>>    remesh;
    std::vector<std::pair<ct::BlockPos3, double>> tilts;
    {
        std::lock_guard lock(gMutex);
        remesh.swap(gRemesh);
        gTiltedHere.clear();
        for (auto const& [key, a] : gTilts) {
            if (key.first != dim) continue;
            tilts.emplace_back(key.second, a);
            gTiltedHere.insert(key.second);
        }
    }
    // mesh the chunk again: the tilted block leaves (or rejoins) the chunk mesh
    for (auto const& [d, p] : remesh) {
        if (d != dim) continue;
        BlockPos bp{p.x, p.y, p.z};
        if (!region.hasChunksAt(bp, 0, false)) continue;
        Block const& b = region.getBlock(bp);
        region.fireBlockChanged(bp, 0, b, b, 3, BlockChangedEventTarget::SelfBlock, nullptr, nullptr);
    }
    if (tilts.empty()) return;
    building::Drawer d(ctx);
    if (!d.ok()) return;
    auto& tess  = d.tessellator();
    int   drawn = 0;
    for (auto const& [p, angle] : tilts) {
        BlockPos bp{p.x, p.y, p.z};
        if (!region.hasChunksAt(bp, 0, false)) continue;
        Block const& b     = region.getBlock(bp);
        auto         shape = feature::tracks::shapeAt(region, bp);
        if (!shape) continue;
        auto it = gTiltMeshes.find({&b, angle});
        if (it == gTiltMeshes.end()) {
            std::vector<building::Quad> quads = building::quadsOf(tess, b);
            for (auto& q : quads)
                for (auto& v : q.pos) {
                    ct::Vec3d t = ct::tiltModelPoint({v.x, v.y, v.z}, *shape, angle);
                    v           = glm::vec3{float(t.x), float(t.y), float(t.z)};
                }
            if (quads.empty()) continue;
            it = gTiltMeshes.emplace(std::pair{&b, angle}, building::buildMesh(tess, quads)).first;
        }
        auto const& mesh = it->second;
        if (!mesh) continue;
        d.light(region, bp);
        d.draw(*mesh, glm::translate(glm::mat4{1.f}, glm::vec3{float(p.x), float(p.y), float(p.z)}));
        ++drawn;
    }
    if (drawn && !gLoggedTilt) {
        gLoggedTilt = true;
        log().info("tracks: drawing {} tilted track(s)", drawn);
    }
}

void renderCurves(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int  dim    = static_cast<int>(player->getDimensionId());
    renderTilts(ctx, dim, player->getDimensionBlockSource());
    std::unique_ptr<BaseActorRenderContext::Impl> const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3 origin = impl->mCameraTargetPosition;
    ct::Vec3d o{origin.x, origin.y, origin.z};

    std::lock_guard lock(gMutex);
    if (gCurves.empty()) return;
    size_t quads = 0;
    std::vector<RenderCurve const*> visible;
    for (auto const& [key, rc] : gCurves) {
        if (std::get<0>(key) != dim) continue;
        if (rc.centre.distanceTo(o) - rc.reach > 160) continue;
        visible.push_back(&rc);
        quads += rc.quads.size();
    }
    if (visible.empty()) return;

    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(quads * 4), false);
    for (auto const* rc : visible) {
        for (auto const& q : rc->quads) {
            tess.color(q.r, q.g, q.b, 1.0f);
            for (auto const& v : q.v) tess.vertex(float(v.x - o.x), float(v.y - o.y), float(v.z - o.z));
        }
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLoggedFirstFrame) {
        gLoggedFirstFrame = true;
        log().info("tracks: rendering {} curve(s) ({} quads)", visible.size(), quads);
    }
}

} // namespace

// The chunk mesh leaves out tilted tracks (drawn above) while the server runs Levitate.
LL_STATIC_HOOK(
    TracksExtractHook,
    ll::memory::HookPriority::Normal,
    &ClientBlockPipeline::WorldExtractorStep::extractCustomBlock,
    std::optional<ClientBlockPipeline::PipelineError>,
    Block const&                                                       block,
    ClientBlockPipeline::WorldExtractorStep::BlockIterationData const& iterData,
    ClientBlockPipeline::WorldExtractorStep::ExtractionData&           extraction,
    ClientBlockPipeline::WorldView::Interface*                         worldView,
    bool                                                               shouldGenerateFaceCulling,
    ClientBlockPipeline::VisualReference                               visualReference,
    ClientBlockPipeline::ITintResolver&                                tintResolver,
    BiomeTintCache*                                                    biomeTintCache,
    BlockPos const&                                                    chunkMinPos,
    bool                                                               checkIgnoreBlockTransforms
) {
    if (visualReference == ClientBlockPipeline::VisualReference::Block && feature::tracks::isTrack(block)) {
        BlockPos const& rel = iterData.relativePos;
        ct::BlockPos3   pos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z};
        bool            tilted = false;
        {
            std::lock_guard lock(gMutex);
            tilted = gTiltedHere.contains(pos); // tilts of the dimension the player is in
        }
        if (tilted) return std::nullopt;
    }
    return origin(
        block,
        iterData,
        extraction,
        worldView,
        shouldGenerateFaceCulling,
        visualReference,
        tintResolver,
        biomeTintCache,
        chunkMinPos,
        checkIgnoreBlockTransforms
    );
}

LL_TYPE_INSTANCE_HOOK(
    TracksRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);
    if (renderAlphaLayer) return;
    try {
        renderCurves(renderContext);
    } catch (...) {
    }
}

class TrackRendererModule {
public:
    void install() {
        feature::net::client::on("tracks.curves", [](nlohmann::json const& msg) { onMessage(msg); });
        TracksRenderHook::hook();
        TracksExtractHook::hook();
        // the next world (or server) sends its own curves
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gCurves.clear();
            gTilts.clear();
            gRemesh.clear();
            gTiltedHere.clear();
            gTiltMeshes.clear();
            gLoggedFirstFrame = false;
            gLoggedTilt       = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        TracksRenderHook::unhook();
        TracksExtractHook::unhook();
        std::lock_guard lock(gMutex);
        gCurves.clear();
        gTilts.clear();
        gTiltMeshes.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

std::optional<double> tiltAt(int dim, core::tracks::BlockPos3 const& pos) {
    std::lock_guard lock(gMutex);
    auto it = gTilts.find({dim, pos});
    if (it == gTilts.end()) return std::nullopt;
    return it->second;
}

/// Number of curves the client knows (selftests / debugging).
size_t knownCurves() {
    std::lock_guard lock(gMutex);
    return gCurves.size();
}

} // namespace lv::client::tracks

LV_REGISTER_MODULE(lv::client::tracks::TrackRendererModule);

#endif // LV_CLIENT
