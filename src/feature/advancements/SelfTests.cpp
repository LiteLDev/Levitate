// Selftests of the advancements unit (U45, testPos dx 4500..4549).

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "core/advancements/Advancements.h"
#include "feature/advancements/Advancements.h"
#include "feature/advancements/Internal.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "selftest/SelfTest.h"

namespace {

/// testPos puts dx 45xx 180 blocks south, outside the area the selftest run keeps ticking (z +-64):
/// U45's tests move 240 blocks north, to rows no unit uses (z -62..-58), so entities there tick.
constexpr int kU45Shift = -240;

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace adv   = lv::feature::advancements;
namespace ca    = lv::core::advancements;
namespace items = lv::feature::common::items;
namespace net   = lv::feature::net;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

void clearInventory(Player& p) {
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, items::empty());
    p.setOffhandSlot(items::empty());
}

void forget(Player& p) {
    for (auto const& d : ca::all()) adv::revoke(p, d.id);
}

LV_REGISTER_SELFTEST("advancements.award_toast_chat", [](Context& ctx) {
    auto& p = ctx.player();
    forget(p);
    ctx.check(!adv::has(p, "belt"), "a new player has no advancements");
    net::setTestCompanion(p, true);
    (void)net::takeTestMessages(p);
    ctx.check(adv::award(p, "belt"), "belt is awarded");
    ctx.check(adv::has(p, "belt"), "the player has it");
    ctx.check(!adv::award(p, "belt"), "a second award changes nothing");
    ctx.check(!adv::award(p, "no_such_advancement"), "unknown ids are refused");
    bool toast = false;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "advancements.toast" && m.payload.value("id", std::string()) == "belt") toast = true;
    ctx.check(toast, "the client mod gets a toast");
    // Create's SILENT entries show no toast
    ctx.check(adv::award(p, "copper"), "copper is awarded");
    bool silent = true;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "advancements.toast") silent = false;
    ctx.check(silent, "a silent advancement has no toast");
    net::setTestCompanion(p, false);
    auto done = adv::doneOf(p);
    ctx.check(done.size() == 2 && done[0] == "belt" && done[1] == "copper", "doneOf lists them in Create's order");
    ctx.check(adv::revoke(p, "belt") && !adv::has(p, "belt"), "revoke takes it away");
    forget(p);
});

LV_REGISTER_SELFTEST("advancements.inventory_criteria", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    forget(p);
    clearInventory(p);
    co_await ctx.ticks(25);
    ctx.check(adv::has(p, "root"), "the root is awarded for free");
    ctx.check(!adv::has(p, "andesite_alloy"), "no andesite alloy yet");
    ctx.give(p, item(ctx, "levitate:andesite_alloy"));
    ctx.give(p, item(ctx, "levitate:wrench"));
    co_await ctx.ticks(25);
    ctx.check(adv::has(p, "andesite_alloy"), "collecting andesite alloy awards Sturdier Rocks");
    ctx.check(!adv::has(p, "wrench_goggles"), "a wrench alone is not Kitted Out");
    ctx.give(p, item(ctx, "levitate:goggles"));
    co_await ctx.ticks(25);
    ctx.check(adv::has(p, "wrench_goggles"), "wrench and goggles award Kitted Out");
    p.setOffhandSlot(item(ctx, "levitate:extendo_grip"));
    co_await ctx.ticks(25);
    ctx.check(adv::has(p, "extendo_grip") && !adv::has(p, "extendo_grip_dual"), "one extendo grip in hand");
    p.getInventory().setItem(p.getSelectedItemSlot(), item(ctx, "levitate:extendo_grip"));
    co_await ctx.ticks(25);
    ctx.check(adv::has(p, "extendo_grip_dual"), "two extendo grips: To Full Extent");
    clearInventory(p);
    forget(p);
});

LV_REGISTER_SELFTEST("advancements.casing_click", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4500, kU45Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    forget(sp);
    clearInventory(sp);
    ctx.require(ctx.setBlock(base, "minecraft:stripped_oak_log"), "a stripped log");
    sp.teleport(Vec3{base.x + 2.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.give(sp, item(ctx, "levitate:andesite_alloy", 4), 0);
    sp.simulateUseItemInSlotOnBlock(0, BlockPos{base.x, base.y, base.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(5);
    ctx.require(adv::blockIdAt(base) == "levitate:andesite_casing", "the log became an andesite casing");
    ctx.check(adv::has(sp, "andesite_casing"), "The Andesite Age is awarded to the player");
    clearInventory(sp);
    forget(sp);
});

LV_REGISTER_SELFTEST("advancements.owner_and_speed", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4505, kU45Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    forget(sp);
    clearInventory(sp);
    // a backtank placed by the player on a shaft end: its owner gets "Pressure to Go" once it turns
    ctx.require(ctx.setBlock(base, "levitate:kinetics_test_source", {{"levitate:axis", 1}}), "source below");
    auto top = at(base, 0, 1, 0);
    ctx.cleanupBlock(top);
    sp.teleport(Vec3{base.x + 2.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    lv::kinetics::setGeneratedSpeed(base, 0);
    co_await ctx.ticks(2);
    ctx.give(sp, item(ctx, "levitate:copper_backtank"), 0);
    sp.simulateUseItemInSlotOnBlock(0, BlockPos{base.x, base.y, base.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(4);
    ctx.require(adv::blockIdAt(top) == "levitate:copper_backtank", "the backtank is placed");
    ctx.check(adv::ownerOf(top) == sp.getUuid().asString(), "the placer owns its advancements");
    ctx.check(!adv::has(sp, "backtank"), "not turning yet");
    lv::kinetics::setGeneratedSpeed(base, 64);
    co_await ctx.ticks(5);
    ctx.check(adv::has(sp, "backtank"), "a turning backtank awards Pressure to Go");
    co_await ctx.ticks(1);
    ctx.check(adv::ownerOf(top).empty(), "the owner is forgotten once every advancement of the block is done");
    lv::kinetics::setGeneratedSpeed(base, 0);
    // awardOwner of a block without an owner does nothing
    adv::awardOwner(top, "backtank");
    clearInventory(sp);
    forget(sp);
});

LV_REGISTER_SELFTEST("advancements.shifting_gears", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4510, kU45Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    forget(sp);
    clearInventory(sp);
    // a large cogwheel (axis y) and a small one placed diagonally next to it
    ctx.require(ctx.setBlock(base, "minecraft:stone"), "floor");
    ctx.require(ctx.setBlock(at(base, 1, 0, 0), "minecraft:stone"), "floor");
    ctx.require(ctx.setBlock(at(base, 0, 1, 0), "levitate:large_cogwheel", {{"levitate:axis", 1}}), "large cogwheel");
    auto smallAt = at(base, 1, 1, 1);
    ctx.require(ctx.setBlock(at(base, 1, 0, 1), "minecraft:stone"), "floor under the small cog");
    ctx.cleanupBlock(smallAt);
    sp.teleport(Vec3{base.x + 3.5f, float(base.y + 1), base.z + 3.5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.give(sp, item(ctx, "levitate:cogwheel"), 0);
    sp.simulateUseItemInSlotOnBlock(0, BlockPos{smallAt.x, smallAt.y - 1, smallAt.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(4);
    ctx.require(adv::blockIdAt(smallAt) == "levitate:cogwheel", "the small cogwheel is placed");
    ctx.check(adv::has(sp, "shifting_gears"), "meshing a small and a large cogwheel awards Shifting Gears");
    clearInventory(sp);
    forget(sp);
});

} // namespace
