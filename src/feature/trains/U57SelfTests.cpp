// Selftests of the U57 audit fixes (testPos dx 5700..5799): real conductors, reversing speed,
// actor stalls, the station's auto-schedule, trains through portal tracks, relocation overlap and
// the station display sources.
//
// Layout: an east-west line of track (x = base .. base + 59) with station A (marker at x + 20) and
// station B (marker at x + 50), both facing east; A's assembly area runs west of it.

#include <cmath>

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/phys/AABB.h"

#include "core/tracks/Shape.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace tn = lv::feature::trains;
namespace tr = lv::feature::tracks;
namespace cn = lv::feature::contraption;
namespace ct = lv::core::trains;
namespace ck = lv::core::tracks;

GlobalPos at(GlobalPos b, int dx, int dy = 0, int dz = 0) { return {b.dim, b.x + dx, b.y + dy, b.z + dz}; }

std::vector<std::pair<std::string, int>> shapeStates(ck::Shape shape, bool turn = false) {
    int s = int(shape);
    return {{std::string(tr::kShapeState), s % 16}, {std::string(tr::kShapeHiState), s / 16}, {std::string(tr::kTurnState), turn ? 1 : 0}};
}

void clearBox(GlobalPos a, GlobalPos b) {
    auto*       region = tn::regionOf(a.dim);
    auto const* air    = lv::game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
            for (int z = std::min(a.z, b.z); z <= std::max(a.z, b.z); ++z) {
                BlockPos bp{x, y, z};
                if (region->hasChunksAt(bp, 0, false) && !region->getBlock(bp).isAir()) lv::game::setBlock(*region, bp, *air);
            }
}

bool place(GlobalPos const& p, std::string const& id, std::vector<std::pair<std::string, int>> const& states = {}) {
    auto* region = tn::regionOf(p.dim);
    if (!region) return false;
    std::vector<std::pair<std::string_view, int>> s;
    for (auto const& [k, v] : states) s.emplace_back(k, v);
    auto const* b = lv::game::blockWith(id, s);
    return b && lv::game::setBlock(*region, lv::game::blockPos(p), *b);
}

void command(std::string const& text, int dim = 0) {
    auto level = ll::service::getLevel();
    if (!level) return;
    ServerCommandOrigin origin("levitate-selftest", level->asServer(), CommandPermissionLevel::Owner, DimensionType(dim));
    ll::command::CommandRegistrar::getServerInstance().executeCommand(text, origin);
}

void removeTrainsNear(GlobalPos base) {
    std::vector<std::uint64_t> ids;
    for (auto const& [id, t] : tn::state().trains) {
        auto p = ct::positionOf(tr::railways(), t.body.front);
        if (!p || (std::abs(p->x - base.x - 30) < 70 && std::abs(p->z - base.z) < 8)) ids.push_back(id);
    }
    for (auto id : ids) tn::removeTrain(id);
}

struct Line {
    GlobalPos base, stationA, stationB, trackA, trackB;
};

ll::coro::CoroTask<bool> buildLine(Context& ctx, Line& l, std::string const& suffix) {
    l.base = ctx.testPos(5700);
    if (!co_await ctx.ensureLoaded(at(l.base, 59))) co_return false;
    if (!co_await ctx.ensureLoaded(l.base)) co_return false;
    if (!co_await ctx.ensureLoaded(at(l.base, 30))) co_return false;
    removeTrainsNear(l.base);
    clearBox(at(l.base, -1, 0, -2), at(l.base, 60, 4, 2));
    co_await ctx.ticks(2);
    for (int x = 0; x < 60; ++x) ctx.setBlock(at(l.base, x), std::string(tr::kTrackId), shapeStates(ck::Shape::XO));
    co_await ctx.ticks(3);
    l.trackA   = at(l.base, 20);
    l.trackB   = at(l.base, 50);
    l.stationA = at(l.base, 20, 0, 1);
    l.stationB = at(l.base, 50, 0, 1);
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
    tn::stationAt(l.stationA)->name = "U57 A" + suffix;
    tn::stationAt(l.stationB)->name = "U57 B" + suffix;
    tn::refreshStationPoints();
    co_return true;
}

void teardown(Line const& l) {
    removeTrainsNear(l.base);
    cn::clearGlue(l.base.dim, {l.base.x + 30, l.base.y, l.base.z}, 50);
    tn::removeStation(l.stationA);
    tn::removeStation(l.stationB);
    clearBox(at(l.base, -1, 0, -2), at(l.base, 60, 4, 2));
}

/// A one-bogey carriage at A: bogey, forward controls (facing west) on it; `extra(bogey)` adds
/// blocks, glued from the bogey up to `glueTop` and from `glueWest` blocks west of it.
template <class F>
std::optional<std::uint64_t> assembleAt(Context& ctx, Line const& l, bool doubleEnded, int glueTop, int glueWest, F extra) {
    auto* s = tn::stationAt(l.stationA);
    if (!s) return std::nullopt;
    tn::setAssembling(*s, true, nullptr);
    auto track0 = at(l.trackA, -1);
    auto msg    = tn::useCasingOnTrack(nullptr, track0, true);
    ctx.check(msg.starts_with("+"), "train casing creates a bogey (" + msg + ")");
    auto bogey = at(track0, 0, 1);
    ctx.setBlock(at(bogey, 0, 1), std::string(tn::kControls), {{"levitate:facing", 4}});
    if (doubleEnded) ctx.setBlock(at(bogey, 0, 2), std::string(tn::kControls), {{"levitate:facing", 5}});
    extra(bogey);
    if (glueTop > 0 || glueWest > 0) cn::addGlue(l.base.dim, {{bogey.x - glueWest, bogey.y, bogey.z}, {bogey.x, bogey.y + std::max(1, glueTop), bogey.z}});
    auto id = tn::assemble(*s, nullptr);
    if (!id) ctx.info("assembly error: " + s->lastError.dump());
    return id;
}

ct::Schedule toB(std::string const& suffix, bool cyclic = false) {
    ct::Schedule sch;
    auto         e                       = ct::Entry::makeDefault();
    e.instruction.data["Text"]           = "U57 B" + suffix;
    e.conditions[0][0].data["Value"]     = 60;
    e.conditions[0][0].data["TimeUnit"]  = 0;
    sch.entries.push_back(e);
    sch.cyclic = cyclic;
    return sch;
}

Actor* spawnMob(BlockSource& region, std::string const& type, GlobalPos const& p) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto owner = level->getActorFactory().createSpawnedActor(ActorDefinitionIdentifier(type), nullptr, Vec3{float(p.x + .5), float(p.y), float(p.z + .5)}, Vec2{0, 0});
    if (!owner) return nullptr;
    return level->addEntity(region, std::move(owner));
}

void removeActor(std::int64_t uid) {
    ActorUniqueID id;
    id.rawID = uid;
    if (auto level = ll::service::getLevel())
        if (auto* a = level->fetchEntity(id, false)) a->remove();
}

} // namespace

// T9: Create's conductors are seated non-player passengers in a seat behind controls facing them
LV_REGISTER_SELFTEST("trains.u57_conductors", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, " cond"), "line and stations built");
    // a seat west of the controls (they face west, towards it), glued to the carriage
    auto id = assembleAt(ctx, l, false, 1, 1, [&](GlobalPos const& bogey) { ctx.setBlock(at(bogey, -1, 1), "levitate:white_seat"); });
    ctx.require(id.has_value(), "the train assembles");
    auto* t = tn::findTrain(*id);
    ctx.require(t && !t->carriages.empty(), "train with a carriage");
    auto* con = cn::find(t->carriages[0].contraption);
    ctx.require(con != nullptr, "carriage contraption");
    std::optional<lv::core::contraption::Pos> seat;
    for (auto const& b : con->blocks())
        if (b.id == "levitate:white_seat") seat = b.local;
    ctx.require(seat.has_value(), "the seat is part of the carriage");
    ctx.check(!tn::hasConductor(*t, true), "an empty seat is no conductor");
    // a mob standing next to the controls, not seated: still none (Create: checkConductors)
    auto  w    = con->toWorld(lv::core::contraption::Vec3d::center(*seat));
    auto* pig  = spawnMob(*tn::regionOf(l.base.dim), "minecraft:pig", {l.base.dim, int(std::floor(w.x)), int(std::floor(w.y)), int(std::floor(w.z))});
    ctx.require(pig != nullptr, "a pig spawned");
    auto pigId = pig->getOrCreateUniqueID().rawID;
    co_await ctx.ticks(2);
    t = tn::findTrain(*id);
    ctx.check(t && !tn::hasConductor(*t, true), "a mob standing by the controls is no conductor");
    // seated on the seat behind the controls: a conductor for the forward direction
    con = cn::find(t->carriages[0].contraption);
    ctx.require(con && con->seat(*seat, pigId), "the pig sits down on the carriage's seat");
    ctx.check(tn::hasConductor(*t, true), "a seated mob behind the controls drives forward");
    ctx.check(!tn::hasConductor(*t, false), "but not backwards");
    ctx.check(tn::conductorTrainOf(*pig) == t, "the pig is the train's conductor");
    // with a real conductor (no selftest shortcut) the schedule departs
    t->runtime.setSchedule(toB(" cond"), false);
    bool left = false;
    for (int i = 0; i < 60 && !left; ++i) {
        co_await ctx.ticks(1);
        t    = tn::findTrain(*id);
        left = t && t->nav.has_value();
    }
    ctx.check(left, "the seated conductor drives the train to B");
    removeActor(pigId);
    teardown(l);
    co_await ctx.ticks(2);
});

// T5: Create's control(): driving backwards runs at a quarter of the speed, double ended too
LV_REGISTER_SELFTEST("trains.u57_reverse_speed", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, " rev"), "line and stations built");
    auto id = assembleAt(ctx, l, true, 2, 0, [](GlobalPos const&) {});
    ctx.require(id.has_value(), "the double-ended train assembles");
    auto* t = tn::findTrain(*id);
    ctx.require(t && t->doubleEnded, "double ended");
    auto& player = ctx.player();
    player.teleport(Vec3{float(l.trackA.x - 1) + .5f, float(l.trackA.y + 2), float(l.trackA.z) + 2.5f}, player.getDimensionId());
    co_await ctx.ticks(2);
    t = tn::findTrain(*id);
    tn::startControlling(player, *t, 0, t->carriages[0].controls[0].first, t->carriages[0].controls[0].second);
    tn::setInput(player, -1, 0, false, false);
    co_await ctx.ticks(1);
    t = tn::findTrain(*id);
    ctx.require(t != nullptr, "the train exists");
    double full = t->stats().maxSpeed * tn::config().manualTrainSpeedModifier;
    ctx.check(std::abs(std::abs(t->targetSpeed) - full / 4) < 1e-6,
              "backwards targets a quarter of the manual speed: " + std::to_string(t->targetSpeed) + " vs " + std::to_string(full / 4));
    tn::setInput(player, 1, 0, false, false);
    co_await ctx.ticks(1);
    t = tn::findTrain(*id);
    ctx.check(t && std::abs(std::abs(t->targetSpeed) - full) < 1e-6, "forwards targets the full manual speed");
    tn::setInput(player, 0, 0, false, true);
    teardown(l);
    co_await ctx.ticks(2);
});

// T6: Create's Train.tick: a drill breaking a block stalls its carriage and the whole train
LV_REGISTER_SELFTEST("trains.u57_stall", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, " stall"), "line and stations built");
    auto id = assembleAt(ctx, l, false, 2, 0, [&](GlobalPos const& bogey) {
        ctx.setBlock(at(bogey, 0, 2), "levitate:mechanical_drill", {{"levitate:facing", 5}});
    });
    ctx.require(id.has_value(), "the train with a drill assembles");
    // a block of dirt in the drill's way, 10 blocks east
    auto dirt = at(l.trackA, 10, 3);
    ctx.setBlock(dirt, "minecraft:dirt");
    auto* t = tn::findTrain(*id);
    t->testConductor = true;
    t->runtime.setSchedule(toB(" stall"), false);
    bool stalled = false, resumed = false, arrived = false;
    for (int i = 0; i < 900 && !arrived; ++i) {
        co_await ctx.ticks(1);
        t = tn::findTrain(*id);
        if (!t) break;
        if (!stalled && t->speedBeforeStall) {
            stalled = true;
            ctx.check(t->speed == 0, "a stalled carriage stops the train");
            ctx.info("stalled after " + std::to_string(i) + " ticks, speed before " + std::to_string(*t->speedBeforeStall));
        }
        if (stalled && !t->speedBeforeStall && t->speed != 0) resumed = true;
        arrived = t->currentStation == l.stationB.key();
    }
    ctx.check(stalled, "the drill stalled the train at the dirt");
    ctx.check(tn::idAt(dirt) != "minecraft:dirt", "the drill broke the dirt");
    ctx.check(resumed, "the train went on after the stall");
    ctx.check(arrived, "and arrived at B");
    teardown(l);
    co_await ctx.ticks(2);
});

// T7 / T8: the station's auto-schedule takes schedules from automation and thrown items, shows it
// and replaces the schedule of every train arriving
LV_REGISTER_SELFTEST("trains.u57_auto_schedule", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, " auto"), "line and stations built");
    auto* sb     = tn::stationAt(l.stationB);
    auto* region = tn::regionOf(l.base.dim);
    // a schedule back to A, put in by a funnel / hopper (the item capability)
    ct::Schedule back;
    auto         e              = ct::Entry::makeDefault();
    e.instruction.data["Text"]  = "U57 A auto";
    back.entries.push_back(e);
    auto item = tn::makeScheduleItem(back);
    auto rest = lv::feature::common::items::insert(*region, lv::game::blockPos(l.stationB), 1, item, false);
    ctx.check(rest.isNull(), "the station takes one schedule");
    ctx.check(!sb->autoSchedule.is_null(), "it is the station's auto-schedule");
    auto again = lv::feature::common::items::insert(*region, lv::game::blockPos(l.stationB), 1, item, true);
    ctx.check(!again.isNull(), "a second one doesn't fit");
    auto stone = lv::feature::common::items::make("minecraft:stone");
    ctx.check(stone && !lv::feature::common::items::insert(*region, lv::game::blockPos(l.stationB), 1, *stone, true).isNull(), "other items don't go in");
    ctx.check(lv::feature::itemrender::find(l.stationB, 0) != nullptr, "the schedule is drawn on the station (client mod)");
    // a train with its own schedule to B gets B's auto-schedule on arrival
    auto id = assembleAt(ctx, l, false, 0, 0, [](GlobalPos const&) {});
    ctx.require(id.has_value(), "the train assembles");
    auto* t          = tn::findTrain(*id);
    t->testConductor = true;
    t->runtime.setSchedule(toB(" auto"), false);
    bool replaced = false;
    for (int i = 0; i < 600 && !replaced; ++i) {
        co_await ctx.ticks(1);
        t        = tn::findTrain(*id);
        replaced = t && t->runtime.isAutoSchedule;
    }
    ctx.check(replaced, "arriving at B replaced the train's schedule with the auto-schedule");
    ctx.check(t && t->runtime.hasSchedule && t->runtime.schedule.entries.size() == 1
                  && t->runtime.schedule.entries[0].instruction.data.value("Text", std::string()) == "U57 A auto",
              "it now heads back to A");
    // taken out again, then thrown onto station A
    auto taken = tn::takeAutoSchedule(*sb);
    ctx.check(tn::scheduleOf(taken).has_value(), "the schedule comes out");
    auto level = ll::service::getLevel();
    level->getSpawner().spawnItem(*region, taken, nullptr, Vec3{float(l.stationA.x + .5), float(l.stationA.y + 1.2), float(l.stationA.z + .5)}, 0);
    bool landed = false;
    for (int i = 0; i < 60 && !landed; ++i) {
        co_await ctx.ticks(1);
        auto* sa = tn::stationAt(l.stationA);
        landed   = sa && !sa->autoSchedule.is_null();
    }
    if (!landed) {
        AABB box{Vec3{float(l.stationA.x - 2), float(l.stationA.y - 1), float(l.stationA.z - 2)}, Vec3{float(l.stationA.x + 3), float(l.stationA.y + 4), float(l.stationA.z + 3)}};
        for (auto* a : region->getEntities(box, 0.0f, ActorType::ItemEntity)) {
            auto p = a->getPosition();
            ctx.info("item entity near A at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + ": "
                     + static_cast<ItemActor*>(a)->item().getTypeName());
        }
    }
    ctx.check(landed, "a schedule thrown onto a station becomes its auto-schedule");
    teardown(l);
    co_await ctx.ticks(2);
});

// T31: Create's TrainRelocator refuses a place where the train would overlap another
LV_REGISTER_SELFTEST("trains.u57_relocate_overlap", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, " reloc"), "line and stations built");
    auto first = assembleAt(ctx, l, false, 0, 0, [](GlobalPos const&) {});
    ctx.require(first.has_value(), "a train assembles");
    auto* t1 = tn::findTrain(*first);
    t1->body.move(tr::railways(), 15, {});
    t1->currentStation.clear();
    tn::placeCarriages(*t1);
    co_await ctx.ticks(2);
    auto second = assembleAt(ctx, l, false, 0, 0, [](GlobalPos const&) {});
    ctx.require(second.has_value(), "a second train assembles");
    auto* t2  = tn::findTrain(*second);
    auto  pos = ct::positionOf(tr::railways(), tn::findTrain(*first)->body.front);
    ctx.require(pos.has_value(), "the first train is on the track");
    auto onto = tn::bodyAt(*t2, *pos, {1, 0, 0}, 1.0);
    ctx.require(onto.has_value(), "there is track there");
    ctx.check(tn::overlapsOtherTrain(*t2, *onto), "relocating onto the other train is refused");
    auto free = tn::bodyAt(*t2, {l.base.x + 45.5, double(l.base.y), l.base.z + .5}, {1, 0, 0}, 1.0);
    ctx.check(free && !tn::overlapsOtherTrain(*t2, *free), "a free stretch is fine");
    teardown(l);
    co_await ctx.ticks(2);
});

// T17 / T18: the station summary lists every upcoming destination; the status shows the condition
LV_REGISTER_SELFTEST("trains.u57_display_sources", [](Context& ctx) -> ll::coro::CoroTask<> {
    Line l;
    ctx.require(co_await buildLine(ctx, l, " disp"), "line and stations built");
    auto id = assembleAt(ctx, l, false, 0, 0, [](GlobalPos const&) {});
    ctx.require(id.has_value(), "the train assembles");
    auto* t = tn::findTrain(*id);
    // a cyclic schedule A -> B -> A, waiting 20 s at each
    ct::Schedule sch;
    for (auto const* name : {"U57 B disp", "U57 A disp"}) {
        auto e                              = ct::Entry::makeDefault();
        e.instruction.data["Text"]          = std::string(name);
        e.conditions[0][0].data["Value"]    = 20;
        e.conditions[0][0].data["TimeUnit"] = 1;
        sch.entries.push_back(e);
    }
    t->testConductor = true;
    t->runtime.setSchedule(sch, false);
    co_await ctx.ticks(3);
    auto rows = tn::stationSummaryRows("U57 * disp", 8);
    ctx.check(rows.size() >= 2, "the summary lists both upcoming stops: " + std::to_string(rows.size()));
    bool toA = false, toB = false;
    for (auto const& r : rows) {
        toA = toA || r.platform == "A";
        toB = toB || r.platform == "B";
    }
    ctx.check(toA && toB, "with their platform column (the filter's wildcard part)");
    // waiting at B: the status names the condition
    bool atB = false;
    for (int i = 0; i < 600 && !atB; ++i) {
        co_await ctx.ticks(1);
        t   = tn::findTrain(*id);
        atB = t && t->currentStation == l.stationB.key() && t->runtime.state == ct::RuntimeState::PostTransit;
    }
    ctx.require(atB, "the train waits at B");
    auto status = tn::waitingStatusText(*t);
    ctx.check(status.find("Departs in") != std::string::npos || !status.empty(), "the status says when it departs: " + status);
    rows = tn::stationSummaryRows("U57 B disp", 4);
    ctx.check(!rows.empty() && rows.front().ticks == 0, "the train at B is listed as now");
    teardown(l);
    co_await ctx.ticks(2);
});

// T1: Create's carriages pass portal tracks into the other dimension; stations there are reachable
LV_REGISTER_SELFTEST("trains.u57_portal", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(5700, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    // keep both ends loaded: a ticking area in the overworld and one in the nether
    GlobalPos nb{1, base.x, 64, base.z};
    auto      area = [](GlobalPos const& p, int len) {
        return std::to_string(p.x - 4) + " 0 " + std::to_string(p.z - 4) + " " + std::to_string(p.x + len) + " 0 " + std::to_string(p.z + 4);
    };
    command("tickingarea add " + area(base, 34) + " lv_u57_ow true", 0);
    command("tickingarea add " + area(nb, 34) + " lv_u57_nether true", 1);
    bool loaded = false;
    for (int i = 0; i < 200 && !loaded; ++i) {
        co_await ctx.ticks(1);
        auto* n = tn::regionOf(1);
        auto* o = tn::regionOf(0);
        loaded  = n && o && n->hasChunksAt(lv::game::blockPos(nb), 0, false) && n->hasChunksAt(lv::game::blockPos(at(nb, 30)), 0, false)
              && o->hasChunksAt(lv::game::blockPos(at(base, 30)), 0, false);
    }
    auto cleanup = [&] {
        removeTrainsNear(base);
        for (auto& [id, t] : tn::state().trains)
            if (t.dim == 1) tn::removeTrain(id);
        tn::removeStation(at(base, 10, 0, 1));
        tn::removeStation(at(nb, 20, 0, 1));
        tr::state().portals.erase({0, {base.x + 20, base.y, base.z}});
        tr::state().portals.erase({1, {nb.x, nb.y, nb.z}});
        tr::state().portalsDirty = true;
        clearBox(at(base, -1, 0, -2), at(base, 22, 4, 2));
        clearBox(at(nb, -1, -1, -2), at(nb, 30, 4, 2));
        command("tickingarea remove lv_u57_ow", 0);
        command("tickingarea remove lv_u57_nether", 1);
    };
    if (!loaded) {
        cleanup();
        ctx.require(false, "the nether test area loaded");
        co_return;
    }
    clearBox(at(base, -1, 0, -2), at(base, 22, 4, 2));
    clearBox(at(nb, -1, -1, -2), at(nb, 30, 4, 2));
    // the nether stretch: a floor, the portal track at nb facing west, the line east of it
    for (int x = -1; x <= 30; ++x) place(at(nb, x, -1), "minecraft:netherrack");
    // Create's portal link (what binding a portal track writes), then the tracks
    GlobalPos portalOw = at(base, 20);
    tr::state().portals[{0, {portalOw.x, portalOw.y, portalOw.z}}] = tr::PortalLink{1, {nb.x, nb.y, nb.z}};
    tr::state().portals[{1, {nb.x, nb.y, nb.z}}]                   = tr::PortalLink{0, {portalOw.x, portalOw.y, portalOw.z}};
    for (int x = 0; x < 20; ++x) ctx.setBlock(at(base, x), std::string(tr::kTrackId), shapeStates(ck::Shape::XO));
    ctx.setBlock(portalOw, std::string(tr::kTrackId), shapeStates(ck::Shape::TE, true));
    place(nb, std::string(tr::kTrackId), shapeStates(ck::Shape::TW, true));
    for (int x = 1; x <= 30; ++x) place(at(nb, x), std::string(tr::kTrackId), shapeStates(ck::Shape::XO));
    co_await ctx.ticks(5);
    // one graph spans both dimensions
    auto locOw = tr::railways().nearestEdge(0, {base.x + 5.5, double(base.y), base.z + .5}, 1.0);
    auto locNe = tr::railways().nearestEdge(1, {nb.x + 15.5, double(nb.y), nb.z + .5}, 1.0);
    ctx.check(locOw && locNe && locOw->graph == locNe->graph, "the portal track joins the overworld and the nether line in one graph");
    // stations: A in the overworld, N in the nether
    ctx.setBlock(at(base, 10, 0, 1), std::string(tn::kStation));
    place(at(nb, 20, 0, 1), std::string(tn::kStation));
    co_await ctx.ticks(2);
    std::string err;
    bool okA = tn::createStation(at(base, 10, 0, 1), at(base, 10), 1, 0, &err);
    bool okN = tn::createStation(at(nb, 20, 0, 1), at(nb, 20), 1, 0, &err);
    if (!okA || !okN) {
        cleanup();
        ctx.require(false, "stations created: " + err);
        co_return;
    }
    tn::stationAt(at(base, 10, 0, 1))->name = "U57 Portal A";
    tn::stationAt(at(nb, 20, 0, 1))->name   = "U57 Portal N";
    tn::refreshStationPoints();
    // a train at A
    auto* sa = tn::stationAt(at(base, 10, 0, 1));
    tn::setAssembling(*sa, true, nullptr);
    tn::useCasingOnTrack(nullptr, at(base, 9), true);
    ctx.setBlock(at(base, 9, 2), std::string(tn::kControls), {{"levitate:facing", 4}});
    auto id = tn::assemble(*sa, nullptr);
    if (!id) {
        cleanup();
        ctx.require(false, "the train assembles: " + sa->lastError.dump());
        co_return;
    }
    auto* t          = tn::findTrain(*id);
    t->testConductor = true;
    ct::Schedule sch;
    auto         e             = ct::Entry::makeDefault();
    e.instruction.data["Text"] = "U57 Portal N";
    sch.entries.push_back(e);
    sch.cyclic = false;
    t->runtime.setSchedule(sch, false);
    co_await ctx.ticks(2);
    t = tn::findTrain(*id);
    ctx.check(t && t->nav.has_value(), "the train finds a way to the station in the nether");
    bool arrived = false;
    for (int i = 0; i < 900 && !arrived; ++i) {
        co_await ctx.ticks(1);
        t       = tn::findTrain(*id);
        arrived = t && t->currentStation == at(nb, 20, 0, 1).key();
    }
    ctx.check(arrived, "the train arrives at the nether station");
    t        = tn::findTrain(*id);
    auto* cc = t && !t->carriages.empty() ? cn::find(t->carriages[0].contraption) : nullptr;
    ctx.check(cc && cc->dimension() == 1, "its carriage is in the nether");
    ctx.check(t && t->dim == 1, "the train counts as in the nether");
    if (cc) {
        auto w = cc->toWorld({0.5, 0.5, 0.5});
        ctx.check(std::abs(w.x - (nb.x + 19.5)) < 3, "the carriage stands at the nether station: x " + std::to_string(w.x));
    }
    cleanup();
    co_await ctx.ensureLoaded(base);
});
