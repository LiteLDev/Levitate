#ifdef LV_CLIENT

// Client side of the kinetic network: rotating parts drawn spinning at their network speed.
//
// - Speeds come from the server over the companion protocol (channel "kinetics.sync", see
//   src/feature/kinetics/KineticsSync.cpp): after the handshake the server streams them per chunk.
//   Other client units read them through client/kinetics/KineticsClient.h.
// - Chunk meshes: a kinetic block with a rotor state (`KineticSpec::rotorState`) is tessellated
//   with `rotor = 1` (its static parts only); its position is remembered.
// - Every frame (block entity pass) each remembered block's rotating parts (`rotor = 2..`) are
//   drawn around their axis at Create's angle: (ticks + partial) * speed * 0.3 + offset, where the
//   offset makes meshing cogs line up (core::kinetics::rotationOffset).
// - Create's overstress flash (KineticEffectHandler / KineticBlockEntityRenderer): when a block's
//   network becomes overstressed its rotating parts flash red, when it recovers spring green, the
//   tint fading by a tenth a tick (U52).
// - Eased parts (the flywheel's wheel) chase the network speed by 1/64 a tick (U52).
// Vanilla clients (no mod) simply see the static `rotor = 0` model.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "nlohmann/json.hpp"
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "glm/gtc/matrix_transform.hpp"

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
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/ptexture/LightTexture.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec3.h"
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
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "client/kinetics/KineticsClient.h"
#include "core/kinetics/Feedback.h"
#include "core/kinetics/RotorModel.h"
#include "core/kinetics/Solver.h"
#include "core/kinetics/SyncCodec.h"
#include "feature/kinetics/KineticsInternal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::kinetics {

namespace k  = core::kinetics;
namespace ks = core::kinetics::sync;
namespace fk = feature::kinetics;
namespace feedback = core::kinetics::feedback;
using fk::KineticSpec;

namespace {

struct Synced {
    float         speed        = 0;
    int           sourceFace   = -1;
    bool          overstressed = false;
    float         effect       = 0; ///< overstress flash: 1 overstressed, -1 recovered (at `effectTick`)
    std::uint64_t effectTick   = 0;
};

/// An eased part's visual speed and angle (FlywheelBlockEntity.visualSpeed / angle).
struct Eased {
    float         visual = 0, angle = 0;
    std::uint64_t tick   = 0;
};

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept {
        return k::GlobalPosHash{}(core::GlobalPos{0, p.x, p.y, p.z});
    }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

/// What the renderer needs about one block state (computed once per Block).
struct RotorInfo {
    KineticSpec const*                    spec   = nullptr;
    Block const*                          hidden = nullptr; ///< `rotor = 1` variant; nullptr: not drawn by us
    k::BlockState                         snap;
};

struct State {
    /// Guards `tracked` and `infos` (chunk builder threads) and `speeds`.
    std::mutex mutex;
    /// speeds per dimension (from the server)
    std::unordered_map<int, std::unordered_map<BlockPos, Synced, PosHash, PosEq>> speeds;
    /// blocks whose rotating parts were taken out of the chunk mesh
    std::unordered_set<BlockPos, PosHash, PosEq> tracked;
    /// per block state (block states are registered again for every world: cleared with the level)
    std::unordered_map<Block const*, RotorInfo> infos;

    // client thread only
    std::unordered_map<BlockPos, Eased, PosHash, PosEq> eased;
    void const*               level      = nullptr;
    bool                      synced     = false; ///< got the first sync of this level
    int                       dim        = -1;
    bool                      loggedDraw = false;
    std::vector<SyncListener> listeners;
};

State& state() {
    static State s;
    return s;
}

/// The cached render info of a block state (thread safe; the reference stays valid until the
/// level changes).
RotorInfo const& rotorInfo(Block const& block) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto [it, fresh] = s.infos.try_emplace(&block);
    if (fresh) {
        auto& info = it->second;
        info.spec  = fk::detail::specOfBlock(block);
        if (info.spec && !info.spec->rotorState.empty()) {
            info.snap = fk::detail::snapshot(block, *info.spec);
            if (game::intState(block, info.spec->rotorState).value_or(-1) == 0) {
                info.hidden = fk::detail::withState(block, info.spec->rotorState, 1);
            }
        }
    }
    return it->second;
}

// ---- rotor meshes ------------------------------------------------------------------------------------------

/// Geometry of every rotor model, read once from the mod's resource pack (client thread).
std::map<std::string, k::RotorModel> const& rotorModels() {
    static std::map<std::string, k::RotorModel> models = [] {
        std::map<std::string, k::RotorModel> out;
        auto dir = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        std::error_code ec;
        for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".json") continue;
            std::ifstream in(entry.path());
            auto          j = nlohmann::json::parse(in, nullptr, false, true);
            if (j.is_discarded()) continue;
            if (auto m = k::parseGeometry(j)) out.merge(*m);
        }
        return out;
    }();
    return models;
}

/// Cached GPU meshes per (model, bone, texture); dropped with the level (client thread only).
std::map<std::string, std::unique_ptr<mce::Mesh>>& meshCache() {
    static std::map<std::string, std::unique_ptr<mce::Mesh>> cache;
    return cache;
}

/// `tint`: 0 white, 1..32 red, -1..-32 spring green by 32nds (the overstress flash).
mce::Mesh* rotorMesh(Tessellator& tess, KineticSpec const& spec, std::string const& bone, int tint = 0) {
    auto  key   = spec.rotorModel + "|" + bone + "|" + spec.rotorTexture + "|" + std::to_string(tint);
    auto& cache = meshCache();
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    auto& slot   = cache[key];
    auto const& models = rotorModels();
    auto  model  = models.find(spec.rotorModel);
    if (model == models.end()) return nullptr;
    auto b = model->second.bones.find(bone);
    if (b == model->second.bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet(spec.rotorTexture, 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };

    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    auto colour = feedback::overstressTint(static_cast<float>(tint) / 32.f);
    tess.color(colour[0], colour[1], colour[2], 1.f);
    for (auto const& q : b->second) {
        // both windings: the part is seen from every side while it turns
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(
        tess.end(Tessellator::UploadMode::Buffered, "levitate_rotor", SupplementaryFieldAutoGenerationMode::None)
    );
    return slot.get();
}
/// `leftLevel`: the client left the level (speeds and tracked blocks go); otherwise a new level
/// object was seen while rendering: the speeds stay (the server may have synced them before the
/// first frame of the level), block-state caches go.
void resetLevelState(bool leftLevel) {
    bool dropTracked = leftLevel;
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    s.dim        = -1;
    s.loggedDraw = false;
    if (leftLevel) {
        s.synced = false;
        s.speeds.clear();
    }
    s.eased.clear();
    s.infos.clear();
    if (dropTracked) s.tracked.clear();
    meshCache().clear();
}

/// Per frame on the client thread: notices a new level.
void frameUpdate(ClientInstance& ci) {
    auto& s      = state();
    auto* level  = ci.getLevel();
    auto* player = ci.getLocalPlayer();
    if (level != s.level) {
        // Chunks of the new level may already be meshed: keep `tracked` (stale positions fail the
        // per-frame check), drop everything else.
        resetLevelState(false);
        s.level = level;
    }
    if (!level || !player) return;
    s.dim = static_cast<int>(player->getDimensionId());
}

/// The client level's tick (0 outside a level).
std::uint64_t clientTick() {
    auto* ci    = lv::client::currentClient();
    auto* level = ci ? ci->getLevel() : nullptr;
    return level ? level->getCurrentTick().tickID : 0;
}

void applyBatch(ks::Batch const& batch) {
    auto            now = clientTick();
    auto&           s   = state();
    int             red = 0, green = 0;
    std::lock_guard lock(s.mutex);
    auto&           m = s.speeds[batch.dim];
    if (batch.reset) m.clear();
    for (auto const& e : batch.entries) {
        BlockPos p{e.x, e.y, e.z};
        if (e.removed) {
            m.erase(p);
            continue;
        }
        Synced next{e.speed, e.sourceFace, e.overstressed};
        if (auto it = m.find(p); it != m.end()) {
            next.effect     = it->second.effect;
            next.effectTick = it->second.effectTick;
            // KineticBlockEntity.read: the overstress state changed while the block turns
            // (overstressed: the theoretical speed turns; recovered: the speed is back)
            if (it->second.overstressed != e.overstressed && (e.overstressed || e.speed != 0)) {
                next.effect     = e.overstressed ? 1.f : -1.f;
                next.effectTick = now + static_cast<std::uint64_t>(feedback::kOverstressEffectDelay);
                ++(e.overstressed ? red : green);
            }
        }
        m[p] = next;
    }
    if (red > 0) log().info("kinetics client: overstress flash (red) on {} blocks", red);
    if (green > 0) log().info("kinetics client: overstress flash (green) on {} blocks", green);
}

/// The overstress flash of a block now (0: none).
float effectAt(Synced const& s, std::uint64_t now) {
    if (s.effect == 0 || now < s.effectTick) return 0;
    return feedback::overstressEffectAfter(s.effect, static_cast<int>(std::min<std::uint64_t>(now - s.effectTick, 1000)));
}

/// Advances an eased part to `now` and returns its angle at `partial` (FlywheelBlockEntity.tick,
/// FlywheelRenderer: angle + visualSpeed * 3/10 * partialTicks).
float easedAngle(BlockPos const& pos, float target, std::uint64_t now, float partial) {
    auto& e = state().eased[pos];
    if (e.tick == 0 || now < e.tick || now - e.tick > 1200) e.tick = now; // new or long unseen: start here
    for (; e.tick < now; ++e.tick) {
        e.visual = feedback::chaseExp(e.visual, target, feedback::kFlywheelChase);
        e.angle  = std::fmod(e.angle + e.visual * .3f, 360.f);
    }
    return e.angle + e.visual * .3f * partial;
}

/// "kinetics.sync" from the server (client thread).
void onSyncMessage(nlohmann::json const& msg) {
    auto& s       = state();
    auto  batches = msg.value("batches", nlohmann::json::array());
    if (!batches.is_array()) return;
    if (!s.synced) {
        s.synced = true;
        log().info("kinetics client: server has the kinetic sync (protocol {})", core::common::kProtocolVersion);
    }
    auto listeners = s.listeners; // listeners may register more
    for (auto const& text : batches) {
        if (!text.is_string()) continue;
        auto batch = ks::decode(text.get<std::string>());
        if (!batch) continue;
        applyBatch(*batch);
        for (auto const& fn : listeners) {
            try {
                fn(*batch);
            } catch (std::exception const& e) {
                log().error("kinetics client: sync listener failed: {}", e.what());
            }
        }
    }
}

struct DrawItem {
    BlockPos         pos;
    Block const*     block;
    RotorInfo const* info;
    Synced           synced;
};

void renderRotors(BaseActorRenderContext& ctx) {
    auto* ci = lv::client::currentClient();
    if (!ci) return;
    frameUpdate(*ci);
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    auto* lr     = ci->getLevelRenderer();
    if (!level || !player || !lr) return;
    auto& region = player->getDimensionBlockSource();
    auto& s      = state();

    std::vector<BlockPos> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.tracked.empty()) return;
        positions.assign(s.tracked.begin(), s.tracked.end());
    }
    // world lookups without the lock (the chunk builders need it)
    std::vector<DrawItem> items;
    std::vector<BlockPos> stale;
    items.reserve(positions.size());
    for (auto const& p : positions) {
        if (!region.hasChunksAt(p, 0, false)) {
            stale.push_back(p);
            continue;
        }
        auto const& b    = region.getBlock(p);
        auto const& info = rotorInfo(b);
        if (!info.hidden) { // not a rotor block (any more)
            stale.push_back(p);
            continue;
        }
        items.push_back(DrawItem{p, &b, &info, {}});
    }
    {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.tracked.erase(p);
        auto const& speeds = s.speeds[s.dim];
        for (auto& item : items) {
            if (auto sp = speeds.find(item.pos); sp != speeds.end()) item.synced = sp->second;
        }
    }
    if (items.empty()) return;

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};

    auto&      screen = ctx.mScreenContext;
    auto&      tess   = screen.tessellator;

    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    double     time   = static_cast<double>(level->getCurrentTick().tickID) + ctx.mFrameAlpha;
    auto&      world  = screen.camera.worldMatrixStack.get();

    auto                      now     = static_cast<std::uint64_t>(level->getCurrentTick().tickID);
    float                     partial = ctx.mFrameAlpha;
    std::vector<k::RotorPart> parts;
    for (auto const& item : items) {
        auto const& info = *item.info;
        k::RenderView view{&info.snap, item.synced.speed, std::nullopt};
        if (item.synced.sourceFace >= 0) view.sourceFacing = static_cast<k::Face>(item.synced.sourceFace);
        parts.clear();
        info.spec->rotorParts(view, parts);
        float effect = effectAt(item.synced, now);
        int   tint   = static_cast<int>(std::lround(effect * 32.f));
        for (auto const& part : parts) {
            auto* mesh = rotorMesh(tess, *info.spec, part.bone, tint);
            if (!mesh) continue;
            float extra  = info.spec->angleOffset ? info.spec->angleOffset(info.snap, part.axis) : 0.f;
            float offset = k::rotationOffset(core::GlobalPos{0, item.pos.x, item.pos.y, item.pos.z}, part.axis, part.largeCog, part.angleOffset + extra);
            float angle  = part.eased ? easedAngle(item.pos, item.synced.speed * part.speedFactor, now, partial) + offset
                                      : k::rotationAngle(time, item.synced.speed * part.speedFactor, offset);

            glm::vec3 axis{part.axis == k::Axis::X ? 1.f : 0.f, part.axis == k::Axis::Y ? 1.f : 0.f, part.axis == k::Axis::Z ? 1.f : 0.f};
            auto      ref = world.push(false);
            glm::mat4 m{1.f};
            m = glm::translate(m, glm::vec3{item.pos.x - cam.x + .5f, item.pos.y - cam.y + .5f, item.pos.z - cam.z + .5f});
            m = glm::rotate(m, glm::radians(angle), axis);
            // the model is built around Y: turn it onto the part's axis
            if (part.axis == k::Axis::X) m = glm::rotate(m, glm::radians(-90.f), glm::vec3{0.f, 0.f, 1.f});
            else if (part.axis == k::Axis::Z) m = glm::rotate(m, glm::radians(90.f), glm::vec3{1.f, 0.f, 0.f});
            m = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
            ref.mat->_m.get() = ref.mat->_m.get() * m;

            mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        }
    }
    if (!s.loggedDraw) {
        s.loggedDraw = true;
        log().info("kinetics client: drawing {} rotating parts", items.size());
    }
}
} // namespace

// Chunk meshes: kinetic blocks with a rotor are tessellated without their rotating parts.
LL_STATIC_HOOK(
    KineticsExtractHook,
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
        if (auto const* hidden = rotorInfo(block).hidden) {
            BlockPos const& rel = iterData.relativePos;
            BlockPos        pos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z};
            {
                auto&           s = state();
                std::lock_guard lock(s.mutex);
                s.tracked.insert(pos);
            }
            return origin(
                *hidden,
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

// Block entity pass (opaque): draw the rotating parts.
LL_TYPE_INSTANCE_HOOK(
    KineticsRenderHook,
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
        renderRotors(renderContext);
    } catch (...) {
    }
}

std::optional<SyncedSpeed> syncedSpeed(int dim, int x, int y, int z) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto            d = s.speeds.find(dim);
    if (d == s.speeds.end()) return std::nullopt;
    auto it = d->second.find(BlockPos{x, y, z});
    if (it == d->second.end()) return std::nullopt;
    return SyncedSpeed{it->second.speed, it->second.sourceFace};
}

void onSync(SyncListener fn) { state().listeners.push_back(std::move(fn)); }

class KineticsClientModule {
public:
    void install() {
        KineticsExtractHook::hook();
        KineticsRenderHook::hook();
        feature::net::client::on(feature::kinetics::detail::kSyncChannel, [](nlohmann::json const& msg) { onSyncMessage(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevelState(true);
            state().level = nullptr;
        });
    }
    void uninstall() {
        KineticsExtractHook::unhook();
        KineticsRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::kinetics

LV_REGISTER_MODULE(lv::client::kinetics::KineticsClientModule);

#endif // LV_CLIENT
