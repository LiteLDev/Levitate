#ifdef LV_CLIENT

// Client side of the item logistics unit: funnel flaps that swing smoothly (client mod only).
//
// Create's FunnelRenderer draws the flap of a horizontal funnel as four strips that swing around
// their top edge after every transfer, each with its own frequency, damped by a value chasing 0
// (Create's FunnelBlockEntity.flap: LerpedFloat, exponential chaser at .05 per tick).
// - Chunk meshes: a horizontal funnel (wall or belt funnel) is left out of the chunk mesh while the
//   server runs Levitate (the companion handshake is done); its position is remembered.
// - Every frame each remembered funnel is drawn from its static model (RP models/blocks/
//   funnel_wall / funnel_belt / funnel_belt_extended .geo.json: plate, hood and the strips
//   flap_0..3), the strips turned by the flap angle.
// - The server sends "funnels.flap" {dim, pos, inward} on every transfer (feature/funnels/Funnel.cpp).
// Vanilla clients (no mod) see the state-driven flap (`levitate:flap` for 4 ticks).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
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
#include "core/kinetics/RotorModel.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::funnels {

namespace k = core::kinetics;
using Json  = nlohmann::json;

namespace {

constexpr char const* kAndesite = "levitate:andesite_funnel";
constexpr char const* kBrass    = "levitate:brass_funnel";
constexpr char const* kMaterials[] = {"frame", "back", "side", "flap"};
constexpr char const* kBones[]     = {"plate", "hood", "flap_0", "flap_1", "flap_2", "flap_3"};

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept {
        return std::size_t(p.x) * 73856093u ^ std::size_t(p.y) * 19349663u ^ std::size_t(p.z) * 83492791u;
    }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

/// Create's FunnelBlockEntity.flap (LerpedFloat): value chases 0 at .05 per tick.
struct Flap {
    float value = .25f, previous = .25f;
};

struct State {
    std::mutex                                       mutex; ///< `tracked` (chunk builder threads)
    std::unordered_set<BlockPos, PosHash, PosEq>     tracked;
    // client thread only
    std::unordered_map<BlockPos, Flap, PosHash, PosEq> flaps;
    void const*                                      level    = nullptr;
    std::uint64_t                                    lastTick = 0;
    bool                                             logged   = false;
    std::map<std::string, std::unique_ptr<mce::Mesh>> meshes;
};

State& state() {
    static State s;
    return s;
}

bool isFunnelId(std::string const& id) { return id == kAndesite || id == kBrass; }

/// Every material instance of the three horizontal funnel models, as separate models (the parser
/// has no material per face: each copy keeps the faces of one material only).
using Models = std::map<std::string, std::map<std::string, k::RotorModel>>; // material -> geometry id -> model
Models const* funnelModels() {
    static std::optional<Models> models = []() -> std::optional<Models> {
        Models out;
        auto   dir = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        for (char const* file : {"funnel_wall", "funnel_belt", "funnel_belt_extended"}) {
            std::ifstream in(dir / (std::string(file) + ".geo.json"));
            auto          j = Json::parse(in, nullptr, false, true);
            if (j.is_discarded()) return std::nullopt;
            for (char const* material : kMaterials) {
                Json copy = j;
                for (auto& geo : copy["minecraft:geometry"])
                    for (auto& bone : geo["bones"])
                        for (auto& cube : bone["cubes"]) {
                            if (!cube.contains("uv") || !cube["uv"].is_object()) continue;
                            Json kept = Json::object();
                            for (auto& [face, uv] : cube["uv"].items())
                                if (uv.value("material_instance", std::string{}) == material) kept[face] = uv;
                            cube["uv"] = kept;
                        }
                auto parsed = k::parseGeometry(copy);
                if (!parsed) return std::nullopt;
                for (auto& [id, model] : *parsed) out[material][id] = std::move(model);
            }
        }
        return out;
    }();
    return models ? &*models : nullptr;
}

int stateOf(Block const& b, char const* name, int fallback = 0) { return game::intState(b, name).value_or(fallback); }

std::string textureOf(Block const& b, std::string const& material) {
    std::string n = b.getTypeName() == kBrass ? "brass" : "andesite";
    if (material != "back") return "levitate_" + n + "_funnel_" + material;
    char const* back = stateOf(b, "levitate:powered") ? "powered" : stateOf(b, "levitate:extracting", 1) ? "extract" : "insert";
    return "levitate_" + n + "_funnel_back_" + back;
}

std::string geometryOf(Block const& b) {
    int shape = stateOf(b, "levitate:shape");
    if (shape == 0) return "geometry.levitate.funnel_wall";
    return shape == 2 ? "geometry.levitate.funnel_belt_extended" : "geometry.levitate.funnel_belt";
}

/// The BP's `minecraft:transformation` Y rotation per facing (the models face north).
float yRotation(int facing) {
    switch (facing) {
    case 3: return 180.f;
    case 4: return 90.f;
    case 5: return -90.f;
    default: return 0.f;
    }
}

mce::Mesh* meshFor(Tessellator& tess, std::string const& geo, std::string const& bone, std::string const& material, std::string const& texture) {
    auto  key = geo + "|" + bone + "|" + material + "|" + texture;
    auto& s   = state();
    if (auto it = s.meshes.find(key); it != s.meshes.end()) return it->second.get();
    auto& slot   = s.meshes[key];
    auto* models = funnelModels();
    if (!models) return nullptr;
    auto m = models->find(material);
    if (m == models->end()) return nullptr;
    auto g = m->second.find(geo);
    if (g == m->second.end()) return nullptr;
    auto b = g->second.bones.find(bone);
    if (b == g->second.bones.end() || b->second.empty()) return nullptr;
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
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_funnel", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// Create's FunnelRenderer: the angle (degrees) of flap strip `segment` for flap value `f`.
float flapAngle(int segment, float f) {
    float intensity = segment == 3 ? 1.5f : float(segment + 1);
    float angle     = std::sin((1 - std::abs(f)) * std::numbers::pi_v<float> * intensity) * 30.f * f;
    if (f > 0) angle *= .5f;
    return angle;
}

void tickFlaps(std::uint64_t tick) {
    auto& s = state();
    if (tick == s.lastTick) return;
    std::uint64_t steps = s.lastTick == 0 || tick < s.lastTick ? 1 : std::min<std::uint64_t>(tick - s.lastTick, 20);
    s.lastTick          = tick;
    for (std::uint64_t i = 0; i < steps; ++i) {
        for (auto it = s.flaps.begin(); it != s.flaps.end();) {
            auto& f    = it->second;
            f.previous = f.value;
            f.value += (0 - f.value) * .05f;
            if (std::abs(f.value) < 1 / 512.f) f.value = 0;
            if (f.value == 0 && f.previous == 0) it = s.flaps.erase(it);
            else ++it;
        }
    }
}

void resetLevel(bool dropTracked) {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (dropTracked) s.tracked.clear();
    }
    s.flaps.clear();
    s.meshes.clear();
    s.lastTick = 0;
    s.logged   = false;
}

void onFlap(Json const& msg) {
    auto p = msg.value("pos", Json::array());
    if (!p.is_array() || p.size() != 3) return;
    auto& f    = state().flaps[BlockPos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()}];
    f.value    = msg.value("inward", true) ? 1.f : -1.f;
    f.previous = f.value;
}

void renderFunnels(BaseActorRenderContext& ctx) {
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
    tickFlaps(static_cast<std::uint64_t>(level->getCurrentTick().tickID));
    std::vector<BlockPos> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.tracked.empty()) return;
        positions.assign(s.tracked.begin(), s.tracked.end());
    }
    auto&                 region = player->getDimensionBlockSource();
    std::vector<BlockPos> stale;

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&      screen = ctx.mScreenContext;
    auto&      tess   = screen.tessellator;
    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    auto&      world  = screen.camera.worldMatrixStack.get();
    float      alpha  = ctx.mFrameAlpha;
    int        drawn  = 0;

    for (auto const& p : positions) {
        if (!region.hasChunksAt(p, 0, false)) {
            stale.push_back(p);
            continue;
        }
        auto const& block  = region.getBlock(p);
        int         facing = stateOf(block, "levitate:facing", 1);
        if (!isFunnelId(block.getTypeName()) || facing < 2 || facing > 5) {
            stale.push_back(p);
            continue;
        }
        float f = 0;
        if (auto it = s.flaps.find(p); it != s.flaps.end()) f = it->second.previous + (it->second.value - it->second.previous) * alpha;
        auto  geo   = geometryOf(block);
        float yRot  = yRotation(facing);
        // the strips turn around their top edge (the flap bones' pivot): model (0, 13 + dy, z)
        int   shape = stateOf(block, "levitate:shape");
        float dy    = shape == 0 ? 0.f : -3.f;
        float pz    = (shape == 2 ? -8.f : -2.f) + .5f;
        glm::vec3 pivot{.5f, (13.f + dy) / 16.f, (pz + 8.f) / 16.f};
        for (int bi = 0; bi < 6; ++bi) {
            std::string bone   = kBones[bi];
            int         strip  = bi - 2;
            float       angle  = strip >= 0 ? flapAngle(strip, f) : 0.f;
            for (char const* mat : kMaterials) {
                auto* mesh = meshFor(tess, geo, bone, mat, textureOf(block, mat));
                if (!mesh) continue;
                auto      ref = world.push(false);
                glm::mat4 m{1.f};
                m = glm::translate(m, glm::vec3{p.x - cam.x, p.y - cam.y, p.z - cam.z});
                m = glm::translate(m, glm::vec3{.5f, .5f, .5f});
                m = glm::rotate(m, glm::radians(yRot), glm::vec3{0.f, 1.f, 0.f});
                m = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
                if (angle != 0) {
                    m = glm::translate(m, pivot);
                    m = glm::rotate(m, glm::radians(-angle), glm::vec3{1.f, 0.f, 0.f});
                    m = glm::translate(m, -pivot);
                }
                ref.mat->_m.get() = ref.mat->_m.get() * m;
                mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
            }
        }
        ++drawn;
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.tracked.erase(p);
    }
    if (drawn && !s.logged) {
        s.logged = true;
        log().info("funnels client: drawing {} funnels", drawn);
    }
}

} // namespace

// Chunk meshes: horizontal funnels are drawn by us while the server runs Levitate.
LL_STATIC_HOOK(
    FunnelsExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block && isFunnelId(block.getTypeName())
        && feature::net::client::connected()) {
        int facing = game::intState(block, "levitate:facing").value_or(1);
        if (facing >= 2 && facing <= 5 && funnelModels()) {
            BlockPos const& rel = iterData.relativePos;
            BlockPos        pos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z};
            auto&           s = state();
            std::lock_guard lock(s.mutex);
            s.tracked.insert(pos);
            return std::nullopt;
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

LL_TYPE_INSTANCE_HOOK(
    FunnelsRenderHook,
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
        renderFunnels(renderContext);
    } catch (...) {
    }
}

class FunnelsClientModule {
public:
    void install() {
        FunnelsExtractHook::hook();
        FunnelsRenderHook::hook();
        feature::net::client::on("funnels.flap", [](Json const& msg) { onFlap(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevel(true);
            state().level = nullptr;
        });
    }
    void uninstall() {
        FunnelsExtractHook::unhook();
        FunnelsRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::funnels

LV_REGISTER_MODULE(lv::client::funnels::FunnelsClientModule);

#endif // LV_CLIENT
