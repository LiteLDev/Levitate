#ifdef LV_CLIENT

// Client side of the pipes unit (client mod only): the fluid moving through glass pipes, as
// Create's TransparentStraightPipeRenderer / FluidRenderer.renderFluidFlow. Each face of a glass
// pipe with a flow shows a column of the fluid (3 px from the axis, the static fluid's width):
// an inbound flow grows from the face towards the centre, an outbound one from the centre
// towards the face, `progress` / 2 blocks long. The server sends the flows ("pipes.glass",
// src/feature/pipes/PipeEffects.cpp) when they change; the fronts advance here every tick at the
// flow's speed. Chunk meshes draw glass pipes without their static fluid (levitate:fluid 0), so
// only this renderer shows it; vanilla clients keep the static fluid.
//
// Fluid valves: Create's FluidValveRenderer turns the pointer smoothly (0 to -90 degrees about the
// valve's facing as `pointer` goes from closed to open). The server sends each valve's pointer,
// target and rate ("pipes.valve"); chunk meshes draw valves without their handle
// (levitate:pointer 1) and the handle is drawn here, turned by the pointer moving `rate` per tick.
// Vanilla clients see the handle switch between open and closed (levitate:enabled).

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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
#include "core/kinetics/RotorModel.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::pipes {

using Json = nlohmann::json;

namespace {

constexpr char const* kGlassPipe = "levitate:glass_fluid_pipe";
constexpr char const* kValve     = "levitate:fluid_valve";
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

struct Valve {
    float         pointer = 0, target = 0, rate = 0;
    std::uint64_t tick = 0; ///< client tick `pointer` was received at
};

struct State {
    std::mutex                                     mutex; ///< hidden, valves seen (chunk builder threads)
    std::unordered_map<Block const*, Block const*> hidden;
    std::unordered_map<Block const*, Block const*> handleless; ///< valve states without their handle
    std::set<std::array<int, 3>>                   valvesSeen;
    std::map<Key, Pipe>                            pipes;  ///< client thread
    std::map<Key, Valve>                           valves; ///< client thread
    std::map<std::string, std::unique_ptr<mce::Mesh>> handleMeshes; ///< client thread
    bool                                           logged = false, loggedValve = false;
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

void onValve(Json const& msg) {
    try {
        auto& s   = state();
        auto  op  = msg.value("op", "");
        int   dim = msg.value("dim", 0);
        auto  add = [&](Json const& v) {
            auto pos = v.at("pos");
            s.valves[Key{dim, pos.at(0).get<int>(), pos.at(1).get<int>(), pos.at(2).get<int>()}] =
                Valve{v.value("pointer", 0.f), v.value("target", 0.f), v.value("rate", 0.f), clientTick()};
        };
        if (op == "valve") add(msg);
        else if (op == "valves") {
            for (auto it = s.valves.begin(); it != s.valves.end();) it = (*it).first[0] == dim ? s.valves.erase(it) : std::next(it);
            for (auto const& v : msg.value("valves", Json::array())) add(v);
        }
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

// ---- valve handles ---------------------------------------------------------------------------------

/// The handle bone of a valve state (one per facing and along_first), as a mesh in block units.
mce::Mesh* handleMesh(Tessellator& tess, std::string const& bone) {
    auto& s = state();
    if (auto it = s.handleMeshes.find(bone); it != s.handleMeshes.end()) return it->second.get();
    auto& slot = s.handleMeshes[bone];
    static std::optional<core::kinetics::RotorModel> model = []() -> std::optional<core::kinetics::RotorModel> {
        auto          file = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks" / "fluid_valve.geo.json";
        std::ifstream in(file);
        auto          j = Json::parse(in, nullptr, false, true);
        if (j.is_discarded()) return std::nullopt;
        auto parsed = core::kinetics::parseGeometry(j);
        if (!parsed || !parsed->contains("geometry.levitate.fluid_valve")) return std::nullopt;
        return parsed->at("geometry.levitate.fluid_valve");
    }();
    if (!model) return nullptr;
    auto b = model->bones.find(bone);
    if (b == model->bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet("levitate_fluid_valve", 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_valve_handle", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// Create's FluidValveRenderer: the handle of every valve chunk meshes drew without one, turned by
/// the pointer (closed 0 .. open 1, -90 degrees about the facing).
void renderValves(BaseActorRenderContext& rc) {
    auto& s = state();
    std::vector<std::array<int, 3>> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.valvesSeen.empty()) return;
        positions.assign(s.valvesSeen.begin(), s.valvesSeen.end());
    }
    auto* player = client::currentPlayer();
    if (!player) return;
    int   dim    = static_cast<int>(player->getDimensionId());
    auto& region = player->getDimensionBlockSource();
    auto* moving = static_cast<MovingBlockActorRenderer*>(rc.mBlockEntityRenderDispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture = moving->mAtlasTexture.get();
    Vec3  cam  = rc.mImpl.get()->mCameraTargetPosition;
    auto& tess = rc.mScreenContext.tessellator;
    float now  = static_cast<float>(clientTick()) + rc.mFrameAlpha;
    static constexpr char const* faces[]{"down", "up", "north", "south", "west", "east"};
    std::vector<std::array<int, 3>> stale;
    int                             turning = 0;
    for (auto const& p : positions) {
        BlockPos bp{p[0], p[1], p[2]};
        if (!region.hasChunksAt(bp, 0, false) || region.getBlock(bp).getTypeName() != kValve) {
            stale.push_back(p);
            continue;
        }
        auto const& block   = region.getBlock(bp);
        int         facing  = std::clamp(game::intState(block, "levitate:facing").value_or(0), 0, 5);
        int         along   = game::intState(block, "levitate:along_first").value_or(0) != 0 ? 1 : 0;
        float       pointer = game::intState(block, "levitate:enabled").value_or(0) != 0 ? 1.f : 0.f;
        if (auto it = s.valves.find(Key{dim, p[0], p[1], p[2]}); it != s.valves.end()) {
            auto const& v     = it->second;
            float       ticks = std::max(0.f, now - static_cast<float>(v.tick));
            float       step  = v.rate * ticks;
            pointer = v.pointer < v.target ? std::min(v.target, v.pointer + step) : std::max(v.target, v.pointer - step);
        }
        if (pointer > 0.f && pointer < 1.f) ++turning;
        auto* mesh = handleMesh(tess, std::string("handle_closed_") + faces[facing] + "_" + std::to_string(along));
        if (!mesh) continue;
        // the facing axis (model x is mirrored: Bedrock's -x)
        glm::vec3 axis = facing < 2 ? glm::vec3{0, 1, 0} : facing < 4 ? glm::vec3{0, 0, 1} : glm::vec3{1, 0, 0};
        float     sign = (facing % 2 == 1) ? 1.f : -1.f;
        auto&     screen = rc.mScreenContext;
        auto      ref    = screen.camera.worldMatrixStack.get().push(false);
        glm::mat4 m{1.f};
        m = glm::translate(m, glm::vec3{bp.x - cam.x + .5f, bp.y - cam.y + .5f, bp.z - cam.z + .5f});
        m = glm::rotate(m, glm::radians(-90.f * pointer * sign), axis);
        m = glm::translate(m, glm::vec3{-.5f});
        ref.mat->_m.get() = ref.mat->_m.get() * m;
        mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    }
    if (!stale.empty()) {
        for (auto const& p : stale) s.valves.erase(Key{dim, p[0], p[1], p[2]}); // a new valve starts afresh
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.valvesSeen.erase(p);
    }
    if (turning > 0 && !s.loggedValve) {
        s.loggedValve = true;
        log().info("pipes client: a valve pointer turns");
    }
}

/// The valve state without its handle (levitate:pointer 1), cached per state.
Block const* withoutHandle(Block const& block) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto [it, fresh] = s.handleless.try_emplace(&block, nullptr);
    if (fresh && game::intState(block, "levitate:pointer").value_or(1) == 0) {
        if (auto const* type = block.getBlockType().getBlockState(HashedString("levitate:pointer")))
            if (auto b = block.setState(*type, 1)) it->second = b.as_ptr();
    }
    return it->second;
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
        s.handleless.clear();
        s.valvesSeen.clear();
    }
    s.pipes.clear();
    s.valves.clear();
    s.handleMeshes.clear();
    s.logged      = false;
    s.loggedValve = false;
    meshCache().clear();
}

} // namespace

// Chunk meshes: glass pipes lose their static fluid (a world with glass pipes runs Levitate, whose
// server sends the flows drawn here).
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block && block.getTypeName() == kGlassPipe) {
        if (auto const* hidden = withoutFluid(block)) use = hidden;
    } else if (visualReference == ClientBlockPipeline::VisualReference::Block && block.getTypeName() == kValve) {
        if (auto const* hidden = withoutHandle(block)) {
            use                 = hidden;
            BlockPos const& rel = iterData.relativePos;
            auto&           s   = state();
            std::lock_guard lock(s.mutex);
            s.valvesSeen.insert({chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
        }
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
    try {
        if (!renderAlphaLayer) renderValves(renderContext);
        else renderFlows(renderContext); // translucent: drawn with the alpha layer
    } catch (...) {
    }
}

class PipesClientModule {
public:
    void install() {
        PipesExtractHook::hook();
        PipesRenderHook::hook();
        feature::net::client::on("pipes.glass", [](Json const& msg) { onGlass(msg); });
        feature::net::client::on("pipes.valve", [](Json const& msg) { onValve(msg); });
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
