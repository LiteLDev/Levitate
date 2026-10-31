// Selftests of the U54 alignment of the crafting recipes and the crafter (testPos dx 5480..5499).
// The crafting table recipes are checked through the game's own recipe matching (the mechanical
// crafter's vanilla crafting), so these tests also show that Bedrock accepted the files.

#include <map>
#include <string>
#include <vector>

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/VanillaItems.h"
#include "feature/crafter/Internal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::crafter {

namespace {

using selftest::Context;
namespace ci = common::items;

/// The result of a crafting grid given as rows (top first) and a key, "" when nothing matches.
std::string craft(Context& ctx, std::vector<std::string> const& rows, std::map<char, std::string> const& key, int* count = nullptr) {
    std::map<cr::Cell, ItemStack> grid;
    int                           h = static_cast<int>(rows.size());
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < static_cast<int>(rows[static_cast<std::size_t>(r)].size()); ++c) {
            char ch = rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (ch == ' ') continue;
            auto s = ci::make(key.at(ch));
            ctx.require(s.has_value(), "item " + key.at(ch) + " exists");
            grid[{c, h - 1 - r}] = *s;
        }
    auto out = crafterlogic::tryToApplyRecipe(grid);
    if (count) *count = out ? ci::countOf(*out) : 0;
    return out ? ci::idOf(*out) : std::string();
}

} // namespace

// Create 6.0.10's crafting table recipes that were missing or wrong (U54).
LV_REGISTER_SELFTEST("crafter.create_recipes", [](Context& ctx) {
    int n = 0;
    std::string const brass = "levitate:brass_sheet", torch = "minecraft:redstone_torch", dust = "minecraft:redstone";
    // the arm takes an andesite alloy, not an electron tube
    ctx.check(craft(ctx, {"LLA", "L  ", "IC "}, {{'L', brass}, {'A', "levitate:andesite_alloy"}, {'I', "levitate:precision_mechanism"}, {'C', "levitate:brass_casing"}}) == "levitate:mechanical_arm", "mechanical arm");
    ctx.check(craft(ctx, {"LLI", "L  ", "RC "}, {{'L', brass}, {'I', "levitate:electron_tube"}, {'R', "levitate:precision_mechanism"}, {'C', "levitate:brass_casing"}}).empty(), "no arm with an electron tube");
    // 1-wide columns, anywhere in the grid
    ctx.check(craft(ctx, {"B", "C", "S"}, {{'B', "minecraft:oak_slab"}, {'C', "levitate:brass_casing"}, {'S', "levitate:electron_tube"}}) == "levitate:clockwork_bearing", "clockwork bearing column");
    ctx.check(craft(ctx, {"B", "C", "I"}, {{'B', "minecraft:spruce_slab"}, {'C', "levitate:andesite_casing"}, {'I', "levitate:shaft"}}) == "levitate:mechanical_bearing", "mechanical bearing column");
    ctx.check(craft(ctx, {"B", "C", "I"}, {{'B', "minecraft:oak_slab"}, {'C', "minecraft:granite"}, {'I', "levitate:shaft"}}) == "levitate:windmill_bearing", "windmill bearing with granite (c:stones)");
    ctx.check(craft(ctx, {"S", "C", "S"}, {{'S', "levitate:copper_sheet"}, {'C', "minecraft:copper_ingot"}}, &n) == "levitate:fluid_pipe" && n == 4, "vertical fluid pipe gives 4");
    // sails
    ctx.check(craft(ctx, {"WS", "SA"}, {{'W', "minecraft:red_wool"}, {'S', "minecraft:stick"}, {'A', "levitate:andesite_alloy"}}, &n) == "levitate:white_sail" && n == 2, "2 white sails from any wool");
    ctx.check(craft(ctx, {"F"}, {{'F', "levitate:sail_frame"}}) == "levitate:white_sail", "frame -> sail");
    ctx.check(craft(ctx, {"W"}, {{'W', "levitate:white_sail"}}) == "levitate:sail_frame", "sail -> frame");
    ctx.check(craft(ctx, {"SSS", "SAS", "SSS"}, {{'S', "minecraft:stick"}, {'A', "levitate:andesite_alloy"}}).empty(), "no stick frame recipe (Create 6)");
    ctx.check(craft(ctx, {"WD"}, {{'W', "levitate:white_sail"}, {'D', "minecraft:red_dye"}}).empty(), "no dyed sail recipe (dyeing is in the world)");
    // redstone: the torch in the right column, any c:stones
    ctx.check(craft(ctx, {"  T", "RCT", "SSS"}, {{'T', torch}, {'R', dust}, {'C', brass}, {'S', "minecraft:stone"}}) == "levitate:pulse_extender", "pulse extender");
    ctx.check(craft(ctx, {" T ", "RCT", "SSS"}, {{'T', torch}, {'R', dust}, {'C', brass}, {'S', "minecraft:stone"}}).empty(), "no pulse extender with the torch in the middle");
    ctx.check(craft(ctx, {" T ", "RCR", "SSS"}, {{'T', torch}, {'R', dust}, {'C', "minecraft:lever"}, {'S', "minecraft:tuff"}}) == "levitate:powered_latch", "powered latch on tuff");
    ctx.check(craft(ctx, {"RCT", "SSS"}, {{'T', torch}, {'R', dust}, {'C', brass}, {'S', "minecraft:deepslate"}}) == "levitate:pulse_repeater", "pulse repeater on deepslate");
    // 1 -> 1 recipes
    for (auto const* id : {"levitate:filter", "levitate:attribute_filter", "levitate:schedule", "levitate:red_table_cloth", "levitate:brass_table_cloth"})
        ctx.check(craft(ctx, {"X"}, {{'X', id}}) == id, std::string(id) + " clears");
    ctx.check(craft(ctx, {"V"}, {{'V', "levitate:lime_valve_handle"}}) == "levitate:copper_valve_handle", "a dyed valve handle turns back into copper");
    // vanilla items from Create ingredients
    ctx.check(craft(ctx, {" M ", "SES", " P "}, {{'M', "minecraft:milk_bucket"}, {'S', "minecraft:sugar"}, {'E', "minecraft:egg"}, {'P', "levitate:dough"}}) == "minecraft:cake", "cake from dough");
    ctx.check(craft(ctx, {"S", "C", "S"}, {{'S', "levitate:zinc_nugget"}, {'C', "levitate:zinc_ingot"}}) == "minecraft:iron_chain", "chain from zinc");
    ctx.check(craft(ctx, {"SSS", "SPS", "SSS"}, {{'S', "minecraft:stick"}, {'P', "levitate:cardboard"}}) == "minecraft:frame", "item frame from cardboard");
});

// Vanilla clients see a crafter's slot item as a client-only item entity in front of its face.
LV_REGISTER_SELFTEST("crafter.vanilla_items", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5480);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, std::string(kCrafter), {{kFacing, 3}, {kPointing, 0}, {kCovered, 0}}), "crafter facing south");
    co_await ctx.ticks(1);
    auto* region = world::regionOf(p.dim);
    ctx.require(region != nullptr, "region");
    auto planks = ci::make("minecraft:dirt"); // no recipe: the crafter keeps it in its slot
    ctx.require(planks.has_value(), "dirt");
    auto rest = ci::insert(*region, game::blockPos(p), 3, *planks, false);
    ctx.require(ci::isEmpty(rest), "the crafter took the dirt");
    bool found = false;
    for (auto const& s : common::vanillaitems::collect())
        if (s.owner == p) {
            found = true;
            ctx.check(ci::idOf(s.stack) == "minecraft:dirt", "the dirt is shown");
            ctx.check(s.pos.z > p.z + 1.f, "in front of the south face");
        }
    ctx.check(found, "the slot item is listed for vanilla clients");
});

} // namespace lv::feature::crafter
