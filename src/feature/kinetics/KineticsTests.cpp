// Test-only kinetic blocks (levitate:kinetics_test_*, BP blocks of this unit) and the selftests of
// the kinetic network on a real world. The real shafts / cogs / generators belong to U6 / U7.
// Selftest positions: testPos dx 200..299 (U2).

#include <algorithm>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"

#include "feature/kinetics/KineticsInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::kinetics {

namespace {

KineticSpec testSource() {
    auto s                  = specs::generatorShaft(8); // like a hand crank: 8 SU per RPM
    s.initialGeneratedSpeed = [](BlockState const&) { return 16.f; };
    return s;
}

KineticSpec testConsumer() {
    auto s   = specs::shaft();
    s.impact = 4; // like a mixer: 4 SU per RPM
    return s;
}

} // namespace

LV_REGISTER_KINETIC_BLOCK("levitate:kinetics_test_shaft", specs::shaft());
LV_REGISTER_KINETIC_BLOCK("levitate:kinetics_test_cog", specs::cogwheel(false));
LV_REGISTER_KINETIC_BLOCK("levitate:kinetics_test_large_cog", specs::cogwheel(true));
LV_REGISTER_KINETIC_BLOCK("levitate:kinetics_test_source", testSource());
LV_REGISTER_KINETIC_BLOCK("levitate:kinetics_test_consumer", testConsumer());

namespace {

using selftest::Context;

constexpr char const* kShaft    = "levitate:kinetics_test_shaft";
constexpr char const* kCog      = "levitate:kinetics_test_cog";
constexpr char const* kLargeCog = "levitate:kinetics_test_large_cog";
constexpr char const* kSource   = "levitate:kinetics_test_source";
constexpr char const* kConsumer = "levitate:kinetics_test_consumer";
constexpr int         X = 0, Y = 1, Z = 2;

/// Positions of one test: `base` is the test's dx (200..299); x along dx, y and z offsets.
struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy;
        return p;
    }
    bool place(GlobalPos const& p, char const* id, int axis) { return ctx.setBlock(p, id, {{"levitate:axis", axis}}); }
};

std::string speedText(GlobalPos const& p) { return std::to_string(speedAt(p)); }

} // namespace

LV_REGISTER_SELFTEST("kinetics.registry", [](Context& ctx) {
    for (auto const* id : {kShaft, kCog, kLargeCog, kSource, kConsumer}) {
        ctx.check(specOf(id) != nullptr, std::string(id) + " is registered");
    }
    ctx.check(specOf("minecraft:stone") == nullptr, "vanilla blocks are not kinetic");
    ctx.check(stressCapacityOf("levitate:creative_motor") == 16384, "stress table: creative motor capacity");
    ctx.check(stressImpactOf("levitate:mechanical_press") == 8, "stress table: press impact");
    ctx.check(stressImpactOf(kConsumer) == 4, "spec default impact without a table entry");
    ctx.check(speedLevelOf(16) == SpeedLevel::Slow && speedLevelOf(-64) == SpeedLevel::Medium, "speed levels");
    ctx.require(game::blockWith(kShaft, {{"levitate:axis", 0}}) != nullptr, "test blocks are in the behavior pack");
});

LV_REGISTER_SELFTEST("kinetics.shaft_line", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 200};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(a.place(a.at(0), kSource, X), "place the source");
    for (int i = 1; i <= 3; ++i) a.place(a.at(i), kShaft, X);
    co_await ctx.ticks(2);
    for (int i = 0; i <= 3; ++i) ctx.check(speedAt(a.at(i)) == 16, "block " + std::to_string(i) + " turns at 16 RPM, got " + speedText(a.at(i)));
    auto n = nodeAt(a.at(3));
    ctx.check(n && n->source == a.at(2), "the last shaft is driven by its neighbour");
    ctx.check(networkMembers(a.at(1)).size() == 4, "one network of 4 blocks");
    auto net = networkAt(a.at(2));
    ctx.check(net && net->capacity == 8 * 16, "capacity 8 SU/RPM at 16 RPM");
    ctx.info("network of " + std::to_string(net ? net->members : 0) + " blocks at 16 RPM");
});

LV_REGISTER_SELFTEST("kinetics.cogs_mesh", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 210};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.place(a.at(0), kSource, Y);
    a.place(a.at(0, 1), kCog, Y);       // on the source's shaft
    a.place(a.at(1, 1), kCog, Y);       // meshes sideways
    a.place(a.at(0, 2), kLargeCog, Y);  // on top of the first cog
    a.place(a.at(1, 2, 1), kCog, Y);    // diagonal to the large cog
    co_await ctx.ticks(2);
    ctx.check(speedAt(a.at(0, 1)) == 16, "cog on the source: 16, got " + speedText(a.at(0, 1)));
    ctx.check(speedAt(a.at(1, 1)) == -16, "meshing cog turns the other way, got " + speedText(a.at(1, 1)));
    ctx.check(speedAt(a.at(0, 2)) == 16, "large cog on the shaft: 16, got " + speedText(a.at(0, 2)));
    ctx.check(speedAt(a.at(1, 2, 1)) == -32, "small cog at a large cog: -32, got " + speedText(a.at(1, 2, 1)));
});

LV_REGISTER_SELFTEST("kinetics.break_stops", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 220};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.place(a.at(0), kSource, X);
    for (int i = 1; i <= 4; ++i) a.place(a.at(i), kShaft, X);
    co_await ctx.ticks(2);
    ctx.require(speedAt(a.at(4)) == 16, "line turns");
    auto* region = detail::regionOf(0);
    ctx.require(region != nullptr, "overworld");
    game::clearBlock(*region, game::blockPos(a.at(2)));
    co_await ctx.ticks(2);
    ctx.check(speedAt(a.at(1)) == 16, "before the gap still turns");
    ctx.check(speedAt(a.at(3)) == 0 && speedAt(a.at(4)) == 0, "after the gap stops");
    ctx.check(!nodeAt(a.at(2)), "the broken shaft left the network");
    a.place(a.at(2), kShaft, X);
    co_await ctx.ticks(2);
    ctx.check(speedAt(a.at(4)) == 16, "closing the gap turns it again");
});

LV_REGISTER_SELFTEST("kinetics.conflict_breaks", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 230};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.place(a.at(0), kSource, X);
    a.place(a.at(1), kShaft, X);
    a.place(a.at(2), kShaft, X);
    a.place(a.at(4), kSource, X);
    co_await ctx.ticks(2);
    setGeneratedSpeed(a.at(4), -16);
    int breaks = 0;
    auto id    = onBreak([&](BreakEvent const& e) {
        if (e.pos == a.at(3)) ++breaks;
    });
    a.place(a.at(3), kShaft, X); // meets +16 and -16
    co_await ctx.ticks(3);
    removeListener(id);
    auto* region = detail::regionOf(0);
    ctx.check(breaks == 1, "the joining shaft breaks once");
    ctx.check(!nodeAt(a.at(3)), "it left the network");
    ctx.check(region && game::blockIdAt(*region, game::blockPos(a.at(3))) == "minecraft:air", "it is gone from the world");
    ctx.check(speedAt(a.at(2)) == 16 && speedAt(a.at(4)) == -16, "both sides keep turning");
});

LV_REGISTER_SELFTEST("kinetics.overstress", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 240};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.place(a.at(0), kSource, X); // 8 * 16 = 128 SU
    a.place(a.at(1), kConsumer, X);
    a.place(a.at(2), kConsumer, X); // 2 * 4 * 16 = 128 SU: at capacity
    co_await ctx.ticks(2);
    ctx.check(!isOverstressed(a.at(1)), "128 of 128 SU is fine");
    a.place(a.at(3), kConsumer, X); // 192 SU
    co_await ctx.ticks(2);
    ctx.check(isOverstressed(a.at(1)), "192 of 128 SU overstresses");
    ctx.check(speedAt(a.at(2)) == 0 && theoreticalSpeedAt(a.at(2)) == 16, "overstressed blocks stop");
    auto net = networkAt(a.at(1));
    ctx.check(net && net->stress == 192 && net->capacity == 128, "stress sums");
    game::clearBlock(*detail::regionOf(0), game::blockPos(a.at(3)));
    co_await ctx.ticks(2);
    ctx.check(!isOverstressed(a.at(1)) && speedAt(a.at(2)) == 16, "removing a consumer recovers");
});

LV_REGISTER_SELFTEST("kinetics.state_change", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 250};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.place(a.at(0), kSource, X);
    a.place(a.at(1), kShaft, X);
    a.place(a.at(2), kShaft, X);
    co_await ctx.ticks(2);
    a.place(a.at(1), kShaft, Z); // turned sideways
    co_await ctx.ticks(2);
    ctx.check(speedAt(a.at(1)) == 0 && speedAt(a.at(2)) == 0, "a sideways shaft disconnects");
    a.place(a.at(1), kShaft, X);
    co_await ctx.ticks(2);
    ctx.check(speedAt(a.at(2)) == 16, "turned back it connects again");
});

LV_REGISTER_SELFTEST("kinetics.persistence", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 260};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.place(a.at(0), kSource, X);
    a.place(a.at(1), kShaft, X);
    a.place(a.at(2), kConsumer, X);
    co_await ctx.ticks(2);
    setGeneratedSpeed(a.at(0), 24);
    setStressImpact(a.at(2), 2.0);
    detail::reloadFromDb(); // save, drop and load the whole network from the world DB
    auto n = nodeAt(a.at(2));
    ctx.check(n && n->speed == 24, "speed survives a reload");
    ctx.check(n && n->source == a.at(1), "source survives a reload");
    ctx.check(n && n->impact == 2.0, "stress override survives a reload");
    auto src = nodeAt(a.at(0));
    ctx.check(src && src->generatedSpeed == 24, "generated speed survives a reload");
    auto net = networkAt(a.at(1));
    ctx.check(net && net->stress == 2.0 * 24, "network stress is recomputed");
});

LV_REGISTER_SELFTEST("kinetics.listener", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area                 a{ctx, 270};
    std::vector<float>   seen;
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto id = onSpeedChanged(kShaft, [&](SpeedChange const& c) {
        if (c.pos == a.at(1)) seen.push_back(c.speed);
    });
    a.place(a.at(0), kSource, X);
    a.place(a.at(1), kShaft, X);
    co_await ctx.ticks(2);
    setGeneratedSpeed(a.at(0), -48);
    co_await ctx.ticks(2);
    removeListener(id);
    ctx.check(!seen.empty() && seen.front() == 16, "the listener sees the shaft start");
    ctx.check(!seen.empty() && seen.back() == -48, "and its new speed");
});

LV_REGISTER_SELFTEST("kinetics.describe_and_sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 280};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.place(a.at(0), kSource, X);
    a.place(a.at(1), kShaft, X);
    co_await ctx.ticks(2);
    auto lines = detail::describe(a.at(1), nullptr);
    bool speed = std::any_of(lines.begin(), lines.end(), [](std::string const& l) { return l.find("16 RPM") != std::string::npos; });
    ctx.check(speed, "the dump shows the speed");
    for (auto const& l : lines) ctx.info(l);
    auto        payloads = detail::fullSyncPayloads(0);
    auto        p        = a.at(1);
    std::string needle   = std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z) + ",16,";
    bool        found    = std::any_of(payloads.begin(), payloads.end(), [&](std::string const& s) {
        return s.find(needle) != std::string::npos;
    });
    ctx.check(found, "the full sync carries the shaft's speed");
});

} // namespace lv::feature::kinetics
