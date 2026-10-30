// Selftests of the U49 transmission additions: cuckoo clocks and the flywheel (testPos dx
// 4930..4949 and the surprise at 4959; dz -2..0). The clock reads a pinned day time
// (setClockDayTimeOverride) so the tests don't depend on the world's time.

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/advancements/Advancements.h"
#include "feature/display/Display.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::transmission {

namespace {

using selftest::Context;

constexpr char kSource[] = "levitate:kinetics_test_source";

BlockSource& world() { return *regionOf(0); }
int          st(GlobalPos const& p, char const* s) { return stateAt(world(), p, s, -1); }
GlobalPos    at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

/// A clock facing north (front -z) driven by a test source behind it (south, axis z).
ll::coro::CoroTask<GlobalPos> poweredClock(Context& ctx, int dx, char const* id) {
    auto p = ctx.testPos(dx, -1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, id, {{kHorizontalFacing, 2}}), std::string(id) + " placed");
    ctx.setBlock(at(p, 0, 0, 1), kSource, {{kAxis, 2}});
    co_await ctx.ticks(3);
    co_return p;
}

struct TimeGuard {
    ~TimeGuard() { setClockDayTimeOverride(std::nullopt); }
};

} // namespace

LV_REGISTER_SELFTEST("transmission.cuckoo_clock_hands", [](Context& ctx) -> ll::coro::CoroTask<> {
    TimeGuard guard;
    setClockDayTimeOverride(3000); // 9:00
    auto p = co_await poweredClock(ctx, 4930, kCuckooClock);
    ctx.check(kinetics::speedAt(p) == 16, "the clock turns with the shaft at its back, got " + std::to_string(kinetics::speedAt(p)));
    ctx.check(kinetics::stressImpactOf(kCuckooClock) == 1, "1 SU per RPM");
    co_await ctx.ticks(2);
    ctx.check(st(p, kHour) == 9 && st(p, kMinute) == 0, "9:00: hour 9 minute 0, got " + std::to_string(st(p, kHour)) + " / " + std::to_string(st(p, kMinute)));
    setClockDayTimeOverride(9500); // 15:30
    co_await ctx.ticks(2);
    ctx.check(st(p, kHour) == 3 && st(p, kMinute) == 6, "15:30: hour 3 minute 6, got " + std::to_string(st(p, kHour)) + " / " + std::to_string(st(p, kMinute)));
    // a clock standing still keeps its hands (Create: no tick without speed)
    auto q = at(p, 2, 0, 0);
    ctx.setBlock(q, kCuckooClock, {{kHorizontalFacing, 2}, {kHour, 1}, {kMinute, 1}});
    setClockDayTimeOverride(6000);
    co_await ctx.ticks(4);
    ctx.check(st(q, kHour) == 1 && st(q, kMinute) == 1, "an unpowered clock's hands stay");
    ctx.check(clockAnimation(q).first == 0, "an unpowered clock doesn't cuckoo at noon");
    // the display unit's sources (Create's TimeOfDayDisplaySource / StopwatchDisplaySource)
    auto textOf = [&](GlobalPos const& at, char const* id) -> std::string {
        for (auto const* s : display::sourcesAt(world(), at)) {
            if (s->id != id || !s->text) continue;
            nlohmann::json   cfg = nlohmann::json::object();
            display::Context dc{world(), at, at, at, cfg, 0};
            auto             lines = s->text(dc, display::TargetStats{});
            return lines.empty() ? std::string("(empty)") : lines.front();
        }
        return "(no source)";
    };
    auto shown = textOf(p, "time_of_day");
    ctx.check(shown != "(no source)" && shown != "--:--", "a turning clock shows the time of day, got " + shown);
    ctx.check(textOf(p, "stopwatch") != "(no source)", "and offers the stopwatch");
    ctx.check(textOf(q, "time_of_day") == "--:--", "a still clock shows --:--, got " + textOf(q, "time_of_day"));
    // placement: the back goes to a shaft facing the new clock
    auto c = at(p, 4, 0, 0);
    ctx.setBlock(at(c, 1, 0, 0), kShaft, {{kAxis, 0}});
    PlaceContext pc;
    pc.clickedPos = at(c, 0, -1, 0);
    pc.face       = Face::Up;
    pc.pos        = c;
    auto states   = stateForPlacement(world(), kCuckooClock, pc, ctx.player());
    int  facing   = -1;
    if (states)
        for (auto const& [k, v] : *states)
            if (k == std::string_view(kHorizontalFacing)) facing = v;
    ctx.check(facing == 1, "a shaft east of it: the clock faces west (1), got " + std::to_string(facing));
});

LV_REGISTER_SELFTEST("transmission.cuckoo_clock_noon", [](Context& ctx) -> ll::coro::CoroTask<> {
    TimeGuard guard;
    setClockDayTimeOverride(6000); // noon: the pig
    auto p = co_await poweredClock(ctx, 4935, kCuckooClock);
    co_await ctx.ticks(2);
    auto [anim, progress] = clockAnimation(p);
    ctx.check(anim == 1, "noon starts the pig animation, got " + std::to_string(anim));
    // the doors open around progress 25..40
    for (int i = 0; i < 40 && clockAnimation(p).second < 31; ++i) co_await ctx.ticks(1);
    ctx.check(st(p, kCuckoo) == 1, "the pig is out at progress 31, cuckoo state " + std::to_string(st(p, kCuckoo)));
    setClockDayTimeOverride(7000); // 13:00: nothing starts again
    for (int i = 0; i < 90 && clockAnimation(p).first != 0; ++i) co_await ctx.ticks(1);
    ctx.check(clockAnimation(p).first == 0, "the animation ends after progress 100");
    co_await ctx.ticks(1);
    ctx.check(st(p, kCuckoo) == 0, "the doors are shut again");

    // dusk: the creeper, and Create's "Is it Time?" for the owner nearby
    auto& player = ctx.player();
    advancements::revoke(player, "cuckoo_clock");
    advancements::setOwner(p, player);
    setClockDayTimeOverride(12550); // 18:33
    co_await ctx.ticks(3);
    ctx.check(clockAnimation(p).first == 2, "dusk starts the creeper animation, got " + std::to_string(clockAnimation(p).first));
    ctx.check(advancements::has(player, "cuckoo_clock"), "the owner nearby gets the cuckoo_clock advancement");
});

LV_REGISTER_SELFTEST("transmission.flywheel", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4940, -1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kSource, {{kAxis, 0}});
    ctx.require(ctx.setBlock(at(p, 1, 0, 0), kFlywheel, {{kAxis, 0}}), "flywheel placed");
    ctx.setBlock(at(p, 2, 0, 0), kShaft, {{kAxis, 0}});
    ctx.setBlock(at(p, 1, 0, -1), kFlywheel, {{kAxis, 2}}); // beside it, across: not connected
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(at(p, 1, 0, 0)) == 16, "the flywheel spins at 16 RPM, got " + std::to_string(kinetics::speedAt(at(p, 1, 0, 0))));
    ctx.check(kinetics::speedAt(at(p, 2, 0, 0)) == 16, "and passes it on along its shaft");
    ctx.check(kinetics::speedAt(at(p, 1, 0, -1)) == 0, "a flywheel on another axis next to it stays still");
    ctx.check(kinetics::stressImpactOf(kFlywheel) == 0, "Create: no stress impact");
});

LV_REGISTER_SELFTEST("transmission.cuckoo_clock_surprise", [](Context& ctx) -> ll::coro::CoroTask<> {
    TimeGuard guard;
    setClockDayTimeOverride(6000);
    auto p = ctx.testPos(4959, -1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(at(p, 0, 0, 1), kSource, {{kAxis, 2}});
    ctx.setBlock(at(p, 2, 0, 0), "minecraft:oak_planks"); // in the blast
    ctx.require(ctx.setBlock(p, kMysteriousCuckooClock, {{kHorizontalFacing, 2}}), "mysterious clock placed");
    // keep the test player out of the blast
    auto& player = ctx.player();
    player.teleport(Vec3{static_cast<float>(p.x) - 14.5f, static_cast<float>(p.y) + 1.f, static_cast<float>(p.z) + .5f}, DimensionType(0));
    co_await ctx.ticks(3);
    ctx.check(clockAnimation(p).first == 3, "a mysterious clock's noon is the surprise, got " + std::to_string(clockAnimation(p).first));
    for (int i = 0; i < 70 && game::intState(world().getBlock(game::blockPos(p)), kHorizontalFacing).has_value(); ++i) co_await ctx.ticks(1);
    ctx.check(world().getBlock(game::blockPos(p)).getTypeName() != kMysteriousCuckooClock, "the clock is gone at progress 50");
    ctx.check(world().getBlock(game::blockPos(at(p, 2, 0, 0))).getTypeName() != "minecraft:oak_planks", "the explosion broke the planks next to it");
    markSurpriseForTest();
    ctx.check(surpriseDeathMessage("Steve") == "death.attack.create.cuckoo_surprise", "explosion deaths get Create's message, got " + surpriseDeathMessage("Steve"));
});

} // namespace lv::feature::transmission
