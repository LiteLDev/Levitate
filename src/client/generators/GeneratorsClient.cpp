#ifdef LV_CLIENT

// Client side of the generators unit (U7), client mod only.
//
// - Hand crank / valve handle handles: drawn every frame with Create's independent handle angle
//   (the kinetics renderer only spins their short shaft): a hand crank's handle chases 10/3 of
//   the block's speed (HandCrankBlockEntity.tick, the angle in radians is the sum / 360); a valve
//   handle turns from 0 to its target angle over the turn's ticks (the server tells companions
//   about each turn) and follows the network like a shaft when another source drives it.
// - Large water wheel: the structure blocks around the wheel show their cell of the wheel to
//   vanilla clients; the client mod hides them (as the kinetics renderer hides the wheel's own
//   cell) and the kinetics renderer spins the whole wheel instead.
// Speeds come from the kinetic sync channel (read here as well, the kinetics renderer keeps its
// own copy).

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "glm/gtc/matrix_transform.hpp"
#include "nlohmann/json.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/network/ClientNetworkHandler.h"
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
#include "mc/network/packet/ScriptMessagePacket.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "core/generators/Generators.h"
#include "core/kinetics/RotorModel.h"
#include "core/kinetics/Solver.h"
#include "core/kinetics/SyncCodec.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::generators {

namespace k  = core::kinetics;
namespace ks = core::kinetics::sync;
namespace cg = core::generators;
using Face   = k::Face;

namespace {

enum class Kind : std::uint8_t { None, Crank, Valve, Structure };

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept { return k::GlobalPosHash{}(core::GlobalPos{0, p.x, p.y, p.z}); }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

/// What the client knows about a block state (computed once per Block, copied out under the lock).
struct Info {
    Kind         kind   = Kind::None;
    Block const* hidden = nullptr; ///< structure: its `rotor = 1` variant
    int          facing = 1;
};

struct Synced {
    float speed      = 0;
    int   sourceFace = -1;
};
struct ValveTurn {
    int           dim    = 0;
    std::uint64_t start  = 0;
    int           total  = 0;
    int           target = 0;
    bool          backwards = false;
    int           facing = 1;
};
struct Crank {
    std::uint64_t tick     = 0;
    float         chasing  = 0;
    float         angle    = 0; ///< Create's independentAngle
};

struct State {
    std::shared_mutex                                                              infoMutex; ///< infos (chunk builder threads)
    std::unordered_map<Block const*, Info>                                         infos;
    std::mutex                                                                     mutex; ///< tracked (chunk builder threads), speeds, valves
    std::unordered_set<BlockPos, PosHash, PosEq>                                   tracked;
    std::unordered_map<int, std::unordered_map<BlockPos, Synced, PosHash, PosEq>> speeds;
    std::unordered_map<BlockPos, ValveTurn, PosHash, PosEq>                        valves;
    // client thread only
    std::unordered_map<BlockPos, Crank, PosHash, PosEq> cranks;
    void const*                                         level      = nullptr;
    bool                                                loggedDraw = false;
};

State& state() {
    static State s;
    return s;
}

Info computeInfo(Block const& block) {
    Info        info;
    std::string id = block.getTypeName();
    if (!id.starts_with("levitate:")) return info;
    if (id == "levitate:hand_crank") {
        info.kind = Kind::Crank;
    } else if (id.ends_with("_valve_handle")) {
        info.kind = Kind::Valve;
    } else if (id == "levitate:water_wheel_structure") {
        if (game::intState(block, "levitate:rotor").value_or(-1) == 0) {
            if (auto const* st = block.getBlockType().getBlockState(HashedString("levitate:rotor"))) {
                if (auto b = block.setState(*st, 1)) {
                    info.kind   = Kind::Structure;
                    info.hidden = b.as_ptr();
                }
            }
        }
        return info;
    } else {
        return info;
    }
    int f       = game::intState(block, "levitate:facing").value_or(1);
    info.facing = f >= 0 && f <= 5 ? f : 1;
    return info;
}

Info infoOf(Block const& block) {
    auto& s = state();
    {
        std::shared_lock lock(s.infoMutex);
        if (auto it = s.infos.find(&block); it != s.infos.end()) return it->second;
    }
    Info            info = computeInfo(block);
    std::unique_lock lock(s.infoMutex);
    s.infos.emplace(&block, info);
    return info;
}

/// The rotor geometry and terrain texture of a handle block.
std::pair<std::string, std::string> handleModel(Block const& block, Kind kind) {
    std::string id = block.getTypeName();
    return {kind == Kind::Crank ? "geometry.levitate.hand_crank.rotor" : "geometry.levitate.valve_handle.rotor", "levitate_" + id.substr(9)};
}

// ---- meshes (as the kinetics renderer builds its rotor meshes) -------------------------------------------

std::map<std::string, k::RotorModel> const& models() {
    static std::map<std::string, k::RotorModel> out = [] {
        std::map<std::string, k::RotorModel> m;
        auto dir = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        for (auto const* file : {"hand_crank.geo.json", "valve_handle.geo.json"}) {
            std::ifstream in(dir / file);
            auto          j = nlohmann::json::parse(in, nullptr, false, true);
            if (j.is_discarded()) continue;
            if (auto g = k::parseGeometry(j)) m.merge(*g);
        }
        return m;
    }();
    return out;
}

std::map<std::string, std::unique_ptr<mce::Mesh>>& meshCache() {
    static std::map<std::string, std::unique_ptr<mce::Mesh>> cache;
    return cache;
}

mce::Mesh* handleMesh(Tessellator& tess, std::string const& model, std::string const& bone, std::string const& texture) {
    auto  key   = model + "|" + bone + "|" + texture;
    auto& cache = meshCache();
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    auto&       slot = cache[key];
    auto const& all  = models();
    auto        m    = all.find(model);
    if (m == all.end()) return nullptr;
    auto b = m->second.bones.find(bone);
    if (b == m->second.bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet(texture, 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_handle", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// A new level (block states are registered again) or leaving it (`all`: the speeds too).
void resetLevel(bool all) {
    auto& s = state();
    {
        std::unique_lock lock(s.infoMutex);
        s.infos.clear();
    }
    std::lock_guard lock(s.mutex);
    s.valves.clear();
    s.cranks.clear();
    s.loggedDraw = false;
    if (all) {
        s.speeds.clear();
        s.tracked.clear();
    }
    meshCache().clear();
}

/// The handle's angle in degrees about the facing's positive axis.
float handleAngle(Info const& info, int dim, BlockPos const& pos, Synced const& synced, std::uint64_t tick, float partial) {
    auto& s = state();
    if (info.kind == Kind::Crank) {
        // HandCrankBlockEntity.tick / getIndependentAngle: angle (radians) = (independentAngle + pt * chasing) / 360
        auto& c = s.cranks[pos];
        if (c.tick == 0 || tick < c.tick || tick - c.tick > 200) c.tick = tick;
        while (c.tick < tick) {
            c.chasing = cg::crankChase(c.chasing, synced.speed);
            c.angle   = std::fmod(c.angle + c.chasing, 360.f * 360.f * 2.f * 3.14159265f);
            ++c.tick;
        }
        float rad = (c.angle + partial * c.chasing) / 360.f;
        return rad * 180.f / 3.14159265f;
    }
    // valve handle: a turn in progress (or its target at rest), or the network's angle when driven
    std::optional<ValveTurn> turn;
    {
        std::lock_guard lock(s.mutex);
        if (auto it = s.valves.find(pos); it != s.valves.end() && it->second.dim == dim) turn = it->second;
    }
    float elapsed = turn ? static_cast<float>(tick - turn->start) + partial : 0.f;
    if ((!turn || elapsed >= static_cast<float>(turn->total)) && synced.sourceFace >= 0 && synced.speed != 0) {
        auto  axis   = k::axisOf(static_cast<Face>(info.facing));
        float offset = k::rotationOffset(core::GlobalPos{0, pos.x, pos.y, pos.z}, axis, false, 0);
        return k::rotationAngle(static_cast<double>(tick) + partial, synced.speed, offset);
    }
    if (!turn) return 0.f;
    return cg::valveAngle(turn->target, turn->total, elapsed, turn->backwards, static_cast<Face>(turn->facing));
}

struct DrawItem {
    BlockPos     pos;
    Info         info;
    Block const* block;
    Synced       synced;
};

void renderHandles(BaseActorRenderContext& ctx) {
    auto* ci = lv::client::currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    auto& s      = state();
    if (level != s.level) {
        resetLevel(false);
        s.level = level;
    }
    if (!level || !player) return;
    auto& region = player->getDimensionBlockSource();
    int   dim    = static_cast<int>(player->getDimensionId());

    std::vector<BlockPos> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.tracked.empty()) return;
        positions.assign(s.tracked.begin(), s.tracked.end());
    }
    std::vector<DrawItem> items;
    std::vector<BlockPos> stale;
    for (auto const& p : positions) {
        if (!region.hasChunksAt(p, 0, false)) {
            stale.push_back(p);
            continue;
        }
        auto const& block = region.getBlock(p);
        auto        info  = infoOf(block);
        if (info.kind != Kind::Crank && info.kind != Kind::Valve) {
            stale.push_back(p);
            continue;
        }
        items.push_back(DrawItem{p, info, &block, {}});
    }
    {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) {
            s.tracked.erase(p);
            s.valves.erase(p);
        }
        auto const& speeds = s.speeds[dim];
        for (auto& item : items)
            if (auto sp = speeds.find(item.pos); sp != speeds.end()) item.synced = sp->second;
    }
    for (auto const& p : stale) s.cranks.erase(p);
    if (items.empty()) return;

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&      screen = ctx.mScreenContext;
    auto&      tess   = screen.tessellator;
    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    auto       tick   = static_cast<std::uint64_t>(level->getCurrentTick().tickID);
    float      alpha  = ctx.mFrameAlpha;
    auto&      world  = screen.camera.worldMatrixStack.get();

    for (auto const& item : items) {
        auto const& info          = item.info;
        auto        facing        = static_cast<Face>(info.facing);
        auto [model, textureName] = handleModel(*item.block, info.kind);
        auto* mesh                = handleMesh(tess, model, k::isPositive(facing) ? "handle_pos" : "handle_neg", textureName);
        if (!mesh) continue;
        float     angle = handleAngle(info, dim, item.pos, item.synced, tick, alpha);
        auto      a     = k::axisOf(facing);
        glm::vec3 axis{a == k::Axis::X ? 1.f : 0.f, a == k::Axis::Y ? 1.f : 0.f, a == k::Axis::Z ? 1.f : 0.f};
        auto      ref = world.push(false);
        glm::mat4 m{1.f};
        m = glm::translate(m, glm::vec3{item.pos.x - cam.x + .5f, item.pos.y - cam.y + .5f, item.pos.z - cam.z + .5f});
        m = glm::rotate(m, glm::radians(angle), axis);
        // the model is built around Y: turn it onto the facing's axis (as the kinetics renderer)
        if (a == k::Axis::X) m = glm::rotate(m, glm::radians(-90.f), glm::vec3{0.f, 0.f, 1.f});
        else if (a == k::Axis::Z) m = glm::rotate(m, glm::radians(90.f), glm::vec3{1.f, 0.f, 0.f});
        m = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
        ref.mat->_m.get() = ref.mat->_m.get() * m;
        mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    }
    if (!s.loggedDraw) {
        s.loggedDraw = true;
        log().info("generators client: drawing {} handles", items.size());
    }
}

void onSync(std::string const& text) {
    auto batch = ks::decode(text);
    if (!batch) return;
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto&           m = s.speeds[batch->dim];
    if (batch->reset) m.clear();
    for (auto const& e : batch->entries) {
        BlockPos p{e.x, e.y, e.z};
        if (e.removed) m.erase(p);
        else m[p] = Synced{e.speed, e.sourceFace};
    }
}

} // namespace

// Chunk meshes: remember where handles are; hide the cells of large water wheels.
LL_STATIC_HOOK(
    GeneratorsExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block) {
        auto info = infoOf(block);
        if (info.kind == Kind::Structure && info.hidden) {
            return origin(*info.hidden, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
        }
        if (info.kind == Kind::Crank || info.kind == Kind::Valve) {
            BlockPos const& rel = iterData.relativePos;
            auto&           s   = state();
            std::lock_guard lock(s.mutex);
            s.tracked.insert(BlockPos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
        }
    }
    return origin(block, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    GeneratorsRenderHook,
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
        renderHandles(renderContext);
    } catch (...) {
    }
}

// The kinetic speed sync: keep a copy of the speeds (the kinetics renderer consumes the packet).
LL_TYPE_INSTANCE_HOOK(
    GeneratorsSyncHook,
    ll::memory::HookPriority::High,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&   source,
    ScriptMessagePacket const& packet
) {
    if (*packet.mMessageId == ks::kSyncId) onSync(*packet.mMessageValue);
    origin(source, packet);
}

class GeneratorsClientModule {
public:
    void install() {
        GeneratorsExtractHook::hook();
        GeneratorsRenderHook::hook();
        GeneratorsSyncHook::hook();
        feature::net::client::on("generators.valve", [](nlohmann::json const& msg) {
            auto pos = msg.value("pos", nlohmann::json::array());
            if (!pos.is_array() || pos.size() != 4) return;
            auto* ci    = lv::client::currentClient();
            auto* level = ci ? ci->getLevel() : nullptr;
            if (!level) return;
            ValveTurn t;
            t.dim       = pos[0].get<int>();
            t.start     = static_cast<std::uint64_t>(level->getCurrentTick().tickID);
            t.total     = msg.value("ticks", 0);
            t.target    = msg.value("target", 0);
            t.backwards = msg.value("backwards", false);
            t.facing    = msg.value("facing", 1);
            auto&           s = state();
            std::lock_guard lock(s.mutex);
            s.valves[BlockPos{pos[1].get<int>(), pos[2].get<int>(), pos[3].get<int>()}] = t;
        });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevel(true);
            state().level = nullptr;
        });
    }
    void uninstall() {
        GeneratorsExtractHook::unhook();
        GeneratorsRenderHook::unhook();
        GeneratorsSyncHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::generators

LV_REGISTER_MODULE(lv::client::generators::GeneratorsClientModule);

#endif // LV_CLIENT
