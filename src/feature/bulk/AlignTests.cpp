// Selftests of the U54 alignment of the bulk processing unit (testPos dx 5430..5459): Create's
// runtime-generated cutting and washing recipes, vanilla hoppers feeding and emptying a millstone
// (feature/common/Hoppers.h) and the millstone's goggle lines.

#include <set>
#include <string>

#include "mc/world/Container.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"

#include "feature/bulk/Bulk.h"
#include "feature/common/Hoppers.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::bulk {

namespace {

using selftest::Context;
namespace items = common::items;

constexpr char kSource[] = "levitate:kinetics_test_source";

GlobalPos off(GlobalPos p, int dx, int dy, int dz) {
    p.x += dx;
    p.y += dy;
    p.z += dz;
    return p;
}

int countIn(Container& c, std::string const& id) {
    int n = 0;
    for (int i = 0; i < c.getContainerSize(); ++i)
        if (items::idOf(c.getItem(i)) == id) n += items::countOf(c.getItem(i));
    return n;
}

} // namespace

// RuntimeDataGenerator: planks -> 9 shapes (50 ticks) for every wood, concrete powder -> concrete.
LV_REGISTER_SELFTEST("bulk.runtime_recipes", [](Context& ctx) {
    int planks = 0;
    for (auto const& r : common::recipes::ofType("cutting")) {
        if (r.ingredients.empty() || r.ingredients[0].items.empty() || !r.ingredients[0].items[0].ends_with("_planks")) continue;
        if (r.id.find("_planks_to_") == std::string::npos) continue;
        ++planks;
        ctx.check(r.processingTime == 50, r.id + " takes 50 ticks");
        ctx.check(r.results.size() == 1 && items::make(r.results[0].id).has_value(), r.id + ": the output exists on Bedrock");
    }
    ctx.check(planks == 12 * 9, "108 planks recipes, got " + std::to_string(planks));
    auto oak = items::make("minecraft:oak_planks");
    ctx.require(oak.has_value(), "oak planks");
    std::set<std::string> outs;
    for (auto const* r : common::recipes::withFirstIngredient("cutting", *oak))
        for (auto const& res : r->results) outs.insert(res.id + "x" + std::to_string(res.count));
    for (auto const* want : {"minecraft:oak_stairsx1", "minecraft:oak_slabx2", "minecraft:wooden_doorx1", "minecraft:trapdoorx1", "minecraft:oak_signx1"})
        ctx.check(outs.contains(want), std::string("oak planks -> ") + want);
    int washed = 0;
    for (auto const* colour : {"white", "light_gray", "black", "magenta"}) {
        auto powder = items::make(std::string("minecraft:") + colour + "_concrete_powder");
        ctx.require(powder.has_value(), std::string(colour) + " concrete powder");
        auto const* r = common::recipes::findFirst("splashing", *powder);
        if (r && r->results.size() == 1 && r->results[0].id == std::string("minecraft:") + colour + "_concrete") ++washed;
    }
    ctx.check(washed == 4, "concrete powder washes into concrete");
});

// NeoForge hoppers use the item capability: a hopper above feeds the millstone, one below empties it.
LV_REGISTER_SELFTEST("bulk.hopper_millstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = off(ctx.testPos(5430), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& region = ctx.player().getDimensionBlockSource();
    auto  below  = base, mill = off(base, 0, 1, 0), above = off(base, 0, 2, 0);
    // a cogwheel beside the millstone drives it (Create's millstone is a small cog)
    auto cog = off(mill, 1, 0, 0), src = off(base, 1, 0, 0);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 1}}), "source");
    ctx.require(ctx.setBlock(cog, "levitate:cogwheel", {{"levitate:axis", 1}}), "cogwheel");
    ctx.require(ctx.setBlock(mill, std::string(kMillstone)), "millstone");
    ctx.require(ctx.setBlock(below, "minecraft:hopper", {{"facing_direction", 4}}), "hopper below, pointing west");
    ctx.require(ctx.setBlock(above, "minecraft:hopper", {{"facing_direction", 0}}), "hopper above, pointing down");
    kinetics::refresh(src);
    kinetics::setGeneratedSpeed(src, 256);
    co_await ctx.ticks(3);
    ctx.require(kinetics::speedAt(mill) != 0, "the millstone turns: " + std::to_string(kinetics::speedAt(mill)));
    Container* top = region.tryGetContainer(game::blockPos(above));
    Container* bot = region.tryGetContainer(game::blockPos(below));
    ctx.require(top && bot, "hopper containers");
    auto bones = items::make("minecraft:bone", 2);
    ctx.require(bones.has_value(), "bones");
    top->addItem(*bones);
    bool done = false;
    for (int i = 0; i < 200 && !done; ++i) {
        co_await ctx.ticks(1);
        done = countIn(*bot, "minecraft:bone_meal") >= 3;
    }
    ctx.check(countIn(*top, "minecraft:bone") == 0, "the top hopper fed both bones into the millstone");
    ctx.check(done, "the bottom hopper took bone meal out: " + std::to_string(countIn(*bot, "minecraft:bone_meal")));
    // funnels have no item capability in Create: a hopper doesn't feed one
    ctx.check(!common::hoppers::reaches("levitate:andesite_funnel") && common::hoppers::reaches(std::string(kMillstone)), "hopper reach rules");
    kinetics::setGeneratedSpeed(src, 0);
    for (auto* c : {top, bot})
        for (int i = 0; i < c->getContainerSize(); ++i) c->setItem(i, ItemStack{});
});

} // namespace lv::feature::bulk
