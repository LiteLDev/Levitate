// Selftests of the U59 alignment of the building blocks (testPos dx 5920..5939): Create's
// requiresCorrectToolForDrops per block family, and the copycats' material rules (accepted
// materials, light, harvesting, creative breaks).

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/block/Block.h"

#include "feature/building/Building.h"
#include "feature/common/Items.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace bld   = lv::feature::building;
namespace items = lv::feature::common::items;

ItemStack u59Tool(Context& ctx, std::string const& id) {
    auto s = items::make(id, 1);
    ctx.require(s.has_value(), "item " + id);
    return *s;
}

bool u59Drops(Context& ctx, char const* block, std::string const& toolId) {
    auto const* info = bld::infoOf(std::string_view(block));
    ctx.require(info != nullptr, std::string(block) + " is a building block");
    return bld::canHarvest(*info, nullptr, toolId.empty() ? ItemStack{} : u59Tool(ctx, toolId));
}

} // namespace

LV_REGISTER_SELFTEST("building.u59_harvest_tools", [](Context& ctx) {
    // casings: axe or pickaxe
    ctx.check(!u59Drops(ctx, "levitate:andesite_casing", ""), "a punched andesite casing drops nothing");
    ctx.check(u59Drops(ctx, "levitate:andesite_casing", "minecraft:wooden_axe"), "an axe harvests a casing");
    ctx.check(u59Drops(ctx, "levitate:railway_casing", "minecraft:wooden_pickaxe"), "a pickaxe harvests the railway casing");
    // copper roofing: a stone pickaxe at least
    ctx.check(!u59Drops(ctx, "levitate:copper_shingles", "minecraft:wooden_pickaxe"), "copper shingles need more than a wooden pickaxe");
    ctx.check(u59Drops(ctx, "levitate:copper_shingles", "minecraft:stone_pickaxe"), "a stone pickaxe harvests copper shingles");
    ctx.check(u59Drops(ctx, "levitate:waxed_oxidized_copper_tile_slab", "minecraft:iron_pickaxe"), "an iron pickaxe harvests copper tile slabs");
    // pickaxe only
    for (auto const* b : {"levitate:andesite_bars", "levitate:brass_door", "levitate:train_trapdoor", "levitate:metal_girder", "levitate:placard",
                          "levitate:copper_table_cloth"}) {
        ctx.check(!u59Drops(ctx, b, "minecraft:wooden_axe"), std::string(b) + " needs a pickaxe");
        ctx.check(u59Drops(ctx, b, "minecraft:wooden_pickaxe"), std::string(b) + " drops with one");
    }
    // anything goes
    for (auto const* b : {"levitate:framed_glass", "levitate:andesite_scaffolding", "levitate:andesite_ladder", "levitate:white_table_cloth",
                          "levitate:wooden_bracket", "levitate:oak_window"})
        ctx.check(u59Drops(ctx, b, ""), std::string(b) + " drops by hand");
    // an empty copycat is copycat_base (axe or pickaxe); with a material, the material's rule
    auto const* panel = bld::infoOf(std::string_view("levitate:copycat_panel"));
    ctx.check(!bld::canHarvest(*panel, nullptr, ItemStack{}), "an empty copycat needs a tool");
    auto const* planks = lv::game::blockWith("minecraft:oak_planks");
    ctx.require(planks != nullptr, "oak planks");
    ctx.check(bld::canHarvest(*panel, planks, ItemStack{}), "a copycat of planks drops by hand");
});

LV_REGISTER_SELFTEST("building.u59_copycat_material", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5920);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& player = ctx.player();
    // CopycatBlockEntity.updateLight: a glowstone copycat glows at 15
    ctx.require(ctx.setBlock(p, "levitate:copycat_panel", {{"levitate:facing", 1}}), "a copycat panel");
    ctx.require(bld::useCopycat(player, u59Tool(ctx, "minecraft:glowstone"), p, 1), "glowstone goes on it");
    auto const* b = bld::blockAt(p);
    ctx.check(b && bld::stateOf(*b, "levitate:light") == 15, "the panel gives light 15");
    ctx.require(bld::useCopycat(player, u59Tool(ctx, "levitate:wrench"), p, 1), "the wrench resets it");
    b = bld::blockAt(p);
    ctx.check(b && bld::stateOf(*b, "levitate:light") == 0, "and the light goes");
    // getAcceptedBlockState: #copycat_allow (a barrel) and blocks without a block entity (a crafting
    // table); not #copycat_deny, block entities or stairs
    ctx.check(bld::useCopycat(player, u59Tool(ctx, "minecraft:crafting_table"), p, 1), "a crafting table is accepted");
    ctx.require(bld::useCopycat(player, u59Tool(ctx, "levitate:wrench"), p, 1), "reset");
    ctx.check(bld::useCopycat(player, u59Tool(ctx, "minecraft:barrel"), p, 1), "a barrel is accepted (copycat_allow)");
    ctx.require(bld::useCopycat(player, u59Tool(ctx, "levitate:wrench"), p, 1), "reset");
    ctx.check(!bld::useCopycat(player, u59Tool(ctx, "minecraft:chest"), p, 1), "a chest is not");
    ctx.check(!bld::useCopycat(player, u59Tool(ctx, "minecraft:oak_stairs"), p, 1), "stairs are not");
    // CopycatBlock.playerWillDestroy: a creative player's break keeps the material
    ctx.require(bld::useCopycat(player, u59Tool(ctx, "minecraft:oak_planks"), p, 1), "planks go on it");
    bld::notePlayerBreak(p, true);
    ctx.check(bld::creativeBreakAt(p), "the break is noted as creative");
    ctx.cleanupBlock(p);
});
