#include <gtest/gtest.h>

#include <vector>

#include "core/basin/BasinLogic.h"

using namespace lv::core::basin;
using lv::core::common::HeatLevel;

// ---- blaze burner --------------------------------------------------------------------------------

TEST(Burner, HeatFromFuel) {
    BurnerState s;
    EXPECT_EQ(burnerHeat(s), HeatLevel::Smouldering);
    s = {Fuel::Normal, 1600, false};
    EXPECT_EQ(burnerHeat(s), HeatLevel::Kindled);
    s.burnTime = 125;
    EXPECT_EQ(burnerHeat(s), HeatLevel::Kindled);
    s.burnTime = 124;
    EXPECT_EQ(burnerHeat(s), HeatLevel::Fading);
    s = {Fuel::Special, 10, false};
    EXPECT_EQ(burnerHeat(s), HeatLevel::Seething);
}

TEST(Burner, FuelRules) {
    BurnerState s;
    EXPECT_TRUE(tryUpdateFuel(s, {Fuel::Normal, 1600}, true, false));
    EXPECT_EQ(s.burnTime, 1600);
    // above the insertion threshold: players overflow up to the capacity, automation refuses
    EXPECT_FALSE(tryUpdateFuel(s, {Fuel::Normal, 1600}, false, true));
    EXPECT_TRUE(tryUpdateFuel(s, {Fuel::Normal, 1600}, true, false));
    EXPECT_EQ(s.burnTime, 3200);
    EXPECT_TRUE(tryUpdateFuel(s, {Fuel::Normal, 20000}, true, false));
    EXPECT_EQ(s.burnTime, kMaxHeatCapacity);
    EXPECT_TRUE(tryUpdateFuel(s, {Fuel::Normal, 100}, true, false)); // full: accepted, nothing changes
    EXPECT_EQ(s.burnTime, kMaxHeatCapacity);
    // at or below the threshold the burn time adds up
    s = {Fuel::Normal, 500, false};
    EXPECT_TRUE(tryUpdateFuel(s, {Fuel::Normal, 1600}, false, false));
    EXPECT_EQ(s.burnTime, 2100);
    // special fuel replaces normal fuel, never the other way round
    EXPECT_TRUE(tryUpdateFuel(s, {Fuel::Special, kSpecialBurnTime}, false, false));
    EXPECT_EQ(s.fuel, Fuel::Special);
    EXPECT_EQ(s.burnTime, kSpecialBurnTime);
    EXPECT_FALSE(tryUpdateFuel(s, {Fuel::Normal, 1600}, true, false));
    EXPECT_FALSE(tryUpdateFuel(s, {Fuel::Special, kSpecialBurnTime}, true, false));
    // a first fuel is taken whole, even above the capacity (as Create)
    BurnerState lava;
    EXPECT_TRUE(tryUpdateFuel(lava, {Fuel::Normal, 20000}, true, false));
    EXPECT_EQ(lava.burnTime, 20000);
    BurnerState creative{Fuel::None, 0, true};
    EXPECT_FALSE(tryUpdateFuel(creative, {Fuel::Normal, 1600}, true, false));
    EXPECT_FALSE(tryUpdateFuel(s, {Fuel::None, 0}, true, false));
}

TEST(Burner, TickRunsDown) {
    BurnerState s{Fuel::Special, 2, false};
    tickBurner(s);
    EXPECT_EQ(s.burnTime, 1);
    tickBurner(s);
    EXPECT_EQ(s.fuel, Fuel::Normal);
    EXPECT_EQ(s.burnTime, kMaxHeatCapacity / 2);
    s.burnTime = 1;
    tickBurner(s);
    EXPECT_EQ(s.fuel, Fuel::None);
    EXPECT_EQ(burnerHeat(s), HeatLevel::Smouldering);
    BurnerState c{Fuel::None, 0, true};
    tickBurner(c);
    EXPECT_EQ(c, (BurnerState{Fuel::None, 0, true}));
}

TEST(Burner, CreativeCycle) {
    EXPECT_EQ(nextActiveLevel(HeatLevel::None), HeatLevel::Smouldering);
    EXPECT_EQ(nextActiveLevel(HeatLevel::Seething), HeatLevel::Smouldering);
    EXPECT_EQ(creativeNext(HeatLevel::Smouldering), HeatLevel::Kindled);
    EXPECT_EQ(creativeNext(HeatLevel::Fading), HeatLevel::Kindled);
    EXPECT_EQ(creativeNext(HeatLevel::Kindled), HeatLevel::Seething);
    EXPECT_EQ(creativeNext(HeatLevel::Seething), HeatLevel::Smouldering);
    EXPECT_EQ(burnerComparator(HeatLevel::None), 0);
    EXPECT_EQ(burnerComparator(HeatLevel::Smouldering), 0);
    EXPECT_EQ(burnerComparator(HeatLevel::Seething), 3);
}

// ---- press ---------------------------------------------------------------------------------------

TEST(Press, RunningTickSpeed) {
    EXPECT_EQ(pressRunningTickSpeed(0), 0);
    EXPECT_EQ(pressRunningTickSpeed(16), 2);   // 1 + 16/512 * 59 = 2.84
    EXPECT_EQ(pressRunningTickSpeed(-64), 8);  // 1 + 7.375
    EXPECT_EQ(pressRunningTickSpeed(256), 30); // 1 + 29.5
    EXPECT_EQ(pressRunningTickSpeed(512), 60);
    EXPECT_EQ(pressRunningTickSpeed(1024), 60);
}

TEST(Press, HeadOffset) {
    EXPECT_FLOAT_EQ(pressHeadOffset(false, 0, 100, 0), 0);
    EXPECT_FLOAT_EQ(pressHeadOffset(true, 0, 0, 0), 0);
    EXPECT_FLOAT_EQ(pressHeadOffset(true, 120, 120, 0), 1); // (120/240*2)^3 = 1
    EXPECT_NEAR(pressHeadOffset(true, 60, 60, 0), 0.125f, 1e-5);
    EXPECT_FLOAT_EQ(pressHeadOffset(true, 200, 200, 0), 0.5f); // (240-200)/240*3
    EXPECT_FLOAT_EQ(pressHeadOffset(true, 240, 240, 0), 0);
    EXPECT_FLOAT_EQ(pressModeHeadOffset(PressMode::Basin), 22.f / 16.f);
    EXPECT_FLOAT_EQ(pressModeHeadOffset(PressMode::Belt), 19.f / 16.f);
    EXPECT_FLOAT_EQ(pressModeHeadOffset(PressMode::World), 1.f);
}

// ---- mixer ---------------------------------------------------------------------------------------

TEST(Mixer, ProcessingTicks) {
    EXPECT_EQ(mixerProcessingTicks(64, 0), 3 * 15 + 1);   // log2(8) = 3
    EXPECT_EQ(mixerProcessingTicks(256, 0), 1 * 15 + 1);  // log2(2) = 1
    EXPECT_EQ(mixerProcessingTicks(512, 0), 1);           // log2(1) = 0
    EXPECT_EQ(mixerProcessingTicks(-100, 0), 2 * 15 + 1); // int(5.12) = 5 -> 2
    EXPECT_EQ(mixerProcessingTicks(32, 200), 4 * 30 + 1); // recipe speed 2
    EXPECT_EQ(mixerProcessingTicks(32, 50), 4 * 8 + 1);   // ceil(7.5) = 8
}

TEST(Mixer, HeadMotion) {
    EXPECT_FLOAT_EQ(mixerHeadOffset(false, 0, 0), 7.f / 16.f);
    EXPECT_FLOAT_EQ(mixerHeadOffset(true, 0, 0), 7.f / 16.f);
    EXPECT_FLOAT_EQ(mixerHeadOffset(true, 20, 0), 1 + 7.f / 16.f);
    EXPECT_NEAR(mixerHeadOffset(true, 10, 0), .5f + 7.f / 16.f, 1e-5);
    EXPECT_NEAR(mixerHeadOffset(true, 40, 0), 7.f / 16.f, 1e-5);
    EXPECT_FLOAT_EQ(mixerHeadSpeed(64, false, 0), 32);
    EXPECT_FLOAT_EQ(mixerHeadSpeed(64, true, 10), 64);
    EXPECT_FLOAT_EQ(mixerHeadSpeed(64, true, 18), 128);
    EXPECT_FLOAT_EQ(mixerHeadSpeed(64, true, 30), 64);
}

// ---- tanks ---------------------------------------------------------------------------------------

TEST(BasinTanks, EnforcedVariety) {
    std::vector<Fluid> tanks(2);
    EXPECT_EQ(fillTanks(tanks, {"minecraft:water", 1500}, kTankCapacity, false), 1000);
    EXPECT_EQ(fillTanks(tanks, {"minecraft:water", 500}, kTankCapacity, false), 0); // no second water tank
    EXPECT_EQ(fillTanks(tanks, {"minecraft:lava", 250}, kTankCapacity, true), 250);
    EXPECT_TRUE(tanks[1].empty());
    EXPECT_EQ(fillTanks(tanks, {"minecraft:lava", 250}, kTankCapacity, false), 250);
    EXPECT_EQ(fillTanks(tanks, {"minecraft:milk", 250}, kTankCapacity, false), 0);
    auto d = drainTanks(tanks, "minecraft:lava", 1000, false);
    EXPECT_EQ(d, (Fluid{"minecraft:lava", 250}));
    EXPECT_TRUE(tanks[1].empty());
    d = drainTanks(tanks, "", 300, true);
    EXPECT_EQ(d, (Fluid{"minecraft:water", 300}));
    EXPECT_EQ(tanks[0].amount, 1000);
}

// ---- crafting ------------------------------------------------------------------------------------

TEST(BasinCrafting, Compression) {
    EXPECT_TRUE(canCompress(std::vector<std::string>(9, "minecraft:iron_ingot")));
    EXPECT_TRUE(canCompress(std::vector<std::string>(4, "minecraft:snowball")));
    EXPECT_FALSE(canCompress(std::vector<std::string>(2, "minecraft:snowball")));
    std::vector<std::string> mixed(9, "minecraft:iron_ingot");
    mixed[4] = "minecraft:gold_ingot";
    EXPECT_FALSE(canCompress(mixed));
    EXPECT_FALSE(canCompress(std::vector<std::string>(4, "")));
    EXPECT_TRUE(ignoredInAutomation("levitate:foo_manual_only"));
    EXPECT_FALSE(ignoredInAutomation("minecraft:bread"));
}

TEST(BasinCrafting, Comparator) {
    EXPECT_EQ(redstoneFromInventory(std::vector<int>(9, 0), std::vector<int>(9, 64)), 0);
    std::vector<int> one(9, 0);
    one[0] = 1;
    EXPECT_EQ(redstoneFromInventory(one, std::vector<int>(9, 64)), 1);
    EXPECT_EQ(redstoneFromInventory(std::vector<int>(9, 64), std::vector<int>(9, 64)), 15);
}
