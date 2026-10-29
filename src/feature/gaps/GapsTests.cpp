// Selftests of U37 `gaps` (testPos dx 3700..3749): sequenced gearshift limits on bearings,
// pistons and gantries (Create's SequenceContext), and the client mod's train control channels.

#include <cmath>

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/tracks/Shape.h"
#include "feature/bearings/Bearings.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/pistons/Pistons.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::gaps {

namespace {

using core::GlobalPos;
using selftest::Context;
using Json = nlohmann::json;
namespace kn = feature::kinetics;
namespace tm = feature::transmission;
namespace br = feature::bearings;
namespace ps = feature::pistons;
namespace tn = feature::trains;
namespace tr = feature::tracks;
namespace cn = feature::contraption;
namespace ctm = core::transmission;

constexpr char kSource[] = "levitate:kinetics_test_source";

GlobalPos at(GlobalPos p, int dx, int dy = 0, int dz = 0) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

std::string str(double v) { return std::to_string(v); }

/// Disassembles contraptions a failed test left.
struct Leftovers {
    std::vector<GlobalPos> bearings, actuators;
    ~Leftovers() {
        for (auto const& b : bearings)
            if (auto id = br::bearingState(b).value("id", cn::Id{0}); id && cn::find(id)) cn::disassemble(id);
        for (auto const& a : actuators)
            if (auto id = ps::contraptionAt(a); id && cn::find(id)) cn::disassemble(id);
    }
};

/// source (X) -> sequenced gearshift -> bearing facing up on top of the gearshift's output shaft.
struct BearingRig {
    GlobalPos source, gearshift, bearing, front;
};

ll::coro::CoroTask<BearingRig> bearingRig(Context& ctx, int dx) {
    auto p = ctx.testPos(dx);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    BearingRig r{p, at(p, 0, 1), at(p, 0, 2), at(p, 0, 3)};
    // vertical line: source, gearshift (axis y), bearing facing up, planks on its face
    ctx.setBlock(r.source, kSource, {{"levitate:axis", 1}});
    ctx.setBlock(r.gearshift, tm::kSequencedGearshift, {{tm::kAxis, 1}});
    ctx.setBlock(r.bearing, std::string(br::kMechanicalBearing), {{"levitate:facing", 1}});
    ctx.setBlock(r.front, "minecraft:oak_planks");
    br::track(r.bearing);
    co_await ctx.ticks(3);
    co_return r;
}

double bearingAngle(GlobalPos const& bearing) {
    auto id = br::bearingState(bearing).value("id", cn::Id{0});
    auto* c = id ? cn::find(id) : nullptr;
    return c ? c->transform().angle : -1;
}

/// Runs the gearshift's sequence and returns the largest bearing angle seen while it ran.
ll::coro::CoroTask<double> runBearing(Context& ctx, BearingRig const& r, int maxTicks) {
    ctx.setBlock(at(r.gearshift, 1), "minecraft:redstone_block");
    double most = 0;
    bool   ran  = false;
    for (int i = 0; i < maxTicks; ++i) {
        co_await ctx.ticks(1);
        double a = bearingAngle(r.bearing);
        if (a >= 0) ran = true;
        most = std::max(most, a);
        if (ran && tm::sequencerCurrent(r.gearshift) == -1) break;
    }
    co_return most;
}

} // namespace

LV_REGISTER_SELFTEST("gaps.gearshift_turns_bearing_90", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto r = co_await bearingRig(ctx, 3700);
    Leftovers left{{r.bearing}, {}};
    kn::setGeneratedSpeed(r.source, 16);
    tm::setSequencerInstructions(r.gearshift, {{ctm::Instr::TurnAngle, 1, 90}, {ctm::Instr::End, 1, 1}});
    co_await ctx.ticks(2);
    double most = co_await runBearing(ctx, r, 80);
    // 16 RPM = 4.8 degrees per tick: 18 full steps and a last 3.6 degree one; Create's instruction
    // runs ceil(90 / 4.8) + 2 = 21 ticks, the bearing stops at the limit
    ctx.check(std::abs(most - 90) < 1e-3, "the bearing turned exactly 90 degrees, got " + str(most));
    co_await ctx.ticks(4);
    ctx.check(bearingAngle(r.bearing) < 0, "the gearshift's output stops: the structure is placed back");
    ctx.check(ps::idAt(r.front) == "minecraft:oak_planks", "the planks are back in front");
});

LV_REGISTER_SELFTEST("gaps.gearshift_two_equal_steps", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto r = co_await bearingRig(ctx, 3705);
    Leftovers left{{r.bearing}, {}};
    kn::setGeneratedSpeed(r.source, 16);
    // two equal steps: the speed doesn't change between them, the second instruction still counts
    tm::setSequencerInstructions(
        r.gearshift,
        {{ctm::Instr::TurnAngle, 2, 45}, {ctm::Instr::TurnAngle, 2, 45}, {ctm::Instr::End, 1, 1}}
    );
    co_await ctx.ticks(2);
    double most = co_await runBearing(ctx, r, 80);
    ctx.check(std::abs(most - 90) < 1e-3, "45 + 45 degrees at double speed, got " + str(most));
    co_await ctx.ticks(4);
});

LV_REGISTER_SELFTEST("gaps.gearshift_moves_piston_3", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(3710);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    // a south-facing piston with four poles behind (range 4), its shaft along x; gearshift and
    // source east of it
    for (int z = -5; z <= 6; ++z) ctx.cleanupBlock(at(o, 0, 0, z));
    for (int i = 1; i <= 4; ++i) ctx.setBlock(at(o, 0, 0, -i), std::string(ps::kPole), {{ps::kFacing, int(core::contraption::Dir::South)}});
    ctx.setBlock(o, std::string(ps::kPiston), {{ps::kFacing, int(core::contraption::Dir::South)}, {ps::kAxisFirst, 1}, {ps::kState, 0}});
    auto gear = at(o, 1), source = at(o, 2);
    ctx.setBlock(gear, tm::kSequencedGearshift, {{tm::kAxis, 0}});
    ctx.setBlock(source, kSource, {{"levitate:axis", 0}});
    ps::trackActuator(o);
    Leftovers left{{}, {o}};
    co_await ctx.ticks(3);
    kn::setGeneratedSpeed(source, 128);
    // 128 RPM = .25 blocks per tick; the instruction runs ceil(3 / .25) + 2 = 14 ticks, two more
    // than the piston needs: without the limit it would go on to offset 3.5
    tm::setSequencerInstructions(gear, {{ctm::Instr::TurnDistance, 1, 3}, {ctm::Instr::End, 1, 1}});
    co_await ctx.ticks(2);
    ctx.setBlock(at(gear, 0, 1), "minecraft:redstone_block");
    float most = 0;
    bool  ran  = false;
    for (int i = 0; i < 80; ++i) {
        co_await ctx.ticks(1);
        ran  = ran || ps::runningAt(o);
        most = std::max(most, ps::offsetOf(o));
        if (ran && !ps::runningAt(o)) break;
    }
    ctx.check(ran, "the piston ran: " + ps::lastErrorAt(o));
    ctx.check(std::abs(most - 3.f) < 1e-4f, "it never went past 3 blocks, got " + str(most));
    ctx.check(!ps::runningAt(o) && ps::offsetOf(o) == 3.f, "it stopped at offset 3, got " + str(ps::offsetOf(o)));
    ctx.check(ps::idAt(at(o, 0, 0, 3)) == ps::kHead, "the head stands 3 blocks out");
});

LV_REGISTER_SELFTEST("gaps.sequence_limit_gantry", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(3720);
    o.y += 1;
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    for (int x = -1; x <= 6; ++x)
        for (int y = 0; y <= 2; ++y) ctx.cleanupBlock(at(o, x, y));
    for (int i = 0; i < 6; ++i) {
        ctx.setBlock(at(o, i), std::string(ps::kGantryShaft), {{ps::kFacing, int(core::contraption::Dir::East)}, {ps::kPart, 3}});
        ps::trackShaft(at(o, i));
    }
    auto carriage = at(o, 4, 1);
    ctx.setBlock(carriage, std::string(ps::kGantryCarriage), {{ps::kFacing, int(core::contraption::Dir::Up)}, {ps::kAxisFirst, 0}});
    ctx.setBlock(at(o, 4, 2), "minecraft:oak_planks");
    auto source = at(o, -1);
    ctx.setBlock(source, kSource, {{"levitate:axis", 0}});
    kn::setStressCapacity(source, 4096); // the shafts' table impact (U2) would overstress the test source
    co_await ctx.ticks(3);
    // a "turn to move 2 blocks" instruction reaching the shaft line (set at the source: the
    // gearshift does the same for what it drives)
    using SC = kn::SequenceContext;
    kn::setSequenceContext(source, SC{SC::Kind::MoveDistance, 2});
    kn::setGeneratedSpeed(source, 128); // the carriage runs west towards the start of the line
    co_await ctx.ticks(40);
    auto ids = ps::gantryContraptions();
    ctx.require(ids.size() == 1, "the gantry assembled and holds (" + std::to_string(ids.size()) + " moving)");
    auto* c = cn::find(ids[0]);
    ctx.require(c != nullptr, "the gantry exists");
    ctx.check(std::abs(c->transform().offset.x + 2) < 1e-6, "it moved exactly 2 blocks, got " + str(c->transform().offset.x));
    kn::setSequenceContext(source, std::nullopt);
    kn::setGeneratedSpeed(source, 0);
    co_await ctx.ticks(4);
    ctx.check(ps::idAt(at(o, 2, 1)) == ps::kGantryCarriage, "the carriage is placed 2 blocks west");
    ctx.check(ps::idAt(at(o, 2, 2)) == "minecraft:oak_planks", "with its load");
    kn::setStressCapacity(source, std::nullopt);
    for (auto id : ps::gantryContraptions()) cn::disassemble(id);
});

// ---- trains: the client mod's controls channels ---------------------------------------------------

namespace {

std::vector<std::pair<std::string, int>> straightTrack() {
    int s = int(core::tracks::Shape::XO);
    return {{std::string(tr::kShapeState), s % 16}, {std::string(tr::kShapeHiState), s / 16}, {std::string(tr::kTurnState), 0}};
}

void clearBox(GlobalPos a, GlobalPos b) {
    auto* region = tn::regionOf(a.dim);
    auto const* air = game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
            for (int z = std::min(a.z, b.z); z <= std::max(a.z, b.z); ++z) {
                BlockPos bp{x, y, z};
                if (!region->getBlock(bp).isAir()) game::setBlock(*region, bp, *air);
            }
}

bool hasMessage(std::vector<core::common::Envelope> const& msgs, std::string const& channel, bool active) {
    for (auto const& m : msgs)
        if (m.channel == channel && m.payload.value("active", !active) == active) return true;
    return false;
}

Json keys(bool forward, bool back) { return {{"pressed", Json::array({forward, back, false, false, false, false})}}; }

} // namespace

LV_REGISTER_SELFTEST("gaps.train_channel_input", [](Context& ctx) -> ll::coro::CoroTask<> {
    // a 60 block line south of the other tests (dz 8), a station at x + 20
    auto base = ctx.testPos(3730, 8);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(at(base, 59)), "line end loaded");
    clearBox(at(base, -1, 0, -2), at(base, 60, 4, 2));
    co_await ctx.ticks(2);
    for (int x = 0; x < 60; ++x) ctx.setBlock(at(base, x), std::string(tr::kTrackId), straightTrack());
    co_await ctx.ticks(3);
    auto track   = at(base, 20);
    auto station = at(base, 20, 0, 1);
    ctx.setBlock(station, std::string(tn::kStation));
    co_await ctx.ticks(2);
    std::string err;
    ctx.require(tn::createStation(station, track, 1, 0, &err), "station created: " + err);
    tn::stationAt(station)->name = "U37 Station";
    tn::refreshStationPoints();
    auto* s = tn::stationAt(station);
    tn::setAssembling(*s, true, nullptr);
    auto track0 = at(track, -1);
    tn::useCasingOnTrack(nullptr, track0, true);
    auto bogey = at(track0, 0, 1);
    ctx.setBlock(at(bogey, 0, 1), std::string(tn::kControls), {{"levitate:facing", 4}});
    // a steam whistle on the carriage: the train's horn
    ctx.setBlock(at(bogey, 0, 2), "levitate:steam_whistle");
    cn::addGlue(base.dim, {{bogey.x, bogey.y, bogey.z}, {bogey.x, bogey.y + 2, bogey.z}});
    auto id = tn::assemble(*s, nullptr);
    ctx.require(id.has_value(), "the train assembles: " + s->lastError.dump());

    auto& player = ctx.player();
    net::setTestCompanion(player, true);
    (void)net::takeTestMessages(player);
    player.teleport(Vec3{float(track.x - 1) + .5f, float(track.y + 2), float(track.z) + 2.5f}, player.getDimensionId());
    co_await ctx.ticks(2);
    auto* t = tn::findTrain(*id);
    ctx.require(t && !t->carriages.empty() && !t->carriages[0].controls.empty(), "the train has controls");
    tn::startControlling(player, *t, 0, t->carriages[0].controls[0].first, 1);
    ctx.check(hasMessage(net::takeTestMessages(player), "trains.controls", true), "the client mod is told to capture the keys");

    double startX = core::trains::positionOf(tr::railways(), t->body.front)->x;
    for (int i = 0; i < 40; ++i) {
        if (i % 5 == 0) net::injectFromClient(player, "trains.interact", keys(true, false)); // keep-alive every 5 ticks
        co_await ctx.ticks(1);
    }
    t = tn::findTrain(*id);
    ctx.require(t != nullptr, "the train exists");
    ctx.check(t->speed > 0.15, "W from the client mod accelerates (" + str(t->speed) + ")");
    ctx.check(core::trains::positionOf(tr::railways(), t->body.front)->x > startX + 2, "the train moved east");
    bool hud = false;
    for (auto const& m : net::takeTestMessages(player)) hud = hud || m.channel == "trains.hud";
    ctx.check(hud, "the HUD gets speed and throttle");

    net::injectFromClient(player, "trains.throttle", {{"throttle", 0.5}});
    ctx.check(std::abs(t->throttle - 0.5) < 1e-9, "the mouse wheel sets the throttle, got " + str(t->throttle));
    net::injectFromClient(player, "trains.throttle", {{"throttle", 0.0}});
    ctx.check(std::abs(t->throttle - 1 / 18.0) < 1e-9, "at least one notch (1/18)");
    net::injectFromClient(player, "trains.throttle", {{"honk", true}});
    ctx.check(tn::honkTicksOf(*id) == 20, "the sprint key honks the whistle (20 ticks), got " + std::to_string(tn::honkTicksOf(*id)));
    net::injectFromClient(player, "trains.throttle", {{"honk", false}});
    ctx.check(tn::honkTicksOf(*id) == 6, "releasing it lets the horn fade (6 ticks)");

    double before = t->speed;
    net::injectFromClient(player, "trains.interact", keys(false, true));
    co_await ctx.ticks(1);
    t = tn::findTrain(*id);
    ctx.check(t && t->speed < before, "S brakes");
    net::injectFromClient(player, "trains.interact", {{"stop", true}});
    t = tn::findTrain(*id);
    ctx.check(t && t->controller.empty(), "Esc stops controlling");
    ctx.check(hasMessage(net::takeTestMessages(player), "trains.controls", false), "the client mod releases the keys");

    net::setTestCompanion(player, false);
    tn::removeTrain(*id);
    cn::clearGlue(base.dim, {base.x + 20, base.y, base.z}, 40);
    tn::removeStation(station);
    clearBox(at(base, -1, 0, -2), at(base, 60, 4, 2));
    co_await ctx.ticks(2);
});

} // namespace lv::feature::gaps
