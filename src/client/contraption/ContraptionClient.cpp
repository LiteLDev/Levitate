// Client mod: which display entity shows which contraption block, the contraption's motion, and
// actor visuals (see ContraptionClient.h; the server side is in feature/contraption/Engine.cpp).
//
// Bedrock clients never apply the movement the server sends for a falling block that isn't
// falling (the position stays where the entity was spawned), so the display entities can't
// follow the server on their own. The server sends each contraption's transform instead
// ("contraption.motion", every tick it changes); here it is consumed one per client tick and
// every display entity is put at its block's place, the previous place kept as the entity's old
// position, so the game draws it in between at each frame (Create's clients interpolate the
// contraption entity in the same way). The block's turn is drawn by Render.cpp (displayTurn).

#ifdef LV_CLIENT

#include "client/contraption/ContraptionClient.h"
#include "client/contraption/Display.h"

#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "core/contraption/Motion.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::contraption {

namespace {

using Json = nlohmann::json;
namespace cc = core::contraption;

struct Block {
    std::int64_t display = 0;
    cc::Pos      local;
    std::string  id;
};
struct Known {
    int                                       dim = 0, axis = 1, quarter = 0;
    std::vector<Block>                        blocks;
    std::map<std::tuple<int, int, int>, Json> visuals;
    cc::MotionBuffer                          motion;
    bool                                      moved = false; ///< logged once
};

std::map<std::uint64_t, Known>& known() {
    static std::map<std::uint64_t, Known> k;
    return k;
}
/// display entity (ActorUniqueID) -> contraption
std::unordered_map<std::int64_t, std::uint64_t>& displays() {
    static std::unordered_map<std::int64_t, std::uint64_t> d;
    return d;
}

cc::Pos posOf(Json const& j, std::size_t at) { return {j[at].get<int>(), j[at + 1].get<int>(), j[at + 2].get<int>()}; }

/// [axis, angle, tx, ty, tz, px, py, pz] from `at` (see Engine::transformJson).
std::optional<cc::Transform> transformOf(Json const& j, std::size_t at) {
    if (!j.is_array() || j.size() < at + 8) return std::nullopt;
    for (std::size_t i = at; i < at + 8; ++i)
        if (!j[i].is_number()) return std::nullopt;
    int axis = j[at].get<int>();
    if (axis < 0 || axis > 2) return std::nullopt;
    cc::Transform t;
    t.axis   = static_cast<cc::Axis>(axis);
    t.angle  = j[at + 1].get<double>();
    t.origin = {j[at + 2].get<double>(), j[at + 3].get<double>(), j[at + 4].get<double>()};
    t.offset = {};
    t.pivot  = {j[at + 5].get<double>(), j[at + 6].get<double>(), j[at + 7].get<double>()};
    return t;
}

void forget(std::uint64_t id, Known const& k) {
    for (auto const& b : k.blocks) {
        auto it = displays().find(b.display);
        if (it != displays().end() && it->second == id) displays().erase(it);
    }
}

void onBlocks(Json const& msg) {
    auto  id = msg.value("id", std::uint64_t{0});
    auto& k  = known()[id];
    forget(id, k);
    k.dim     = msg.value("dim", 0);
    k.axis    = msg.value("axis", 1);
    k.quarter = msg.value("quarter", 0);
    k.blocks.clear();
    for (auto const& b : msg.value("blocks", Json::array())) {
        if (!b.is_array() || b.size() != 5) continue;
        k.blocks.push_back({b[0].get<std::int64_t>(), posOf(b, 1), b[4].get<std::string>()});
        displays()[k.blocks.back().display] = id;
    }
    k.visuals.clear();
    for (auto const& v : msg.value("visuals", Json::array())) {
        if (!v.is_array() || v.size() != 4) continue;
        k.visuals[{v[0].get<int>(), v[1].get<int>(), v[2].get<int>()}] = v[3];
    }
    // the transform when the displays were spawned (motion messages carry the changes; a
    // contraption known already keeps interpolating from them)
    if (auto t = transformOf(msg.value("transform", Json()), 0); t && !k.motion.ready()) k.motion.reset(*t);
}

void onMotion(Json const& msg) {
    for (auto const& m : msg.value("m", Json::array())) {
        if (!m.is_array() || m.empty() || !m[0].is_number_unsigned()) continue;
        auto t = transformOf(m, 1);
        if (!t) continue;
        auto  id = m[0].get<std::uint64_t>();
        auto& k  = known()[id];
        k.motion.push(*t);
        if (!k.moved && k.motion.queued() > 0) {
            k.moved = true;
            log().info("contraption client: contraption #{} moves", id);
        }
    }
}

void onVisual(Json const& msg) {
    auto it = known().find(msg.value("id", std::uint64_t{0}));
    auto l  = msg.value("local", Json::array());
    if (it == known().end() || !l.is_array() || l.size() != 3) return;
    auto key = std::tuple{l[0].get<int>(), l[1].get<int>(), l[2].get<int>()};
    auto v   = msg.value("visual", Json());
    if (v.is_null()) it->second.visuals.erase(key);
    else it->second.visuals[key] = std::move(v);
}

void onRemove(std::uint64_t id) {
    auto it = known().find(id);
    if (it == known().end()) return;
    forget(id, it->second);
    known().erase(it);
}

void clearAll() {
    known().clear();
    displays().clear();
}

Level* clientLevel() {
    auto* ci = currentClient();
    return ci ? reinterpret_cast<Level*>(ci->getLevel()) : nullptr;
}

::Actor* displayActor(Level& level, std::int64_t display) {
    ActorUniqueID uid;
    uid.rawID = display;
    return level.fetchEntity(uid, false);
}

cc::Vec3d feetOf(cc::Transform const& t, cc::Pos const& local) {
    return t.toWorld(cc::Vec3d::center(local)) - cc::Vec3d{0, 0.5, 0};
}
Vec3 mcVec(cc::Vec3d const& v) { return Vec3{float(v.x), float(v.y), float(v.z)}; }

/// One client tick: every contraption takes its next transform and its displays move there.
void tick() {
    auto* level  = clientLevel();
    auto* player = currentPlayer();
    if (!level || !player || known().empty()) return;
    int dim = static_cast<int>(player->getDimensionId());
    for (auto& [id, k] : known()) {
        if (!k.motion.ready()) continue;
        k.motion.step();
        if (k.dim != dim) continue;
        auto const& prev = k.motion.previous();
        auto const& cur  = k.motion.current();
        for (auto const& b : k.blocks) {
            auto* actor = displayActor(*level, b.display);
            if (!actor) continue;
            // moveTo takes the feet (and keeps the bounding box with the entity, so it isn't
            // culled); the entity's position is its feet + the falling block's height offset
            Vec3 feet = mcVec(feetOf(cur, b.local));
            actor->moveTo(feet, actor->getRotation());
            auto& sv    = *actor->mBuiltInComponents->mStateVectorComponent;
            sv.mPosPrev = mcVec(feetOf(prev, b.local)) + (actor->getPosition() - feet);
        }
    }
}

double turnOf(Known const& k, float partialTick) {
    return k.motion.at(partialTick).angle - k.quarter * 90.0;
}

} // namespace

bool displayTurn(Actor const& actor, float partialTick, int& axis, float& degrees) {
    if (displays().empty()) return false;
    auto d = displays().find(actor.getOrCreateUniqueID().rawID);
    if (d == displays().end()) return false;
    auto k = known().find(d->second);
    if (k == known().end() || !k->second.motion.ready()) return false;
    axis    = static_cast<int>(k->second.motion.current().axis);
    degrees = static_cast<float>(std::remainder(turnOf(k->second, partialTick), 360.0));
    return true;
}

void forEachBlock(std::string const& blockId, float partialTicks, std::function<void(BlockView const&)> const& fn) {
    auto* level  = clientLevel();
    auto* player = currentPlayer();
    if (!level || !player || known().empty()) return;
    int dim = static_cast<int>(player->getDimensionId());
    for (auto const& [id, k] : known()) {
        if (k.dim != dim || !k.motion.ready()) continue;
        auto t = k.motion.at(partialTicks);
        for (auto const& b : k.blocks) {
            if (!blockId.empty() && b.id != blockId) continue;
            if (!displayActor(*level, b.display)) continue;
            BlockView v;
            v.contraption = id;
            v.local       = b.local;
            v.blockId     = b.id;
            v.center      = t.toWorld(cc::Vec3d::center(b.local));
            v.axis        = static_cast<int>(t.axis);
            v.angle       = t.angle;
            auto vis      = k.visuals.find({b.local.x, b.local.y, b.local.z});
            v.visual      = vis == k.visuals.end() ? nullptr : &vis->second;
            fn(v);
        }
    }
}

class ContraptionClientModule {
public:
    void install() {
        namespace net = feature::net;
        net::client::on("contraption.blocks", [](Json const& m) { onBlocks(m); });
        net::client::on("contraption.motion", [](Json const& m) { onMotion(m); });
        net::client::on("contraption.visual", [](Json const& m) { onVisual(m); });
        net::client::on("contraption.remove", [](Json const& m) { onRemove(m.value("id", std::uint64_t{0})); });
        net::client::on("contraption.reset", [](Json const&) { clearAll(); });
        auto& bus = ll::event::EventBus::getInstance();
        mExit     = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { clearAll(); });
        mTick     = bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { tick(); });
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        bus.removeListener(mExit);
        bus.removeListener(mTick);
    }

private:
    ll::event::ListenerPtr mExit, mTick;
};

} // namespace lv::client::contraption

LV_REGISTER_MODULE(lv::client::contraption::ContraptionClientModule);

#endif // LV_CLIENT
