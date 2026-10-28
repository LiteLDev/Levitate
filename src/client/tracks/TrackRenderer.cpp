// Client: draws curved tracks (Bedrock blocks can't be curved).
//
// The server sends every curve as a script message on the "levitate:tracks" channel (see
// src/feature/tracks/TrackSync.cpp); this file keeps them per dimension and draws ties and rails
// along each curve every frame, after the block entities of the opaque pass.

#ifdef LV_CLIENT

#include <array>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/network/ClientNetworkHandler.h"
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
#include "mc/network/packet/ScriptMessagePacket.h"

#include "client/Client.h"
#include "client/tracks/TrackRenderer.h"
#include "core/tracks/Curve.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::tracks {

namespace ct = core::tracks;

namespace {

constexpr std::string_view kChannel = "levitate:tracks";

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

// Model sizes in blocks (our own track model: ties 2 px high, rails 2 px on top of them)
constexpr double kPx         = 1 / 16.0;
constexpr double kTieHalfW   = 15 * kPx;
constexpr double kTieHalfL   = 2 * kPx;
constexpr double kTieH       = 2 * kPx;
constexpr double kRailOffset = 11 * kPx;
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
    // colours of our track texture (tracks.ps1): dark spruce ties, andesite rails
    float const tieR = 0.33f, tieG = 0.24f, tieB = 0.16f;
    float const railR = 0.66f, railG = 0.67f, railB = 0.63f;

    for (size_t i = 0; i < segs.size(); ++i) {
        auto const& s    = segs[i];
        ct::Vec3d   up   = s.faceNormal;
        ct::Vec3d   side = s.normal;
        ct::Vec3d   fwd  = s.derivative;
        // ties on every second segment (one every block), not on the end blocks' edges
        if (i % 2 == 1 && i + 1 < segs.size()) {
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

void onMessage(std::string const& value) {
    auto msg = nlohmann::json::parse(value, nullptr, false);
    if (msg.is_discarded()) return;
    std::string op  = msg.value("op", "");
    int         dim = msg.value("dim", 0);
    std::lock_guard lock(gMutex);
    if (op == "reset") {
        gCurves.clear();
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

void renderCurves(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int  dim    = static_cast<int>(player->getDimensionId());
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

LL_TYPE_INSTANCE_HOOK(
    TracksScriptMessageHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&   source,
    ScriptMessagePacket const& packet
) {
    std::string const& id = packet.mMessageId;
    if (id == kChannel) {
        onMessage(packet.mMessageValue);
        return;
    }
    origin(source, packet);
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
        TracksScriptMessageHook::hook();
        TracksRenderHook::hook();
    }
    void uninstall() {
        TracksRenderHook::unhook();
        TracksScriptMessageHook::unhook();
        std::lock_guard lock(gMutex);
        gCurves.clear();
    }
};

/// Number of curves the client knows (selftests / debugging).
size_t knownCurves() {
    std::lock_guard lock(gMutex);
    return gCurves.size();
}

} // namespace lv::client::tracks

LV_REGISTER_MODULE(lv::client::tracks::TrackRendererModule);

#endif // LV_CLIENT
