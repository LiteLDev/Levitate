#ifdef LV_CLIENT

// Client mod: animated deployers and mechanical arms (Create's DeployerRenderer / ArmRenderer).
//
// Both are kinetic blocks with a rotor state, so the kinetics renderer already tessellates them
// with `rotor = 1` (their moving parts hidden) and spins their shaft / cog. This file draws the
// rest every frame:
// - deployer: the pole and hand (pointing, holding or punching) at Create's hand offset, from the
//   phase / timer / reach the server sends on "assembly.deployer" when the phase changes;
// - arm: turntable, lower arm, upper arm and claw at the angles between the previous and the next
//   interaction point ("assembly.arm"), resting when nothing was received.
// Both draw the item they hold (Create's DeployerRenderer / ArmRenderer) with the world item
// renderer (client/render/WorldItems.h): in the deployer's hand, between the arm's claws.
// Vanilla clients see the static model (`rotor = 0`).

#include <cmath>
#include <filesystem>
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
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/gui/screens/ScreenContext.h"
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
#include "client/render/WorldItems.h"
#include "feature/common/Items.h"
#include "core/assembly/Arm.h"
#include "core/assembly/Deployer.h"
#include "core/kinetics/RotorModel.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::assembly {

namespace ca = core::assembly;
using Json   = nlohmann::json;

namespace {

constexpr char kDeployerId[] = "levitate:deployer";
constexpr char kArmId[]      = "levitate:mechanical_arm";

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept {
        return static_cast<std::size_t>(p.x) * 73856093u ^ static_cast<std::size_t>(p.y) * 19349663u ^ static_cast<std::size_t>(p.z) * 83492791u;
    }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

struct DeployerAnim {
    int       state = 0, timer = 0, speed = 0, pose = 0;
    float     reach = .5f;
    uint64_t  at    = 0; ///< client tick when received
    ItemStack held;
};
struct ArmAnim {
    int           phase = 0;
    float         progress = 1, step = 0;
    ca::ArmAngles from, to;
    bool          target = false;
    uint64_t      at     = 0;
    ItemStack     held;
};

struct State {
    std::mutex                                                   mutex;
    std::unordered_set<BlockPos, PosHash, PosEq>                 tracked; ///< chunk builder threads add
    std::unordered_map<BlockPos, DeployerAnim, PosHash, PosEq>   deployers;
    std::unordered_map<BlockPos, ArmAnim, PosHash, PosEq>        arms;
    bool                                                         logged = false;
};
State& st() {
    static State s;
    return s;
}

uint64_t clientTick() {
    auto* ci = currentClient();
    auto* level = ci ? ci->getLevel() : nullptr;
    return level ? level->getCurrentTick().tickID : 0;
}

std::optional<BlockPos> posOf(Json const& msg) {
    auto p = msg.value("pos", Json());
    if (!p.is_array() || p.size() != 4) return std::nullopt;
    return BlockPos{p[1].get<int>(), p[2].get<int>(), p[3].get<int>()};
}

ca::ArmAngles anglesOf(Json const& j) {
    ca::ArmAngles a;
    if (j.is_array() && j.size() == 4) {
        a.base  = j[0].get<float>();
        a.lower = j[1].get<float>();
        a.upper = j[2].get<float>();
        a.head  = j[3].get<float>();
    }
    return a;
}

// ---- meshes --------------------------------------------------------------------------------------

std::map<std::string, core::kinetics::RotorModel> const& models() {
    static std::map<std::string, core::kinetics::RotorModel> m = [] {
        std::map<std::string, core::kinetics::RotorModel> out;
        auto dir = Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        for (auto const* name : {"deployer_head.geo.json", "mechanical_arm_parts.geo.json"}) {
            std::ifstream in(dir / name);
            auto          j = Json::parse(in, nullptr, false, true);
            if (j.is_discarded()) continue;
            if (auto g = core::kinetics::parseGeometry(j)) out.merge(*g);
        }
        return out;
    }();
    return m;
}

std::map<std::string, std::unique_ptr<mce::Mesh>>& meshes() {
    static std::map<std::string, std::unique_ptr<mce::Mesh>> m;
    return m;
}

mce::Mesh* mesh(Tessellator& tess, std::string const& model, std::string const& bone, std::string const& texture) {
    auto  key = model + "|" + bone;
    auto& c   = meshes();
    if (auto it = c.find(key); it != c.end()) return it->second.get();
    auto& slot = c[key];
    auto  mit  = models().find(model);
    if (mit == models().end()) return nullptr;
    auto b = mit->second.bones.find(bone);
    if (b == mit->second.bones.end() || b->second.empty()) return nullptr;
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
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_assembly", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// The rotation that turns a model built facing up to `facing` (as the BP permutations do).
glm::mat4 facingRotation(int facing) {
    glm::mat4 m{1.f};
    switch (facing) {
    case 0:
        return glm::rotate(m, glm::radians(180.f), {1, 0, 0});
    case 2:
        return glm::rotate(m, glm::radians(-90.f), {1, 0, 0});
    case 3:
        return glm::rotate(m, glm::radians(90.f), {1, 0, 0});
    case 4:
        return glm::rotate(m, glm::radians(90.f), {0, 0, 1});
    case 5:
        return glm::rotate(m, glm::radians(-90.f), {0, 0, 1});
    default:
        return m;
    }
}

struct Drawer {
    BaseActorRenderContext& ctx;
    mce::MaterialPtr const& material;
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> const& texture;
    Vec3                    cam;

    void draw(mce::Mesh* m, glm::mat4 const& local) {
        if (!m) return;
        auto& screen = ctx.mScreenContext;
        auto& world  = screen.camera.worldMatrixStack.get();
        auto  ref    = world.push(false);
        ref.mat->_m.get() = ref.mat->_m.get() * local;
        m->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    }
    glm::mat4 at(BlockPos const& p) const {
        return glm::translate(glm::mat4{1.f}, glm::vec3{p.x - cam.x, p.y - cam.y, p.z - cam.z});
    }
};

/// A held item on a hand / claw whose tip is at `tip` (pointing along its +Y): blocks stand on
/// it, flat items lie on it facing out (Create's deployer / arm item).
void drawHeld(Drawer& d, ItemStack const& stack, glm::mat4 const& tip, float size) {
    if (feature::common::items::isEmpty(stack)) return;
    float scale = render::isBlockItem(stack) ? size * .75f : size;
    render::drawItem(d.ctx, stack, glm::scale(glm::translate(tip, glm::vec3{0.f, 1 / 64.f, 0.f}), glm::vec3{scale}), render::Style::Lying);
}

void drawDeployer(Drawer& d, Tessellator& tess, BlockPos const& pos, Block const& block, DeployerAnim const* anim, double now) {
    int   facing = game::intState(block, "levitate:facing").value_or(1);
    float offset = 0;
    int   pose   = 0;
    if (anim) {
        pose            = anim->pose;
        double elapsed  = now - static_cast<double>(anim->at);
        float  timer    = std::max(0.f, static_cast<float>(anim->timer - std::floor(elapsed) * anim->speed));
        float  partial  = static_cast<float>(elapsed - std::floor(elapsed));
        offset = ca::deployerHandOffset(static_cast<ca::DeployerState>(anim->state), timer, anim->speed, partial, anim->reach, static_cast<ca::HandPose>(pose));
    }
    glm::mat4 m = d.at(pos);
    m           = glm::translate(m, glm::vec3{.5f, .5f, .5f});
    m           = m * facingRotation(facing);
    m           = glm::translate(m, glm::vec3{0.f, offset, 0.f});
    m           = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
    constexpr char kModel[] = "geometry.levitate.deployer_head";
    d.draw(mesh(tess, kModel, "pole", "levitate_deployer"), m);
    d.draw(mesh(tess, kModel, pose == static_cast<int>(ca::HandPose::Punching) ? "fist" : "hand", "levitate_deployer"), m);
    if (anim) drawHeld(d, anim->held, glm::translate(m, glm::vec3{.5f, 17 / 16.f, .5f}), .5f);
}

void drawArm(Drawer& d, Tessellator& tess, BlockPos const& pos, Block const& block, ArmAnim const* anim, double now) {
    bool          ceiling = game::intState(block, "levitate:ceiling").value_or(0) != 0;
    ca::ArmAngles a       = ca::armRestAngles();
    if (anim) {
        float progress = std::min(1.f, anim->progress + static_cast<float>(now - static_cast<double>(anim->at)) * anim->step);
        a              = ca::armPoseAt(anim->from, anim->to, anim->target, progress);
        if (anim->phase == static_cast<int>(ca::ArmPhase::Dancing)) {
            float t = static_cast<float>(now);
            a.base += std::sin(t * .25f) * 30.f;
            a.lower += std::sin(t * .5f) * 10.f;
            a.head += std::sin(t * .7f) * 30.f;
        }
    }
    constexpr char kModel[] = "geometry.levitate.mechanical_arm_parts";
    constexpr char kTex[]   = "levitate_mechanical_arm";
    glm::mat4      base     = d.at(pos);
    base = glm::translate(base, glm::vec3{.5f, ceiling ? .25f : .75f, .5f});
    if (ceiling) base = glm::rotate(base, glm::radians(180.f), {1, 0, 0});
    auto centered = [](glm::mat4 m) { return glm::translate(m, glm::vec3{-.5f, 0.f, -.5f}); };
    // turntable below the joint
    glm::mat4 turn = glm::rotate(glm::translate(base, glm::vec3{0.f, -6 / 16.f, 0.f}), glm::radians(a.base), {0, 1, 0});
    d.draw(mesh(tess, kModel, "turntable", kTex), centered(turn));
    glm::mat4 lower = glm::rotate(glm::rotate(base, glm::radians(a.base), {0, 1, 0}), glm::radians(a.lower - 90.f), {1, 0, 0});
    d.draw(mesh(tess, kModel, "lower", kTex), centered(lower));
    glm::mat4 upper = glm::rotate(glm::translate(lower, glm::vec3{0.f, static_cast<float>(ca::kArmLowerLength), 0.f}), glm::radians(a.upper + 180.f), {1, 0, 0});
    d.draw(mesh(tess, kModel, "upper", kTex), centered(upper));
    glm::mat4 head = glm::rotate(glm::translate(upper, glm::vec3{0.f, static_cast<float>(ca::kArmUpperLength), 0.f}), glm::radians(90.f - a.lower - a.upper), {1, 0, 0});
    d.draw(mesh(tess, kModel, "head", kTex), centered(head));
    if (anim) drawHeld(d, anim->held, glm::translate(head, glm::vec3{0.f, 4 / 16.f, 0.f}), .4f);
}

void renderParts(BaseActorRenderContext& ctx) {
    auto* ci = currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    if (!level || !player) return;
    auto& s = st();
    std::vector<BlockPos> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.tracked.empty()) return;
        positions.assign(s.tracked.begin(), s.tracked.end());
    }
    auto& region = player->getDimensionBlockSource();
    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    Drawer d{ctx, material, texture, ctx.mImpl.get()->mCameraTargetPosition};
    auto&       tess = ctx.mScreenContext.tessellator;
    double      now  = static_cast<double>(level->getCurrentTick().tickID) + ctx.mFrameAlpha;

    std::vector<BlockPos> stale;
    int                   drawn = 0;
    for (auto const& p : positions) {
        if (!region.hasChunksAt(p, 0, false)) {
            stale.push_back(p);
            continue;
        }
        auto const& block = region.getBlock(p);
        auto const& id    = block.getTypeName();
        if (id != kDeployerId && id != kArmId) {
            stale.push_back(p);
            continue;
        }
        // only blocks the chunk mesh shows without their moving parts
        if (game::intState(block, "levitate:rotor").value_or(0) != 0) continue;
        if (id == kDeployerId) {
            std::optional<DeployerAnim> anim;
            {
                std::lock_guard lock(s.mutex);
                if (auto it = s.deployers.find(p); it != s.deployers.end()) anim = it->second;
            }
            drawDeployer(d, tess, p, block, anim ? &*anim : nullptr, now);
        } else {
            std::optional<ArmAnim> anim;
            {
                std::lock_guard lock(s.mutex);
                if (auto it = s.arms.find(p); it != s.arms.end()) anim = it->second;
            }
            drawArm(d, tess, p, block, anim ? &*anim : nullptr, now);
        }
        ++drawn;
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.tracked.erase(p);
    }
    if (drawn > 0 && !s.logged) {
        s.logged = true;
        log().info("assembly client: drawing {} deployer / arm part set(s)", drawn);
    }
}

} // namespace

// Chunk meshes: remember where deployers and arms are (the kinetics client hides their moving parts).
LL_STATIC_HOOK(
    AssemblyExtractHook,
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
        auto const& id = block.getTypeName();
        if (id == kDeployerId || id == kArmId) {
            BlockPos const& rel = iterData.relativePos;
            std::lock_guard lock(st().mutex);
            st().tracked.insert(BlockPos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
        }
    }
    return origin(block, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    AssemblyRenderHook,
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
        renderParts(renderContext);
    } catch (...) {
    }
}

class AssemblyClientModule {
public:
    void install() {
        AssemblyExtractHook::hook();
        AssemblyRenderHook::hook();
        feature::net::client::on("assembly.deployer", [](Json const& msg) {
            auto p = posOf(msg);
            if (!p) return;
            DeployerAnim a;
            a.state = msg.value("state", 0);
            a.timer = msg.value("timer", 0);
            a.speed = msg.value("speed", 0);
            a.reach = msg.value("reach", .5f);
            a.pose  = msg.value("pose", 0);
            a.at    = clientTick();
            if (auto h = msg.find("held"); h != msg.end() && h->is_object()) a.held = feature::common::items::fromJson(*h);
            std::lock_guard lock(st().mutex);
            st().deployers[*p] = a;
        });
        feature::net::client::on("assembly.arm", [](Json const& msg) {
            auto p = posOf(msg);
            if (!p) return;
            ArmAnim a;
            a.phase    = msg.value("phase", 0);
            a.progress = msg.value("progress", 1.f);
            a.step     = msg.value("step", 0.f);
            a.from     = anglesOf(msg.value("from", Json()));
            a.to       = anglesOf(msg.value("to", Json()));
            a.target   = msg.value("target", false);
            a.at       = clientTick();
            if (auto h = msg.find("heldItem"); h != msg.end() && h->is_object()) a.held = feature::common::items::fromJson(*h);
            std::lock_guard lock(st().mutex);
            st().arms[*p] = a;
        });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(st().mutex);
            st().tracked.clear();
            st().deployers.clear();
            st().arms.clear();
            meshes().clear();
            st().logged = false;
        });
    }
    void uninstall() {
        AssemblyRenderHook::unhook();
        AssemblyExtractHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::assembly

LV_REGISTER_MODULE(lv::client::assembly::AssemblyClientModule);

#endif // LV_CLIENT
