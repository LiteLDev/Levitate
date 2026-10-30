// Selftests of the U49 redstone additions: desk bell and rose quartz lamp (testPos dx 4945..4953,
// dz -2..0).

#include <string>

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/redstone/Internal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::redstone {

namespace {

using lv::selftest::Context;

GP           at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }
Block const& blockAt(Context& ctx, GP const& p) { return regionOf(ctx).getBlock(game::blockPos(p)); }
int          st(Context& ctx, GP const& p, char const* name) { return state(blockAt(ctx, p), name, -1); }
int          wire(Context& ctx, GP const& p) { return state(blockAt(ctx, p), "redstone_signal", -1); }

bool placeVanilla(Context& ctx, GP const& p, std::string const& id, Json const& states) {
    auto const* b = vanillaBlock(id, states);
    if (!b) return false;
    ctx.cleanupBlock(p);
    return game::setBlock(regionOf(ctx), game::blockPos(p), *b);
}

} // namespace

LV_REGISTER_SELFTEST("redstone.desk_bell", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4945, -1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(game::blockWith(kDeskBell) != nullptr, "levitate:desk_bell is registered");
    ctx.setBlock(at(p, 0, -1, 0), "minecraft:stone");
    ctx.require(ctx.setBlock(p, kDeskBell, {{sFacing, kUp}}), "bell placed");
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 0, "a bell starts unpowered");
    auto&     sp = static_cast<SimulatedPlayer&>(ctx.player());
    ItemStack none;
    int       rings = deskBellRings();
    ctx.check(use(Click{sp, p, kUp, {0.5, 0.5, 0.5}, none, false}, blockAt(ctx, p), Kind::DeskBell), "a click rings the bell");
    ctx.check(deskBellRings() == rings + 1, "one ring");
    ctx.check(st(ctx, p, sPowered) == 1, "ringing powers it");
    co_await ctx.ticks(3);
    ctx.check(wire(ctx, at(p, 1, 0, 0)) == 15, "the bell powers the dust beside it, got " + std::to_string(wire(ctx, at(p, 1, 0, 0))));
    co_await ctx.ticks(20);
    ctx.check(st(ctx, p, sPowered) == 0, "20 ticks later it is released");
    co_await ctx.ticks(3);
    ctx.check(wire(ctx, at(p, 1, 0, 0)) == 0, "and the dust goes dark, got " + std::to_string(wire(ctx, at(p, 1, 0, 0))));
    auto stick = common::items::make("minecraft:stick");
    ctx.check(use(Click{sp, p, kUp, {0.5, 0.5, 0.5}, *stick, false}, blockAt(ctx, p), Kind::DeskBell), "any item rings it too");
    ctx.check(!use(Click{sp, p, kUp, {0.5, 0.5, 0.5}, *stick, true}, blockAt(ctx, p), Kind::DeskBell), "sneaking with an item doesn't");
    // the wrench turns it
    auto const* h = goggles::wrench::handlerOf(kDeskBell);
    ctx.check(h != nullptr, "the bell is wrenchable");
});

LV_REGISTER_SELFTEST("redstone.rose_quartz_lamp", [](Context& ctx) -> ll::coro::CoroTask<> {
    // three lamps in a row along x (a, b, c)
    auto a = ctx.testPos(4948, -1);
    ctx.require(co_await ctx.ensureLoaded(a), "test area loaded");
    ctx.require(game::blockWith(kRoseQuartzLamp) != nullptr, "levitate:rose_quartz_lamp is registered");
    auto b = at(a, 1, 0, 0), c = at(a, 2, 0, 0);
    for (auto const& q : {a, b, c}) ctx.setBlock(q, kRoseQuartzLamp);
    co_await ctx.ticks(3);
    ctx.check(st(ctx, a, sPowering) == 0 && st(ctx, c, sPowering) == 0, "lamps start dark");
    // a signal into a lights it, and it stays lit without the signal
    ctx.setBlock(at(a, 0, 0, -1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, a, sPowering) == 1 && st(ctx, a, sPowered) == 1, "a lights up");
    game::clearBlock(regionOf(ctx), game::blockPos(at(a, 0, 0, -1)));
    co_await ctx.ticks(4);
    ctx.check(st(ctx, a, sPowering) == 1 && st(ctx, a, sPowered) == 0, "a stays lit after the signal is gone");
    // a signal into c moves the light to c
    ctx.setBlock(at(c, 0, 0, -1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, c, sPowering) == 1, "c lights up");
    ctx.check(st(ctx, a, sPowering) == 0 && st(ctx, b, sPowering) == 0, "the rest of the cluster goes dark");
    ctx.check(st(ctx, c, sActivate) == 0, "the activation is done");
    // output: 15 to dust, nothing into other lamps, the distance to a comparator
    ctx.setBlock(at(c, 0, 0, 1), "minecraft:redstone_wire");
    co_await ctx.ticks(4);
    ctx.check(wire(ctx, at(c, 0, 0, 1)) == 15, "a lit lamp powers dust, got " + std::to_string(wire(ctx, at(c, 0, 0, 1))));
    auto& region = regionOf(ctx);
    ctx.check(lampSignal(region, game::blockPos(c), blockAt(ctx, c), kWest) == 0, "nothing into the lamp next to it");
    ctx.require(placeVanilla(ctx, at(a, -1, 0, 0), "minecraft:unpowered_comparator", {{"minecraft:cardinal_direction", "east"}}), "comparator placed");
    co_await ctx.ticks(2);
    auto out = lampComparatorOutput(region, game::blockPos(a));
    ctx.check(out && *out == 13, "the comparator reads 15 minus the distance to the lit lamp: 13, got " + (out ? std::to_string(*out) : std::string("none")));
    ctx.info("comparator: " + circuitDebug(region, game::blockPos(at(a, -1, 0, 0))));
    // the wrench toggles a lamp
    auto const* h = goggles::wrench::handlerOf(kRoseQuartzLamp);
    ctx.require(h && h->rotate, "the lamp has a wrench rotation");
    auto const* lit = h->rotate(blockAt(ctx, b), 1);
    ctx.check(lit && state(*lit, sPowering) == 1, "wrenching a dark lamp lights it");
});

} // namespace lv::feature::redstone
