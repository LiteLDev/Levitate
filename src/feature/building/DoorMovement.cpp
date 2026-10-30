// Sliding doors and train doors on contraptions (Create's SlidingDoorMovementBehaviour, U38).
//
// Owned by U38 (contraptions), not by the building unit: it only registers the movement behaviour
// of U10's door blocks. Each tick (also while the contraption stands still) a carried door asks
// its door control: the elevator contact the cabin stands at (U20, BlockData `data["pistons"]
// ["door"]`) or the station the train stopped at (U27, Station::door). The door opens when the
// control allows its side (all / north / east / south / west / none, Create's DoorControl) and
// the contraption doesn't move; it closes again as soon as it moves. Both halves change their
// carried states (`contraption::setBlockStates`); doors on other contraptions stay closed.

#include <cmath>
#include <optional>

#include "nlohmann/json.hpp"

#include "feature/actors/Actors.h"
#include "feature/common/BlockData.h"
#include "feature/contraption/ActorsExt.h"
#include "feature/contraption/Contraption.h"
#include "feature/trains/Trains.h"
#include "mod/Log.h"

namespace lv::feature::building::door_movement {

namespace con = ::lv::feature::contraption;
namespace cc  = ::lv::core::contraption;
namespace bd  = ::lv::feature::common::blockdata;
using Json    = nlohmann::json;

namespace {

constexpr char const* kDoors[]{
    "levitate:andesite_door",
    "levitate:brass_door",
    "levitate:copper_door",
    "levitate:train_door",
    "levitate:framed_glass_door",
};

// (the carried block id of the door at ctx.localPos)
std::string carriedId(con::MovementContext const& ctx) {
    if (auto const* b = con::actors_ext::carriedAt(ctx.contraption, ctx.localPos)) return b->id;
    return {};
}

std::int64_t intState(cc::States const& s, char const* key) {
    auto it = s.find(key);
    if (it == s.end()) return 0;
    auto const* v = std::get_if<std::int64_t>(&it->second);
    return v ? *v : 0;
}

/// The door control in effect (0 all, 1 north, 2 east, 3 south, 4 west, 5 none), nullopt: none.
std::optional<int> doorControl(con::Contraption const& c) {
    if (c.kind() == "elevator") {
        // Create's getElevatorDoorControl: the contact at the floor the cabin arrived at
        auto const* doc = bd::get(c.anchor());
        if (!doc || !doc->contains("pistons")) return std::nullopt;
        auto const& d = (*doc)["pistons"];
        if (!d.value("arrived", false)) return std::nullopt; // Create: only an arrived elevator opens
        auto col = d.value("col", Json());
        if (!col.is_object()) return std::nullopt;
        int  ey = static_cast<int>(std::floor(0.5 + d.value("ey", c.transform().origin.y))) + d.value("cy", 0);
        auto const* contact = bd::get({c.dimension(), col.value("x", 0), ey, col.value("z", 0)});
        if (!contact || !contact->contains("pistons")) return 0; // Create's default: all doors
        return std::clamp((*contact)["pistons"].value("door", 0), 0, 5);
    }
    // Create's getTrainStationDoorControl: the station the train stopped at
    if (auto mode = trains::stationDoorModeOf(c.id())) return static_cast<int>(*mode);
    return std::nullopt;
}

/// Create's getDoorFacing: the side of the contraption the door faces, in world directions.
cc::Dir doorFacing(con::MovementContext const& ctx) {
    auto facing = static_cast<cc::Dir>(std::clamp<std::int64_t>(intState(ctx.states, "levitate:facing"), 2, 5));
    auto axis   = cc::axisOf(facing);
    auto bounds = con::bounds(ctx.contraption.id());
    // the positive direction of its axis, turned around when the door lies on the negative side
    cc::Vec3d centre = bounds ? bounds->center() : cc::Vec3d{0.5, 0.5, 0.5};
    cc::Vec3d n      = cc::Vec3d::of(cc::offset(facing));
    cc::Vec3d diff   = cc::Vec3d::center(ctx.localPos) - n * 0.45 - centre;
    double    along  = axis == cc::Axis::X ? diff.x : axis == cc::Axis::Y ? diff.y : diff.z;
    cc::Vec3d dir    = axis == cc::Axis::X ? cc::Vec3d{1, 0, 0} : axis == cc::Axis::Y ? cc::Vec3d{0, 1, 0} : cc::Vec3d{0, 0, 1};
    if (along < 0) dir = dir * -1.0;
    auto w = ctx.contraption.transform().rotateVector(dir);
    if (std::abs(w.x) >= std::abs(w.z)) return w.x >= 0 ? cc::Dir::East : cc::Dir::West;
    return w.z >= 0 ? cc::Dir::South : cc::Dir::North;
}

bool controlMatches(int mode, cc::Dir side) {
    switch (mode) {
    case 0: return true;
    case 1: return side == cc::Dir::North;
    case 2: return side == cc::Dir::East;
    case 3: return side == cc::Dir::South;
    case 4: return side == cc::Dir::West;
    default: return false;
    }
}

/// Create's shouldOpen.
bool shouldOpen(con::MovementContext const& ctx) {
    if (actors::actorDisabled(ctx.contraption.id(), carriedId(ctx))) return false;
    bool elevator = ctx.contraption.kind() == "elevator";
    if (!elevator && ctx.motion.length() >= 1.0 / 128) return false;
    auto mode = doorControl(ctx.contraption);
    if (!mode) return false;
    return controlMatches(*mode, doorFacing(ctx));
}

void toggle(con::MovementContext& ctx, bool open) {
    int  value = open ? 1 : 0;
    bool upper = intState(ctx.states, "levitate:upper") != 0;
    con::setBlockStates(ctx.contraption.id(), ctx.localPos, {{"levitate:open", std::int64_t{value}}});
    cc::Pos other = ctx.localPos + cc::Pos{0, upper ? -1 : 1, 0};
    if (auto const* b = con::actors_ext::carriedAt(ctx.contraption, other); b && b->id == carriedId(ctx))
        con::setBlockStates(ctx.contraption.id(), other, {{"levitate:open", std::int64_t{value}}});
    if (open) con::actors_ext::playSound(ctx.contraption.dimension(), ctx.position, "open.iron_door", 0.125f, 1.f);
    log().info("contraption #{}: door at {} {} {} {}", ctx.contraption.id(), ctx.localPos.x, ctx.localPos.y, ctx.localPos.z, open ? "opened" : "closed");
}

void tick(con::MovementContext& ctx) {
    // both halves run: the lower half decides for the door (Create toggles both from either)
    if (intState(ctx.states, "levitate:upper") != 0) return;
    bool open = shouldOpen(ctx);
    // Create's shouldUpdate: not on the first movement, then only when the wish changes
    if (!ctx.data.contains("Seen")) {
        ctx.data["Seen"] = true;
        if (open) return;
    }
    bool changed = !ctx.data.contains("Open") || ctx.data["Open"].get<bool>() != open;
    ctx.data["Open"] = open;
    if (!changed) return;
    bool current = intState(ctx.states, "levitate:open") != 0;
    if (current != open) toggle(ctx, open);
}

struct Registrar {
    Registrar() {
        for (auto const* id : kDoors) {
            con::MovementBehaviour b;
            b.tick           = tick;
            b.tickWhileStill = true;
            con::registerActor(id, std::move(b));
        }
    }
} const gRegistrar;

} // namespace

} // namespace lv::feature::building::door_movement
