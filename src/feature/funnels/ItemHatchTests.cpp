// Selftests of the item hatch (U49, testPos dx 4990..4999).

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace items  = lv::feature::common::items;
namespace common = lv::feature::common;
using Facing     = ScriptModuleMinecraft::ScriptFacing;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockPos  bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

/// Items of `id` in the container at `pos`.
int stored(Player& p, GlobalPos const& pos, std::string const& id) {
    auto& region = p.getDimensionBlockSource();
    int   n      = common::items::slotCount(region, bp(pos), -1);
    auto  h      = common::items::find(common::Access{region, bp(pos), -1});
    if (!h) return 0;
    int total = 0;
    for (int i = 0; i < n; ++i) {
        auto s = h->getSlot(common::Access{region, bp(pos), -1}, i);
        if (!items::isEmpty(s) && items::idOf(s) == id) total += items::countOf(s);
    }
    return total;
}

void clearInventory(Player& p) {
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, items::empty());
}

LV_REGISTER_SELFTEST("funnels.item_hatch", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4990), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    ctx.require(ctx.setBlock(base, "minecraft:stone"), "floor");
    auto chest = at(base, 0, 1, 0);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "a chest");
    auto hatch = at(base, 0, 1, -1); // on the chest's north side
    ctx.cleanupBlock(hatch);
    sp.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z - 3.5f}, DimensionType(0));
    co_await ctx.ticks(2);
    // the top of a block refuses a hatch
    auto floor2 = at(base, 2, 0, 0);
    ctx.require(ctx.setBlock(floor2, "minecraft:stone"), "a second floor block");
    ctx.cleanupBlock(at(floor2, 0, 1, 0));
    sp.setSelectedSlot(0);
    ctx.give(sp, item(ctx, "levitate:item_hatch"), 0);
    sp.simulateUseItemInSlotOnBlock(0, bp(floor2), Facing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(3);
    ctx.check(lv::game::blockIdAt(sp.getDimensionBlockSource(), bp(at(floor2, 0, 1, 0))) != "levitate:item_hatch", "nothing placed on a floor");
    // sneak-click the chest's north side (a plain click opens the chest)
    clearInventory(sp);
    ctx.give(sp, item(ctx, "levitate:item_hatch"), 0);
    sp.setSneaking(true);
    co_await ctx.ticks(1);
    sp.simulateUseItemInSlotOnBlock(0, bp(chest), Facing::North, Vec3{.5f, .5f, 0.f});
    co_await ctx.ticks(3);
    sp.setSneaking(false);
    auto& region = sp.getDimensionBlockSource();
    ctx.require(lv::game::blockIdAt(region, bp(hatch)) == "levitate:item_hatch", "the hatch is on the chest's side");
    ctx.check(lv::game::intState(region.getBlock(bp(hatch)), "levitate:facing").value_or(-1) == 3, "facing the chest (south)");

    // the held item goes in
    clearInventory(sp);
    sp.getInventory().setItem(0, item(ctx, "minecraft:apple", 10));
    sp.getInventory().setItem(1, item(ctx, "minecraft:apple", 5));
    sp.getInventory().setItem(12, item(ctx, "minecraft:bread", 7));
    sp.getInventory().setItem(13, item(ctx, "minecraft:white_shulker_box", 1));
    sp.setSelectedSlot(0);
    co_await ctx.ticks(1);
    sp.simulateUseItemInSlotOnBlock(0, bp(hatch), Facing::North, Vec3{.5f, .3f, .5f});
    co_await ctx.ticks(2);
    ctx.check(stored(sp, chest, "minecraft:apple") == 10, "the held apples are deposited");
    ctx.check(items::countOf(sp.getInventory().getItem(1)) == 5, "other hotbar slots stay");
    ctx.check(lv::game::intState(region.getBlock(bp(hatch)), "levitate:open").value_or(0) == 1, "the hatch opens");
    co_await ctx.ticks(12);
    ctx.check(lv::game::intState(region.getBlock(bp(hatch)), "levitate:open").value_or(1) == 0, "and closes 10 ticks later");

    // sneaking: everything outside the hotbar that passes the filter
    common::filterslot::set(hatch, item(ctx, "minecraft:bread"));
    sp.setSneaking(true);
    co_await ctx.ticks(1);
    sp.simulateUseItemInSlotOnBlock(1, bp(hatch), Facing::North, Vec3{.5f, .7f, .5f});
    co_await ctx.ticks(2);
    sp.setSneaking(false);
    ctx.check(stored(sp, chest, "minecraft:bread") == 7, "the inventory's bread is deposited");
    ctx.check(items::countOf(sp.getInventory().getItem(1)) == 5, "hotbar items stay");
    ctx.check(!items::isEmpty(sp.getInventory().getItem(13)), "the shulker box (filtered out, and never deposited) stays");
    common::filterslot::set(hatch, items::empty());

    // nothing to put in: the hatch stays shut
    sp.getInventory().setItem(13, items::empty());
    sp.setSneaking(true);
    co_await ctx.ticks(1);
    sp.simulateUseItemInSlotOnBlock(1, bp(hatch), Facing::North, Vec3{.5f, .7f, .5f});
    co_await ctx.ticks(2);
    sp.setSneaking(false);
    ctx.check(lv::game::intState(region.getBlock(bp(hatch)), "levitate:open").value_or(1) == 0, "nothing deposited: closed");
    clearInventory(sp);
});

} // namespace
