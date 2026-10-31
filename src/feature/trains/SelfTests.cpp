// Selftests of the trains unit (U27, testPos dx 2700..2799).
//
// Layout: an east-west line of track (x = base .. base + 79) with station A (marker at x + 20)
// and station B (marker at x + 60), both facing east; the assembly area of A runs west of it.

#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/tracks/Shape.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/tracks/SignalsExt.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TrainsExt.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "feature/contraption/U38Test.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace tn = lv::feature::trains;
namespace tr = lv::feature::tracks;
namespace cn = lv::feature::contraption;
namespace ct = lv::core::trains;
namespace sg = lv::feature::tracks::signals;

GlobalPos at(GlobalPos b, int dx, int dy = 0, int dz = 0) { return {b.dim, b.x + dx, b.y + dy, b.z + dz}; }

std::vector<std::pair<std::string, int>> xo() {
    int s = int(lv::core::tracks::Shape::XO);
    return {{std::string(tr::kShapeState), s % 16}, {std::string(tr::kShapeHiState), s / 16}, {std::string(tr::kTurnState), 0}};
}

/// Clears blocks (and carriages / stations) in a box without drops.
void clearBox(GlobalPos a, GlobalPos b) {
    auto* region = tn::regionOf(a.dim);
    auto const* air = lv::game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
            for (int z = std::min(a.z, b.z); z <= std::max(a.z, b.z); ++z) {
                BlockPos bp{x, y, z};
                if (!region->getBlock(bp).isAir()) lv::game::setBlock(*region, bp, *air);
            }
}

void removeTrainsNear(GlobalPos base) {
    std::vector<std::uint64_t> ids;
    for (auto const& [id, t] : tn::state().trains) {
        auto p = ct::positionOf(tr::railways(), t.body.front);
        if (!p || (std::abs(p->x - base.x - 40) < 80 && std::abs(p->z - base.z) < 8)) ids.push_back(id);
    }
    for (auto id : ids) tn::removeTrain(id);
}

struct Line {
    GlobalPos base, stationA, stationB, trackA, trackB;
};

/// Builds the line and both stations (A "U27 Station A" at x + 20, B at x + 60).
ll::coro::CoroTask<bool> buildLine(Context& ctx, Line& l, int dx, std::string const& suffix, int dz = 0) {
    l.base = ctx.testPos(dx, dz);
    if (!co_await ctx.ensureLoaded(l.base)) co_return false;
    if (!co_await ctx.ensureLoaded(at(l.base, 79))) co_return false;
    if (!co_await ctx.ensureLoaded(at(l.base, 40))) co_return false;
    clearBox(at(l.base, -1, 0, -2), at(l.base, 80, 4, 2));
    co_await ctx.ticks(2);
    for (int x = 0; x < 80; ++x) ctx.setBlock(at(l.base, x), std::string(tr::kTrackId), xo());
    co_await ctx.ticks(3);
    l.trackA   = at(l.base, 20);
    l.trackB   = at(l.base, 60);
    l.stationA = at(l.base, 20, 0, 1);
    l.stationB = at(l.base, 60, 0, 1);
    ctx.setBlock(l.stationA, std::string(tn::kStation));
    ctx.setBlock(l.stationB, std::string(tn::kStation));
    co_await ctx.ticks(2);
    std::string err;
    bool        okA = tn::createStation(l.stationA, l.trackA, 1, 0, &err);
    bool        okB = tn::createStation(l.stationB, l.trackB, 1, 0, &err);
    if (!okA || !okB) {
        ctx.info("station creation failed: " + err);
        co_return false;
    }
    tn::stationAt(l.stationA)->name = "U27 Station A" + suffix;
    tn::stationAt(l.stationB)->name = "U27 Station B" + suffix;
    tn::refreshStationPoints();
    co_return true;
}

void teardown(Line const& l) {
    removeTrainsNear(l.base);
    cn::clearGlue(l.base.dim, {l.base.x + 40, l.base.y, l.base.z}, 60);
    tn::removeStation(l.stationA);
    tn::removeStation(l.stationB);
    clearBox(at(l.base, -1, 0, -2), at(l.base, 80, 4, 2));
}

/// A one-bogey carriage at A's station marker: bogey, forward controls above it and (double
/// ended) backward controls on top, glued together.
std::optional<std::uint64_t> assembleTrain(Context& ctx, Line const& l, bool doubleEnded) {
    auto* s = tn::stationAt(l.stationA);
    if (!s) return std::nullopt;
    tn::setAssembling(*s, true, nullptr);
    auto track0 = at(l.trackA, -1);
    auto msg    = tn::useCasingOnTrack(nullptr, track0, true);
    ctx.check(msg.starts_with("+"), "train casing creates a bogey (" + msg + ")");
    auto bogey = at(track0, 0, 1);
    // controls: forward ones face the assembly direction (west, levitate:facing 4)
    ctx.setBlock(at(bogey, 0, 1), std::string(tn::kControls), {{"levitate:facing", 4}});
    if (doubleEnded) {
        ctx.setBlock(at(bogey, 0, 2), std::string(tn::kControls), {{"levitate:facing", 5}});
        cn::addGlue(l.base.dim, {{bogey.x, bogey.y, bogey.z}, {bogey.x, bogey.y + 2, bogey.z}});
    }
    auto id = tn::assemble(*s, nullptr);
    if (!id) ctx.info("assembly error: " + s->lastError.dump());
    return id;
}

/// Every carried block's display entity stands at the block's world position.
bool displaysFollow(cn::Contraption const& con) {
    auto level = ll::service::getLevel();
    if (!level) return false;
    for (auto const& b : con.blocks()) {
        ActorUniqueID uid;
        uid.rawID = b.display;
        auto* a   = level->fetchEntity(uid, false);
        if (!a) return false;
        auto   w = con.toWorld(lv::core::contraption::Vec3d::center(b.local));
        auto   f = a->getPosition();
        double d = std::abs(f.x - w.x) + std::abs(f.z - w.z);
        if (d > 1.0) return false;
    }
    return true;
}

ct::Schedule twoStops(std::string const& suffix, int waitTicks) {
    ct::Schedule sch;
    for (auto const* name : {"U27 Station B", "U27 Station A"}) {
        auto e = ct::Entry::makeDefault();
        e.instruction.data["Text"]          = std::string(name) + suffix;
        e.conditions[0][0].data["Value"]    = waitTicks;
        e.conditions[0][0].data["TimeUnit"] = 0;
        sch.entries.push_back(e);
    }
    return sch;
}

// U38 (R146 / R106 / R5): relocating a train with the wrench puts it on another stretch of track;
// the station it stands at gives its doors' control; removing it places nothing
LV_REGISTER_SELFTEST("trains.relocate", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, 3830, " relocate", ::lv::feature::contraption::kU38Dz), "line and stations built");
    auto id = assembleTrain(ctx, l, false);
    ctx.require(id.has_value(), "train assembled");
    co_await ctx.ticks(5);
    auto* t = tn::findTrain(*id);
    ctx.require(t && !t->carriages.empty(), "train with a carriage");
    auto cid = t->carriages[0].contraption;
    if (!t->currentStation.empty()) {
        auto mode = tn::stationDoorModeOf(cid);
        ctx.check(mode && *mode == tn::DoorMode::All, "the station's door control applies to the carriage");
    }
    auto before = cn::find(cid)->transform().toWorld({0.5, 0.5, 0.5});
    // Create's TrainRelocator: 12 blocks further along the line, facing the same way
    lv::core::trains::Vec3d point{l.base.x + 42.5, double(l.base.y), l.base.z + 0.5};
    auto body = tn::bodyAt(*t, point, {1, 0, 0}, 1.0);
    ctx.require(body.has_value(), "the train fits there");
    tn::relocate(*t, std::move(*body));
    co_await ctx.ticks(3);
    t = tn::findTrain(*id);
    ctx.require(t && cn::find(cid), "train still there");
    auto after = cn::find(cid)->transform().toWorld({0.5, 0.5, 0.5});
    ctx.check(std::abs(after.x - before.x) > 5 && std::abs(after.z - before.z) < 0.5, "the carriage stands at the new place: moved " + std::to_string(after.x - before.x));
    ctx.check(!t->derailed && std::abs(t->speed) < 1e-9, "on track and stopped");
    // off the track: it doesn't fit
    lv::core::trains::Vec3d away{l.base.x + 42.5, double(l.base.y), l.base.z + 9.5};
    ctx.check(!tn::bodyAt(*t, away, {1, 0, 0}, 1.0).has_value(), "no track there: refused");
    // Create's train remove: the carriages vanish, nothing is placed
    tn::removeTrain(*id);
    co_await ctx.ticks(2);
    ctx.check(!cn::find(cid), "the carriage is gone");
    bool placed = false;
    for (int x = 0; x < 80; ++x)
        for (int y = 1; y <= 3; ++y) placed = placed || tn::isBogey(tn::idAt(at(l.base, x, y)));
    ctx.check(!placed, "no bogey was placed back");
    teardown(l);
});
} // namespace

LV_REGISTER_SELFTEST("trains.blocks", [](Context& ctx) {
    for (auto id : {tn::kStation, tn::kSmallBogey, tn::kLargeBogey, tn::kControls})
        ctx.check(lv::game::blockWith(id) != nullptr, std::string(id) + " is registered");
    auto item = tn::makeScheduleItem(twoStops("", 20));
    ctx.require(!item.isNull(), "the schedule item exists");
    auto back = tn::scheduleOf(item);
    ctx.require(back.has_value(), "the schedule reads back from the item");
    ctx.check(back->entries.size() == 2, "both entries are stored");
    ctx.check(back->entries[0].instruction.data.value("Text", std::string()) == "U27 Station B", "the destination is stored");
});

LV_REGISTER_SELFTEST("trains.station_and_assembly_rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, 2700, " rules"), "line and stations built");
    auto* s = tn::stationAt(l.stationA);
    ctx.require(s && s->point().has_value(), "station A is an edge point on the graph");
    ctx.check(tn::bogeysOf(*s).empty(), "no bogeys yet");
    // outside assembly mode the track isn't an assembly area
    ctx.check(tn::useCasingOnTrack(nullptr, at(l.trackA, -1), true).empty(), "no assembly area while not assembling");
    tn::setAssembling(*s, true, nullptr);
    ctx.check(tn::intStateAt(l.stationA, tn::kAssemblingState).value_or(0) == 1, "the station block shows assembly mode");
    ctx.check(!tn::assemble(*s, nullptr), "assembly without bogeys fails");
    ctx.check(s->lastError.value("key", std::string()) == "create.train_assembly.no_bogeys", "error: no bogeys");
    // a bogey 2 blocks in is not allowed, one at the marker is; clicking next to it cycles its size
    ctx.check(tn::useCasingOnTrack(nullptr, at(l.trackA, -3), true) == "!", "no bogey 2 blocks in");
    ctx.check(tn::useCasingOnTrack(nullptr, at(l.trackA, -2), true).starts_with("+") == false, "bogey placement at 1 is refused");
    auto first = tn::useCasingOnTrack(nullptr, at(l.trackA, -1), true);
    ctx.check(first.starts_with("+"), "bogey created at the station marker");
    ctx.check(tn::idAt(at(l.trackA, -1, 1)) == tn::kSmallBogey, "a small bogey sits on the track");
    tn::useCasingOnTrack(nullptr, at(l.trackA, -1), true);
    ctx.check(tn::idAt(at(l.trackA, -1, 1)) == tn::kLargeBogey, "clicking again makes it large");
    tn::useCasingOnTrack(nullptr, at(l.trackA, -1), true);
    ctx.check(tn::bogeysOf(*s).size() == 1, "one bogey on the assembly track");
    // nothing attached
    ctx.check(!tn::assemble(*s, nullptr), "a lone bogey doesn't assemble");
    ctx.check(s->lastError.value("key", std::string()) == "create.train_assembly.nothing_attached", "error: nothing attached");
    // sideways controls
    ctx.setBlock(at(l.trackA, -1, 2), std::string(tn::kControls), {{"levitate:facing", 2}});
    ctx.check(!tn::assemble(*s, nullptr), "sideways controls don't assemble");
    ctx.check(s->lastError.value("key", std::string()) == "create.train_assembly.sideways_controls", "error: sideways controls");
    // backwards only
    ctx.setBlock(at(l.trackA, -1, 2), std::string(tn::kControls), {{"levitate:facing", 5}});
    ctx.check(!tn::assemble(*s, nullptr), "backward controls alone don't assemble");
    ctx.check(s->lastError.value("key", std::string()) == "create.train_assembly.no_controls", "error: no controls");
    tn::setAssembling(*s, false, nullptr);
    teardown(l);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("trains.schedule_round_trip", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, 2700, ""), "line and stations built");
    auto id = assembleTrain(ctx, l, true);
    ctx.require(id.has_value(), "the train assembles");
    auto* t = tn::findTrain(*id);
    ctx.require(t != nullptr, "the train exists");
    ctx.check(t->carriages.size() == 1, "one carriage");
    ctx.check(t->doubleEnded, "controls both ways make it double ended");
    ctx.check(cn::find(t->carriages[0].contraption) != nullptr, "the carriage is a contraption");
    ctx.check(tn::idAt(at(l.trackA, -1, 1)) != tn::kSmallBogey, "the bogey left the world");
    ctx.check(tn::trainAt(*tn::stationAt(l.stationA)) == t, "the train waits at station A");
    auto startX = cn::find(t->carriages[0].contraption)->transform().offset.x;

    t->testConductor = true;
    t->runtime.setSchedule(twoStops("", 10), false);
    bool atB = false, backAtA = false;
    for (int i = 0; i < 1500 && !(atB && backAtA); ++i) {
        co_await ctx.ticks(1);
        t = tn::findTrain(*id);
        if (!t) break;
        if (!atB && t->currentStation == l.stationB.key()) {
            atB = true;
            ctx.info("arrived at B after " + std::to_string(i) + " ticks");
            auto* con = cn::find(t->carriages[0].contraption);
            ctx.check(con && std::abs(con->transform().offset.x - startX - 40) < 0.6, "the carriage moved 40 blocks east");
            co_await ctx.ticks(2);
            con = cn::find(t->carriages[0].contraption);
            if (con) ctx.check(displaysFollow(*con), "the carriage's display entities are at its blocks");
        }
        if (atB && t->currentStation == l.stationA.key()) backAtA = true;
    }
    ctx.check(atB, "the schedule drove the train to station B");
    ctx.check(backAtA, "and back to station A (reversing, double ended)");
    t = tn::findTrain(*id);
    ctx.require(t != nullptr, "the train still exists");
    ctx.check(!tn::canDisassemble(*tn::stationAt(l.stationA)), "reversed in, its front isn't at A's marker: no disassembly");
    // one more trip forward to B, then disassemble there
    ct::Schedule once;
    auto         e = ct::Entry::makeDefault();
    e.instruction.data["Text"] = "U27 Station B";
    once.entries.push_back(e);
    once.cyclic = false;
    t->runtime.setSchedule(once, false);
    for (int i = 0; i < 1500; ++i) {
        co_await ctx.ticks(1);
        t = tn::findTrain(*id);
        if (!t || (t->currentStation == l.stationB.key() && t->runtime.completed)) break;
    }
    ctx.require(t && t->currentStation == l.stationB.key(), "the train is back at B");
    std::string name = t->name;
    ctx.check(tn::canDisassemble(*tn::stationAt(l.stationB)), "the train can be disassembled at B");
    ctx.check(tn::disassemble(*tn::stationAt(l.stationB), nullptr), "disassembly starts");
    co_await ctx.ticks(6);
    ctx.check(!tn::findTrain(*id), "the train is gone");
    ctx.check(tn::idAt(at(l.trackB, -1, 1)) == tn::kSmallBogey, "the bogey stands at B's station marker");
    ctx.check(tn::idAt(at(l.trackB, -1, 2)) == tn::kControls, "the controls are back on it");
    ctx.check(tn::idAt(at(l.trackB, -1, 3)) == tn::kControls, "and the second controls on top");
    ctx.check(tn::stationAt(l.stationB)->lastTrainName == name, "station B remembers the train's name");
    teardown(l);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("trains.manual_controls", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, 2700, " manual"), "line and stations built");
    auto id = assembleTrain(ctx, l, false);
    ctx.require(id.has_value(), "the train assembles");
    auto& player = ctx.player();
    auto* t      = tn::findTrain(*id);
    // stand next to the controls (a driver must stay within 8 blocks)
    player.teleport(Vec3{float(l.trackA.x - 1) + .5f, float(l.trackA.y + 2), float(l.trackA.z) + 2.5f}, player.getDimensionId());
    co_await ctx.ticks(2);
    t = tn::findTrain(*id);
    tn::startControlling(player, *t, 0, t->carriages[0].controls[0].first, 1);
    ctx.check(t->controller == tn::uuidOf(player), "the player controls the train");
    double startX = ct::positionOf(tr::railways(), t->body.front)->x;
    for (int i = 0; i < 60; ++i) {
        tn::setInput(player, 1, 0, false, false);
        co_await ctx.ticks(1);
    }
    t = tn::findTrain(*id);
    ctx.require(t != nullptr, "the train exists");
    double x = ct::positionOf(tr::railways(), t->body.front)->x;
    ctx.check(t->speed > 0.2, "holding forward accelerates (" + std::to_string(t->speed) + ")");
    ctx.check(t->speed <= 1.4 * 0.75 + 1e-9, "manual speed is capped by manualTrainSpeedModifier");
    ctx.check(x > startX + 5, "the train moved east");
    ctx.check(t->currentStation.empty(), "it left the station");
    // braking: backwards brakes at twice the acceleration
    double before = t->speed;
    tn::setInput(player, -1, 0, false, false);
    co_await ctx.ticks(1);
    t = tn::findTrain(*id);
    ctx.check(t && t->speed < before, "pulling back brakes");
    tn::setInput(player, 0, 0, false, true);
    co_await ctx.ticks(1);
    t = tn::findTrain(*id);
    ctx.check(t && t->controller.empty(), "sneaking stops controlling");
    // coasting stops it
    for (int i = 0; i < 200 && t && t->speed > 0; ++i) {
        co_await ctx.ticks(1);
        t = tn::findTrain(*id);
    }
    ctx.check(t && t->speed == 0, "an unguided train coasts to a stop");
    teardown(l);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("trains.derail_and_collide", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, 2700, " crash"), "line and stations built");
    auto first = assembleTrain(ctx, l, false);
    ctx.require(first.has_value(), "the first train assembles");
    auto* t1 = tn::findTrain(*first);
    // park it 12 blocks east of A
    t1->body.move(tr::railways(), 12, {});
    t1->currentStation.clear();
    tn::placeCarriages(*t1);
    co_await ctx.ticks(3);
    auto second = assembleTrain(ctx, l, false);
    ctx.require(second.has_value(), "a second train assembles behind it");
    auto* t2 = tn::findTrain(*second);
    t2->currentStation.clear();
    t2->speed = 0.6;
    bool crashed = false;
    for (int i = 0; i < 60 && !crashed; ++i) {
        co_await ctx.ticks(1);
        t1 = tn::findTrain(*first);
        t2 = tn::findTrain(*second);
        crashed = t1 && t2 && t1->derailed && t2->derailed;
    }
    ctx.check(crashed, "running into the other train derails both");
    ctx.check(t2 && t2->speed <= 0, "the moving train bounces back");
    ctx.check(tn::idAt(at(l.base, 30)) == tr::kTrackId, "the collision broke no blocks");
    tn::removeTrain(*second);
    // the other: the track beneath it disappears
    tn::removeTrain(*first);
    co_await ctx.ticks(3);
    auto third = assembleTrain(ctx, l, false);
    ctx.require(third.has_value(), "a third train assembles");
    auto* t3 = tn::findTrain(*third);
    // remove the track under its bogey
    clearBox(at(l.trackA, -1), at(l.trackA, 0));
    co_await ctx.ticks(4);
    t3 = tn::findTrain(*third);
    ctx.check(t3 && t3->derailed, "removing the track beneath it derails the train");
    teardown(l);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("trains.ext_and_commands", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, 2700, " ext"), "line and stations built");
    auto id = assembleTrain(ctx, l, false);
    ctx.require(id.has_value(), "the train assembles");
    auto infos = tr::trains::all();
    bool found = false;
    for (auto const& i : infos) found = found || (i.id == *id && i.station == "U27 Station A ext");
    ctx.check(found, "TrainsExt lists the train at its station");
    bool listed = false;
    for (auto const& s : tr::trains::stations()) listed = listed || (s.name == "U27 Station A ext" && !s.presentTrain.empty());
    ctx.check(listed, "TrainsExt lists station A with its train");
    // an edge point ahead: the train entering it is reported (TrainsExt.h)
    auto pt = *tn::stationAt(l.stationA)->point();
    lv::core::tracks::EdgePoint p;
    p.id       = "u27test:point";
    p.type     = "levitate:u27test";
    p.node1    = pt.from;
    p.node2    = pt.to;
    p.position = std::min(pt.pos + 4, ct::edgeLength(tr::railways(), pt) - 1);
    ctx.require(tr::railways().addPoint(p), "the test edge point is on the graph");
    static int  entered    = 0;
    static bool registered = false;
    entered                = 0;
    if (!registered) {
        registered = true;
        tr::trains::onPointEntered([](std::uint64_t, lv::core::tracks::EdgePoint const& e) {
            if (e.id == "u27test:point") ++entered;
        });
    }
    auto* t = tn::findTrain(*id);
    t->testConductor = true;
    ct::Schedule s;
    auto         e = ct::Entry::makeDefault();
    e.instruction.data["Text"] = "U27 Station B ext";
    s.entries.push_back(e);
    t->runtime.setSchedule(s, false);
    for (int i = 0; i < 900 && t && t->currentStation != l.stationB.key(); ++i) {
        co_await ctx.ticks(1);
        t = tn::findTrain(*id);
    }
    ctx.check(entered >= 1, "the train entering the edge point is reported");
    ctx.check(t && t->currentStation == l.stationB.key(), "the train arrives at B");
    tr::railways().removePoint("u27test:point");
    tn::removeTrain(*id);
    co_await ctx.ticks(2);
    ctx.check(!tn::findTrain(*id), "removing a train (killtrain) works");
    teardown(l);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("trains.signal", [](Context& ctx) -> ll::coro::CoroTask<> {
    // two trains on one line split by a signal (U28): the second waits at red until the first
    // leaves the section
    Line l;
    ctx.require(co_await buildLine(ctx, l, 2700, " signal"), "line and stations built");
    auto& sp   = static_cast<SimulatedPlayer&>(ctx.player());
    auto  item = lv::feature::common::items::make(sg::kSignalId, 4);
    ctx.require(item.has_value(), "the signal item exists");
    ctx.give(sp, *item, 0);
    auto look = [&](GlobalPos const& p) { sp.simulateLookAt(Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, sim::LookDuration::Continuous); };
    auto use  = [&](GlobalPos const& p) {
        return sp.simulateUseItemInSlotOnBlock(0, BlockPos{p.x, p.y, p.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.0f, 0.5f});
    };
    GlobalPos track = at(l.base, 40), signal = at(l.base, 40, 0, 2);
    sp.teleport(Vec3{track.x - 2.5f, float(track.y), track.z + 2.5f}, DimensionType(l.base.dim));
    co_await ctx.ticks(2);
    look(at(l.base, 46));
    co_await ctx.ticks(10);
    use(track);
    co_await ctx.ticks(10);
    ctx.cleanupBlock(signal);
    use(at(signal, 0, -1));
    co_await ctx.ticks(5);
    ctx.require(tn::idAt(signal) == sg::kSignalId, "the signal is placed next to the line");

    auto first = assembleTrain(ctx, l, false);
    ctx.require(first.has_value(), "the first train assembles");
    auto* t1 = tn::findTrain(*first);
    // park it past the signal, inside the next section
    t1->body.move(tr::railways(), 26, {});
    t1->currentStation.clear();
    tn::placeCarriages(*t1);
    co_await ctx.ticks(17); // Create's SignalBlockEntity: a green signal turns red 15 ticks later
    ctx.check(sg::signalState(signal) == lv::core::signals::SignalState::Red, "the occupied section shows red");

    auto second = assembleTrain(ctx, l, false);
    ctx.require(second.has_value(), "the second train assembles");
    auto* t2 = tn::findTrain(*second);
    t2->testConductor = true;
    ct::Schedule s;
    auto         e = ct::Entry::makeDefault();
    e.instruction.data["Text"] = "U27 Station B signal";
    s.entries.push_back(e);
    t2->runtime.setSchedule(s, false);
    for (int i = 0; i < 400; ++i) co_await ctx.ticks(1);
    t2 = tn::findTrain(*second);
    ctx.require(t2 != nullptr, "the second train exists");
    double x2 = ct::positionOf(tr::railways(), t2->body.front)->x;
    ctx.info("second train waits at x " + std::to_string(x2 - l.base.x) + " speed " + std::to_string(t2->speed));
    ctx.check(t2->nav.has_value() && std::abs(t2->speed) < 1e-6, "the second train waits");
    ctx.check(x2 < track.x + 0.5 && x2 > track.x - 3, "in front of the red signal");
    // the first train leaves: green, the second goes on to B
    tn::removeTrain(*first);
    for (int i = 0; i < 900 && t2 && t2->currentStation != l.stationB.key(); ++i) {
        co_await ctx.ticks(1);
        t2 = tn::findTrain(*second);
    }
    ctx.check(t2 && t2->currentStation == l.stationB.key(), "after the first train left, the second arrives at B");
    tn::removeTrain(*second);
    teardown(l);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("trains.curve", [](Context& ctx) -> ll::coro::CoroTask<> {
    // a line east, a 90° curve (made with the track item, as a player) and a line north
    GlobalPos base = ctx.testPos(2740, 12);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(at(base, 29, 0, -30)), "test area loaded (north)");
    clearBox(at(base, -1, 0, -32), at(base, 32, 4, 2));
    co_await ctx.ticks(2);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    auto tracksItem = lv::feature::common::items::make(tr::kTrackId, 64);
    ctx.require(tracksItem.has_value(), "track item");
    ctx.give(sp, *tracksItem, 0);
    GlobalPos a = at(base, 20), b = at(base, 29, 0, -9);
    ctx.setBlock(a, std::string(tr::kTrackId), xo());
    auto zo = xo();
    zo[0].second = int(lv::core::tracks::Shape::ZO);
    ctx.setBlock(b, std::string(tr::kTrackId), zo);
    auto look = [&](GlobalPos const& p) {
        sp.simulateLookAt(Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, sim::LookDuration::Continuous);
    };
    auto use = [&](GlobalPos const& p) {
        return sp.simulateUseItemInSlotOnBlock(0, BlockPos{p.x, p.y, p.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.0f, 0.5f});
    };
    sp.teleport(Vec3{a.x - 1.5f, float(a.y), a.z + .5f}, DimensionType(base.dim));
    co_await ctx.ticks(2);
    look(at(a, 6));
    co_await ctx.ticks(2);
    use(a);
    co_await ctx.ticks(6);
    sp.teleport(Vec3{b.x + .5f, float(b.y), b.z + 3.5f}, DimensionType(base.dim));
    co_await ctx.ticks(2);
    look(b);
    co_await ctx.ticks(2);
    use(b);
    co_await ctx.ticks(4);
    for (int x = 0; x < 20; ++x) ctx.setBlock(at(base, x), std::string(tr::kTrackId), xo());
    for (int z = -10; z >= -30; --z) ctx.setBlock(at(base, 29, 0, z), std::string(tr::kTrackId), zo);
    co_await ctx.ticks(4);

    Line l;
    l.base     = base;
    l.trackA   = at(base, 8);
    l.stationA = at(base, 8, 0, 1);
    l.trackB   = at(base, 29, 0, -24);
    l.stationB = at(base, 30, 0, -24);
    ctx.setBlock(l.stationA, std::string(tn::kStation));
    ctx.setBlock(l.stationB, std::string(tn::kStation));
    co_await ctx.ticks(2);
    std::string err;
    ctx.require(tn::createStation(l.stationA, l.trackA, 1, 0, &err), "station A: " + err);
    ctx.require(tn::createStation(l.stationB, l.trackB, 0, -1, &err), "station B: " + err);
    tn::stationAt(l.stationA)->name = "U27 Curve A";
    tn::stationAt(l.stationB)->name = "U27 Curve B";
    tn::refreshStationPoints();
    auto id = assembleTrain(ctx, l, false);
    ctx.require(id.has_value(), "the train assembles");
    auto* t = tn::findTrain(*id);
    t->testConductor = true;
    ct::Schedule sch;
    auto         e = ct::Entry::makeDefault();
    e.instruction.data["Text"] = "U27 Curve B";
    sch.entries.push_back(e);
    sch.cyclic = false;
    t->runtime.setSchedule(sch, false);
    bool   arrived = false, sawTurn = false, followed = true;
    double maxAngle = 0;
    for (int i = 0; i < 1200 && !arrived; ++i) {
        co_await ctx.ticks(1);
        t = tn::findTrain(*id);
        if (!t) break;
        auto* con = cn::find(t->carriages[0].contraption);
        if (!con) break;
        double angle = std::abs(con->transform().angle);
        maxAngle     = std::max(maxAngle, angle);
        if (angle > 20 && angle < 70) {
            sawTurn = true;
            if (i % 5 == 0) followed = followed && displaysFollow(*con);
        }
        arrived = t->currentStation == l.stationB.key();
    }
    ctx.check(sawTurn, "the carriage turned through the curve");
    ctx.check(followed, "display entities follow the carriage through the curve");
    ctx.check(arrived, "the train arrived at B on the north branch");
    ctx.check(std::abs(maxAngle - 90) < 3, "the carriage ends up turned 90° (" + std::to_string(maxAngle) + ")");
    co_await ctx.ticks(3);
    if (t) {
        if (auto* con = cn::find(t->carriages[0].contraption)) ctx.check(displaysFollow(*con), "display entities stand at the carriage at B");
    }
    removeTrainsNear(base);
    tn::removeTrain(*id);
    tn::removeStation(l.stationA);
    tn::removeStation(l.stationB);
    clearBox(at(base, -1, 0, -32), at(base, 32, 4, 2));
    co_await ctx.ticks(2);
});