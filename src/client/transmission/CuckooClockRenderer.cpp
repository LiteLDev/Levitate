#ifdef LV_CLIENT

// Cuckoo clocks (U49), drawn by the client mod as Create's CuckooClockRenderer: the hands chase
// the time of day (a fifth of the way per tick, only while the clock turns; 24 times faster
// outside the overworld), the doors swing open and the pig / creeper slides out while the server's
// animation runs (`transmission.cuckoo`: start and progress). The chunk mesh holds the clock
// without these parts (`levitate:rotor` = 1, done by the kinetic renderer, which also spins the
// shaft at the back); vanilla clients see the static parts the server's block states pick.

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
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
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "client/kinetics/KineticsClient.h"
#include "core/kinetics/RotorModel.h"
#include "core/kinetics/Types.h"
#include "core/transmission/CuckooClock.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::transmission {

namespace k  = core::kinetics;
namespace ct = core::transmission;

namespace {

struct PosKey {
    int  x, y, z;
    bool operator==(PosKey const&) const = default;
};
struct PosKeyHash {
    std::size_t operator()(PosKey const& p) const noexcept { return k::GlobalPosHash{}(core::GlobalPos{0, p.x, p.y, p.z}); }
};

struct Animation {
    ct::CuckooAnimation type      = ct::CuckooAnimation::None;
    std::uint64_t       startTick = 0; ///< client tick the message arrived
    int                 progress  = 0; ///< server progress at that moment
};

struct Hands {
    float hour = 0, minute = 0, prevHour = 0, prevMinute = 0;
    bool  init = false;
};

struct State {
    std::mutex                                                              mutex; ///< `meshed` (chunk builder threads)
    std::unordered_set<PosKey, PosKeyHash>                                  meshed;
    std::unordered_map<int, std::unordered_map<PosKey, Animation, PosKeyHash>> animations;
    std::unordered_map<PosKey, Hands, PosKeyHash>                           hands;
    std::map<std::string, std::unique_ptr<mce::Mesh>>                       meshes;
    std::map<std::string, std::vector<k::ModelQuad>>                        bones;
    std::uint64_t                                                           lastTick = 0;
    bool                                                                    loaded   = false;
    bool                                                                    logged   = false;
};

State& state() {
    static State s;
    return s;
}

bool isClock(std::string const& id) { return id == "levitate:cuckoo_clock" || id == "levitate:mysterious_cuckoo_clock"; }

void loadBones() {
    auto& s  = state();
    s.loaded = true;
    auto path = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks" / "cuckoo_clock.geo.json";
    std::ifstream in(path);
    if (!in) return;
    auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded()) return;
    auto models = k::parseGeometry(j);
    if (!models) return;
    auto m = models->find("geometry.levitate.cuckoo_clock");
    if (m == models->end()) return;
    for (auto const* bone : {"hour_0", "minute_0", "door_left", "door_right", "pig", "creeper"}) {
        if (auto b = m->second.bones.find(bone); b != m->second.bones.end()) s.bones[bone] = b->second;
    }
}

mce::Mesh* boneMesh(Tessellator& tess, std::string const& bone) {
    auto& s = state();
    if (!s.loaded) loadBones();
    if (auto it = s.meshes.find(bone); it != s.meshes.end()) return it->second.get();
    auto& slot = s.meshes[bone];
    auto  b    = s.bones.find(bone);
    if (b == s.bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet("levitate_cuckoo_clock", 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_cuckoo_clock_part", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// Create's moveHands for every clock in sight, once per game tick.
void advanceHands(std::uint64_t tick, int dim, std::int64_t dayTime, std::vector<std::pair<PosKey, Block const*>> const& clocks) {
    auto& s = state();
    if (tick == s.lastTick) return;
    int steps  = s.lastTick == 0 || tick < s.lastTick ? 1 : static_cast<int>(std::min<std::uint64_t>(tick - s.lastTick, 20));
    s.lastTick = tick;
    bool natural = dim == 0;
    auto time    = ct::clockTime(dayTime, natural);
    for (auto const& [p, block] : clocks) {
        auto& h = s.hands[p];
        if (!h.init) {
            // start where the static model shows them
            h.hour = h.prevHour = 30.f * static_cast<float>(game::intState(*block, "levitate:hour").value_or(0));
            h.minute = h.prevMinute = 30.f * static_cast<float>(game::intState(*block, "levitate:minute").value_or(0));
            h.init                  = true;
        }
        auto speed = kinetics::syncedSpeed(dim, p.x, p.y, p.z);
        h.prevHour = h.hour, h.prevMinute = h.minute;
        if (!speed || speed->speed == 0) continue;
        for (int i = 0; i < steps; ++i) {
            h.hour   = ct::chaseAngle(h.hour, ct::hourHandAngle(time.hours));
            h.minute = ct::chaseAngle(h.minute, ct::minuteHandAngle(time.minutes));
        }
    }
}

float yawOf(int horizontalFacing) {
    switch (horizontalFacing) {
    case 0: return 180.f; // south
    case 1: return 90.f;  // west
    case 3: return 270.f; // east
    default: return 0.f;  // north
    }
}

void drawClocks(BaseActorRenderContext& ctx) {
    auto* ci = currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    if (!level || !player) return;
    auto& s = state();
    std::vector<PosKey> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.meshed.empty()) return;
        positions.assign(s.meshed.begin(), s.meshed.end());
    }
    auto&                                        region = player->getDimensionBlockSource();
    int                                          dim    = static_cast<int>(player->getDimensionId());
    std::vector<std::pair<PosKey, Block const*>> clocks;
    std::vector<PosKey>                          stale;
    for (auto const& p : positions) {
        BlockPos bp{p.x, p.y, p.z};
        if (!region.hasChunksAt(bp, 0, false)) continue;
        auto const& block = region.getBlock(bp);
        if (!isClock(block.getTypeName())) {
            stale.push_back(p);
            continue;
        }
        clocks.emplace_back(p, &block);
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) {
            s.meshed.erase(p);
            s.hands.erase(p);
        }
    }
    if (clocks.empty()) return;
    std::uint64_t tick = level->getCurrentTick().tickID;
    advanceHands(tick, dim, level->getTime(), clocks);

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&      screen = ctx.mScreenContext;
    auto&      tess   = screen.tessellator;
    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    float      alpha  = ctx.mFrameAlpha;
    auto&      world  = screen.camera.worldMatrixStack.get();
    auto&      anims  = s.animations[dim];

    auto draw = [&](PosKey const& p, float yaw, std::string const& bone, glm::mat4 const& local) {
        auto* mesh = boneMesh(tess, bone);
        if (!mesh) return;
        auto      ref = world.push(false);
        glm::mat4 m{1.f};
        m = glm::translate(m, glm::vec3{p.x - cam.x + .5f, p.y - cam.y + .5f, p.z - cam.z + .5f});
        m = glm::rotate(m, glm::radians(yaw), glm::vec3{0.f, 1.f, 0.f});
        m = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
        ref.mat->_m.get() = ref.mat->_m.get() * m * local;
        mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    };
    auto about = [](glm::vec3 pivot, float degrees, glm::vec3 axis) {
        glm::mat4 m{1.f};
        m = glm::translate(m, pivot);
        m = glm::rotate(m, glm::radians(degrees), axis);
        return glm::translate(m, -pivot);
    };

    for (auto const& [p, block] : clocks) {
        float yaw = yawOf(game::intState(*block, "levitate:horizontal_facing").value_or(2));
        auto& h   = s.hands[p];
        // hands: clockwise as seen from the front (about +z of the north-facing model)
        glm::vec3 dial{8.f / 16, 6.f / 16, 2.5f / 16};
        draw(p, yaw, "hour_0", about(dial, h.prevHour + (h.hour - h.prevHour) * alpha, {0.f, 0.f, 1.f}));
        draw(p, yaw, "minute_0", about(dial, h.prevMinute + (h.minute - h.prevMinute) * alpha, {0.f, 0.f, 1.f}));
        // doors and figure
        float angle = 0;
        auto  a     = anims.find(p);
        bool  anim  = a != anims.end() && a->second.type != ct::CuckooAnimation::None;
        float value = 0;
        if (anim) {
            value = static_cast<float>(a->second.progress) + static_cast<float>(tick - a->second.startTick) + alpha;
            if (value > ct::kCuckooAnimationEnd + 2) anim = false;
            else angle = ct::cuckooDoorAngle(a->second.type, value);
        }
        draw(p, yaw, "door_left", about({10.f / 16, 0.f, 2.5f / 16}, -angle, {0.f, 1.f, 0.f}));
        draw(p, yaw, "door_right", about({6.f / 16, 0.f, 2.5f / 16}, angle, {0.f, 1.f, 0.f}));
        if (anim) {
            // the figure is modelled out; it slides half a block back in while the doors close
            float     back = (1.f - angle / 135.f) * .5f;
            glm::mat4 m    = glm::translate(glm::mat4{1.f}, glm::vec3{0.f, 0.f, back});
            draw(p, yaw, a->second.type == ct::CuckooAnimation::Pig ? "pig" : "creeper", m);
        }
    }
    if (!s.logged) {
        s.logged = true;
        log().info("transmission client: drawing {} cuckoo clocks", clocks.size());
    }
}

void onCuckoo(nlohmann::json const& msg) {
    auto& s   = state();
    int   dim = msg.value("dim", 0);
    auto& map = s.animations[dim];
    if (msg.value("reset", false)) map.clear();
    std::uint64_t now = 0;
    if (auto* ci = currentClient(); ci && ci->getLevel()) now = ci->getLevel()->getCurrentTick().tickID;
    for (auto const& e : msg.value("set", nlohmann::json::array())) {
        if (!e.is_array() || e.size() != 5) continue;
        auto& an     = map[PosKey{e[0].get<int>(), e[1].get<int>(), e[2].get<int>()}];
        an.type      = static_cast<ct::CuckooAnimation>(std::clamp(e[3].get<int>(), 0, 3));
        an.progress  = e[4].get<int>();
        an.startTick = now;
        log().info("transmission client: cuckoo clock animation {} at {} {} {}", static_cast<int>(an.type), e[0].get<int>(), e[1].get<int>(), e[2].get<int>());
    }
    for (auto const& e : msg.value("remove", nlohmann::json::array())) {
        if (e.is_array() && e.size() == 3) map.erase(PosKey{e[0].get<int>(), e[1].get<int>(), e[2].get<int>()});
    }
}

} // namespace

// Chunk meshes: remembers where clocks are (the kinetic renderer hides their animated parts).
LL_STATIC_HOOK(
    CuckooClockExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block && isClock(block.getTypeName())) {
        BlockPos const& rel = iterData.relativePos;
        std::lock_guard lock(state().mutex);
        state().meshed.insert(PosKey{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
    }
    return origin(block, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    CuckooClockRenderHook,
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
        drawClocks(renderContext);
    } catch (...) {
    }
}

class CuckooClockRendererModule {
public:
    void install() {
        feature::net::client::on("transmission.cuckoo", onCuckoo);
        CuckooClockExtractHook::hook();
        CuckooClockRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto& s = state();
            {
                std::lock_guard lock(s.mutex);
                s.meshed.clear();
            }
            s.animations.clear();
            s.hands.clear();
            s.meshes.clear();
            s.lastTick = 0;
            s.logged   = false;
        });
    }
    void uninstall() {
        CuckooClockRenderHook::unhook();
        CuckooClockExtractHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::transmission

LV_REGISTER_MODULE(lv::client::transmission::CuckooClockRendererModule);

#endif // LV_CLIENT
