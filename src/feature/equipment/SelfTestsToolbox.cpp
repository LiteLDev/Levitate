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
#include "mc/world/actor/player/PlayerUISlot.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"

#include "core/equipment/Toolbox.h"
#include "feature/net/Net.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

/// testPos puts dx 45xx 180 blocks south, outside the area the selftest run keeps ticking (z +-64):
/// U45's tests move 240 blocks north, to rows no unit uses (z -62..-58), so entities there tick.
constexpr int kU45Shift = -240;

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace eq    = lv::feature::equipment;
namespace ce    = lv::core::equipment;
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
    auto sap = at(ctx.testPos(4575, kU45Shift), 0, 1, 0);
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
    auto sap = at(ctx.testPos(4582, kU45Shift), 0, 1, 0);
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
    auto sap = at(ctx.testPos(4588, kU45Shift), 0, 1, 0);
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

// Create's ToolboxBlockEntity openTracker: the lid is open while anyone has the screen open (the
// block state `levitate:toolbox_lid` = 1, which vanilla clients show as the open model).
LV_REGISTER_SELFTEST("equipment.toolbox_lid", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4570, kU45Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(base, "levitate:cyan_toolbox", {{"levitate:facing", 3}}), "toolbox placed");
    p.teleport(Vec3{base.x + 2.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(12);
    auto lid = [&] { return eq::stateAt(base, "levitate:toolbox_lid").value_or(-1); };
    ctx.check(lid() == 0, "a closed toolbox shows its closed lid");
    // the form (vanilla players): open while the form is up
    eq::openToolbox(p, base);
    ctx.check(eq::toolboxOpenCount(base) == 1, "one player has it open");
    ctx.check(lid() == 1, "the lid opens");
    ctx.check(eq::stateAt(base, eq::kFacing).value_or(-1) == 3, "it keeps its facing");
    eq::toolboxScreenClosed(p, base);
    ctx.check(eq::toolboxOpenCount(base) == 0 && lid() == 0, "closing the screen closes the lid");
    // the native screen (client mod): "screen.closed" closes it
    co_await ctx.ticks(12);
    net::setTestCompanion(p, true);
    eq::openToolbox(p, base);
    ctx.check(lid() == 1, "the native screen opens the lid");
    // a second player keeps it open
    auto& other = ctx.spawnPlayer("lid_other");
    other.teleport(Vec3{base.x - 1.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(12);
    eq::openToolbox(other, base);
    ctx.check(eq::toolboxOpenCount(base) == 2, "two players have it open");
    net::injectFromClient(p, "screen.closed", {{"type", "equipment.toolbox"}});
    ctx.check(eq::toolboxOpenCount(base) == 1 && lid() == 1, "it stays open while the other player has it open");
    (void)net::takeTestMessages(p);
    net::setTestCompanion(p, false);
    // walking away (Create's menu: 8 blocks) closes it for that player
    other.teleport(Vec3{base.x + 20.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(22);
    ctx.check(eq::toolboxOpenCount(base) == 0 && lid() == 0, "the lid closes when the last player walks away");
    // the debug hold (e2e): open for a while, then shut
    ctx.check(eq::holdToolboxOpen(base, 15), "lid held open");
    ctx.check(lid() == 1, "held open");
    co_await ctx.ticks(30);
    ctx.check(lid() == 0, "it closes after the hold");
    // dyeing an open toolbox keeps it open
    co_await ctx.ticks(12);
    eq::openToolbox(p, base);
    auto dye = item(ctx, "minecraft:orange_dye");
    eq::useToolbox(p, dye, base);
    ctx.check(eq::idAt(base) == "levitate:orange_toolbox" && lid() == 1, "dyed while open, the lid stays open");
    eq::toolboxScreenClosed(p, base);
    ctx.check(lid() == 0, "and closes");
});

// Create's ToolboxDyeingRecipe keeps the toolbox's data: the crafted toolbox gets the user data of
// the toolbox in the grid (the craft hook calls keepDyedToolboxData; the client e2e crafts it).
LV_REGISTER_SELFTEST("equipment.toolbox_dye_craft", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4572, kU45Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    ce::ToolboxInv inv;
    inv.insert(ce::TStack{"minecraft:apple", items::toJson(item(ctx, "minecraft:apple")).dump(), 9, 64}, false, false);
    auto box = item(ctx, "levitate:brown_toolbox");
    items::setData(box, eq::kToolboxData, {{"inv", inv.toJson()}});
    auto setGrid = [&](int first, std::vector<ItemStack> const& stacks) {
        for (int i = 28; i <= 40; ++i) p.setPlayerUIItem(static_cast<PlayerUISlot>(i), items::empty(), false);
        for (std::size_t i = 0; i < stacks.size(); ++i)
            p.setPlayerUIItem(static_cast<PlayerUISlot>(first + static_cast<int>(i)), stacks[i], false);
    };
    // crafting table grid: toolbox + red dye
    setGrid(32, {items::empty(), box, items::empty(), item(ctx, "minecraft:red_dye")});
    auto result = item(ctx, "levitate:red_toolbox");
    ctx.check(eq::keepDyedToolboxData(p, result), "the dyed toolbox keeps the data (crafting table)");
    auto data   = items::getData(result, eq::kToolboxData);
    auto copied = ce::ToolboxInv::fromJson(data.is_object() ? data.value("inv", nlohmann::json::object()) : nlohmann::json::object());
    ctx.check(copied.count(0) == 9, "with the 9 apples");
    // the inventory's 2x2 grid
    setGrid(28, {item(ctx, "minecraft:blue_dye"), box});
    auto blue = item(ctx, "levitate:blue_toolbox");
    ctx.check(eq::keepDyedToolboxData(p, blue), "the dyed toolbox keeps the data (2x2 grid)");
    // anything else in the grid: not Create's dyeing recipe
    setGrid(32, {box, item(ctx, "minecraft:red_dye"), item(ctx, "minecraft:stick")});
    auto other = item(ctx, "levitate:red_toolbox");
    ctx.check(!eq::keepDyedToolboxData(p, other), "no data with a third ingredient");
    setGrid(28, {});
    clearInventory(p);
});
