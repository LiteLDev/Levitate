#ifdef LV_CLIENT

// Desk bells (U49), drawn by the client mod as Create's DeskBellRenderer: after a ring
// (`redstone.bell`) the plunger dips and the bell wobbles (its animation falls from 1 to 0 by
// 0.05 a tick, the wobble's phase is random per ring). Chunk meshes hold the bell without its bell
// and plunger (`levitate:rotor` = 1, swapped in here), which are drawn every frame instead; vanilla
// clients see the static model with the plunger pressed while the bell is powered.

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
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
#include "core/kinetics/RotorModel.h"
#include "core/kinetics/Types.h"
#include "core/redstone/RoseQuartzLamp.h"
#include "feature/net/Net.h"
#include "feature/redstone/Internal.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::redstone {

namespace k  = core::kinetics;
namespace cr = core::redstone;
namespace fr = feature::redstone;

namespace {

constexpr char kBell[] = "levitate:desk_bell";

struct PosKey {
    int  x, y, z;
    bool operator==(PosKey const&) const = default;
};
struct PosKeyHash {
    std::size_t operator()(PosKey const& p) const noexcept { return k::GlobalPosHash{}(core::GlobalPos{0, p.x, p.y, p.z}); }
};

struct Ring {
    std::uint64_t start  = 0;
    float         offset = 0;
};

struct State {
    std::mutex                                                          mutex; ///< `meshed`, `hidden` (chunk builder threads)
    std::unordered_set<PosKey, PosKeyHash>                              meshed;
    std::unordered_map<Block const*, Block const*>                      hidden;
    std::unordered_map<int, std::unordered_map<PosKey, Ring, PosKeyHash>> rings;
    std::map<std::string, std::unique_ptr<mce::Mesh>>                   meshes;
    std::map<std::string, std::vector<k::ModelQuad>>                    bones;
    bool                                                                loaded = false;
    bool                                                                logged = false;
    std::mt19937                                                        rng{4949};
};

State& state() {
    static State s;
    return s;
}

/// The `rotor = 1` variant of a desk bell block state (nullptr: not a bell). Thread safe.
Block const* hiddenVariant(Block const& block) {
    if (block.getTypeName() != kBell) return nullptr;
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto [it, fresh] = s.hidden.try_emplace(&block, nullptr);
    if (fresh && game::intState(block, fr::sRotor).value_or(-1) == 0) {
        auto const* h = fr::withStates(block, {{fr::sRotor, 1}});
        it->second    = h != &block ? h : nullptr;
    }
    return it->second;
}

void loadBones() {
    auto& s  = state();
    s.loaded = true;
    auto path = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks" / "desk_bell.geo.json";
    std::ifstream in(path);
    if (!in) return;
    auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded()) return;
    auto models = k::parseGeometry(j);
    if (!models) return;
    auto m = models->find("geometry.levitate.desk_bell");
    if (m == models->end()) return;
    for (auto const* bone : {"bell", "plunger"}) {
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
    auto  tex = BlockGraphics::getTextureUVCoordinateSet("levitate_desk_bell", 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_desk_bell_part", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// The block model's facing rotation (generators.ps1's table: Y-up turned onto the facing).
glm::mat4 facingRotation(int facing) {
    glm::mat4 m{1.f};
    switch (facing) {
    case 0: return glm::rotate(m, glm::radians(180.f), glm::vec3{1.f, 0.f, 0.f});
    case 2: return glm::rotate(m, glm::radians(-90.f), glm::vec3{1.f, 0.f, 0.f});
    case 3: return glm::rotate(m, glm::radians(90.f), glm::vec3{1.f, 0.f, 0.f});
    case 4: return glm::rotate(m, glm::radians(90.f), glm::vec3{0.f, 0.f, 1.f});
    case 5: return glm::rotate(m, glm::radians(-90.f), glm::vec3{0.f, 0.f, 1.f});
    default: return m;
    }
}

void drawBells(BaseActorRenderContext& ctx) {
    auto* ci = currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    if (!level || !player) return;
    auto&               s = state();
    std::vector<PosKey> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.meshed.empty()) return;
        positions.assign(s.meshed.begin(), s.meshed.end());
    }
    auto&                                        region = player->getDimensionBlockSource();
    int                                          dim    = static_cast<int>(player->getDimensionId());
    std::vector<std::pair<PosKey, Block const*>> bells;
    std::vector<PosKey>                          stale;
    for (auto const& p : positions) {
        BlockPos bp{p.x, p.y, p.z};
        if (!region.hasChunksAt(bp, 0, false)) continue;
        auto const& block = region.getBlock(bp);
        if (block.getTypeName() != kBell) stale.push_back(p);
        else bells.emplace_back(p, &block);
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.meshed.erase(p);
    }
    if (bells.empty()) return;

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&         screen = ctx.mScreenContext;
    auto&         tess   = screen.tessellator;
    Vec3 const    cam    = ctx.mImpl.get()->mCameraTargetPosition;
    float         alpha  = ctx.mFrameAlpha;
    auto&         world  = screen.camera.worldMatrixStack.get();
    std::uint64_t tick   = level->getCurrentTick().tickID;
    auto&         rings  = s.rings[dim];

    for (auto const& [p, block] : bells) {
        float anim = 0, offset = 0;
        if (auto r = rings.find(p); r != rings.end()) {
            anim   = std::max(0.f, 1.f - cr::kDeskBellAnimationStep * (static_cast<float>(tick - r->second.start) + alpha));
            offset = r->second.offset;
        }
        auto      pose = cr::deskBellPose(anim, offset);
        glm::mat4 base{1.f};
        base = glm::translate(base, glm::vec3{p.x - cam.x + .5f, p.y - cam.y + .5f, p.z - cam.z + .5f});
        base = base * facingRotation(game::intState(*block, fr::sFacing).value_or(1));
        base = glm::translate(base, glm::vec3{-.5f, -.5f, -.5f});
        auto draw = [&](char const* bone, glm::mat4 const& local) {
            auto* mesh = boneMesh(tess, bone);
            if (!mesh) return;
            auto ref          = world.push(false);
            ref.mat->_m.get() = ref.mat->_m.get() * base * local;
            mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        };
        draw("plunger", glm::translate(glm::mat4{1.f}, glm::vec3{0.f, -pose.plunger, 0.f}));
        glm::vec3 pivot{.5f, 7.f / 16, .5f};
        glm::mat4 bell = glm::translate(glm::mat4{1.f}, pivot);
        bell           = glm::rotate(bell, glm::radians(pose.tiltX), glm::vec3{1.f, 0.f, 0.f});
        bell           = glm::rotate(bell, glm::radians(pose.tiltZ), glm::vec3{0.f, 0.f, 1.f});
        bell           = glm::translate(bell, -pivot);
        draw("bell", bell);
    }
    if (!s.logged) {
        s.logged = true;
        log().info("redstone client: drawing {} desk bells", bells.size());
    }
}

void onBell(nlohmann::json const& msg) {
    auto p = msg.value("pos", nlohmann::json());
    if (!p.is_array() || p.size() != 4) return;
    auto&         s   = state();
    std::uint64_t now = 0;
    if (auto* ci = currentClient(); ci && ci->getLevel()) now = ci->getLevel()->getCurrentTick().tickID;
    std::uniform_real_distribution<float> phase(0.f, 2.f * 3.14159265f);
    s.rings[p[0].get<int>()][PosKey{p[1].get<int>(), p[2].get<int>(), p[3].get<int>()}] = Ring{now, phase(s.rng)};
    log().info("redstone client: desk bell at {} {} {} rings", p[1].get<int>(), p[2].get<int>(), p[3].get<int>());
}

} // namespace

// Chunk meshes: desk bells are tessellated without their bell and plunger.
LL_STATIC_HOOK(
    DeskBellExtractHook,
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
        if (auto const* hidden = hiddenVariant(block)) {
            BlockPos const& rel = iterData.relativePos;
            {
                std::lock_guard lock(state().mutex);
                state().meshed.insert(PosKey{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
            }
            return origin(*hidden, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
        }
    }
    return origin(block, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    DeskBellRenderHook,
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
        drawBells(renderContext);
    } catch (...) {
    }
}

class DeskBellRendererModule {
public:
    void install() {
        feature::net::client::on("redstone.bell", onBell);
        DeskBellExtractHook::hook();
        DeskBellRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto& s = state();
            {
                std::lock_guard lock(s.mutex);
                s.meshed.clear();
                s.hidden.clear();
            }
            s.rings.clear();
            s.meshes.clear();
            s.logged = false;
        });
    }
    void uninstall() {
        DeskBellRenderHook::unhook();
        DeskBellExtractHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::redstone

LV_REGISTER_MODULE(lv::client::redstone::DeskBellRendererModule);

#endif // LV_CLIENT
