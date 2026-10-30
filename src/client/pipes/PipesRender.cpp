#ifdef LV_CLIENT

// Client side of the pipes unit (client mod only): the fluid moving through glass pipes, as
// Create's TransparentStraightPipeRenderer / FluidRenderer.renderFluidFlow. Each face of a glass
// pipe with a flow shows a column of the fluid (3 px from the axis, the static fluid's width):
// an inbound flow grows from the face towards the centre, an outbound one from the centre
// towards the face, `progress` / 2 blocks long. The server sends the flows ("pipes.glass",
// src/feature/pipes/PipeEffects.cpp) when they change; the fronts advance here every tick at the
// flow's speed. Chunk meshes draw glass pipes without their static fluid (levitate:fluid 0), so
// only this renderer shows it; vanilla clients keep the static fluid.

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "glm/gtc/matrix_transform.hpp"
#include "nlohmann/json.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
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
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::pipes {

using Json = nlohmann::json;

namespace {

constexpr char const* kGlassPipe = "levitate:glass_fluid_pipe";
constexpr float       kRadius    = 3.f / 16.f; ///< the fluid column's half width (the static fluid's)

struct Flow {
    int   face = 0, slot = 1;
    bool  inbound = true;
    float progress = 0, speed = 0;
};
struct Pipe {
    std::vector<Flow> flows;
    std::uint64_t     tick = 0; ///< client tick the progress was received at
};
using Key = std::array<int, 4>; // dim, x, y, z

struct State {
    std::mutex                                     mutex; ///< hidden (chunk builder threads)
    std::unordered_map<Block const*, Block const*> hidden;
    std::map<Key, Pipe>                            pipes; ///< client thread
    bool                                           logged = false;
};

State& state() {
    static State s;
    return s;
}

std::uint64_t clientTick() {
    auto* ci = client::currentClient();
    auto* l  = ci ? ci->getLevel() : nullptr;
    return l ? static_cast<std::uint64_t>(l->getCurrentTick().tickID) : 0;
}

void onGlass(Json const& msg) {
    try {
        auto& s   = state();
        auto  pos = msg.at("pos");
        Key   key{msg.value("dim", 0), pos.at(0).get<int>(), pos.at(1).get<int>(), pos.at(2).get<int>()};
        auto  flows = msg.value("flows", Json::array());
        if (flows.empty()) {
            s.pipes.erase(key);
            return;
        }
        Pipe p;
        p.tick = clientTick();
        for (auto const& f : flows) p.flows.push_back({f.at(0).get<int>(), f.at(1).get<int>(), f.at(2).get<int>() != 0, f.at(3).get<float>(), f.at(4).get<float>()});
        s.pipes[key] = std::move(p);
    } catch (...) {
    }
}

// ---- meshes --------------------------------------------------------------------------------------

std::map<int, std::unique_ptr<mce::Mesh>>& meshCache() {
    static std::map<int, std::unique_ptr<mce::Mesh>> c;
    return c;
}

char const* slotTexture(int slot) {
    static constexpr char const* names[]{"levitate_pipe_fluid_potion", "levitate_pipe_fluid_water",     "levitate_pipe_fluid_lava",
                                         "levitate_pipe_fluid_honey",  "levitate_pipe_fluid_chocolate", "levitate_pipe_fluid_milk",
                                         "levitate_pipe_fluid_potion", "levitate_pipe_fluid_tea"};
    return names[std::clamp(slot, 0, 7)];
}

/// A unit cube (0..1) textured with the fluid of `slot`, scaled into place per flow.
mce::Mesh* cubeMesh(Tessellator& tess, int slot) {
    auto& cache = meshCache();
    if (auto it = cache.find(slot); it != cache.end()) return it->second.get();
    auto& out = cache[slot];
    auto  tex = BlockGraphics::getTextureUVCoordinateSet(slotTexture(slot), 0, 0);
    // the middle half of the texture, as the static fluid's 6 px wide faces
    float u0 = tex._u0 + (tex._u1 - tex._u0) * .25f, u1 = tex._u0 + (tex._u1 - tex._u0) * .75f;
    float v0 = tex._v0 + (tex._v1 - tex._v0) * .25f, v1 = tex._v0 + (tex._v1 - tex._v0) * .75f;
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        tess.simpleQuad(a, Vec2{u0, v0}, b, Vec2{u0, v1}, c, Vec2{u1, v1}, d, Vec2{u1, v0});
        tess.simpleQuad(d, Vec2{u1, v0}, c, Vec2{u1, v1}, b, Vec2{u0, v1}, a, Vec2{u0, v0});
    };
    quad({0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}); // up
    quad({0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}); // down
    quad({0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}); // north
    quad({0, 1, 1}, {1, 1, 1}, {1, 0, 1}, {0, 0, 1}); // south
    quad({0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {0, 0, 0}); // west
    quad({1, 1, 0}, {1, 0, 0}, {1, 0, 1}, {1, 1, 1}); // east
    out = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_pipe_fluid", SupplementaryFieldAutoGenerationMode::None));
    return out.get();
}

/// The box of a flow on `face` (block coordinates 0..1): Create's renderFluidFlow.
std::pair<glm::vec3, glm::vec3> flowBox(int face, bool inbound, float progress) {
    static constexpr int dirs[6][3]{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    auto const& d    = dirs[std::clamp(face, 0, 5)];
    float       len  = std::clamp(progress, 0.f, 1.f) * .5f;
    float       from = inbound ? .5f - len : 0.f, to = inbound ? .5f : len; // distance from the centre
    glm::vec3   lo{.5f - kRadius}, hi{.5f + kRadius};
    for (int a = 0; a < 3; ++a) {
        if (d[a] == 0) continue;
        float x0 = .5f + d[a] * from, x1 = .5f + d[a] * to;
        lo[a]    = std::min(x0, x1);
        hi[a]    = std::max(x0, x1);
    }
    return {lo, hi};
}

void renderFlows(BaseActorRenderContext& rc) {
    auto& s = state();
    if (s.pipes.empty()) return;
    auto* player = client::currentPlayer();
    if (!player) return;
    int  dim     = static_cast<int>(player->getDimensionId());
    auto& region = player->getDimensionBlockSource();
    auto& dispatcher = rc.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const&  material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerBlend)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture = moving->mAtlasTexture.get();
    Vec3         cam   = rc.mImpl.get()->mCameraTargetPosition;
    auto&        tess  = rc.mScreenContext.tessellator;
    float        now   = static_cast<float>(clientTick()) + rc.mFrameAlpha;
    int          drawn = 0;
    for (auto it = s.pipes.begin(); it != s.pipes.end();) {
        auto const& [key, pipe] = *it;
        BlockPos pos{key[1], key[2], key[3]};
        if (key[0] != dim) {
            ++it;
            continue;
        }
        if (region.hasChunksAt(pos, 0, false) && region.getBlock(pos).getTypeName() != kGlassPipe) {
            it = s.pipes.erase(it); // the pipe is gone (the server sends a removal too)
            continue;
        }
        float ticks = std::max(0.f, now - static_cast<float>(pipe.tick));
        for (auto const& f : pipe.flows) {
            float progress = std::min(1.f, f.progress + f.speed * ticks);
            if (progress <= 0.f) continue;
            auto [lo, hi] = flowBox(f.face, f.inbound, progress);
            auto* mesh    = cubeMesh(tess, f.slot);
            if (!mesh) continue;
            auto& screen = rc.mScreenContext;
            auto& world  = screen.camera.worldMatrixStack.get();
            auto  ref    = world.push(false);
            glm::mat4 m{1.f};
            m = glm::translate(m, glm::vec3{pos.x - cam.x, pos.y - cam.y, pos.z - cam.z} + lo);
            m = glm::scale(m, hi - lo);
            ref.mat->_m.get() = ref.mat->_m.get() * m;
            mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
            ++drawn;
        }
        ++it;
    }
    if (drawn > 0 && !s.logged) {
        s.logged = true;
        log().info("pipes client: drawing {} glass pipe flows", drawn);
    }
}

/// The glass pipe state without its static fluid, cached per state.
Block const* withoutFluid(Block const& block) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto [it, fresh] = s.hidden.try_emplace(&block, nullptr);
    if (fresh && game::intState(block, "levitate:fluid").value_or(0) != 0) {
        if (auto const* type = block.getBlockType().getBlockState(HashedString("levitate:fluid")))
            if (auto b = block.setState(*type, 0)) it->second = b.as_ptr();
    }
    return it->second;
}

void reset() {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.hidden.clear();
    }
    s.pipes.clear();
    s.logged = false;
    meshCache().clear();
}

} // namespace

// Chunk meshes: glass pipes lose their static fluid while the server is a Levitate server (it
// sends the flows drawn here).
LL_STATIC_HOOK(
    PipesExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block && block.getTypeName() == kGlassPipe && feature::net::client::connected()) {
        if (auto const* hidden = withoutFluid(block)) use = hidden;
    }
    return origin(*use, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    PipesRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);
    if (!renderAlphaLayer) return; // translucent: drawn with the alpha layer
    try {
        renderFlows(renderContext);
    } catch (...) {
    }
}

class PipesClientModule {
public:
    void install() {
        PipesExtractHook::hook();
        PipesRenderHook::hook();
        feature::net::client::on("pipes.glass", [](Json const& msg) { onGlass(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { reset(); });
    }
    void uninstall() {
        PipesExtractHook::unhook();
        PipesRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
        reset();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::pipes

LV_REGISTER_MODULE(lv::client::pipes::PipesClientModule);

#endif // LV_CLIENT
