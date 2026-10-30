// Selftests of the equipment unit's U45 rows (testPos dx 4570..4589): the tree fertilizer's dream
// world, the toolbox lid while the screen is open, dyeing a toolbox in the crafting grid.

#include <string>

#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/phys/AABB.h"

#include "core/equipment/Toolbox.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace eq    = lv::feature::equipment;
namespace ce    = lv::core::equipment;
namespace items = lv::feature::common::items;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

void clearInventory(Player& p) {
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, items::empty());
}

/// Clears a tree's space around `c` (and the dropped items) before and after a test.
void clearTreeSpace(Player& p, GlobalPos const& c) {
    for (int y = 0; y <= 24; ++y)
        for (int x = -10; x <= 10; ++x)
            for (int z = -10; z <= 10; ++z) {
                auto q = at(c, x, y, z);
                if (eq::idAt(q) != "minecraft:air" && !eq::idAt(q).empty()) eq::clearAt(q);
            }
    auto& region = p.getDimensionBlockSource();
    AABB  box{Vec3{float(c.x - 11), float(c.y - 4), float(c.z - 11)}, Vec3{float(c.x + 12), float(c.y + 26), float(c.z + 12)}};
    for (auto* e : region.getEntities(box, 0.0f))
        if (e && e->isType(ActorType::ItemEntity)) e->remove();
}

int countIn(GlobalPos const& c, std::string const& suffix) {
    int n = 0;
    for (int y = 0; y <= 24; ++y)
        for (int x = -7; x <= 7; ++x)
            for (int z = -7; z <= 7; ++z)
                if (eq::idAt(at(c, x, y, z)).ends_with(suffix)) ++n;
    return n;
}

} // namespace

// Create's TreesDreamWorld: the tree grows without room; logs never replace unbreakable blocks,
// leaves never replace solid blocks.
LV_REGISTER_SELFTEST("equipment.tree_fertilizer_no_room", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto sap = at(ctx.testPos(4575), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(sap), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    clearTreeSpace(p, sap);
    ctx.require(ctx.setBlock(at(sap, 0, -1, 0), "minecraft:dirt"), "dirt");
    ctx.require(ctx.setBlock(sap, "minecraft:oak_sapling"), "oak sapling");
    // no room: bedrock right above the sapling (Bedrock's own growth fails), stone in the crown
    ctx.require(ctx.setBlock(at(sap, 0, 1, 0), "minecraft:bedrock"), "bedrock above");
    ctx.require(ctx.setBlock(at(sap, 1, 4, 0), "minecraft:stone"), "stone in the crown");
    auto fert = item(ctx, "levitate:tree_fertilizer", 2);
    ctx.check(eq::useTreeFertilizer(p, fert, sap), "fertilizer used on the sapling");
    ctx.check(eq::idAt(sap) == "minecraft:oak_log", "the sapling became the trunk (" + eq::idAt(sap) + ")");
    ctx.check(eq::idAt(at(sap, 0, 1, 0)) == "minecraft:bedrock", "the bedrock is untouched");
    ctx.check(eq::idAt(at(sap, 0, 2, 0)) == "minecraft:oak_log", "the trunk goes on above the bedrock");
    ctx.check(eq::idAt(at(sap, 1, 4, 0)) == "minecraft:stone", "leaves don't replace the stone");
    int leaves = countIn(sap, "_leaves");
    ctx.check(leaves >= 10, "the crown was placed (" + std::to_string(leaves) + " leaves)");
    ctx.check(items::countOf(fert) == 1, "one fertilizer was used");
    clearTreeSpace(p, sap);
    clearInventory(p);
});

// A 2x2 of dark oak saplings grows Bedrock's dark oak (its quadruple feature) from any of them.
LV_REGISTER_SELFTEST("equipment.tree_fertilizer_dark_oak", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto sap = at(ctx.testPos(4582), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(sap), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    clearTreeSpace(p, sap);
    for (int x = 0; x < 2; ++x)
        for (int z = 0; z < 2; ++z) {
            ctx.require(ctx.setBlock(at(sap, x, -1, z), "minecraft:dirt"), "dirt");
            ctx.require(ctx.setBlock(at(sap, x, 0, z), "minecraft:dark_oak_sapling"), "dark oak sapling");
        }
    auto fert = item(ctx, "levitate:tree_fertilizer", 2);
    ctx.check(eq::useTreeFertilizer(p, fert, at(sap, 1, 0, 1)), "fertilizer used on a sapling of the 2x2");
    for (int x = 0; x < 2; ++x)
        for (int z = 0; z < 2; ++z)
            ctx.check(eq::idAt(at(sap, x, 0, z)) == "minecraft:dark_oak_log", "the 2x2 became the trunk (" + eq::idAt(at(sap, x, 0, z)) + ")");
    ctx.check(countIn(sap, "_leaves") >= 20, "the crown was placed");
    clearTreeSpace(p, sap);
    // a single dark oak sapling grows nothing (Create: the grower needs the 2x2), the item is used
    ctx.setBlock(at(sap, 0, -1, 0), "minecraft:dirt");
    ctx.require(ctx.setBlock(sap, "minecraft:dark_oak_sapling"), "single dark oak sapling");
    ctx.check(eq::useTreeFertilizer(p, fert, sap), "fertilizer used on the single sapling");
    ctx.check(eq::idAt(sap) == "minecraft:dark_oak_sapling", "a single dark oak sapling stays");
    clearTreeSpace(p, sap);
    clearInventory(p);
});

// Create: a hanging mangrove propagule does nothing (the item is not used); a standing one and an
// azalea grow their trees.
LV_REGISTER_SELFTEST("equipment.tree_fertilizer_propagule", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto sap = at(ctx.testPos(4588), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(sap), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    clearTreeSpace(p, sap);
    ctx.require(ctx.setBlock(at(sap, 0, 1, 0), "minecraft:mangrove_leaves"), "leaves");
    ctx.require(ctx.setBlock(sap, "minecraft:mangrove_propagule", {{"hanging", 1}}), "hanging propagule");
    auto fert = item(ctx, "levitate:tree_fertilizer", 3);
    eq::useTreeFertilizer(p, fert, sap);
    ctx.check(eq::idAt(sap) == "minecraft:mangrove_propagule", "the hanging propagule stays");
    ctx.check(items::countOf(fert) == 3, "no fertilizer was used");
    clearTreeSpace(p, sap);
    // a standing propagule on mud and an azalea on dirt grow (dream world or Bedrock's own growth)
    ctx.require(ctx.setBlock(at(sap, 0, -1, 0), "minecraft:mud"), "mud");
    ctx.require(ctx.setBlock(sap, "minecraft:mangrove_propagule"), "propagule");
    ctx.check(eq::useTreeFertilizer(p, fert, sap), "fertilizer used on the propagule");
    ctx.check(countIn(sap, "mangrove_log") >= 2, "the propagule grew a mangrove");
    clearTreeSpace(p, sap);
    ctx.require(ctx.setBlock(at(sap, 0, -1, 0), "minecraft:dirt"), "dirt");
    ctx.require(ctx.setBlock(sap, "minecraft:azalea"), "azalea");
    ctx.check(eq::useTreeFertilizer(p, fert, sap), "fertilizer used on the azalea");
    ctx.check(eq::idAt(sap) == "minecraft:oak_log", "the azalea grew an azalea tree (" + eq::idAt(sap) + ")");
    ctx.check(items::countOf(fert) == 1, "two fertilizers were used");
    clearTreeSpace(p, sap);
    clearInventory(p);
});
