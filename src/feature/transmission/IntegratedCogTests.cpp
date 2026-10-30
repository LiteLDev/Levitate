// Selftests of the cog placement helpers on blocks with an integrated cog (U50, testPos dx
// 5000..5099): Create's CogwheelBlockItem.onItemUseFirst on a mechanical mixer, a mechanical
// crafter and a mechanical arm, through the shared interaction dispatch (so the helper's order
// before the clicked block's own use is tested too), and the network speed of the new cog.
// Rotation comes from U2's test generator (levitate:kinetics_test_source, 16 RPM).

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::transmission {

namespace {

using selftest::Context;
namespace items = feature::common::items;
namespace ia    = feature::common::interactions;

constexpr char kSource[]  = "levitate:kinetics_test_source";
constexpr char kMixer[]   = "levitate:mechanical_mixer";   // U14
constexpr char kCrafter[] = "levitate:mechanical_crafter"; // U16
constexpr char kArm[]     = "levitate:mechanical_arm";     // U15
constexpr int  X = 0, Y = 1, Z = 2;

BlockSource& world() { return *regionOf(0); }

GlobalPos at(Context& ctx, int dx, int dy, int dz) {
    auto p = ctx.testPos(5000 + dx, dz);
    p.y += dy;
    return p;
}

/// A right-click with `stack` on `pos` as the GameMode hook dispatches it (not sneaking, main hand).
ia::Result click(Context& ctx, ItemStack& stack, GlobalPos const& pos, Face face, ia::Vec hit) {
    auto&       player  = ctx.player();
    auto const& block   = world().getBlock(game::blockPos(pos));
    std::string blockId = idAt(world(), pos);
    std::string itemId  = items::idOf(stack);
    ia::UseOnContext c{player, stack, itemId, block, blockId, pos, static_cast<int>(face), hit, false, true};
    return ia::dispatchUseOn(c);
}

float speed(GlobalPos const& p) { return kinetics::speedAt(p); }
int   axisAt(GlobalPos const& p) { return stateAt(world(), p, kAxis, -1); }

} // namespace

LV_REGISTER_SELFTEST("transmission.integrated_cog_mixer", [](Context& ctx) -> ll::coro::CoroTask<> {
    ctx.require(game::blockWith(kMixer) != nullptr, "the mechanical mixer is registered");
    ctx.require(co_await ctx.ensureLoaded(at(ctx, 0, 0, 0)), "test area loaded");
    ctx.player().setPlayerGameType(GameType::Creative);
    // source (y) -> cogwheel (y) -> mixer east of it
    auto src = at(ctx, 0, 0, 0), cog = at(ctx, 0, 1, 0), mixer = at(ctx, 1, 1, 0);
    ctx.setBlock(src, kSource, {{kAxis, Y}});
    ctx.setBlock(cog, kCogwheel, {{kAxis, Y}});
    ctx.setBlock(mixer, kMixer);
    co_await ctx.ticks(4);
    ctx.check(speed(mixer) == -16.f, "the mixer meshes with the cogwheel beside it: " + std::to_string(speed(mixer)));

    // a large cogwheel on the mixer's east face, nearer its south half: DiagonalCogHelper puts it
    // east then south of the mixer, around the mixer's axis (y)
    auto large = items::make(kLargeCogwheel);
    ctx.require(large.has_value(), "large cogwheel item");
    auto target = at(ctx, 2, 1, 1);
    ctx.check(click(ctx, *large, mixer, Face::East, {1., .5, .7}) == ia::Result::Consume, "the large cog helper takes the click");
    ctx.check(idAt(world(), target) == kLargeCogwheel && axisAt(target) == Y, "large cogwheel placed diagonal to the mixer, axis y");
    ctx.cleanupBlock(target);
    co_await ctx.ticks(4);
    ctx.check(speed(target) == 8.f, "the large cog turns at -1/2 of the mixer: " + std::to_string(speed(target)));

    // a small cogwheel on the mixer's south face: beside it on that side, axis y
    co_await ctx.ticks(6); // one assisted use per 5 ticks
    auto small  = items::make(kCogwheel);
    auto beside = at(ctx, 1, 1, 1);
    ctx.require(small.has_value(), "cogwheel item");
    // the spot south of the mixer touches the large cog (east of it): isValidCogwheelPosition
    // refuses it, the next nearest side (north) is taken
    ctx.check(click(ctx, *small, mixer, Face::South, {.5, .5, 1.}) == ia::Result::Consume, "the small cog helper takes the click");
    ctx.check(idAt(world(), beside) != kCogwheel, "no small cog beside the large cog");
    auto north = at(ctx, 1, 1, -1);
    ctx.check(idAt(world(), north) == kCogwheel && axisAt(north) == Y, "small cogwheel placed north of the mixer, axis y");
    ctx.cleanupBlock(north);
    co_await ctx.ticks(4);
    ctx.check(speed(north) == 16.f, "the small cog turns opposite the mixer: " + std::to_string(speed(north)));

    // the top of the mixer (on its axis): no helper, the click passes to the plain placement
    co_await ctx.ticks(6);
    ctx.check(!cogItemUseFirst(ctx.player(), *small, mixer, .5, 1., .5), "no helper on the mixer's shaft (top centre)");
    ctx.player().setPlayerGameType(GameType::Survival);
});

LV_REGISTER_SELFTEST("transmission.integrated_cog_crafter", [](Context& ctx) -> ll::coro::CoroTask<> {
    ctx.require(game::blockWith(kCrafter) != nullptr, "the mechanical crafter is registered");
    ctx.require(co_await ctx.ensureLoaded(at(ctx, 20, 0, 0)), "test area loaded");
    ctx.player().setPlayerGameType(GameType::Survival);
    // crafter facing north (axis z), driven from behind (south) by a source along z
    auto crafter = at(ctx, 20, 1, 0), src = at(ctx, 20, 1, 1);
    ctx.setBlock(crafter, kCrafter, {{kFacing, static_cast<int>(Face::North)}});
    ctx.setBlock(src, kSource, {{kAxis, Z}});
    co_await ctx.ticks(4);
    ctx.check(speed(crafter) == 16.f, "the crafter turns with the source: " + std::to_string(speed(crafter)));

    // a cogwheel on the crafter's east side: beside it, on the crafter's axis (z)
    auto cog = items::make(kCogwheel);
    ctx.require(cog.has_value(), "cogwheel item");
    auto east = at(ctx, 21, 1, 0);
    ctx.check(click(ctx, *cog, crafter, Face::East, {1., .5, .5}) == ia::Result::Consume, "the cog helper takes the click");
    ctx.check(idAt(world(), east) == kCogwheel && axisAt(east) == Z, "cogwheel placed east of the crafter, axis z");
    ctx.check(items::isEmpty(*cog), "the placed cog used the item (survival)");
    ctx.cleanupBlock(east);
    co_await ctx.ticks(4);
    ctx.check(speed(east) == -16.f, "the cog turns opposite the crafter: " + std::to_string(speed(east)));

    // the crafter's front off its centre: the helper runs before the crafter takes the item
    co_await ctx.ticks(6);
    cog      = items::make(kCogwheel);
    auto west = at(ctx, 19, 1, 0);
    ctx.check(click(ctx, *cog, crafter, Face::North, {.1, .5, 0.}) == ia::Result::Consume, "the helper takes a click on the front's rim");
    ctx.check(idAt(world(), west) == kCogwheel && axisAt(west) == Z, "cogwheel placed west of the crafter (nearest the hit), axis z");
    ctx.cleanupBlock(west);

    // the front's centre is the shaft: no helper, the crafter's own use takes the cog into its slot
    co_await ctx.ticks(6);
    cog = items::make(kCogwheel);
    ctx.check(click(ctx, *cog, crafter, Face::North, {.5, .5, 0.}) == ia::Result::Consume, "the crafter takes the click");
    ctx.check(items::isEmpty(*cog), "the crafter took the cogwheel into its slot");
    ctx.check(idAt(world(), at(ctx, 20, 2, 0)) != kCogwheel && idAt(world(), at(ctx, 20, 1, -1)) != kCogwheel,
              "and no cogwheel was placed");
});

LV_REGISTER_SELFTEST("transmission.integrated_cog_arm", [](Context& ctx) -> ll::coro::CoroTask<> {
    ctx.require(game::blockWith(kArm) != nullptr, "the mechanical arm is registered");
    ctx.require(co_await ctx.ensureLoaded(at(ctx, 40, 0, 0)), "test area loaded");
    ctx.player().setPlayerGameType(GameType::Creative);
    // Create's ArmBlock is an ICogWheel: shaft below, a small cog on its base
    auto arm = at(ctx, 40, 1, 0), src = at(ctx, 40, 0, 0);
    ctx.setBlock(src, kSource, {{kAxis, Y}});
    ctx.setBlock(arm, kArm);
    co_await ctx.ticks(4);
    ctx.check(speed(arm) == 16.f, "the arm turns with the shaft below: " + std::to_string(speed(arm)));
    auto cog = items::make(kCogwheel);
    ctx.require(cog.has_value(), "cogwheel item");
    auto west = at(ctx, 39, 1, 0);
    ctx.check(click(ctx, *cog, arm, Face::West, {0., .5, .5}) == ia::Result::Consume, "the cog helper takes the click");
    ctx.check(idAt(world(), west) == kCogwheel && axisAt(west) == Y, "cogwheel placed west of the arm, axis y");
    ctx.cleanupBlock(west);
    co_await ctx.ticks(4);
    ctx.check(speed(west) == -16.f, "the cog meshes with the arm's cog: " + std::to_string(speed(west)));
    ctx.player().setPlayerGameType(GameType::Survival);
});

} // namespace lv::feature::transmission
