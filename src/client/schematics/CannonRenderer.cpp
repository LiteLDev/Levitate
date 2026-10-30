#ifdef LV_CLIENT

// Client mod: the schematicannon's moving barrel (Create's SchematicannonRenderer).
//
// - Chunk meshes: a schematicannon (`levitate:barrel` = 0, what vanilla clients see) is tessellated
//   with `levitate:barrel` = 1, whose permutation hides the connector and barrel bones; its
//   position is remembered.
// - Every frame (block entity pass) the connector is drawn turned towards the printer's current
//   target (yaw) and the barrel also pitched up (Create: pitch = atan2(distance, dy * 3 + distance
//   * 2) + 10 degrees from vertical), moving from the previous target to the new one over a tick;
//   for 10 ticks after a launch the barrel recoils (up to 0.1 block). The target comes from the
//   server ("schematics.cannon", src/feature/schematics/Cannon.cpp), the launches from
//   "schematics.launch" (Tools.cpp). Without a target the barrel rests south, 40 degrees from
//   vertical, as Create's.

#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include "glm/gtc/matrix_transform.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
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
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "client/ponder/Ponder.h"
#include "client/schematics/SchematicsClient.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::schematics {

namespace {

constexpr char kCannonId[]    = "levitate:schematicannon";
constexpr char kBarrelState[] = "levitate:barrel";
using Clock                   = std::chrono::steady_clock;
using Key                     = std::tuple<int, int, int, int>;

struct Aim {
    std::optional<std::array<int, 3>> target, previous;
    Clock::time_point                 changed{};
};

struct State {
    std::mutex                                  mutex;
    std::set<std::tuple<int, int, int>>         tracked; ///< cannons in chunk meshes (current dimension)
    std::map<Block const*, Block const*>        hidden;  ///< barrel = 0 -> barrel = 1
    std::map<Key, Aim>                          aims;
    bool                                        logged = false;
};
State& st() {
    static State s;
    return s;
}

Block const* hiddenOf(Block const& block) {
    auto&           s = st();
    std::lock_guard lock(s.mutex);
    auto [it, fresh] = s.hidden.try_emplace(&block, nullptr);
    if (fresh && block.getTypeName() == kCannonId && game::intState(block, kBarrelState).value_or(-1) == 0)
        it->second = game::blockWith(kCannonId, {{kBarrelState, 1}}); // the only state of the block
    return it->second;
}

/// Create's getCannonAngles: (yaw, pitch from vertical) in degrees towards `diff`.
std::pair<double, double> angles(double dx, double dy, double dz) {
    double yaw      = std::atan2(dx, dz) * 180.0 / 3.14159265358979323846;
    double distance = std::sqrt(dx * dx + dz * dz);
    double pitch    = std::atan2(distance, dy * 3 + distance * 2) * 180.0 / 3.14159265358979323846 + 10;
    return {yaw, pitch};
}

void onCannon(nlohmann::json const& msg) {
    auto p = msg.value("pos", nlohmann::json());
    if (!p.is_array() || p.size() != 3) return;
    Key  key{msg.value("dim", 0), p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
    auto t = msg.value("target", nlohmann::json());
    std::optional<std::array<int, 3>> target;
    if (t.is_array() && t.size() == 3) target = std::array<int, 3>{t[0].get<int>(), t[1].get<int>(), t[2].get<int>()};
    std::lock_guard lock(st().mutex);
    auto&           a = st().aims[key];
    a.previous        = a.target;
    a.target          = target;
    a.changed         = Clock::now();
}

void renderCannons(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int   dim    = static_cast<int>(player->getDimensionId());
    auto& region = player->getDimensionBlockSource();
    std::vector<std::tuple<int, int, int>> positions;
    {
        std::lock_guard lock(st().mutex);
        positions.assign(st().tracked.begin(), st().tracked.end());
    }
    if (positions.empty()) return;
    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&      screen = ctx.mScreenContext;
    auto&      tess   = screen.tessellator;
    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    auto&      world  = screen.camera.worldMatrixStack.get();

    ponder::cp::BlockDef def{kCannonId, {{kBarrelState, 0}}, {}};
    auto* connector = ponder::modelPartMesh(tess, def, {"base", "barrel"});
    auto* barrel    = ponder::modelPartMesh(tess, def, {"base", "connector"});
    if (!connector || !barrel) return;

    std::vector<std::tuple<int, int, int>> stale;
    for (auto const& [x, y, z] : positions) {
        BlockPos bp{x, y, z};
        if (!region.hasChunksAt(bp, 0, false) || region.getBlock(bp).getTypeName() != kCannonId) {
            stale.emplace_back(x, y, z);
            continue;
        }
        double yaw = 0, pitch = 40; // Create's resting barrel
        {
            std::lock_guard lock(st().mutex);
            if (auto it = st().aims.find({dim, x, y, z}); it != st().aims.end() && it->second.target) {
                auto const& a  = it->second;
                double      f  = std::clamp(std::chrono::duration<double>(Clock::now() - a.changed).count() * 20.0, 0.0, 1.0);
                auto        to = *a.target;
                auto        from = a.previous.value_or(to);
                double      dx = from[0] + (to[0] - from[0]) * f - x, dy = from[1] + (to[1] - from[1]) * f - y,
                       dz = from[2] + (to[2] - from[2]) * f - z;
                std::tie(yaw, pitch) = angles(dx, dy, dz);
            }
        }
        float recoil = cannonRecoil(dim, x, y, z);
        // our model's barrel points north, 22.5 degrees up: turn it to Create's yaw (0 = south)
        // and its elevation (90 - pitch)
        float     turn  = static_cast<float>(yaw) + 180.f;
        float     raise = static_cast<float>(90.0 - pitch) - 22.5f;
        glm::vec3 pivot{0.5f, 13.f / 16.f, 0.5f};
        glm::mat4 base = glm::translate(glm::mat4{1.f}, glm::vec3{x - cam.x, y - cam.y, z - cam.z});
        glm::mat4 yawM = glm::translate(base, pivot);
        yawM           = glm::rotate(yawM, glm::radians(turn), glm::vec3{0.f, 1.f, 0.f});
        {
            auto      ref = world.push(false);
            glm::mat4 m   = glm::translate(yawM, -pivot);
            ref.mat->_m.get() = ref.mat->_m.get() * m;
            connector->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        }
        {
            auto      ref = world.push(false);
            glm::mat4 m   = glm::rotate(yawM, glm::radians(raise), glm::vec3{1.f, 0.f, 0.f});
            m             = glm::translate(m, -pivot);
            // recoil: back along the barrel (north, 22.5 degrees up in the model)
            float r = recoil / 100.f;
            m       = glm::translate(m, glm::vec3{0.f, -std::sin(glm::radians(22.5f)) * r, std::cos(glm::radians(22.5f)) * r});
            ref.mat->_m.get() = ref.mat->_m.get() * m;
            barrel->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        }
    }
    std::lock_guard lock(st().mutex);
    for (auto const& p : stale) st().tracked.erase(p);
    if (!st().logged && !positions.empty()) {
        st().logged = true;
        log().info("schematics client: drawing {} schematicannon barrel(s)", positions.size());
    }
}

} // namespace

LL_STATIC_HOOK(
    CannonExtractHook,
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
        if (auto const* hidden = hiddenOf(block)) {
            BlockPos const& rel = iterData.relativePos;
            {
                std::lock_guard lock(st().mutex);
                st().tracked.emplace(chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z);
            }
            return origin(*hidden, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
        }
    }
    return origin(block, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    CannonRenderHook,
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
        renderCannons(renderContext);
    } catch (...) {
    }
}

class CannonRendererModule {
public:
    void install() {
        CannonExtractHook::hook();
        CannonRenderHook::hook();
        feature::net::client::on("schematics.cannon", [](nlohmann::json const& msg) { onCannon(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(st().mutex);
            st().tracked.clear();
            st().hidden.clear(); // block states are registered again for the next world
            st().aims.clear();
            st().logged = false;
        });
    }
    void uninstall() {
        CannonExtractHook::unhook();
        CannonRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::schematics

LV_REGISTER_MODULE(lv::client::schematics::CannonRendererModule);

#endif // LV_CLIENT
