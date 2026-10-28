#ifdef LV_CLIENT

// Client side of the kinetic network: rotating parts drawn spinning at their network speed.
//
// - Speeds come from the server over the kinetic sync channel (src/feature/kinetics/
//   KineticsSync.cpp): the client mod says hello, the server acks and streams speeds per chunk.
// - Chunk meshes: a kinetic block with a rotor state (`KineticSpec::rotorState`) is tessellated
//   with `rotor = 1` (its static parts only); its position is remembered.
// - Every frame (block entity pass) each remembered block's rotating parts (`rotor = 2..`) are
//   drawn around their axis at Create's angle: (ticks + partial) * speed * 0.3 + offset, where the
//   offset makes meshing cogs line up (core::kinetics::rotationOffset).
// Vanilla clients (no mod) simply see the static `rotor = 0` model.

#include <chrono>
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
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
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
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/ScriptMessagePacket.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "core/kinetics/Solver.h"
#include "core/kinetics/SyncCodec.h"
#include "feature/kinetics/KineticsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::kinetics {

namespace k  = core::kinetics;
namespace ks = core::kinetics::sync;
namespace fk = feature::kinetics;
using fk::KineticSpec;

namespace {

struct Synced {
    float speed      = 0;
    int   sourceFace = -1;
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
    std::unordered_map<int, Block const*> parts;            ///< rotor value -> block
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
    void const*                           level      = nullptr;
    bool                                  acked      = false;
    int                                   hellos     = 0;
    int                                   dim        = -1;
    std::chrono::steady_clock::time_point lastHello{};
    bool                                  loggedDraw = false;
};

/// Hellos sent to a server before giving up (it does not run the kinetic sync).
constexpr int kMaxHellos = 30;

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

Block const* partVariant(Block const& block, RotorInfo const& info, int value) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto&           parts = const_cast<RotorInfo&>(info).parts;
    if (auto it = parts.find(value); it != parts.end()) return it->second;
    auto const* b = fk::detail::withState(block, info.spec->rotorState, value);
    parts[value]  = b;
    return b;
}

void resetLevelState(bool dropTracked) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    s.acked      = false;
    s.hellos     = 0;
    s.dim        = -1;
    s.lastHello  = {};
    s.loggedDraw = false;
    s.speeds.clear();
    s.infos.clear();
    if (dropTracked) s.tracked.clear();
}

void sendHello() {
    auto pkt = MinecraftPackets::createPacket(MinecraftPacketIds::ScriptMessagePacket);
    if (!pkt) return;
    auto& msg          = static_cast<ScriptMessagePacket&>(*pkt);
    *msg.mMessageId    = std::string(ks::kHelloId);
    *msg.mMessageValue = std::to_string(ks::kProtocolVersion);
    msg.sendToServer();
}

/// Per frame on the client thread: notices a new level and says hello until acked.
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
    s.dim    = static_cast<int>(player->getDimensionId());
    auto now = std::chrono::steady_clock::now();
    if (!s.acked && s.hellos < kMaxHellos && now - s.lastHello > std::chrono::seconds(2)) {
        s.lastHello = now;
        ++s.hellos;
        sendHello();
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
    auto&      blocks = lr->getBlockRenderer();
    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    double     time   = static_cast<double>(level->getCurrentTick().tickID) + ctx.mFrameAlpha;
    auto&      world  = screen.camera.worldMatrixStack.get();

    std::vector<k::RotorPart> parts;
    for (auto const& item : items) {
        auto const& info = *item.info;
        k::RenderView view{&info.snap, item.synced.speed, std::nullopt};
        if (item.synced.sourceFace >= 0) view.sourceFacing = static_cast<k::Face>(item.synced.sourceFace);
        parts.clear();
        info.spec->rotorParts(view, parts);
        for (auto const& part : parts) {
            auto const* partBlock = partVariant(*item.block, info, part.rotorValue);
            if (!partBlock) continue;
            float extra  = info.spec->angleOffset ? info.spec->angleOffset(info.snap, part.axis) : 0.f;
            float offset = k::rotationOffset(core::GlobalPos{0, item.pos.x, item.pos.y, item.pos.z}, part.axis, part.largeCog, part.angleOffset + extra);
            float angle  = k::rotationAngle(time, item.synced.speed * part.speedFactor, offset);

            glm::vec3 axis{part.axis == k::Axis::X ? 1.f : 0.f, part.axis == k::Axis::Y ? 1.f : 0.f, part.axis == k::Axis::Z ? 1.f : 0.f};
            auto      ref = world.push(false);
            glm::mat4 m{1.f};
            m = glm::translate(m, glm::vec3{item.pos.x - cam.x + .5f, item.pos.y - cam.y + .5f, item.pos.z - cam.z + .5f});
            m = glm::rotate(m, glm::radians(angle), axis);
            m = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
            ref.mat->_m.get() = ref.mat->_m.get() * m;

            auto& mesh = blocks.getMeshForBlock(tess, *partBlock);
            mesh.renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
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

// Sync channel: ack and speed batches from the server.
LL_TYPE_INSTANCE_HOOK(
    KineticsClientPacketHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&   source,
    ScriptMessagePacket const& packet
) {
    auto const& id = *packet.mMessageId;
    if (id == ks::kSyncId) {
        onSync(*packet.mMessageValue);
        return;
    }
    if (id == ks::kAckId) {
        auto& s = state();
        if (!s.acked) log().info("kinetics client: server has the kinetic sync (protocol {})", *packet.mMessageValue);
        s.acked = true;
        return;
    }
    origin(source, packet);
}

class KineticsClientModule {
public:
    void install() {
        KineticsExtractHook::hook();
        KineticsRenderHook::hook();
        KineticsClientPacketHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevelState(true);
            state().level = nullptr;
        });
    }
    void uninstall() {
        KineticsExtractHook::unhook();
        KineticsRenderHook::unhook();
        KineticsClientPacketHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::kinetics

LV_REGISTER_MODULE(lv::client::kinetics::KineticsClientModule);

#endif // LV_CLIENT
