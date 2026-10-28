#include <gtest/gtest.h>

#include "core/packages/Address.h"
#include "core/packages/Chain.h"

using namespace lv::core::packages;

// ---- addresses (Create's PackageItem.matchAddress + catnip Glob) ---------------------------------

TEST(PackageAddress, BlankAndStar) {
    EXPECT_TRUE(matchAddress("", ""));
    EXPECT_TRUE(matchAddress("  ", ""));
    EXPECT_FALSE(matchAddress("home", ""));
    EXPECT_FALSE(matchAddress("", "home"));
    EXPECT_TRUE(matchAddress("anything", "*"));
    EXPECT_TRUE(matchAddress("*", "anything"));
    EXPECT_TRUE(matchAddress("", "*")); // a "*" port catches unaddressed boxes too
}

TEST(PackageAddress, EqualAndCaseSensitive) {
    EXPECT_TRUE(matchAddress("Storage", "Storage"));
    EXPECT_FALSE(matchAddress("storage", "Storage"));
}

TEST(PackageAddress, GlobBothWays) {
    EXPECT_TRUE(matchAddress("farm_1", "farm_*"));
    EXPECT_TRUE(matchAddress("farm_*", "farm_1"));
    EXPECT_TRUE(matchAddress("farm_2", "farm_?"));
    EXPECT_FALSE(matchAddress("farm_22", "farm_?"));
    EXPECT_FALSE(matchAddress("mine", "farm_*"));
    EXPECT_TRUE(matchAddress("east.depot", "*.depot"));
}

TEST(PackageGlob, Syntax) {
    EXPECT_TRUE(Glob("{a,b}x").matches("bx"));
    EXPECT_FALSE(Glob("{a,b}x").matches("cx"));
    EXPECT_TRUE(Glob("[a-c]1").matches("b1"));
    EXPECT_FALSE(Glob("[a-c]1").matches("d1"));
    EXPECT_TRUE(Glob("[!a-c]1").matches("d1"));
    EXPECT_FALSE(Glob("[!a-c]1").matches("a1"));
    EXPECT_TRUE(Glob("a\\*b").matches("a*b"));
    EXPECT_FALSE(Glob("a\\*b").matches("axb"));
    EXPECT_TRUE(Glob("a.b").matches("a.b"));
    EXPECT_FALSE(Glob("a.b").matches("axb"));
    EXPECT_TRUE(Glob("x,y}").matches("x,y}"));
    // negated groups: at the start "anything not starting with", after * "not ending with"
    EXPECT_TRUE(Glob("{!tmp}").matches("storage"));
    EXPECT_FALSE(Glob("{!tmp}").matches("tmp_1"));
    EXPECT_TRUE(Glob("*{!_old}").matches("depot"));
    EXPECT_FALSE(Glob("*{!_old}").matches("depot_old"));
}

TEST(PackageGlob, Pathological) {
    std::string longA(200, 'a');
    EXPECT_FALSE(matchAddress(longA, "*a*a*a*a*a*a*a*a*a*a*b"));
    std::string huge(5000, 'x');
    EXPECT_TRUE(matchAddress(huge, huge));
    EXPECT_FALSE(matchAddress(huge, "x*"));
}

TEST(PackageGlob, InvalidMatchesOnlyEmpty) {
    for (char const* bad : {"[]", "[!]", "[abc", "{a,{b}}", "{abc", "abc\\", "[c-a]"}) {
        Glob g(bad);
        EXPECT_TRUE(g.invalid()) << bad;
        EXPECT_TRUE(g.matches("")) << bad;
        EXPECT_FALSE(g.matches("abc")) << bad;
    }
}

// ---- chain geometry ------------------------------------------------------------------------------

TEST(ChainGeometry, StatsAlongX) {
    auto s = connectionStats({0, 0, 0}, {8, 0, 0}, false);
    // direction 90 degrees (east), the chain leaves 35 degrees before it
    EXPECT_NEAR(s.tangentAngle, 55.f, 1e-4);
    EXPECT_NEAR(s.start.y, 6 / 16.0, 1e-9);
    EXPECT_GT(s.chainLength, 5.f);
    EXPECT_LT(s.chainLength, 8.f);
    auto r = connectionStats({0, 0, 0}, {8, 0, 0}, true);
    EXPECT_NEAR(r.tangentAngle, 125.f, 1e-4);
}

TEST(ChainGeometry, Angles) {
    EXPECT_NEAR(wrapAngle(-10), 350, 1e-4);
    EXPECT_NEAR(wrapAngle(725), 5, 1e-4);
    EXPECT_NEAR(shortestAngleDiff(350, 10), 20, 1e-4);
    EXPECT_NEAR(shortestAngleDiff(10, 350), -20, 1e-4);
    // forward motion crosses 90 between 80 and 100
    EXPECT_TRUE(loopThresholdCrossed(100, 80, 90, false));
    EXPECT_FALSE(loopThresholdCrossed(85, 80, 90, false));
    EXPECT_TRUE(loopThresholdCrossed(80, 100, 90, true));
    EXPECT_FALSE(loopThresholdCrossed(100, 80, 90, true));
}

TEST(ChainGeometry, SpeedsAndCost) {
    EXPECT_NEAR(distancePerTick(36), 0.1f, 1e-6);
    EXPECT_NEAR(degreesPerTick(36), 0.1f / (3.14159265f * 1.5f) * 360.f, 1e-4);
    EXPECT_LT(degreesPerTick(-36), 0);
    EXPECT_EQ(chainCost({1, 0, 0}), 1);
    EXPECT_EQ(chainCost({5, 0, 0}), 2);
    EXPECT_EQ(chainCost({10, 0, 0}), 4);
    EXPECT_EQ(chainCost({32, 0, 0}), 13);
}

TEST(ChainGeometry, Validation) {
    EXPECT_EQ(validateConnection({40, 0, 0}, 32).value_or(""), "too_far");
    EXPECT_EQ(validateConnection({2, 0, 0}, 32).value_or(""), "too_close");
    EXPECT_EQ(validateConnection({1, 6, 0}, 32).value_or(""), "cannot_connect_vertically");
    EXPECT_EQ(validateConnection({4, 5, 0}, 32).value_or(""), "too_steep");
    EXPECT_FALSE(validateConnection({6, 2, 3}, 32).has_value());
}

TEST(ChainGeometry, RaySegment) {
    auto hit = raySegment({0, 0, -5}, {0, 0, 1}, 10, {-2, 0.1, 0}, {2, 0.1, 0}, 0.25);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->t, 5, 1e-6);
    EXPECT_NEAR(hit->along, 0.5, 1e-6);
    EXPECT_FALSE(raySegment({0, 0, -5}, {0, 0, 1}, 3, {-2, 0, 0}, {2, 0, 0}, 0.25).has_value());
    auto pts = pointsAlongChains(connectionStats({0, 0, 0}, {8, 0, 0}, false), {0.5, 0.5, 0.5}, 5);
    EXPECT_EQ(pts.size(), 5u);
}

// ---- routing -------------------------------------------------------------------------------------

TEST(ChainRouting, PortsAndAdvertising) {
    RoutingTable a, b;
    Offset       ab{8, 0, 0};
    a.receivePortInfo("home", {});
    EXPECT_TRUE(a.changed);
    EXPECT_EQ(a.exitFor("home"), Offset{});
    a.advertiseTo(ab, b);
    ASSERT_EQ(b.entries.size(), 1u);
    EXPECT_EQ(b.entries[0].distance, 1);
    EXPECT_EQ(b.exitFor("home"), ab.negated());
    EXPECT_EQ(b.exitFor("elsewhere"), Offset{});
    // wildcard ports are the last resort
    b.receivePortInfo("*", {0, 0, 5});
    EXPECT_EQ(b.exitFor("home"), ab.negated());
    EXPECT_EQ(b.exitFor("elsewhere"), (Offset{0, 0, 5}));
    // port entries expire unless refreshed
    for (int i = 0; i < RoutingTable::kPortEntryTimeout; ++i) a.tick();
    EXPECT_TRUE(a.entries.empty());
}

// ---- packing -------------------------------------------------------------------------------------

TEST(PackagePacking, InsertStacked) {
    std::vector<Stack> slots(kPackageSlots);
    EXPECT_EQ(insertStacked(slots, {"stone", 100, 64}), 0);
    EXPECT_EQ(slots[0].count, 64);
    EXPECT_EQ(slots[1].count, 36);
    EXPECT_EQ(insertStacked(slots, {"stone", 30, 64}), 0);
    EXPECT_EQ(slots[1].count, 64);
    EXPECT_EQ(slots[2].count, 2);
    EXPECT_EQ(insertStacked(slots, {"sword", 1, 1}), 0);
    EXPECT_EQ(slots[3].key, "sword");
    EXPECT_EQ(insertStacked(slots, {"dirt", 64 * 6, 64}), 64);
}

TEST(PackageRepack, OrderComplete) {
    EXPECT_TRUE(isOrderComplete({{1, 0, true, 0, true}}));
    EXPECT_FALSE(isOrderComplete({{1, 0, true, 1, true}}));
    EXPECT_FALSE(isOrderComplete({{1, 0, false, 0, true}}));
    EXPECT_TRUE(isOrderComplete({{1, 0, false, 0, false}, {1, 0, false, 1, true}, {1, 1, true, 0, true}}));
    EXPECT_FALSE(isOrderComplete({{1, 0, false, 0, false}, {1, 1, true, 0, true}}));
}

TEST(PackageRepack, OrderedFirst) {
    auto pkgs = repackSlots({{"a", 70, 64}, {"b", 5, 64}}, {{"b", 5, 64}, {"a", 70, 64}});
    ASSERT_EQ(pkgs.size(), 1u);
    ASSERT_EQ(pkgs[0].size(), 3u);
    EXPECT_EQ(pkgs[0][0], (Stack{"b", 5, 64}));
    EXPECT_EQ(pkgs[0][1], (Stack{"a", 64, 64}));
    EXPECT_EQ(pkgs[0][2], (Stack{"a", 6, 64}));
    auto many = repackSlots({{"c", 64 * 10, 64}}, {});
    ASSERT_EQ(many.size(), 2u);
    EXPECT_EQ(many[0].size(), 9u);
    EXPECT_EQ(many[1].size(), 1u);
}
