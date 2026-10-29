#ifdef LV_CLIENT

// Client side of the basin unit (client mod only): the parts that move.
//
// - Mechanical press: the head goes down and up (Create's PressingBehaviour.getRenderedHeadOffset,
//   times the mode's travel). The shaft is the kinetic renderer's (rotor state).
// - Mechanical mixer: the whisk lowers into the basin and spins (Create's mixer renderer: angle
//   = time * headSpeed * 0.6 degrees). The cog is the kinetic renderer's.
// - Blaze burner: the blaze head looks at the player while idle and faces forward when heating a
//   basin (Create's BlazeBurnerBlockEntity.tickAnimation), bobbing up and down.
// - Basin: the fluid surface at Create's height (2/16 + 12/16 * fill, fill eased), and the stacks
//   inside (Create's BasinRenderer): a ring around the centre, tilted, slowly turning, bobbing on
//   the fluid, one more copy per 8 items, drawn by the world item renderer.
//
// State comes from the server over "basin.machines" (BasinSync.cpp); between messages the client
// runs the press / mixer cycle itself, as Create's client block entities do. Chunk meshes leave the
// moving parts out (rotor state 1); vanilla clients see the static models.

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
#include "client/render/WorldItems.h"
#include "core/basin/BasinLogic.h"
#include "core/kinetics/RotorModel.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::basin {

namespace cb = core::basin;
namespace ck = core::kinetics;
using Json   = nlohmann::json;

namespace {

constexpr char const* kBasin  = "levitate:basin";
constexpr char const* kPress  = "levitate:mechanical_press";
constexpr char const* kMixer  = "levitate:mechanical_mixer";
constexpr char const* kBurner = "levitate:blaze_burner";

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept {
        return std::hash<long long>{}((static_cast<long long>(p.x) * 73856093) ^ (static_cast<long long>(p.y) * 19349663) ^ (static_cast<long long>(p.z) * 83492791));
    }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

/// What the server told us about a machine, advanced locally between messages.
struct Machine {
    int   kind = -1;
    // press / mixer
    bool  running = false;
    int   ticks = 0, prev = 0, mode = 0;
    float speed   = 0;
    bool  released = false; ///< the server passed the half cycle (press) / the processing (mixer)
    // burner
    float headAngle = 0, headAnim = 0;
    bool  angleInit = false;
    // basin
    std::vector<std::pair<std::string, int>> fluids;
    std::vector<ItemStack>                   items;
};

struct State {
    std::mutex                                                   mutex; ///< tracked (chunk builder threads)
    std::unordered_set<BlockPos, PosHash, PosEq>                 tracked;
    std::unordered_map<Block const*, Block const*>               burnerHidden; ///< burner state -> rotor 1 state
    // client thread only
    std::unordered_map<BlockPos, Machine, PosHash, PosEq>        machines;
    int                                                          dim = 0;
    std::uint64_t                                                lastTick = 0;
    bool                                                         logged = false;
};

State& state() {
    static State s;
    return s;
}

// ---- meshes --------------------------------------------------------------------------------------

std::map<std::string, ck::RotorModel> const& models() {
    static std::map<std::string, ck::RotorModel> m = [] {
        std::map<std::string, ck::RotorModel> out;
        auto dir = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        for (auto const* file : {"mechanical_press.geo.json", "mechanical_mixer_whisk.geo.json", "blaze_burner_head.geo.json"}) {
            std::ifstream in(dir / file);
            auto          j = nlohmann::json::parse(in, nullptr, false, true);
            if (j.is_discarded()) continue;
            if (auto parsed = ck::parseGeometry(j)) out.merge(*parsed);
        }
        return out;
    }();
    return m;
}

std::map<std::string, std::unique_ptr<mce::Mesh>>& meshCache() {
    static std::map<std::string, std::unique_ptr<mce::Mesh>> c;
    return c;
}

mce::Mesh* boneMesh(Tessellator& tess, std::string const& model, std::string const& bone, std::string const& texture) {
    auto  key   = model + "|" + bone + "|" + texture;
    auto& cache = meshCache();
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    auto& slot = cache[key];
    auto  m    = models().find(model);
    if (m == models().end()) return nullptr;
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
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_basin", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// The fluid surface: a 12x12 px quad at y = 0 inside the basin walls.
mce::Mesh* fluidMesh(Tessellator& tess, std::string const& texture) {
    auto  key   = "fluid|" + texture;
    auto& cache = meshCache();
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    auto& slot = cache[key];
    auto  tex  = BlockGraphics::getTextureUVCoordinateSet(texture, 0, 0);
    float a = 2.f / 16.f, b = 14.f / 16.f;
    float u0 = tex._u0 + (tex._u1 - tex._u0) * a, u1 = tex._u0 + (tex._u1 - tex._u0) * b;
    float v0 = tex._v0 + (tex._v1 - tex._v0) * a, v1 = tex._v0 + (tex._v1 - tex._v0) * b;
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    tess.simpleQuad(Vec3{a, 0, a}, Vec2{u0, v0}, Vec3{a, 0, b}, Vec2{u0, v1}, Vec3{b, 0, b}, Vec2{u1, v1}, Vec3{b, 0, a}, Vec2{u1, v0});
    tess.simpleQuad(Vec3{b, 0, a}, Vec2{u1, v0}, Vec3{b, 0, b}, Vec2{u1, v1}, Vec3{a, 0, b}, Vec2{u0, v1}, Vec3{a, 0, a}, Vec2{u0, v0});
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_basin_fluid", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

std::string fluidTexture(std::string const& fluid) {
    if (fluid == "minecraft:water") return "levitate_basin_fluid_water";
    if (fluid == "minecraft:lava") return "levitate_basin_fluid_lava";
    if (fluid == "minecraft:milk") return "levitate_basin_fluid_milk";
    if (fluid == "levitate:honey") return "levitate_basin_fluid_honey";
    if (fluid == "levitate:chocolate") return "levitate_basin_fluid_chocolate";
    if (fluid == "levitate:tea") return "levitate_basin_fluid_tea";
    return "levitate_basin_fluid_generic";
}

// ---- state from the server -------------------------------------------------------------------------

void onMachines(Json const& msg) {
    auto& s = state();
    s.dim   = msg.value("dim", 0);
    for (auto const& e : msg.value("list", Json::array())) {
        auto p = e.value("p", Json::array());
        if (!p.is_array() || p.size() != 3) continue;
        BlockPos pos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
        int      kind = e.value("k", -1);
        if (kind < 0) {
            s.machines.erase(pos);
            continue;
        }
        auto& m = s.machines[pos];
        m.kind  = kind;
        if (auto op = e.find("op"); op != e.end()) {
            bool running = op->value("running", false);
            int  ticks   = op->value("ticks", 0);
            bool press   = op->value("press", true);
            if (!running || !m.running || std::abs(ticks - m.ticks) > 2 || ticks >= (press ? cb::kPressCycle / 2 : 20)) {
                m.prev  = op->value("prev", ticks);
                m.ticks = ticks;
            }
            m.released = press ? ticks >= cb::kPressCycle / 2 : ticks > 20;
            m.running  = running;
            m.mode     = op->value("mode", 0);
            m.speed    = op->value("speed", 0.f);
        }
        if (auto b = e.find("basin"); b != e.end()) {
            m.fluids.clear();
            for (auto const& f : b->value("fluids", Json::array()))
                if (f.is_array() && f.size() == 2) m.fluids.emplace_back(f[0].get<std::string>(), f[1].get<int>());
            m.items.clear();
            for (auto const& i : b->value("stacks", Json::array())) {
                ItemStack stack = feature::common::items::fromJson(i);
                if (!feature::common::items::isEmpty(stack)) m.items.push_back(std::move(stack));
            }
        }
    }
}

/// One client tick of the press / mixer cycles (Create runs them client side too).
void clientTick(Machine& m) {
    if (m.kind == 1 && m.running) { // press
        int add = cb::pressRunningTickSpeed(m.speed);
        m.prev  = m.ticks;
        if (m.ticks >= cb::kPressCycle) return;
        m.ticks += add;
        if (m.prev < cb::kPressCycle / 2 && m.ticks >= cb::kPressCycle / 2 && !m.released) m.ticks = cb::kPressCycle / 2;
        m.ticks = std::min(m.ticks, cb::kPressCycle);
    } else if (m.kind == 2 && m.running) { // mixer
        if (m.ticks == 20 && !m.released) return;
        if (m.ticks < 40) ++m.ticks;
    }
}

// ---- drawing ---------------------------------------------------------------------------------------

struct Drawer {
    BaseActorRenderContext& ctx;
    Tessellator&            tess;
    mce::MaterialPtr const& material;
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture;
    Vec3                    cam;

    void draw(mce::Mesh* mesh, BlockPos const& pos, glm::mat4 const& local) {
        if (!mesh) return;
        auto& screen = ctx.mScreenContext;
        auto& world  = screen.camera.worldMatrixStack.get();
        auto  ref    = world.push(false);
        glm::mat4 m{1.f};
        m = glm::translate(m, glm::vec3{pos.x - cam.x, pos.y - cam.y, pos.z - cam.z});
        m = m * local;
        ref.mat->_m.get() = ref.mat->_m.get() * m;
        mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    }
};

glm::mat4 aroundCentreY(float degrees) {
    glm::mat4 m{1.f};
    m = glm::translate(m, glm::vec3{.5f, 0.f, .5f});
    m = glm::rotate(m, glm::radians(degrees), glm::vec3{0.f, 1.f, 0.f});
    m = glm::translate(m, glm::vec3{-.5f, 0.f, -.5f});
    return m;
}

std::string heatName(int blaze) {
    if (blaze >= 4) return "seething";
    if (blaze >= 2) return "kindled";
    return "smouldering";
}

/// The stacks in a basin (Create's BasinRenderer): spread on a ring of 2 px around the centre,
/// each turned to its place and tilted by 65°, at Create's level under the fluid surface (bobbing)
/// or on the bottom; the ring turns slowly.
void drawBasinItems(BaseActorRenderContext& rc, BlockPos const& pos, Machine const& m, float fluidSurface, double time) {
    if (m.items.empty()) return;
    float level = std::clamp(fluidSurface - .3f, .125f, .6f);
    float part  = 360.f / static_cast<float>(m.items.size());
    float ring  = static_cast<float>(std::fmod(time * .5, 360.0));
    std::uint32_t seed = static_cast<std::uint32_t>(pos.x) * 73856093u ^ static_cast<std::uint32_t>(pos.y) * 19349663u
                       ^ static_cast<std::uint32_t>(pos.z) * 83492791u;
    auto random = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) - .5f;
    };
    glm::mat4 base = glm::translate(glm::mat4{1.f}, render::relative(rc, {pos.x + .5, pos.y + .2, pos.z + .5}));
    base           = glm::rotate(base, glm::radians(ring), glm::vec3{0.f, 1.f, 0.f});
    for (std::size_t i = 0; i < m.items.size(); ++i) {
        float     angle = part * static_cast<float>(i);
        float     bob   = fluidSurface > 0 ? (std::sin(static_cast<float>(time) / 12.f + glm::radians(angle)) + 1.5f) / 32.f : 0.f;
        glm::mat4 at    = glm::rotate(base, glm::radians(angle), glm::vec3{0.f, 1.f, 0.f});
        at              = glm::translate(at, glm::vec3{.125f, level + bob, 0.f});
        at              = glm::rotate(at, glm::radians(35.f), glm::vec3{0.f, 1.f, 0.f});
        at              = glm::rotate(at, glm::radians(65.f), glm::vec3{1.f, 0.f, 0.f});
        int copies = 1 + feature::common::items::countOf(m.items[i]) / 8;
        for (int c = 0; c < copies; ++c) {
            glm::mat4 copy = c == 0 ? at : glm::translate(at, glm::vec3{random(), random(), random()} * (2 / 16.f));
            render::drawItem(rc, m.items[i], glm::scale(copy, glm::vec3{.375f}), render::Style::Facing);
        }
    }
}

float shortestAngle(float from, float to) {
    float d = std::fmod(to - from, 360.f);
    if (d > 180.f) d -= 360.f;
    if (d < -180.f) d += 360.f;
    return d;
}

void renderParts(BaseActorRenderContext& rc) {
    auto* ci = lv::client::currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    if (!level || !player) return;
    auto& region = player->getDimensionBlockSource();
    auto& s      = state();

    std::vector<BlockPos> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.tracked.empty()) return;
        positions.assign(s.tracked.begin(), s.tracked.end());
    }
    auto tick = static_cast<std::uint64_t>(level->getCurrentTick().tickID);
    int  steps = s.lastTick == 0 ? 0 : static_cast<int>(std::min<std::uint64_t>(tick - s.lastTick, 20));
    if (tick < s.lastTick) steps = 0;
    s.lastTick = tick;
    for (auto& [pos, m] : s.machines)
        for (int i = 0; i < steps; ++i) clientTick(m);

    auto& dispatcher = rc.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    Drawer      d{rc, rc.mScreenContext.tessellator, material, moving->mAtlasTexture.get(), rc.mImpl.get()->mCameraTargetPosition};
    float       partial = rc.mFrameAlpha;
    double      time    = static_cast<double>(tick) + partial;
    auto const& me      = player->getPosition();

    std::vector<BlockPos> stale;
    int                   drawn = 0;
    for (auto const& pos : positions) {
        if (!region.hasChunksAt(pos, 0, false)) {
            stale.push_back(pos);
            continue;
        }
        auto const& block = region.getBlock(pos);
        auto        id    = block.getTypeName();
        auto        it    = s.machines.find(pos);
        Machine     none;
        Machine&    m = it != s.machines.end() ? it->second : none;
        if (id == kPress) {
            float offset = cb::pressHeadOffset(m.running, m.prev, m.ticks, partial) * cb::pressModeHeadOffset(static_cast<cb::PressMode>(m.mode));
            glm::mat4 local{1.f};
            local = glm::translate(local, glm::vec3{0.f, -offset, 0.f});
            if (game::intState(block, "levitate:axis").value_or(0) == 2) local = local * aroundCentreY(90.f);
            d.draw(boneMesh(d.tess, "geometry.levitate.mechanical_press", "head", "levitate_mechanical_press_head"), pos, local);
            ++drawn;
        } else if (id == kMixer) {
            float offset = cb::mixerHeadOffset(m.running, m.ticks, partial) - 7.f / 16.f;
            float speed  = cb::mixerHeadSpeed(m.speed, m.running, m.ticks);
            float angle  = static_cast<float>(std::fmod(time * speed * 0.6, 360.0));
            glm::mat4 local = glm::translate(glm::mat4{1.f}, glm::vec3{0.f, -offset, 0.f}) * aroundCentreY(angle);
            d.draw(boneMesh(d.tess, "geometry.levitate.mechanical_mixer_whisk", "pole", "levitate_mechanical_mixer_pole"), pos, local);
            d.draw(boneMesh(d.tess, "geometry.levitate.mechanical_mixer_whisk", "whisk", "levitate_mechanical_mixer_whisk"), pos, local);
            ++drawn;
        } else if (id == kBurner) {
            Machine& m  = s.machines[pos]; // keeps the head's angle between frames
            int  blaze  = game::intState(block, "levitate:blaze").value_or(1);
            int  facing = game::intState(block, "levitate:facing").value_or(3);
            // Create: the head faces forward while heating something above, else looks at the player
            BlockPos above{pos.x, pos.y + 1, pos.z};
            auto     aboveId = region.getBlock(above).getTypeName();
            bool     active  = blaze >= 2 && (aboveId == kBasin || aboveId.find("fluid_tank") != std::string::npos);
            float    facingAngle = facing == 2 ? 0.f : facing == 3 ? 180.f : facing == 4 ? 90.f : 270.f;
            float    target      = facingAngle;
            if (!active) {
                double dx = me.x - (pos.x + .5), dz = me.z - (pos.z + .5);
                // model north (-z) is the face: angle 0 looks north, +90 west
                target = static_cast<float>(std::atan2(-dx, -dz) * 180.0 / std::numbers::pi);
            }
            if (!m.angleInit) {
                m.headAngle = facingAngle;
                m.angleInit = true;
            }
            float rate  = active ? .125f : .25f;
            m.headAngle += shortestAngle(m.headAngle, target) * std::min(1.f, rate * static_cast<float>(steps));
            m.headAnim  += ((active ? 1.f : 0.f) - m.headAnim) * std::min(1.f, .25f * static_cast<float>(steps));
            if (m.kind < 0) m.kind = 3;
            int   hash  = (pos.x * 31 + pos.y * 17 + pos.z * 13) % 13;
            float rt    = static_cast<float>(time) + std::abs(hash) * 16.f;
            float bob   = std::sin(std::fmod(rt / 16.f, 2.f * std::numbers::pi_v<float>)) / (blaze >= 2 ? 64.f : 16.f);
            float headY = bob - m.headAnim * .175f * .75f;
            glm::mat4 local = glm::translate(glm::mat4{1.f}, glm::vec3{0.f, headY, 0.f}) * aroundCentreY(m.headAngle);
            auto heat = heatName(blaze);
            d.draw(boneMesh(d.tess, "geometry.levitate.blaze_burner_head", "head", "levitate_blaze_burner_head_" + heat), pos, local);
            d.draw(boneMesh(d.tess, "geometry.levitate.blaze_burner_head", "face", "levitate_blaze_burner_head_" + heat + "_front"), pos, local);
            ++drawn;
        } else if (id == kBasin) {
            int total = 0;
            for (auto const& [f, n] : m.fluids) total += n;
            float surface = 0;
            if (total >= 1) {
                float fill = std::clamp(total / 2000.f, 0.f, 1.f);
                fill       = 1 - (1 - fill) * (1 - fill);
                surface    = 2.f / 16.f + 12.f / 16.f * fill;
                d.draw(fluidMesh(d.tess, fluidTexture(m.fluids.back().first)), pos, glm::translate(glm::mat4{1.f}, glm::vec3{0.f, surface, 0.f}));
            }
            drawBasinItems(rc, pos, m, surface, time);
            if (total >= 1 || !m.items.empty()) ++drawn;
        } else {
            stale.push_back(pos);
        }
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.tracked.erase(p);
    }
    if (drawn > 0 && !s.logged) {
        s.logged = true;
        log().info("basin client: drawing {} moving parts", drawn);
    }
}

/// The burner state with its head left out (rotor 1), cached per state.
Block const* hiddenBurner(Block const& block) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto [it, fresh] = s.burnerHidden.try_emplace(&block, nullptr);
    if (fresh && game::intState(block, "levitate:rotor").value_or(1) == 0) {
        it->second = game::blockWith(kBurner, {{"levitate:blaze", game::intState(block, "levitate:blaze").value_or(1)},
                                               {"levitate:facing", game::intState(block, "levitate:facing").value_or(3)},
                                               {"levitate:rotor", 1}});
    }
    return it->second;
}

void reset() {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.tracked.clear();
        s.burnerHidden.clear();
    }
    s.machines.clear();
    s.lastTick = 0;
    s.logged   = false;
    meshCache().clear();
}

} // namespace

// Chunk meshes: remember the unit's blocks; burners lose their head (drawn here instead).
LL_STATIC_HOOK(
    BasinExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block) {
        auto const& name = block.getTypeName();
        bool        mine = name == kPress || name == kMixer || name == kBurner || name == kBasin;
        if (mine) {
            BlockPos const& rel = iterData.relativePos;
            BlockPos        pos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z};
            {
                auto&           s = state();
                std::lock_guard lock(s.mutex);
                s.tracked.insert(pos);
            }
            if (name == kBurner) {
                if (auto const* hidden = hiddenBurner(block)) use = hidden;
            }
        }
    }
    return origin(*use, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    BasinRenderHook,
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

class BasinClientModule {
public:
    void install() {
        BasinExtractHook::hook();
        BasinRenderHook::hook();
        feature::net::client::on("basin.machines", [](Json const& msg) { onMachines(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { reset(); });
    }
    void uninstall() {
        BasinExtractHook::unhook();
        BasinRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::basin

LV_REGISTER_MODULE(lv::client::basin::BasinClientModule);

#endif // LV_CLIENT
