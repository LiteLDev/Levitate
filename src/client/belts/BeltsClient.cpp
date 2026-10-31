#ifdef LV_CLIENT

// Client side of the belts unit: belts drawn moving (client mod only).
//
// - Chunk meshes: a belt block is left out of the chunk mesh while the server runs Levitate (the
//   companion handshake is done); its position is remembered.
// - Every frame (block entity pass) each remembered belt is drawn from the same geometry as its
//   static model (RP models/blocks/belt.geo.json, the bones its block states show): the band with
//   its texture scrolled at the belt's surface speed (1 px steps, as the pixel art moves), the
//   pulleys turning at Create's kinetic angle, the casing as is.
// - Speeds come from the server ("belts.query" / "belts.speed", feature/belts/BeltSync.cpp).
// Items on belts are server-side display item entities for every client (SPEC §8).
// Vanilla clients (no mod) see the static model of each belt block.

#include <cmath>
#include <filesystem>
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
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/kinetics/KineticsClient.h"
#include "client/render/Outline.h"
#include "core/belts/Belt.h"
#include "core/kinetics/RotorModel.h"
#include "core/kinetics/Solver.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::belts {

namespace cb = core::belts;
namespace k  = core::kinetics;
using Json   = nlohmann::json;

namespace {

constexpr char const* kBeltId = "levitate:belt";
constexpr char const* kModel  = "geometry.levitate.belt";

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept {
        return std::size_t(p.x) * 73856093u ^ std::size_t(p.y) * 19349663u ^ std::size_t(p.z) * 83492791u;
    }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

struct State {
    std::mutex                                           mutex; ///< `tracked` (chunk builder threads)
    std::unordered_set<BlockPos, PosHash, PosEq>         tracked;
    // client thread only
    std::unordered_map<BlockPos, float, PosHash, PosEq>  speeds;
    std::unordered_set<BlockPos, PosHash, PosEq>         asked;
    int                                                  dim   = -1;
    void const*                                          level = nullptr;
    int                                                  frame = 0;
    bool                                                 logged = false;
    std::map<std::string, std::unique_ptr<mce::Mesh>>    meshes;
    // weighted ejector target selection ("belts.ejector", U55), for the arc preview
    std::optional<BlockPos>                              ejectorSel;
    int                                                  ejectorDim  = 0;
    int                                                  ejectorMax  = 32;
    std::uint64_t                                        ejectorTick = 0;
    bool                                                 ejectorLogged = false;
    // belt connector selection ("belts.connector"), for the preview
    std::optional<BlockPos>                              connectorFirst;
    int                                                  connectorDim = 0;
    int                                                  connectorMax = 20;
    std::uint64_t                                        previewTick  = 0;
    bool                                                 previewLogged = false;
};

State& state() {
    static State s;
    return s;
}

std::optional<cb::SegmentState> segmentOf(Block const& b) {
    if (b.getTypeName() != kBeltId) return std::nullopt;
    cb::SegmentState s;
    s.facing = game::intState(b, "levitate:facing").value_or(0) & 3;
    s.slope  = cb::Slope(std::clamp(game::intState(b, "levitate:slope").value_or(0), 0, 4));
    s.part   = cb::Part(std::clamp(game::intState(b, "levitate:part").value_or(1), 0, 3));
    s.casing = cb::Casing(std::clamp(game::intState(b, "levitate:casing").value_or(0), 0, 2));
    s.color  = game::intState(b, "levitate:dyed").value_or(0) ? game::intState(b, "levitate:color").value_or(0) : -1;
    return s;
}

/// The bones the static model shows for a state (the BP's bone_visibility).
std::vector<std::string> visibleBones(cb::SegmentState const& s) {
    std::vector<std::string> out;
    bool mid = s.part == cb::Part::Middle || s.part == cb::Part::Pulley;
    switch (s.slope) {
    case cb::Slope::Horizontal:
        if (mid) out.insert(out.end(), {"h_top_mid", "h_bot_mid"});
        else if (s.part == cb::Part::Start) out.insert(out.end(), {"h_top_start", "h_bot_start", "h_cap_start"});
        else out.insert(out.end(), {"h_top_end", "h_bot_end", "h_cap_end"});
        if (s.casing != cb::Casing::None) out.push_back("casing");
        break;
    case cb::Slope::Upward:
    case cb::Slope::Downward:
        out.push_back("slope");
        if (s.casing != cb::Casing::None) out.push_back("casing_slope");
        break;
    case cb::Slope::Vertical: {
        out.insert(out.end(), {"v_front", "v_back"});
        bool up = s.facing == 0 || s.facing == 3;
        if ((s.part == cb::Part::Start && up) || (s.part == cb::Part::End && !up)) out.push_back("v_cap_bottom");
        if ((s.part == cb::Part::End && up) || (s.part == cb::Part::Start && !up)) out.push_back("v_cap_top");
        if (s.casing != cb::Casing::None) out.push_back("casing_vertical");
        break;
    }
    case cb::Slope::Sideways:
        out.insert(out.end(), {"w_left", "w_right"});
        if (s.part == cb::Part::Start) out.push_back("w_cap_start");
        if (s.part == cb::Part::End) out.push_back("w_cap_end");
        break;
    }
    if (s.part != cb::Part::Middle) out.push_back(s.slope == cb::Slope::Sideways ? "pulley_y" : "pulley");
    return out;
}

/// Y rotation (degrees, right-handed) of the model (made facing south) for a state, as the BP's
/// `minecraft:transformation`.
float yRotation(cb::SegmentState const& s) {
    static constexpr float kRot[] = {0, -90, 180, 90};
    float r = kRot[s.facing & 3];
    if (s.slope == cb::Slope::Downward) r += 180;
    return r;
}

std::string textureOf(std::string const& bone, cb::SegmentState const& s) {
    static constexpr char const* kColors[] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                              "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
    if (bone == "pulley" || bone == "pulley_y") return "levitate_belt_pulley";
    if (bone == "casing" || bone == "casing_slope" || bone == "casing_vertical") return s.casing == cb::Casing::Brass ? "levitate_belt_casing_brass" : "levitate_belt_casing_andesite";
    return s.color >= 0 ? std::string("levitate_belt_") + kColors[s.color & 15] : "levitate_belt";
}

bool isBand(std::string const& bone) { return bone != "pulley" && bone != "pulley_y" && !bone.starts_with("casing"); }

k::RotorModel const* beltModel() {
    static std::optional<k::RotorModel> model = []() -> std::optional<k::RotorModel> {
        auto file = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks" / "belt.geo.json";
        std::ifstream in(file);
        auto          j = Json::parse(in, nullptr, false, true);
        if (j.is_discarded()) return std::nullopt;
        auto m = k::parseGeometry(j);
        if (!m || !m->contains(kModel)) return std::nullopt;
        return m->at(kModel);
    }();
    return model ? &*model : nullptr;
}

/// The quads of a band bone with the texture moved by `shift` (0..1 of the texture height):
/// a quad whose texture would run past the edge is split there (the atlas doesn't wrap).
std::vector<k::ModelQuad> scrolled(std::vector<k::ModelQuad> const& quads, float shift) {
    std::vector<k::ModelQuad> out;
    for (auto const& q : quads) {
        // the pair of edges along which v changes: (0 -> 3, 1 -> 2) or (0 -> 1, 3 -> 2)
        bool vDown = std::abs(q.uv[0][1] - q.uv[3][1]) > 1e-6f;
        int  a0 = 0, a1 = vDown ? 3 : 1, b0 = vDown ? 1 : 3, b1 = 2;
        float v0 = q.uv[a0][1], v1 = q.uv[a1][1];
        if (std::abs(v1 - v0) < 1e-6f || shift == 0) {
            out.push_back(q);
            continue;
        }
        auto lerpP = [](std::array<float, 3> const& p, std::array<float, 3> const& r, float t) {
            return std::array<float, 3>{p[0] + (r[0] - p[0]) * t, p[1] + (r[1] - p[1]) * t, p[2] + (r[2] - p[2]) * t};
        };
        float lo = std::min(v0, v1), hi = std::max(v0, v1);
        float cut = 1.f - shift; // texture v where the shifted v reaches 1
        auto  emit = [&](float from, float to, float delta) {
            // the part of the quad between texture v `from` .. `to`, its v moved by `delta`
            float      t0 = (from - v0) / (v1 - v0), t1 = (to - v0) / (v1 - v0);
            k::ModelQuad n = q;
            n.pos[a0]      = lerpP(q.pos[a0], q.pos[a1], t0);
            n.pos[a1]      = lerpP(q.pos[a0], q.pos[a1], t1);
            n.pos[b0]      = lerpP(q.pos[b0], q.pos[b1], t0);
            n.pos[b1]      = lerpP(q.pos[b0], q.pos[b1], t1);
            n.uv[a0][1]    = from + delta;
            n.uv[a1][1]    = to + delta;
            n.uv[b0][1]    = from + delta;
            n.uv[b1][1]    = to + delta;
            out.push_back(n);
        };
        if (hi <= cut) {
            emit(v0, v1, shift);
        } else if (lo >= cut) {
            emit(v0, v1, shift - 1);
        } else if (v0 < v1) {
            emit(v0, cut, shift);
            emit(cut, v1, shift - 1);
        } else {
            emit(v0, cut, shift - 1);
            emit(cut, v1, shift);
        }
    }
    return out;
}

mce::Mesh* meshFor(Tessellator& tess, std::string const& bone, std::string const& texture, int phase) {
    auto  key = bone + "|" + texture + "|" + std::to_string(phase);
    auto& s   = state();
    if (auto it = s.meshes.find(key); it != s.meshes.end()) return it->second.get();
    auto& slot  = s.meshes[key];
    auto* model = beltModel();
    if (!model) return nullptr;
    auto b = model->bones.find(bone);
    if (b == model->bones.end() || b->second.empty()) return nullptr;
    auto quads = phase ? scrolled(b->second, float(phase) / 16.f) : b->second;
    auto tex   = BlockGraphics::getTextureUVCoordinateSet(texture, 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : quads) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_belt", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

void resetLevel(bool dropTracked) {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (dropTracked) s.tracked.clear();
    }
    s.speeds.clear();
    s.asked.clear();
    s.meshes.clear();
    s.logged = false;
}

void onSpeeds(Json const& msg) {
    auto& s = state();
    if (msg.value("dim", -1) != s.dim) return;
    for (auto const& e : msg.value("speeds", Json::array())) {
        if (!e.is_array() || e.size() != 4) continue;
        s.speeds[BlockPos{e[0].get<int>(), e[1].get<int>(), e[2].get<int>()}] = e[3].get<float>();
    }
}

std::optional<cb::Axis> shaftAxisOf(BlockSource& region, BlockPos const& p) {
    if (!region.hasChunksAt(p, 0, false)) return std::nullopt;
    auto const& b  = region.getBlock(p);
    auto const& id = b.getTypeName();
    if (id != "levitate:shaft" && id != "levitate:kinetics_test_shaft") return std::nullopt;
    return cb::Axis(std::clamp(game::intState(b, "levitate:axis").value_or(0), 0, 2));
}

/// Create's BeltConnectorHandler.tick on the client: dust along the planned belt (green when the
/// shafts can be connected, red when not), once per game tick while the connector is held.
void connectorPreview(Level& level, LocalPlayer& player) {
    auto& s = state();
    if (!s.connectorFirst) return;
    auto tick = static_cast<std::uint64_t>(level.getCurrentTick().tickID);
    if (tick == s.previewTick) return;
    s.previewTick = tick;
    auto const& held = player.getSelectedItem();
    if (held.isNull() || held.getTypeName() != "levitate:belt_connector") return;
    if (int(player.getDimensionId()) != s.connectorDim) return;
    auto&    region = player.getDimensionBlockSource();
    BlockPos first  = *s.connectorFirst;
    auto     axis   = shaftAxisOf(region, first);
    if (!axis) return;
    static std::mt19937 rng{std::random_device{}()};
    // client-side particles (Level::addParticleEffect reaches the level renderer; spawnParticleEffect
    // would go to the server particle manager)
    static HashedString const kValid{"levitate:belt_connector_valid"}, kInvalid{"levitate:belt_connector_invalid"};
    auto dust = [&](double x, double y, double z, bool valid) {
        static_cast<ILevel&>(level).addParticleEffect(valid ? kValid : kInvalid, Vec3{float(x), float(y), float(z)});
    };
    auto& hit = level.getHitResult();
    if (hit.mType != HitResultType::Tile) {
        std::uniform_real_distribution<double> jitter(-.25, .25);
        if (rng() % 50 == 0) dust(first.x + .5 + jitter(rng), first.y + .5 + jitter(rng), first.z + .5 + jitter(rng), true);
        return;
    }
    BlockPos selected = hit.mBlock;
    if (!region.hasChunksAt(selected, 0, false) || game::replaceable(region, selected)) return;
    if (!shaftAxisOf(region, selected)) {
        auto n   = cb::faceNormal(hit.mFacing);
        selected = BlockPos{selected.x + n.x, selected.y + n.y, selected.z + n.z};
    }
    cb::Vec3i a{first.x, first.y, first.z}, b{selected.x, selected.y, selected.z};
    if (!((b - a).distSqr() < 1LL * s.connectorMax * s.connectorMax)) return;
    cb::ConnectWorld w;
    w.loaded    = [&](cb::Vec3i const& p) { return region.hasChunksAt(BlockPos{p.x, p.y, p.z}, 0, false); };
    w.shaftAxis = [&](cb::Vec3i const& p) { return shaftAxisOf(region, BlockPos{p.x, p.y, p.z}); };
    w.speed     = [&](cb::Vec3i const& p) {
        auto sp = lv::client::kinetics::syncedSpeed(s.connectorDim, p.x, p.y, p.z);
        return sp ? sp->speed : 0.f;
    };
    w.replaceable = [&](cb::Vec3i const& p) { return game::replaceable(region, BlockPos{p.x, p.y, p.z}); };
    bool valid    = shaftAxisOf(region, selected) && cb::canConnect(w, a, b, s.connectorMax) == cb::ConnectError::None;
    auto line     = cb::previewLine(a, b, *axis);
    if (!line) return;
    if (!s.previewLogged) {
        s.previewLogged = true;
        log().info("belts client: connector preview towards {} {} {} ({} blocks, {})", selected.x, selected.y, selected.z, line->length, valid ? "valid" : "invalid");
    }
    for (float f = 0; f < line->length; f += 1 / 16.f) {
        if (rng() % 10 != 0) continue;
        dust(first.x + .5 + line->step.x * f, first.y + .5 + line->step.y * f, first.z + .5 + line->step.z * f, valid);
    }
}

/// Create's EjectorTargetHandler (client): while the ejector item is held, the selected target is
/// outlined and dust shows the arc from where the ejector would be placed (green when the target
/// is straight ahead, red when it would be corrected onto the axis).
void ejectorPreview(BaseActorRenderContext& ctx, Level& level, LocalPlayer& player) {
    auto& s = state();
    if (!s.ejectorSel) return;
    auto const& held = player.getSelectedItem();
    if (held.isNull() || held.getTypeName() != "levitate:weighted_ejector") return;
    if (int(player.getDimensionId()) != s.ejectorDim) return;
    BlockPos sel = *s.ejectorSel;
    {
        render::Lines lines(ctx);
        lines.outline({double(sel.x), double(sel.y), double(sel.z)}, {sel.x + 1.0, sel.y + 1.0, sel.z + 1.0}, render::Rgb::hex(0xffcb74), 1.0 / 16);
        auto const& hit = level.getHitResult();
        if (hit.mType != HitResultType::Tile) return;
        auto     n = cb::faceNormal(hit.mFacing);
        BlockPos pos{hit.mBlock.x + n.x, hit.mBlock.y + n.y, hit.mBlock.z + n.z};
        int      xDiff = sel.x - pos.x, yDiff = sel.y - pos.y, zDiff = sel.z - pos.z;
        int      validX = std::abs(zDiff) > std::abs(xDiff) ? 0 : xDiff;
        int      validZ = std::abs(zDiff) < std::abs(xDiff) ? 0 : zDiff;
        cb::Vec3i ejector{sel.x - validX, pos.y, sel.z - validZ};
        auto     facing = cb::ejectorTargetFacing(ejector, {sel.x, sel.y, sel.z}, s.ejectorMax);
        if (!facing) return;
        bool valid = xDiff == validX && zDiff == validZ;
        auto colour = render::Rgb::hex(valid ? 0x9ede73 : 0xff7171);
        lines.outline({double(ejector.x), double(ejector.y), double(ejector.z)}, {ejector.x + 1.0, ejector.y + 0.0, ejector.z + 1.0}, colour, 1.0 / 16);
        auto tick = static_cast<std::uint64_t>(level.getCurrentTick().tickID);
        if (tick == s.ejectorTick) return;
        s.ejectorTick = tick;
        auto   launch    = cb::planLaunch(std::abs(validX + validZ), yDiff);
        double total     = launch.ticks + 3;
        int    segments  = int(total) / 3 + 1;
        double offset    = total / segments;
        double renderT   = double(tick) / 3.0;
        auto   step      = cb::facingNormal(*facing);
        static HashedString const kValid{"levitate:belt_connector_valid"}, kInvalid{"levitate:belt_connector_invalid"};
        for (int i = 0; i < segments; ++i) {
            double t           = std::fmod(renderT, offset) + i * offset;
            auto [along, up]   = launch.at(t);
            Vec3 at{float(ejector.x + .5 + step.x * along), float(ejector.y + .5 + up), float(ejector.z + .5 + step.z * along)};
            static_cast<ILevel&>(level).addParticleEffect(valid ? kValid : kInvalid, at);
        }
        if (!s.ejectorLogged) {
            s.ejectorLogged = true;
            log().info("belts client: ejector arc to {} {} {} ({})", sel.x, sel.y, sel.z, valid ? "valid" : "corrected");
        }
    }
}

void onEjector(Json const& msg) {
    auto& s = state();
    auto  p = msg.value("pos", Json::array());
    if (!p.is_array() || p.size() != 3) {
        s.ejectorSel.reset();
        return;
    }
    s.ejectorSel    = BlockPos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
    s.ejectorDim    = msg.value("dim", 0);
    s.ejectorMax    = msg.value("max", 32);
    s.ejectorLogged = false;
    log().info("belts client: ejector target {} {} {}", p[0].get<int>(), p[1].get<int>(), p[2].get<int>());
}

void onConnector(Json const& msg) {
    auto& s = state();
    if (msg.value("clear", false)) {
        s.connectorFirst.reset();
        return;
    }
    auto p = msg.value("pos", Json::array());
    if (!p.is_array() || p.size() != 3) return;
    s.connectorFirst = BlockPos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
    s.previewLogged  = false;
    s.connectorDim   = msg.value("dim", 0);
    s.connectorMax   = msg.value("max", 20);
    log().info("belts client: connector preview from {} {} {}", p[0].get<int>(), p[1].get<int>(), p[2].get<int>());
}

void renderBelts(BaseActorRenderContext& ctx) {
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
    try {
        connectorPreview(*level, *player);
        ejectorPreview(ctx, *level, *player);
    } catch (...) {
    }
    int dim = int(player->getDimensionId());
    if (dim != s.dim) {
        s.speeds.clear();
        s.asked.clear();
        s.dim = dim;
    }
    std::vector<BlockPos> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.tracked.empty()) return;
        positions.assign(s.tracked.begin(), s.tracked.end());
    }
    auto&                 region = player->getDimensionBlockSource();
    std::vector<BlockPos> stale;
    Json                  ask = Json::array();

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
    int        drawn  = 0;

    for (auto const& p : positions) {
        if (!region.hasChunksAt(p, 0, false)) {
            stale.push_back(p);
            continue;
        }
        auto seg = segmentOf(region.getBlock(p));
        if (!seg) {
            stale.push_back(p);
            continue;
        }
        float rpm = 0;
        if (auto it = s.speeds.find(p); it != s.speeds.end()) rpm = it->second;
        else if (ask.size() < 256 && s.asked.insert(p).second) ask.push_back({p.x, p.y, p.z});

        // the band moves at the items' speed (blocks per tick) -> texture pixels
        float surface = cb::beltMovementSpeed(rpm) * float(cb::facingStep(seg->facing)) * (cb::facingAxis(seg->facing) == cb::Axis::X ? -1.f : 1.f);
        if (seg->slope == cb::Slope::Downward) surface = -surface; // the model is the upward one turned around
        double scroll = std::fmod(time * double(surface) * 16.0, 16.0);
        if (scroll < 0) scroll += 16;
        int phase = int(scroll) & 15;

        auto       axisW = cb::rotationAxis(*seg);
        k::Axis    kAxis = k::Axis(axisW);
        float      angle = k::rotationAngle(time, rpm, k::rotationOffset(core::GlobalPos{0, p.x, p.y, p.z}, kAxis, false));
        glm::vec3  axisV{axisW == cb::Axis::X ? 1.f : 0.f, axisW == cb::Axis::Y ? 1.f : 0.f, axisW == cb::Axis::Z ? 1.f : 0.f};
        float      yRot = yRotation(*seg);

        for (auto const& bone : visibleBones(*seg)) {
            bool  band = isBand(bone);
            auto* mesh = meshFor(tess, bone, textureOf(bone, *seg), band ? phase : 0);
            if (!mesh) continue;
            auto      ref = world.push(false);
            glm::mat4 m{1.f};
            m = glm::translate(m, glm::vec3{p.x - cam.x, p.y - cam.y, p.z - cam.z});
            m = glm::translate(m, glm::vec3{.5f, .5f, .5f});
            if (bone == "pulley" || bone == "pulley_y") m = glm::rotate(m, glm::radians(angle), axisV);
            m = glm::rotate(m, glm::radians(yRot), glm::vec3{0.f, 1.f, 0.f});
            m = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
            ref.mat->_m.get() = ref.mat->_m.get() * m;
            mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        }
        ++drawn;
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.tracked.erase(p);
    }
    if (!ask.empty()) feature::net::client::send("belts.query", {{"pos", ask}});
    if (drawn && !s.logged) {
        s.logged = true;
        log().info("belts client: drawing {} belt segments", drawn);
    }
}

} // namespace

// Chunk meshes: belts are drawn by us while the server runs Levitate.
LL_STATIC_HOOK(
    BeltsExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block && block.getTypeName() == kBeltId
        && feature::net::client::connected() && beltModel()) {
        BlockPos const& rel = iterData.relativePos;
        BlockPos        pos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z};
        auto&           s = state();
        std::lock_guard lock(s.mutex);
        s.tracked.insert(pos);
        return std::nullopt;
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
    BeltsRenderHook,
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
        renderBelts(renderContext);
    } catch (...) {
    }
}

class BeltsClientModule {
public:
    void install() {
        BeltsExtractHook::hook();
        BeltsRenderHook::hook();
        feature::net::client::on("belts.speed", [](Json const& msg) { onSpeeds(msg); });
        feature::net::client::on("belts.connector", [](Json const& msg) { onConnector(msg); });
        feature::net::client::on("belts.ejector", [](Json const& msg) { onEjector(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevel(true);
            state().level = nullptr;
            state().connectorFirst.reset();
            state().ejectorSel.reset();
        });
    }
    void uninstall() {
        BeltsExtractHook::unhook();
        BeltsRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::belts

LV_REGISTER_MODULE(lv::client::belts::BeltsClientModule);

#endif // LV_CLIENT
