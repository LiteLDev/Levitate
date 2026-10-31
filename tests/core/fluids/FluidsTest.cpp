#include <gtest/gtest.h>

#include <map>

#include "core/fluids/Fluids.h"

using namespace lv::core::fluids;

TEST(FluidsPotions, EncodesAndDecodesPotionFluids) {
    auto id = potionFluid(Bottle::Splash, "minecraft:harming");
    EXPECT_EQ(id, "levitate:potion/splash/harming");
    auto pf = parsePotionFluid(id);
    ASSERT_TRUE(pf);
    EXPECT_EQ(pf->bottle, Bottle::Splash);
    EXPECT_EQ(pf->potion, "harming");
    EXPECT_FALSE(parsePotionFluid("levitate:potion/regular/nope"));
    EXPECT_FALSE(parsePotionFluid("levitate:honey"));
    EXPECT_FALSE(parsePotionFluid("levitate:potion/bogus/harming"));
}

TEST(FluidsPotions, ItemsMapToFluidsAndBack) {
    EXPECT_EQ(fluidOfPotionItem("minecraft:potion", 0), "minecraft:water");
    EXPECT_EQ(fluidOfPotionItem("minecraft:splash_potion", 0), "levitate:potion/splash/water");
    EXPECT_EQ(fluidOfPotionItem("minecraft:potion", 23), "levitate:potion/regular/harming");
    EXPECT_EQ(fluidOfPotionItem("minecraft:lingering_potion", 14), "levitate:potion/lingering/swiftness");
    EXPECT_EQ(fluidOfPotionItem("minecraft:apple", 0), "");
    EXPECT_EQ(fluidOfPotionItem("minecraft:potion", 999), "");

    auto water = bottleFor("minecraft:water");
    ASSERT_TRUE(water);
    EXPECT_EQ(water->id, "minecraft:potion");
    EXPECT_EQ(water->aux, 0);
    auto strength = bottleFor("levitate:potion/regular/strength");
    ASSERT_TRUE(strength);
    EXPECT_EQ(strength->id, "minecraft:potion");
    EXPECT_EQ(strength->aux, 31);
    auto splash = bottleFor("levitate:potion/splash/night_vision");
    ASSERT_TRUE(splash);
    EXPECT_EQ(splash->id, "minecraft:splash_potion");
    EXPECT_EQ(splash->aux, 5);
    EXPECT_FALSE(bottleFor("levitate:honey"));
    // every potion round-trips
    for (auto const& p : potionTypes()) {
        for (auto b : {Bottle::Regular, Bottle::Splash, Bottle::Lingering}) {
            auto f = fluidOfPotionItem(bottleItem(b), p.aux);
            auto back = bottleFor(f);
            ASSERT_TRUE(back) << f;
            EXPECT_EQ(back->id, bottleItem(b));
            EXPECT_EQ(back->aux, p.aux);
        }
    }
}

TEST(FluidsNames, KeysAndColours) {
    EXPECT_EQ(fluidNameKey("levitate:honey"), "fluid.create.honey");
    EXPECT_EQ(fluidNameKey("levitate:potion/regular/harming"), "fluid.create.potion");
    EXPECT_EQ(fluidNameKey("minecraft:water"), "fluid_type.minecraft.water");
    EXPECT_EQ(fluidColor("levitate:honey"), 0xEAAE2Fu);
    EXPECT_NE(fluidColor("levitate:potion/regular/healing"), fluidColor("levitate:potion/regular/poison"));
}

TEST(FluidsNames, PotionFluidsAreNamedAfterTheirItem) {
    auto swift = potionFluidName(Bottle::Regular, "long_swiftness");
    ASSERT_TRUE(swift.has_value());
    EXPECT_EQ(swift->key, "item.minecraft.potion.effect.swiftness");
    EXPECT_EQ(swift->english, "Potion of Swiftness");
    auto splash = potionFluidName(Bottle::Splash, "strong_healing");
    ASSERT_TRUE(splash.has_value());
    EXPECT_EQ(splash->key, "item.minecraft.splash_potion.effect.healing");
    EXPECT_EQ(splash->english, "Splash Potion of Healing");
    EXPECT_EQ(potionFluidName(Bottle::Lingering, "turtle_master")->english, "Lingering Potion of the Turtle Master");
    EXPECT_EQ(potionFluidName(Bottle::Regular, "water")->english, "Water Bottle");
    EXPECT_EQ(potionFluidName(Bottle::Splash, "water")->english, "Splash Water Bottle");
    EXPECT_EQ(potionFluidName(Bottle::Regular, "awkward")->english, "Awkward Potion");
    EXPECT_EQ(potionFluidName(Bottle::Lingering, "long_mundane")->key, "item.minecraft.lingering_potion.effect.mundane");
    EXPECT_FALSE(potionFluidName(Bottle::Regular, "no_such_potion").has_value());
}

TEST(FluidsDrain, ItemsFromTheSideCrossTheCentreAndLeave) {
    auto item = insertDrainItem(4); // from the west side
    EXPECT_EQ(item.travel, 5);      // travels east
    EXPECT_FLOAT_EQ(item.beltPosition, 0);
    int moves = 0;
    while (stepDrainItem(item) == DrainStep::Move) ++moves;
    EXPECT_EQ(moves, 3); // 0 -> .125 -> .25 -> .375, then the centre
    EXPECT_FLOAT_EQ(item.beltPosition, .5f);
    EXPECT_TRUE(item.checked);
    moves = 0;
    DrainStep s;
    while ((s = stepDrainItem(item)) == DrainStep::Move) ++moves;
    EXPECT_EQ(s, DrainStep::AtOutput);
    EXPECT_EQ(moves, 4); // .625 .75 .875 1.0, then past the edge
    EXPECT_FLOAT_EQ(item.beltPosition, 1);
}

TEST(FluidsDrain, ItemsFromAboveStartAtTheCentre) {
    auto item = insertDrainItem(1);
    EXPECT_EQ(item.travel, 0); // down
    EXPECT_FLOAT_EQ(item.beltPosition, .5f);
    EXPECT_EQ(stepDrainItem(item), DrainStep::ReachedCentre);
    EXPECT_EQ(insertDrainItem(-1).travel, 0);
}

TEST(FluidsPsi, ConnectsTransfersAndTimesOut) {
    PsiTimer t;
    t.timeout = 60;
    EXPECT_FALSE(t.isConnected());
    t.start();
    EXPECT_EQ(t.transferTimer, 68);
    EXPECT_FALSE(t.isConnected()); // still animating
    for (int i = 0; i < 4; ++i) t.tick(false);
    EXPECT_TRUE(t.isConnected());
    EXPECT_FLOAT_EQ(t.progress(), 1);
    t.transferred();
    EXPECT_EQ(t.transferTimer, 64);
    EXPECT_FALSE(t.releasesContraption());
    int ticks = 0;
    while (!t.tick(false) && ticks < 200) ++ticks;
    EXPECT_EQ(ticks, 63);
    EXPECT_FALSE(t.isConnected());
    EXPECT_TRUE(t.releasesContraption());
}

TEST(FluidsPsi, KeptAliveItHoldsAtTheAnimationThenClosesWhenLeft) {
    PsiTimer t;
    t.start();
    for (int i = 0; i < 100; ++i) {
        t.keepAlive = 2;
        EXPECT_FALSE(t.tick(false));
    }
    EXPECT_EQ(t.transferTimer, kPsiAnimation);
    EXPECT_TRUE(t.isConnected());
    EXPECT_TRUE(t.releasesContraption());
    EXPECT_TRUE(t.tick(false)); // not refreshed: keepAlive 1 -> 0, the contraption is gone
    EXPECT_FALSE(t.connected);
}

TEST(FluidsPsi, PowerDisconnects) {
    PsiTimer t;
    t.start();
    t.tick(false);
    EXPECT_TRUE(t.tick(true));
    EXPECT_FALSE(t.connected);
    EXPECT_FALSE(t.isConnected());
}

TEST(FluidsHose, MovesAtLinearSpeedAndStopsAtObstacles) {
    auto open = [](int) { return true; };
    auto s    = stepHose(0, 256, open);
    EXPECT_FLOAT_EQ(s.offset, .5f);
    EXPECT_TRUE(s.moving);
    s = stepHose(0, -64, open);
    EXPECT_FLOAT_EQ(s.offset, 0);
    EXPECT_FALSE(s.moving);
    auto floor3 = [](int depth) { return depth < 3; };
    s           = stepHose(1.9f, 128, floor3); // 2.15 -> depth 3 is blocked
    EXPECT_FLOAT_EQ(s.offset, 2);
    EXPECT_FALSE(s.moving);
    EXPECT_EQ(hoseEndDepth(2.f), 2);
    EXPECT_EQ(hoseEndDepth(2.1f), 3);
    EXPECT_FALSE(stepHose(1, 0, open).moving);
}

namespace {
struct World {
    std::map<Pos, Cell> cells;
    Cell                solid; // default: a solid block
    CellQuery           query() {
        return [this](Pos const& p) {
            auto it = cells.find(p);
            return it == cells.end() ? solid : it->second;
        };
    }
    void pool(int x0, int x1, int y0, int y1, int z0, int z1, std::string const& fluid) {
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) cells[{x, y, z}] = Cell{fluid, true, false};
    }
};
} // namespace

TEST(FluidsBodies, DrainsTopLayerFirstFarthestFirst) {
    World w;
    w.pool(0, 2, 0, 1, 0, 0, "minecraft:water"); // 3 x 2 x 1
    auto plan = planDrain({0, 0, 0}, w.query(), 128, 10000);
    EXPECT_EQ(plan.fluid, "minecraft:water");
    EXPECT_FALSE(plan.infinite);
    ASSERT_EQ(plan.sources.size(), 6u);
    EXPECT_EQ(plan.sources[0], (Pos{2, 1, 0})); // top layer, farthest
    EXPECT_EQ(plan.sources[2], (Pos{0, 1, 0}));
    EXPECT_EQ(plan.sources[5], (Pos{0, 0, 0}));
}

TEST(FluidsBodies, DrainNeverSearchesBelowAndFindsInfiniteBodies) {
    World w;
    w.pool(0, 0, -3, 0, 0, 0, "minecraft:water");
    auto plan = planDrain({0, 0, 0}, w.query(), 128, 10000);
    EXPECT_EQ(plan.sources.size(), 1u);
    World ocean;
    ocean.pool(-10, 10, 0, 0, -10, 10, "minecraft:water"); // 441 blocks
    auto big = planDrain({0, 0, 0}, ocean.query(), 128, 100);
    EXPECT_TRUE(big.infinite);
    auto small = planDrain({0, 0, 0}, ocean.query(), 128, 1000);
    EXPECT_FALSE(small.infinite);
    EXPECT_EQ(small.sources.size(), 441u);
    auto ranged = planDrain({0, 0, 0}, ocean.query(), 2, 1000);
    EXPECT_EQ(ranged.sources.size(), 13u); // within 2 blocks
    EXPECT_TRUE(planDrain({50, 50, 50}, ocean.query(), 128, 1000).fluid.empty());
}

TEST(FluidsBodies, FillsLowestNearestFirstAndNeverUpwards) {
    World w;
    // a 3 wide trench, 2 deep, open above
    for (int x = -1; x <= 1; ++x)
        for (int y = -2; y <= 1; ++y) w.cells[{x, y, 0}] = Cell{"", false, true};
    auto plan = planFill({0, 0, 0}, "minecraft:water", w.query(), 128, 10000);
    EXPECT_FALSE(plan.infinite);
    ASSERT_EQ(plan.targets.size(), 9u); // y 0, -1, -2 (not y 1: upwards)
    EXPECT_EQ(plan.targets[0], (Pos{0, -2, 0}));
    EXPECT_EQ(plan.targets.back().y, 0);
    // a filled target becomes part of the body
    w.cells[{0, -2, 0}] = Cell{"minecraft:water", true, false};
    auto next           = planFill({0, 0, 0}, "minecraft:water", w.query(), 128, 10000);
    EXPECT_EQ(next.targets.size(), 8u);
    EXPECT_EQ(next.targets[0].y, -2);
    // root blocked: nothing
    w.cells[{5, 5, 5}] = Cell{"minecraft:lava", true, false};
    EXPECT_TRUE(planFill({5, 5, 5}, "minecraft:water", w.query(), 128, 10000).targets.empty());
}

TEST(FluidsBodies, FillIntoAHugeBodyIsInfinite) {
    World ocean;
    ocean.pool(-10, 10, 0, 0, -10, 10, "minecraft:water");
    auto plan = planFill({0, 0, 0}, "minecraft:water", ocean.query(), 128, 100);
    EXPECT_TRUE(plan.infinite);
    auto other = planFill({0, 0, 0}, "minecraft:lava", ocean.query(), 128, 100);
    EXPECT_TRUE(other.targets.empty());
}
