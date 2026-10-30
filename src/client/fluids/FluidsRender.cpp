// Client: fluid machine rendering (client build only).
//
// - Hose pulleys: the hose hanging from the pulley down to its end piece, extrapolated between
//   the server's updates ("fluids.sync" op hose / hoses / hose_remove).
// - Spouts: the fluid stream from the nozzle down to the item while it is filled (op spout), as
//   Create's SpoutRenderer: its width follows ((2p - 1)^2 - 1) / 32 over the 20 filling ticks.
// Both are drawn as coloured boxes after the block entities of the opaque pass.
// - Spout nozzles: Create's SpoutRenderer squeezes the three bits of the nozzle together while it
//   fills (squeeze = the stream's curve, -1 from 10 to 2 ticks left, then back to 0): chunk meshes
//   draw spouts without their nozzle (levitate:nozzle 1) and the bits are drawn here, each one
//   3 / 32 * squeeze higher than the one above it. Vanilla clients see the static nozzle.

#ifdef LV_CLIENT

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "glm/gtc/matrix_transform.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "core/kinetics/RotorModel.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::fluids {

namespace {

using Clock = std::chrono::steady_clock;
using Key   = std::tuple<int, int, int, int>; // dim, x, y, z

struct Hose {
    float             offset = 0;
    float             speed  = 0; ///< blocks per tick
    Clock::time_point at;
};
struct Spout {
    std::uint32_t     color = 0;
    Clock::time_point at;
};

std::mutex           gMutex;
std::map<Key, Hose>  gHoses;
std::map<Key, Spout> gSpouts;
bool                 gLoggedHose = false;

constexpr char const* kSpoutId = "levitate:spout";

/// Spouts seen by the chunk builder (their nozzle is drawn here) and their nozzle-less states.
struct Nozzles {
    std::mutex                                     mutex; ///< chunk builder threads
    std::set<std::array<int, 3>>                   tracked;
    std::unordered_map<Block const*, Block const*> hidden;
    std::map<std::string, std::unique_ptr<mce::Mesh>> meshes; ///< client thread
    bool                                           logged = false;
};
Nozzles& nozzles() {
    static Nozzles n;
    return n;
}

constexpr double kPx = 1 / 16.0;

Key keyOf(int dim, nlohmann::json const& pos) { return {dim, pos.at(0).get<int>(), pos.at(1).get<int>(), pos.at(2).get<int>()}; }

void onSync(nlohmann::json const& msg) {
    auto op  = msg.value("op", "");
    int  dim = msg.value("dim", 0);
    std::lock_guard lock(gMutex);
    try {
        if (op == "hose") {
            gHoses[keyOf(dim, msg.at("pos"))] = {msg.value("offset", 0.f), msg.value("speed", 0.f), Clock::now()};
        } else if (op == "hose_remove") {
            gHoses.erase(keyOf(dim, msg.at("pos")));
        } else if (op == "hoses") {
            for (auto it = gHoses.begin(); it != gHoses.end();) it = std::get<0>(it->first) == dim ? gHoses.erase(it) : std::next(it);
            for (auto const& h : msg.value("hoses", nlohmann::json::array()))
                gHoses[keyOf(dim, h.at("pos"))] = {h.value("offset", 0.f), h.value("speed", 0.f), Clock::now()};
        } else if (op == "spout") {
            gSpouts[keyOf(dim, msg.at("pos"))] = {msg.value("color", 0u), Clock::now()};
        }
    } catch (...) {
    }
}

struct Quad {
    std::array<Vec3, 4> v;
    float               r, g, b;
};

/// The six faces of an axis-aligned box.
void box(std::vector<Quad>& out, Vec3 lo, Vec3 hi, float r, float g, float b) {
    auto add = [&](Vec3 a, Vec3 bb, Vec3 c, Vec3 d, float shade) { out.push_back({{a, bb, c, d}, r * shade, g * shade, b * shade}); };
    add({lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, 1.f);   // up
    add({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, .5f);   // down
    add({lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}, .8f);   // north
    add({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, .8f);   // south
    add({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, .6f);   // west
    add({hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}, .6f);   // east
}

float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

void drawFluids(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player || !ctx.mImpl) return;
    int  dim    = static_cast<int>(player->getDimensionId());
    Vec3 origin = ctx.mImpl->mCameraTargetPosition;
    auto now    = Clock::now();

    std::vector<Quad> quads;
    {
        std::lock_guard lock(gMutex);
        if (gHoses.empty() && gSpouts.empty()) return;
        for (auto const& [k, h] : gHoses) {
            auto [d, x, y, z] = k;
            if (d != dim) continue;
            float ticks  = std::chrono::duration<float>(now - h.at).count() * 20.f;
            float offset = std::max(0.f, h.offset + h.speed * std::min(ticks, 10.f));
            if (offset <= 0.01f) continue;
            double cx = x + .5, cz = z + .5, end = y - offset;
            // the hose (dark rubber) and its copper end piece
            box(quads, {float(cx - 2 * kPx), float(end + 4 * kPx), float(cz - 2 * kPx)}, {float(cx + 2 * kPx), float(y), float(cz + 2 * kPx)},
                lin(0x3a), lin(0x30), lin(0x2c));
            box(quads, {float(cx - 5 * kPx), float(end), float(cz - 5 * kPx)}, {float(cx + 5 * kPx), float(end + 4 * kPx), float(cz + 5 * kPx)},
                lin(0xc4), lin(0x6e), lin(0x43));
        }
        for (auto it = gSpouts.begin(); it != gSpouts.end();) {
            float ticks = std::chrono::duration<float>(now - it->second.at).count() * 20.f;
            if (ticks > 20) {
                it = gSpouts.erase(it);
                continue;
            }
            auto [d, x, y, z] = it->first;
            if (d == dim) {
                // Create's SpoutRenderer: processingPT counts down from 20
                float pt       = 20 - ticks;
                float progress = std::clamp(1 - (pt - 5) / 10, 0.f, 1.f);
                float radius   = std::abs(float(std::pow(2 * progress - 1, 2) - 1)) / 32.f;
                if (radius > 0.002f) {
                    auto   c  = it->second.color;
                    double cx = x + .5, cz = z + .5;
                    box(quads, {float(cx - radius), float(y - 1.2), float(cz - radius)}, {float(cx + radius), float(y + 0.5), float(cz + radius)},
                        lin(int(c >> 16 & 0xff)), lin(int(c >> 8 & 0xff)), lin(int(c & 0xff)));
                }
            }
            ++it;
        }
    }
    if (quads.empty()) return;
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(quads.size() * 4), false);
    for (auto const& q : quads) {
        tess.color(q.r, q.g, q.b, 1.0f);
        for (auto const& v : q.v) tess.vertex(v.x - origin.x, v.y - origin.y, v.z - origin.z);
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLoggedHose) {
        gLoggedHose = true;
        log().info("fluids: rendering {} quads (hoses / spout streams)", quads.size());
    }
}

// ---- spout nozzles ---------------------------------------------------------------------------------

/// Create's SpoutRenderer squeeze for `pt` ticks of processing left (processingTicks - partial).
float squeezeOf(float pt) {
    float progress = std::clamp(1 - (pt - 5) / 10, 0.f, 1.f);
    float squeeze  = float(std::pow(2 * progress - 1, 2) - 1);
    if (pt < 0) squeeze = 0;
    else if (pt < 2) squeeze = -pt / 2; // lerp(pt / 2, 0, -1)
    else if (pt < 10) squeeze = -1;
    return squeeze;
}

mce::Mesh* nozzleMesh(Tessellator& tess, std::string const& bone) {
    auto& n = nozzles();
    if (auto it = n.meshes.find(bone); it != n.meshes.end()) return it->second.get();
    auto& slot = n.meshes[bone];
    static std::optional<core::kinetics::RotorModel> model = []() -> std::optional<core::kinetics::RotorModel> {
        auto          file = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks" / "spout.geo.json";
        std::ifstream in(file);
        auto          j = nlohmann::json::parse(in, nullptr, false, true);
        if (j.is_discarded()) return std::nullopt;
        auto parsed = core::kinetics::parseGeometry(j);
        if (!parsed || !parsed->contains("geometry.levitate.spout")) return std::nullopt;
        return parsed->at("geometry.levitate.spout");
    }();
    if (!model) return nullptr;
    auto b = model->bones.find(bone);
    if (b == model->bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet("levitate_spout", 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_spout_nozzle", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

void drawNozzles(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player || !ctx.mImpl) return;
    auto& n = nozzles();
    std::vector<std::array<int, 3>> positions;
    {
        std::lock_guard lock(n.mutex);
        if (n.tracked.empty()) return;
        positions.assign(n.tracked.begin(), n.tracked.end());
    }
    int   dim    = static_cast<int>(player->getDimensionId());
    auto& region = player->getDimensionBlockSource();
    auto* moving = static_cast<MovingBlockActorRenderer*>(ctx.mBlockEntityRenderDispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture = moving->mAtlasTexture.get();
    Vec3                            cam  = ctx.mImpl->mCameraTargetPosition;
    auto&                           tess = ctx.mScreenContext.tessellator;
    auto                            now  = Clock::now();
    std::vector<std::array<int, 3>> stale;
    int                             squeezing = 0;
    for (auto const& p : positions) {
        BlockPos bp{p[0], p[1], p[2]};
        if (!region.hasChunksAt(bp, 0, false) || region.getBlock(bp).getTypeName() != kSpoutId) {
            stale.push_back(p);
            continue;
        }
        float squeeze = 0;
        {
            std::lock_guard lock(gMutex);
            if (auto it = gSpouts.find(Key{dim, p[0], p[1], p[2]}); it != gSpouts.end()) {
                float ticks = std::chrono::duration<float>(now - it->second.at).count() * 20.f;
                squeeze     = squeezeOf(20 - ticks);
            }
        }
        if (squeeze != 0) ++squeezing;
        float y = 0;
        for (char const* bone : {"nozzle_top", "nozzle_middle", "nozzle_bottom"}) {
            if (auto* mesh = nozzleMesh(tess, bone)) {
                auto& screen = ctx.mScreenContext;
                auto  ref    = screen.camera.worldMatrixStack.get().push(false);
                glm::mat4 m  = glm::translate(glm::mat4{1.f}, glm::vec3{bp.x - cam.x, bp.y - cam.y + y, bp.z - cam.z});
                ref.mat->_m.get() = ref.mat->_m.get() * m;
                mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
            }
            y += -3 * squeeze / 32;
        }
    }
    if (!stale.empty()) {
        std::lock_guard lock(n.mutex);
        for (auto const& p : stale) n.tracked.erase(p);
    }
    if (squeezing > 0 && !n.logged) {
        n.logged = true;
        log().info("fluids: spout nozzle squeezes while filling");
    }
}

/// The spout state without its nozzle (levitate:nozzle 1), cached per state.
Block const* nozzleless(Block const& block) {
    auto&           n = nozzles();
    std::lock_guard lock(n.mutex);
    auto [it, fresh] = n.hidden.try_emplace(&block, nullptr);
    if (fresh) {
        if (auto const* type = block.getBlockType().getBlockState(HashedString("levitate:nozzle")))
            if (auto b = block.setState(*type, 1)) it->second = b.as_ptr();
    }
    return it->second;
}

void resetNozzles() {
    auto& n = nozzles();
    {
        std::lock_guard lock(n.mutex);
        n.tracked.clear();
        n.hidden.clear();
    }
    n.meshes.clear();
    n.logged = false;
}

} // namespace

// Chunk meshes: spouts are drawn without their nozzle (drawn and squeezed above).
LL_STATIC_HOOK(
    FluidsExtractHook,
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
    Block const* use = &block;
    if (visualReference == ClientBlockPipeline::VisualReference::Block && block.getTypeName() == kSpoutId) {
        if (auto const* hidden = nozzleless(block)) {
            use                 = hidden;
            BlockPos const& rel = iterData.relativePos;
            auto&           n   = nozzles();
            std::lock_guard lock(n.mutex);
            n.tracked.insert({chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
        }
    }
    return origin(*use, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    FluidsRenderHook,
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
        drawFluids(renderContext);
        drawNozzles(renderContext);
    } catch (...) {
    }
}

class FluidsRenderModule {
public:
    void install() {
        feature::net::client::on("fluids.sync", [](nlohmann::json const& msg) { onSync(msg); });
        FluidsRenderHook::hook();
        FluidsExtractHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            {
                std::lock_guard lock(gMutex);
                gHoses.clear();
                gSpouts.clear();
                gLoggedHose = false;
            }
            resetNozzles();
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        FluidsRenderHook::unhook();
        FluidsExtractHook::unhook();
        resetNozzles();
        std::lock_guard lock(gMutex);
        gHoses.clear();
        gSpouts.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::fluids

LV_REGISTER_MODULE(lv::client::fluids::FluidsRenderModule);

#endif // LV_CLIENT
