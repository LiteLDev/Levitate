#ifdef LV_CLIENT

// Gauge needles of the speedometer and stressometer (U6), drawn by the client mod as Create's
// GaugeRenderer does: the needle chases the dial target the server sends (`transmission.gauge`)
// by 1/8 of the way per tick (shaking past the end when it is over 1) and is drawn on every head
// face of the gauge. The chunk mesh holds the gauge without its static needles (`levitate:rotor`
// = 1, done by the kinetic renderer), so vanilla clients keep the static needle positions.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>

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
#include "core/transmission/Transmission.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::transmission {

namespace k  = core::kinetics;
namespace ct = core::transmission;

namespace {

struct Dial {
    float target = 0, state = 0, prev = 0;
};

struct PosKey {
    int x, y, z;
    bool operator==(PosKey const&) const = default;
};
struct PosKeyHash {
    std::size_t operator()(PosKey const& p) const noexcept { return k::GlobalPosHash{}(core::GlobalPos{0, p.x, p.y, p.z}); }
};

struct State {
    std::unordered_map<int, std::unordered_map<PosKey, Dial, PosKeyHash>> dials;
    std::uint64_t                                                         lastTick = 0;
    std::unique_ptr<mce::Mesh>                                            needle;
    std::vector<k::ModelQuad>                                             quads;
    float                                                                 sign   = -1;
    bool                                                                  loaded = false;
    bool                                                                  logged = false;
    std::mt19937                                                          rng{1234};
};

State& state() {
    static State s;
    return s;
}

constexpr float kPivotX = .5f, kPivotY = 7.f / 16.f, kPivotZ = -.4f / 16.f;

std::optional<std::map<std::string, k::RotorModel>> loadModels(char const* file) {
    auto path = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks" / file;
    std::ifstream in(path);
    if (!in) return std::nullopt;
    auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded()) return std::nullopt;
    return k::parseGeometry(j);
}

std::array<float, 3> centroid(std::vector<k::ModelQuad> const& quads) {
    std::array<float, 3> c{0, 0, 0};
    int                  n = 0;
    for (auto const& q : quads)
        for (auto const& p : q.pos) {
            for (int i = 0; i < 3; ++i) c[i] += p[i];
            ++n;
        }
    if (n) for (auto& v : c) v /= static_cast<float>(n);
    return c;
}

/// Reads the needle and works out which way the static model turns it (to match vanilla clients).
void loadNeedle() {
    auto& s  = state();
    s.loaded = true;
    if (auto rotors = loadModels("transmission_rotors.geo.json")) {
        auto m = rotors->find("geometry.levitate.transmission_needle");
        if (m != rotors->end() && m->second.bones.contains("needle")) s.quads = m->second.bones.at("needle");
    }
    if (auto gauges = loadModels("transmission_gauge.geo.json")) {
        auto m = gauges->find("geometry.levitate.transmission_gauge_side_x");
        if (m != gauges->end() && m->second.bones.contains("dial_0") && m->second.bones.contains("dial_1")) {
            auto  c0 = centroid(m->second.bones.at("dial_0"));
            auto  c1 = centroid(m->second.bones.at("dial_1"));
            float best = 1e9f;
            for (float sign : {1.f, -1.f}) {
                float a  = glm::radians(sign * 22.5f);
                float dx = c0[0] - kPivotX, dy = c0[1] - kPivotY;
                float rx = kPivotX + dx * std::cos(a) - dy * std::sin(a), ry = kPivotY + dx * std::sin(a) + dy * std::cos(a);
                float d  = (rx - c1[0]) * (rx - c1[0]) + (ry - c1[1]) * (ry - c1[1]);
                if (d < best) {
                    best   = d;
                    s.sign = sign;
                }
            }
        }
    }
}

mce::Mesh* needleMesh(Tessellator& tess) {
    auto& s = state();
    if (!s.loaded) loadNeedle();
    if (s.needle) return s.needle.get();
    if (s.quads.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet("levitate_transmission_rotor", 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : s.quads) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    s.needle = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_gauge_needle", SupplementaryFieldAutoGenerationMode::None));
    return s.needle.get();
}

/// Create's GaugeBlockEntity.tick for every known gauge, once per game tick.
void advance(std::uint64_t tick) {
    auto& s = state();
    if (tick == s.lastTick) return;
    int steps  = s.lastTick == 0 || tick < s.lastTick ? 1 : static_cast<int>(std::min<std::uint64_t>(tick - s.lastTick, 20));
    s.lastTick = tick;
    std::uniform_real_distribution<float> random(0.f, 1.f);
    for (auto& [dim, map] : s.dials) {
        for (auto& [p, d] : map) {
            for (int i = 0; i < steps; ++i) {
                d.prev  = d.state;
                d.state = ct::dialStep(d.state, d.target);
                if (d.state > 1 && random(s.rng) < .5f) d.state -= (d.state - 1) * random(s.rng);
            }
        }
    }
}

int yawOf(k::Face f) {
    switch (f) {
    case k::Face::West:
        return 90;
    case k::Face::South:
        return 180;
    case k::Face::East:
        return 270;
    default:
        return 0;
    }
}

void drawNeedles(BaseActorRenderContext& ctx) {
    auto* ci = currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    if (!level || !player) return;
    auto& s   = state();
    int   dim = static_cast<int>(player->getDimensionId());
    auto  it  = s.dials.find(dim);
    if (it == s.dials.end() || it->second.empty()) return;
    advance(level->getCurrentTick().tickID);

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&      screen = ctx.mScreenContext;
    auto*      mesh   = needleMesh(screen.tessellator);
    if (!mesh) return;
    auto&      region = player->getDimensionBlockSource();
    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    float      alpha  = ctx.mFrameAlpha;
    auto&      world  = screen.camera.worldMatrixStack.get();
    int        drawn  = 0;
    for (auto const& [p, d] : it->second) {
        BlockPos bp{p.x, p.y, p.z};
        if (!region.hasChunksAt(bp, 0, false)) continue;
        auto const& block = region.getBlock(bp);
        auto const& id    = block.getTypeName();
        if (id != "levitate:speedometer" && id != "levitate:stressometer") continue;
        auto facing = static_cast<k::Face>(std::clamp(game::intState(block, "levitate:facing").value_or(2), 0, 5));
        bool along  = game::intState(block, "levitate:along_first").value_or(0) != 0;
        float dial  = d.prev + (d.state - d.prev) * alpha;
        for (k::Face face : {k::Face::North, k::Face::South, k::Face::West, k::Face::East}) {
            if (!ct::gaugeHeadOnFace(facing, along, face)) continue;
            auto o = k::offsetOf(face);
            BlockPos np{p.x + o.x, p.y + o.y, p.z + o.z};
            if (region.hasChunksAt(np, 0, false) && region.getBlock(np).canProvideFullSupport(static_cast<uchar>(k::opposite(face)))) continue;
            auto      ref = world.push(false);
            glm::mat4 m{1.f};
            m = glm::translate(m, glm::vec3{p.x - cam.x + .5f, p.y - cam.y + .5f, p.z - cam.z + .5f});
            m = glm::rotate(m, glm::radians(static_cast<float>(yawOf(face))), glm::vec3{0.f, 1.f, 0.f});
            m = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
            m = glm::translate(m, glm::vec3{kPivotX, kPivotY, kPivotZ});
            m = glm::rotate(m, glm::radians(s.sign * ct::dialAngle(dial)), glm::vec3{0.f, 0.f, 1.f});
            m = glm::translate(m, glm::vec3{-kPivotX, -kPivotY, -kPivotZ});
            ref.mat->_m.get() = ref.mat->_m.get() * m;
            mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
            ++drawn;
        }
    }
    if (drawn && !s.logged) {
        s.logged = true;
        log().info("transmission client: drawing {} gauge needles", drawn);
    }
}

void onGauge(nlohmann::json const& msg) {
    auto& s   = state();
    int   dim = msg.value("dim", 0);
    auto& map = s.dials[dim];
    if (msg.value("reset", false)) map.clear();
    for (auto const& e : msg.value("set", nlohmann::json::array())) {
        if (!e.is_array() || e.size() != 4) continue;
        auto& d  = map[PosKey{e[0].get<int>(), e[1].get<int>(), e[2].get<int>()}];
        d.target = e[3].get<float>();
    }
    for (auto const& e : msg.value("remove", nlohmann::json::array())) {
        if (e.is_array() && e.size() == 3) map.erase(PosKey{e[0].get<int>(), e[1].get<int>(), e[2].get<int>()});
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TransmissionGaugeRenderHook,
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
        drawNeedles(renderContext);
    } catch (...) {
    }
}

class GaugeRendererModule {
public:
    void install() {
        feature::net::client::on("transmission.gauge", onGauge);
        TransmissionGaugeRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto& s = state();
            s.dials.clear();
            s.needle.reset();
            s.lastTick = 0;
            s.logged   = false;
        });
    }
    void uninstall() {
        TransmissionGaugeRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::transmission

LV_REGISTER_MODULE(lv::client::transmission::GaugeRendererModule);

#endif // LV_CLIENT
