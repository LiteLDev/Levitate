// Selftests of the pistons unit (testPos dx 2000..2099). The kinetic source is U2's test
// generator (levitate:kinetics_test_source); real generators belong to U7.

#include <functional>

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"

#include "feature/contraption/Contraption.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pistons/Pistons.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::pistons {

namespace {

using selftest::Context;
namespace ct = feature::contraption;

constexpr char kSource[] = "levitate:kinetics_test_source";

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

/// Clears a box around the test (restored to air afterwards too).
void clearBox(Context& ctx, GlobalPos const& o, int x0, int y0, int z0, int x1, int y1, int z1) {
    EditGuard g;
    for (int x = x0; x <= x1; ++x)
        for (int y = y0; y <= y1; ++y)
            for (int z = z0; z <= z1; ++z) {
                auto q = at(o, x, y, z);
                ctx.cleanupBlock(q);
                if (!idAt(q).empty() && idAt(q) != "minecraft:air") clearAt(q, 2);
            }
}

bool put(Context& ctx, GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {}) {
    ctx.cleanupBlock(p);
    return place(p, id, states, 3);
}

ll::coro::CoroTask<bool> waitFor(Context& ctx, std::function<bool()> cond, int maxTicks) {
    for (int i = 0; i < maxTicks; ++i) {
        if (cond()) co_return true;
        co_await ctx.ticks(1);
    }
    co_return cond();
}

/// Leaves no contraption behind.
struct Cleanup {
    std::vector<GlobalPos> actuators;
    ~Cleanup() {
        for (auto const& p : actuators)
            if (auto id = contraptionAt(p)) ct::disassemble(id);
    }
};

} // namespace

LV_REGISTER_SELFTEST("pistons.registry", [](Context& ctx) {
    for (auto id : {kPiston, kStickyPiston, kRopePulley, kElevatorPulley, kGantryShaft, kGantryCarriage})
        ctx.check(kinetics::specOf(id) != nullptr, std::string(id) + " is kinetic");
    for (auto id : {kPiston, kStickyPiston, kPole, kHead, kRopePulley, kRope, kMagnet, kElevatorPulley, kElevatorContact, kGantryCarriage, kGantryShaft})
        ctx.require(game::blockWith(id) != nullptr, std::string(id) + " is in the behavior pack");
    ctx.check(kinetics::stressImpactOf(kPiston) == 4, "piston impact 4 SU/RPM");
    ctx.check(kinetics::stressImpactOf(kRopePulley) == 4, "rope pulley impact 4 SU/RPM");
    ctx.check(kinetics::stressImpactOf(kElevatorPulley) == 4, "elevator pulley impact 4 SU/RPM");
    ctx.check(kinetics::specOf(kGantryShaft)->impact == 0 && kinetics::specOf(kGantryCarriage)->impact == 0, "gantries have no stress impact (Create: setNoImpact)");
});

LV_REGISTER_SELFTEST("pistons.movement_rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2000);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    clearBox(ctx, o, 0, 0, 0, 4, 2, 0);
    put(ctx, o, kRope);
    put(ctx, at(o, 1, 0, 0), kPole, {{kFacing, int(cc::Dir::Up)}});
    put(ctx, at(o, 2, 0, 0), kPiston, {{kFacing, 1}, {kState, int(PistonState::Moving)}});
    put(ctx, at(o, 3, 0, 0), kRopePulley, {{kRunning, 1}});
    auto rope = ct::ext::describe(o.dim, cpos(o));
    ctx.check(!rope.movable && rope.brittle, "rope: unmovable and brittle (Create: PushReaction.BLOCK)");
    auto pole = ct::ext::describe(o.dim, cpos(at(o, 1, 0, 0)));
    ctx.check(cc::has(pole.notSupportive, cc::Dir::North) && !cc::has(pole.notSupportive, cc::Dir::Up), "poles carry nothing on their sides");
    ctx.check(!ct::ext::describe(o.dim, cpos(at(o, 2, 0, 0))).movable, "moving pistons are unmovable");
    ctx.check(!ct::ext::describe(o.dim, cpos(at(o, 3, 0, 0))).movable, "running pulleys are unmovable");
});

/// A south-facing piston with two poles behind, pushing a block; powered from the east.
struct PistonRig {
    GlobalPos piston, source;
};
static PistonRig buildPiston(Context& ctx, GlobalPos o, std::string_view id, int poles) {
    clearBox(ctx, o, -1, 0, -poles - 1, 1, 1, 6);
    PistonRig r{o, at(o, 1, 0, 0)};
    for (int i = 1; i <= poles; ++i) put(ctx, at(o, 0, 0, -i), kPole, {{kFacing, int(cc::Dir::South)}});
    put(ctx, o, id, {{kFacing, int(cc::Dir::South)}, {kAxisFirst, 1}, {kState, 0}}); // shaft along x
    return r;
}

LV_REGISTER_SELFTEST("pistons.extend_retract", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2010);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    auto rig = buildPiston(ctx, o, kPiston, 2);
    put(ctx, at(o, 0, 0, 1), "minecraft:stone");
    put(ctx, rig.source, kSource, {{"levitate:axis", 0}});
    Cleanup cleanup{{o}};
    co_await Context::ticks(3);
    // south-facing pistons extend with positive speed (Create's movement modifier)
    kinetics::setGeneratedSpeed(rig.source, 128);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "the piston starts: " + lastErrorAt(o));
    ctx.check(stateOr(o, kState, -1) == int(PistonState::Moving), "piston shows the moving state");
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 200), "the piston stops at the end");
    ctx.check(idAt(at(o, 0, 0, 3)) == "minecraft:stone", "the block is pushed two blocks");
    ctx.check(idAt(at(o, 0, 0, 2)) == kHead, "head in front");
    ctx.check(idAt(at(o, 0, 0, 1)) == kPole, "pole between piston and head");
    ctx.check(idAt(at(o, 0, 0, -1)) == "minecraft:air", "back poles went through the piston");
    ctx.check(stateOr(o, kState, -1) == int(PistonState::Extended), "piston extended");
    ctx.check(offsetOf(o) == 2.f, "offset 2");

    kinetics::setGeneratedSpeed(rig.source, -128);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "the piston retracts: " + lastErrorAt(o));
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 200), "the piston stops retracted");
    ctx.check(idAt(at(o, 0, 0, 3)) == "minecraft:stone", "a normal piston leaves the block");
    ctx.check(idAt(at(o, 0, 0, 1)) == "minecraft:air" && idAt(at(o, 0, 0, 2)) == "minecraft:air", "head and pole are back");
    ctx.check(idAt(at(o, 0, 0, -1)) == kPole && idAt(at(o, 0, 0, -2)) == kPole, "poles behind again");
    ctx.check(stateOr(o, kState, -1) == int(PistonState::Retracted), "piston retracted");
});

LV_REGISTER_SELFTEST("pistons.sticky_pull", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2020);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    auto rig = buildPiston(ctx, o, kStickyPiston, 1);
    put(ctx, at(o, 0, 0, 1), "minecraft:stone");
    put(ctx, rig.source, kSource, {{"levitate:axis", 0}});
    Cleanup cleanup{{o}};
    co_await Context::ticks(3);
    kinetics::setGeneratedSpeed(rig.source, 256);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "extends: " + lastErrorAt(o));
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 100), "stops extended");
    ctx.check(idAt(at(o, 0, 0, 2)) == "minecraft:stone", "pushed one block");
    kinetics::setGeneratedSpeed(rig.source, -256);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "retracts: " + lastErrorAt(o));
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 100), "stops retracted");
    ctx.check(idAt(at(o, 0, 0, 1)) == "minecraft:stone", "the sticky piston pulls the block back");
    ctx.check(idAt(at(o, 0, 0, 2)) == "minecraft:air", "nothing left in front");
});

LV_REGISTER_SELFTEST("pistons.collision", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2030);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    auto rig = buildPiston(ctx, o, kPiston, 3);
    put(ctx, at(o, 0, 0, 1), "minecraft:stone");
    put(ctx, at(o, 0, 0, 3), "minecraft:bedrock"); // not in the push line (air between)
    put(ctx, rig.source, kSource, {{"levitate:axis", 0}});
    Cleanup cleanup{{o}};
    co_await Context::ticks(3);
    kinetics::setGeneratedSpeed(rig.source, 128);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "extends: " + lastErrorAt(o));
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 200), "stops at the wall");
    ctx.check(idAt(at(o, 0, 0, 2)) == "minecraft:stone", "the block stops in front of the wall");
    ctx.check(idAt(at(o, 0, 0, 1)) == kHead, "the head stays one block out");
    ctx.check(offsetOf(o) == 1.f, "offset 1 (grid aligned after the collision)");
});

LV_REGISTER_SELFTEST("pistons.assembly_errors", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2040);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    auto rig = buildPiston(ctx, o, kPiston, 0);
    put(ctx, rig.source, kSource, {{"levitate:axis", 0}});
    co_await Context::ticks(3);
    kinetics::setGeneratedSpeed(rig.source, 128);
    co_await Context::ticks(5);
    ctx.check(!runningAt(o), "no poles: no movement");
    ctx.check(lastErrorAt(o) == "create.gui.assembly.exception.noPistonPoles", "noPistonPoles, got '" + lastErrorAt(o) + "'");
    // an unmovable block in the push line
    put(ctx, at(o, 0, 0, -1), kPole, {{kFacing, int(cc::Dir::South)}});
    put(ctx, at(o, 0, 0, 1), "minecraft:stone");
    put(ctx, at(o, 0, 0, 2), "minecraft:bedrock");
    requestAssemble(o);
    co_await Context::ticks(3);
    ctx.check(!runningAt(o), "blocked: no movement");
    ctx.check(lastErrorAt(o) == "create.gui.assembly.exception.unmovableBlock", "unmovableBlock, got '" + lastErrorAt(o) + "'");
});

LV_REGISTER_SELFTEST("pistons.movement_mode", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2050);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    auto rig = buildPiston(ctx, o, kPiston, 2);
    put(ctx, at(o, 0, 0, 1), "minecraft:oak_planks");
    put(ctx, rig.source, kSource, {{"levitate:axis", 0}});
    Cleanup cleanup{{o}};
    setMovementMode(o, cp::MovementMode::MovePlaceReturned);
    co_await Context::ticks(3);
    kinetics::setGeneratedSpeed(rig.source, 256);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "extends: " + lastErrorAt(o));
    co_await Context::ticks(30);
    ctx.check(runningAt(o) && contraptionAt(o) != 0, "away from the start: the structure stays a contraption");
    ctx.check(idAt(at(o, 0, 0, 3)) == "minecraft:air", "no block placed at the end");
    kinetics::setGeneratedSpeed(rig.source, -256);
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 100), "placed back at the start");
    ctx.check(idAt(at(o, 0, 0, 1)) == "minecraft:oak_planks", "the planks are where they started");
    ctx.check(idAt(at(o, 0, 0, -1)) == kPole, "poles behind");
});

LV_REGISTER_SELFTEST("pistons.rope_pulley", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2060);
    o.y += 8;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    clearBox(ctx, o, -1, -8, -1, 1, 0, 1);
    put(ctx, at(o, 0, -8, 0), "minecraft:stone"); // the floor
    put(ctx, o, kRopePulley, {{kAxis, 2}}); // z: integer states must read their value, not an index
    put(ctx, at(o, 0, -1, 0), kRope);
    put(ctx, at(o, 0, -2, 0), kMagnet);
    put(ctx, at(o, 0, -3, 0), "minecraft:oak_planks");
    auto source = at(o, 0, 0, 1);
    put(ctx, source, kSource, {{"levitate:axis", 2}});
    Cleanup cleanup{{o}};
    co_await Context::ticks(3);
    kinetics::setGeneratedSpeed(source, 128);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "the pulley starts: " + lastErrorAt(o));
    ctx.check(idAt(at(o, 0, -1, 0)) == "minecraft:air", "ropes taken in at assembly");
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 200), "stops on the floor");
    ctx.check(idAt(at(o, 0, -7, 0)) == "minecraft:oak_planks", "lowered onto the floor");
    ctx.check(idAt(at(o, 0, -6, 0)) == kMagnet, "magnet above the load");
    int ropes = 0;
    for (int i = 1; i <= 5; ++i) ropes += idAt(at(o, 0, -i, 0)) == kRope;
    ctx.check(ropes == 5, "five rope blocks, got " + std::to_string(ropes));

    kinetics::setGeneratedSpeed(source, -128);
    ctx.require(co_await waitFor(ctx, [&] { return runningAt(o); }, 40), "raises: " + lastErrorAt(o));
    ctx.require(co_await waitFor(ctx, [&] { return !runningAt(o); }, 200), "stops at the top");
    ctx.check(idAt(at(o, 0, -1, 0)) == "minecraft:oak_planks", "pulled up to the pulley");
    ctx.check(idAt(at(o, 0, -7, 0)) == "minecraft:air", "nothing left below");
});

LV_REGISTER_SELFTEST("pistons.rope_break", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2070);
    o.y += 6;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    clearBox(ctx, o, 0, -5, 0, 0, 0, 0);
    put(ctx, o, kRopePulley, {{kAxis, 0}});
    for (int i = 1; i <= 3; ++i) put(ctx, at(o, 0, -i, 0), kRope);
    put(ctx, at(o, 0, -4, 0), kMagnet);
    data(o)["offset"] = 4.0f;
    clearAt(at(o, 0, -2, 0)); // a player / explosion breaks a rope
    co_await Context::ticks(3);
    ctx.check(idAt(at(o, 0, -1, 0)) == "minecraft:air" && idAt(at(o, 0, -3, 0)) == "minecraft:air", "the whole rope breaks");
    ctx.check(idAt(at(o, 0, -4, 0)) == "minecraft:air", "with its magnet");
    ctx.check(offsetOf(o) == 0.f, "the pulley's length resets");
});

LV_REGISTER_SELFTEST("pistons.piston_head_break", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2075);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    clearBox(ctx, o, 0, 0, 0, 0, 0, 4);
    put(ctx, o, kPiston, {{kFacing, int(cc::Dir::South)}, {kState, int(PistonState::Extended)}});
    put(ctx, at(o, 0, 0, 1), kPole, {{kFacing, int(cc::Dir::South)}});
    put(ctx, at(o, 0, 0, 2), kPole, {{kFacing, int(cc::Dir::South)}});
    put(ctx, at(o, 0, 0, 3), kHead, {{kFacing, int(cc::Dir::South)}});
    clearAt(at(o, 0, 0, 3));
    pistonChainBroken(at(o, 0, 0, 3), std::string(kHead), cc::Dir::South, false);
    ctx.check(idAt(at(o, 0, 0, 1)) == "minecraft:air" && idAt(at(o, 0, 0, 2)) == "minecraft:air", "breaking the head breaks the poles");
    ctx.check(stateOr(o, kState, -1) == int(PistonState::Retracted), "the piston retracts");
});

LV_REGISTER_SELFTEST("pistons.gantry", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2080);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    clearBox(ctx, o, -1, 0, 0, 5, 2, 0);
    // shafts facing east along x: start (0), middle (1, 2), end (3)
    for (int i = 0; i < 4; ++i) {
        auto q = at(o, i, 0, 0);
        put(ctx, q, kGantryShaft, {{kFacing, int(cc::Dir::East)}, {kPart, 3}});
        trackShaft(q);
    }
    co_await Context::ticks(3);
    ctx.check(stateOr(o, kPart, -1) == int(cp::ShaftPart::Start), "first shaft: start");
    ctx.check(stateOr(at(o, 1, 0, 0), kPart, -1) == int(cp::ShaftPart::Middle), "middle shaft");
    ctx.check(stateOr(at(o, 3, 0, 0), kPart, -1) == int(cp::ShaftPart::End), "last shaft: end");
    // carriage on top of the third shaft, its pinion across the shaft
    auto carriage = at(o, 2, 1, 0);
    put(ctx, carriage, kGantryCarriage, {{kFacing, int(cc::Dir::Up)}, {kAxisFirst, 0}});
    put(ctx, at(o, 2, 2, 0), "minecraft:oak_planks");
    // two generators: the kinetics table (U2) still gives gantry shafts 4 SU/RPM (Create: 0)
    auto source = at(o, -1, 0, 0), source2 = at(o, 4, 0, 0);
    put(ctx, source, kSource, {{"levitate:axis", 0}});
    put(ctx, source2, kSource, {{"levitate:axis", 0}});
    co_await Context::ticks(3);
    // pinion speed = -rpm / 512 along the shaft facing: positive rpm moves the carriage west
    kinetics::setGeneratedSpeed(source, 128);
    kinetics::setGeneratedSpeed(source2, 128);
    auto moved = co_await waitFor(ctx, [&] { return idAt(at(o, 0, 1, 0)) == kGantryCarriage; }, 200);
    ctx.check(moved, "the carriage runs to the start of the shaft line");
    ctx.check(idAt(at(o, 0, 2, 0)) == "minecraft:oak_planks", "its load comes along");
    ctx.check(idAt(carriage) == "minecraft:air", "the old place is empty");
    // a powered shaft line doesn't move carriages
    put(ctx, at(o, 3, 1, 0), "minecraft:redstone_block");
    co_await Context::ticks(5);
    ctx.check(stateOr(o, kPowered, 0) == 1, "redstone powers the whole line");
    kinetics::setGeneratedSpeed(source, -128);
    kinetics::setGeneratedSpeed(source2, -128);
    co_await Context::ticks(20);
    ctx.check(idAt(at(o, 0, 1, 0)) == kGantryCarriage, "a powered line holds the carriage");
    for (auto id : gantryContraptions()) ct::disassemble(id);
});

LV_REGISTER_SELFTEST("pistons.elevator_contacts", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(2090);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    clearBox(ctx, o, 0, 0, 0, 0, 12, 0);
    ColumnCoords col{o.dim, o.x, o.z, cc::Dir::South};
    for (int y : {0, 4, 8}) put(ctx, at(o, 0, y, 0), kElevatorContact, {{kFacing, int(cc::Dir::South)}});
    gatherColumn(col);
    co_await Context::ticks(2);
    auto floors = columnFloors(col);
    ctx.require(floors.size() == 3, "three floors, got " + std::to_string(floors.size()));
    ctx.check(floors[0].shortName == "0" && floors[1].shortName == "1" && floors[2].shortName == "2", "floors numbered 0, 1, 2");
    setContactNames(at(o, 0, 4, 0), "G", "Ground floor", 0);
    ctx.check(columnFloors(col)[1].shortName == "G", "named floor");
    callToContact(at(o, 0, 8, 0), false);
    ctx.check(columnTargetOf(col) == o.y + 8, "the column targets the called contact");
    ctx.check(stateOr(at(o, 0, 8, 0), kCalling, 0) == 1, "the called contact shows calling");
    triggerContact(col, o.y + 8);
    co_await Context::ticks(2);
    ctx.check(stateOr(at(o, 0, 8, 0), kPowering, 0) == 1, "arrival: the contact emits redstone");
    callToContact(at(o, 0, 0, 0), false);
    co_await Context::ticks(2);
    ctx.check(stateOr(at(o, 0, 8, 0), kPowering, 0) == 0 && stateOr(at(o, 0, 8, 0), kCalling, 0) == 0, "another call turns it off");
    // an elevator without a redstone contact can't assemble
    auto pulley = at(o, 0, 12, 0);
    put(ctx, pulley, kElevatorPulley, {{kFacing, int(cc::Dir::North)}});
    put(ctx, at(o, 0, 10, 0), "minecraft:oak_planks");
    put(ctx, at(o, 1, 12, 0), kSource, {{"levitate:axis", 0}});
    co_await Context::ticks(3);
    elevatorClicked(pulley);
    co_await Context::ticks(3);
    ctx.check(!runningAt(pulley), "no cabin contact: not running");
    ctx.check(lastErrorAt(pulley) == "create.gui.assembly.exception.no_contacts", "no_contacts, got '" + lastErrorAt(pulley) + "'");
    columnSetActive(col, false);
});

} // namespace lv::feature::pistons
