// Selftests of the materials unit (U9, testPos dx 900..999).

#include <cmath>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/HandSlot.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "feature/materials/Materials.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::materials {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;

/// Every item / block id SPEC §2.4 names (other units' recipes rely on them).
char const* const kIds[] = {
    "zinc_ore", "deepslate_zinc_ore", "raw_zinc", "raw_zinc_block", "zinc_ingot", "zinc_nugget", "zinc_block",
    "brass_ingot", "brass_nugget", "brass_block", "brass_sheet", "copper_sheet", "iron_sheet", "golden_sheet",
    "copper_nugget", "andesite_alloy", "andesite_alloy_block", "industrial_iron_block", "weathered_iron_block",
    "crushed_raw_iron", "crushed_raw_gold", "crushed_raw_copper", "crushed_raw_zinc", "crushed_raw_osmium",
    "crushed_raw_platinum", "crushed_raw_silver", "crushed_raw_tin", "crushed_raw_lead", "crushed_raw_quicksilver",
    "crushed_raw_aluminum", "crushed_raw_uranium", "crushed_raw_nickel", "powdered_obsidian", "cinder_flour",
    "wheat_flour", "dough", "rose_quartz", "polished_rose_quartz", "rose_quartz_block", "rose_quartz_tiles",
    "small_rose_quartz_tiles", "electron_tube", "precision_mechanism", "incomplete_precision_mechanism", "propeller",
    "whisk", "brass_hand", "crafter_slot_cover", "sturdy_sheet", "unprocessed_obsidian_sheet", "transmitter", "pulp",
    "cardboard", "cardboard_block", "bound_cardboard_block", "experience_nugget", "experience_block",
    "refined_radiance", "shadow_steel", "chromatic_compound", "bar_of_chocolate", "sweet_roll",
    "chocolate_glazed_berries", "honeyed_apple", "builders_tea", "blaze_cake_base", "blaze_cake", "creative_blaze_cake",
};

std::string stateOf(Block const& b, std::string const& key) {
    auto const& nbt = *b.mSerializationId;
    if (!nbt.contains("states") || !nbt["states"].contains(key)) return {};
    return nbt["states"][key].toSnbt(SnbtFormat::Minimize, 0);
}

int countItemsNear(BlockSource& region, BlockPos const& p, std::string const& id) {
    AABB box{Vec3{p.x - 1.5f, p.y - 1.5f, p.z - 1.5f}, Vec3{p.x + 2.5f, p.y + 2.5f, p.z + 2.5f}};
    int  n = 0;
    for (auto* a : region.getEntities(box, 0.0f, ActorType::ItemEntity)) {
        auto& item = static_cast<ItemActor*>(a)->mItem.get();
        if (!item.isNull() && item.getTypeName() == id) n += item.mCount;
    }
    return n;
}

void clearItemsNear(BlockSource& region, BlockPos const& p) {
    AABB box{Vec3{p.x - 1.5f, p.y - 1.5f, p.z - 1.5f}, Vec3{p.x + 2.5f, p.y + 2.5f, p.z + 2.5f}};
    for (auto* a : region.getEntities(box, 0.0f, ActorType::ItemEntity)) a->remove();
}

} // namespace

LV_REGISTER_SELFTEST("materials.ids_registered", [](Context& ctx) {
    int missing = 0;
    for (auto const* id : kIds) {
        std::string full = std::string("levitate:") + id;
        if (makeStack(full, 1).isNull()) {
            ++missing;
            ctx.check(false, "item not registered: " + full);
        }
    }
    for (auto const& b : allBlocks()) {
        if (!defaultBlock(std::string(b.id))) {
            ++missing;
            ctx.check(false, "block not registered: " + std::string(b.id));
        }
    }
    ctx.info(std::to_string(allBlocks().size()) + " blocks, " + std::to_string(std::size(kIds)) + " ids, missing " + std::to_string(missing));
});

LV_REGISTER_SELFTEST("materials.tool_rules", [](Context& ctx) {
    auto const* ore   = blockInfo("levitate:zinc_ore");
    auto const* stone = blockInfo("levitate:cut_granite");
    auto const* box   = blockInfo("levitate:cardboard_block");
    ctx.require(ore && stone && box, "block table");
    auto iron   = makeStack("minecraft:iron_pickaxe", 1);
    auto wood   = makeStack("minecraft:wooden_pickaxe", 1);
    auto shovel = makeStack("minecraft:iron_shovel", 1);
    ctx.check(toolCanHarvest(*ore, iron), "iron pickaxe harvests zinc ore");
    ctx.check(!toolCanHarvest(*ore, wood), "zinc ore needs an iron tool");
    ctx.check(toolCanHarvest(*stone, wood), "any pickaxe harvests cut granite");
    ctx.check(!toolCanHarvest(*stone, shovel), "cut granite needs a pickaxe");
    ctx.check(!toolCanHarvest(*stone, ItemStack{}), "cut granite drops nothing by hand");
    ctx.check(toolCanHarvest(*box, ItemStack{}), "cardboard drops by hand");
});

LV_REGISTER_SELFTEST("materials.loot_rules", [](Context& ctx) {
    auto const* ore     = blockInfo("levitate:zinc_ore");
    auto const* bound   = blockInfo("levitate:bound_cardboard_block");
    auto const* oreB    = defaultBlock("levitate:zinc_ore");
    auto const* boundB  = defaultBlock("levitate:bound_cardboard_block");
    ctx.require(ore && bound && oreB && boundB, "blocks loaded");
    auto plain = dropsFor(*ore, *oreB, false, 0, [](int) { return 0; });
    ctx.check(plain.size() == 1 && plain[0].first == "levitate:raw_zinc" && plain[0].second == 1, "zinc ore drops one raw zinc");
    auto silk = dropsFor(*ore, *oreB, true, 0, [](int) { return 0; });
    ctx.check(silk.size() == 1 && silk[0].first == "levitate:zinc_ore", "silk touch drops the ore");
    auto lucky = dropsFor(*ore, *oreB, false, 3, [](int n) { return n - 1; });
    ctx.check(lucky.size() == 1 && lucky[0].second == 4, "fortune III: up to 4 raw zinc");
    auto b = dropsFor(*bound, *boundB, false, 0, [](int) { return 0; });
    ctx.check(b.size() == 2, "bound cardboard drops cardboard block + string");
});

LV_REGISTER_SELFTEST("materials.slab_merge", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(900);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = ctx.player().getDimensionBlockSource();
    ctx.require(ctx.setBlock(p, "levitate:cut_asurine_slab"), "place slab");
    auto const& before = region.getBlock(game::blockPos(p));
    ctx.require(singleSlabHalf(before).has_value(), "a single slab");
    ctx.require(mergeSlab(region, game::blockPos(p)), "merged");
    auto const& after = region.getBlock(game::blockPos(p));
    ctx.check(game::blockId(after) == "levitate:cut_asurine_slab", "still the slab block");
    ctx.check(!singleSlabHalf(after).has_value(), "now double");
    ctx.check(!mergeSlab(region, game::blockPos(p)), "a double slab can't merge again");
});

LV_REGISTER_SELFTEST("materials.slab_merge_by_player", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(902);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& player = ctx.player();
    auto& region = player.getDimensionBlockSource();
    ctx.require(ctx.setBlock(p, "levitate:polished_cut_ochrum_slab"), "place slab");
    ctx.cleanupBlock({p.dim, p.x, p.y + 1, p.z});
    auto slab = makeStack("levitate:polished_cut_ochrum_slab", 4);
    ctx.require(!slab.isNull(), "slab item");
    player.mGameMode->useItemOn(slab, game::blockPos(p), 1, Vec3{p.x + 0.5f, p.y + 1.0f, p.z + 0.5f}, HandSlot::Mainhand, nullptr, true);
    ctx.check(player.isCreative() || slab.mCount == 3, "one slab used");
    co_await ctx.ticks(2);
    ctx.check(!singleSlabHalf(region.getBlock(game::blockPos(p))).has_value(), "the slab became double");
    ctx.check(game::blockIdAt(region, BlockPos{p.x, p.y + 1, p.z}) != "levitate:polished_cut_ochrum_slab", "no slab placed above");
});

LV_REGISTER_SELFTEST("materials.player_mining_drops", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(904, 2);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& player = ctx.player();
    auto& region = player.getDimensionBlockSource();
    auto  bp     = game::blockPos(p);
    clearItemsNear(region, bp);
    // iron pickaxe: raw zinc
    ctx.require(ctx.setBlock(p, "levitate:zinc_ore"), "place ore");
    ctx.give(player, makeStack("minecraft:iron_pickaxe", 1), player.getSelectedItemSlot());
    player.mGameMode->destroyBlock(bp, 1);
    co_await ctx.ticks(3);
    ctx.check(game::blockIdAt(region, bp) != "levitate:zinc_ore", "ore broken");
    ctx.check(countItemsNear(region, bp, "levitate:raw_zinc") >= 1, "raw zinc dropped");
    clearItemsNear(region, bp);
    // wooden pickaxe: nothing (needs iron)
    ctx.require(ctx.setBlock(p, "levitate:zinc_ore"), "place ore again");
    ctx.give(player, makeStack("minecraft:wooden_pickaxe", 1), player.getSelectedItemSlot());
    player.mGameMode->destroyBlock(bp, 1);
    co_await ctx.ticks(3);
    ctx.check(countItemsNear(region, bp, "levitate:raw_zinc") == 0 && countItemsNear(region, bp, "levitate:zinc_ore") == 0,
              "nothing dropped with a wooden pickaxe");
    clearItemsNear(region, bp);
});

LV_REGISTER_SELFTEST("materials.experience_nuggets", [](Context& ctx) {
    auto one = experienceOrbValues(1);
    ctx.check(one.size() == 1 && one[0] == 3, "one nugget: one orb of 3");
    int sum = 0;
    for (int v : experienceOrbValues(64)) sum += v;
    ctx.check(sum == 192 && experienceOrbValues(64).size() == 5, "64 nuggets: 192 xp in 5 orbs");
    auto& player = ctx.player();
    auto  stack  = makeStack("levitate:experience_nugget", 10);
    ctx.require(!stack.isNull(), "nugget item");
    int xp = useExperienceNuggets(player, stack, false);
    ctx.check(xp == 30, "10 nuggets give 30 xp, got " + std::to_string(xp));
    ctx.check(stack.isNull() || stack.mCount == 0, "the whole stack is used");
    auto two = makeStack("levitate:experience_nugget", 2);
    useExperienceNuggets(player, two, true);
    ctx.check(two.mCount == 1, "sneaking uses one");
});

LV_REGISTER_SELFTEST("materials.builders_tea", [](Context& ctx) {
    auto& player = ctx.player();
    applyBuildersTea(player);
    auto const* e = player.getEffect(3u);
    ctx.require(e != nullptr, "haste applied");
    ctx.check(e->mDuration->mValue == 3600, "3 minutes");
    ctx.check(e->mAmplifier == 0, "level I");
    ctx.check(!e->mEffectVisible, "no particles");
});

LV_REGISTER_SELFTEST("materials.chromatic_light", [](Context& ctx) {
    auto s = makeStack("levitate:chromatic_compound", 1);
    ctx.require(!s.isNull(), "item");
    ctx.check(chromaticLight(s) == 0, "uncharged");
    setChromaticLight(s, 7);
    ctx.check(chromaticLight(s) == 7, "charge stored in the stack");
    setChromaticLight(s, 0);
    ctx.check(chromaticLight(s) == 0, "charge cleared");
});

LV_REGISTER_SELFTEST("materials.worldgen", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Fresh chunks far from the spawn (the spawn area may predate the mod on BDS).
    auto base = ctx.testPos(906);
    GP   far{0, base.x + 30000, 64, base.z + 30000};
    long long before = decoratedChunks();
    ctx.require(co_await ctx.ensureLoaded(far), "far chunks loaded");
    co_await ctx.ticks(40);
    if (decoratedChunks() == before) {
        ctx.info("no chunk was decorated (flat world): skipped");
        co_return;
    }
    auto& region = ctx.player().getDimensionBlockSource();
    int   zinc = 0, layers = 0, chunks = 0;
    for (int cx = -2; cx <= 2; ++cx) {
        for (int cz = -2; cz <= 2; ++cz) {
            int x0 = (far.x >> 4 << 4) + cx * 16, z0 = (far.z >> 4 << 4) + cz * 16;
            if (!region.hasChunksAt(BlockPos{x0 + 8, 0, z0 + 8}, 0, false)) continue;
            ++chunks;
            for (int x = x0; x < x0 + 16; ++x)
                for (int z = z0; z < z0 + 16; ++z)
                    for (int y = -63; y <= 70; ++y) {
                        auto id = region.getBlock(BlockPos{x, y, z}).getTypeName();
                        if (id.starts_with("levitate:")) {
                            if (id.ends_with("zinc_ore")) ++zinc;
                            else ++layers;
                        }
                    }
        }
    }
    ctx.info("scanned " + std::to_string(chunks) + " chunks: " + std::to_string(zinc) + " zinc ore, " + std::to_string(layers)
             + " striated stone blocks");
    ctx.check(chunks > 0, "chunks scanned");
    ctx.check(zinc > 0, "zinc ore generated");
});

} // namespace lv::feature::materials
