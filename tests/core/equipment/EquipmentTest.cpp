#include <gtest/gtest.h>

#include <algorithm>

#include "core/equipment/Gear.h"
#include "core/equipment/Shaper.h"
#include "core/equipment/Symmetry.h"
#include "core/equipment/Toolbox.h"

using namespace lv::core::equipment;

namespace {
TStack st(std::string id, int n, int max = 64) { return TStack{id, id + "#", n, max}; }
} // namespace

TEST(Toolbox, FirstItemLocksCompartment) {
    ToolboxInv inv;
    EXPECT_TRUE(inv.insert(st("a", 10), false, false).empty());
    EXPECT_EQ(inv.filters[0].match, "a");
    EXPECT_EQ(inv.count(0), 10);
    EXPECT_TRUE(inv.insert(st("b", 5), false, false).empty());
    EXPECT_EQ(inv.filters[1].match, "b");
    // more "a" merges into compartment 0, then fills its remaining slots
    EXPECT_TRUE(inv.insert(st("a", 64 * 3 + 54), false, false).empty());
    EXPECT_EQ(inv.count(0), 256);
    // compartment 0 is full: overflow starts a new compartment (insertItemStacked order)
    auto rest = inv.insert(st("a", 10), false, false);
    EXPECT_TRUE(rest.empty());
    EXPECT_EQ(inv.filters[2].match, "a");
}

TEST(Toolbox, LimitedModeOnlyMatchingFilters) {
    ToolboxInv inv;
    (void)inv.insert(st("a", 1), false, false);
    auto rest = inv.insert(st("c", 3), true, false);
    EXPECT_EQ(rest.count, 3);
    EXPECT_EQ(inv.insert(st("a", 3), true, false).count, 0);
    EXPECT_EQ(inv.count(0), 4);
}

TEST(Toolbox, FilterSurvivesEmptyAndClears) {
    ToolboxInv inv;
    (void)inv.insert(st("a", 5), false, false);
    auto got = inv.extract(0, 64, false);
    EXPECT_EQ(got.count, 5);
    EXPECT_EQ(inv.count(0), 0);
    EXPECT_EQ(inv.filters[0].match, "a");
    // a different item skips the locked empty compartment
    (void)inv.insert(st("b", 1), false, false);
    EXPECT_EQ(inv.filters[1].match, "b");
    EXPECT_TRUE(inv.clearFilter(0));
    EXPECT_TRUE(inv.filters[0].empty());
}

TEST(Toolbox, UnstackablesAndSettle) {
    ToolboxInv inv;
    for (int i = 0; i < 5; ++i) {
        TStack tool{"sword", "sword#" + std::to_string(i), 1, 1};
        auto   r = inv.insert(tool, false, false);
        if (i < 4) EXPECT_TRUE(r.empty());
    }
    EXPECT_EQ(inv.count(0), 4);
    EXPECT_EQ(inv.count(1), 1); // the fifth tool starts the next compartment
    auto one = inv.extract(0, 1, false);
    EXPECT_EQ(one.count, 1);
    EXPECT_EQ(inv.count(0), 3);
    EXPECT_FALSE(inv.slots[0].empty());
    EXPECT_FALSE(inv.slots[2].empty());
    EXPECT_TRUE(inv.slots[3].empty());
}

TEST(Toolbox, JsonRoundTripAndTargets) {
    ToolboxInv inv;
    (void)inv.insert(st("a", 70), false, false);
    auto back = ToolboxInv::fromJson(inv.toJson());
    EXPECT_EQ(back.count(0), 70);
    EXPECT_EQ(back.filters[0].match, "a");
    EXPECT_EQ(hotbarTarget(64), 32);
    EXPECT_EQ(hotbarTarget(16), 8);
    EXPECT_EQ(hotbarTarget(1), 1);
    EXPECT_TRUE(inToolboxRange(0.5, 64, 9.0, 0, 64, 0, 10));
    EXPECT_FALSE(inToolboxRange(0.5, 64, 10.6, 0, 64, 0, 10));
}

TEST(Backtank, FillRates) {
    EXPECT_EQ(backtankFillAmount(16), 1);
    EXPECT_EQ(backtankFillAmount(200), 5);
    EXPECT_EQ(backtankFillAmount(160), 3);
    EXPECT_EQ(backtankFillDelay(16), 16);
    EXPECT_EQ(backtankFillDelay(64), 7);
    EXPECT_EQ(backtankFillDelay(256), 0);
    EXPECT_EQ(backtankFillDelay(0), 20);
}

TEST(Backtank, CostsAndWarnings) {
    EXPECT_EQ(airToolCost(900, 1000), 1);
    EXPECT_EQ(airToolCost(900, 200), 4);
    EXPECT_EQ(airToolCost(900, 0), 0);
    EXPECT_EQ(backtankWarning(91, 90, 900), 1);
    EXPECT_EQ(backtankWarning(90, 89, 900), 0);
    EXPECT_EQ(backtankWarning(2, 1, 900), 2);
    EXPECT_EQ(backtankBarWidth(450, 900), 7);
    EXPECT_EQ(backtankBarDamage(900, 900, 900), 0);
    EXPECT_EQ(backtankBarDamage(0, 900, 900), 899);
    EXPECT_EQ(backtankTimeLeft(901), "15:00");
    EXPECT_EQ(backtankTimeLeft(61), "1:00");
}

TEST(Potato, TypesAndMotion) {
    auto const* p = potatoTypeFor("minecraft:potato");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->reload, 15);
    EXPECT_FLOAT_EQ(p->damage, 5);
    EXPECT_EQ(potatoCooldown(*p, true), 10);
    auto const* fish = potatoTypeFor("minecraft:cooked_salmon");
    ASSERT_NE(fish, nullptr);
    EXPECT_TRUE(fish->sticky);
    EXPECT_EQ(potatoTypeFor("minecraft:stone"), nullptr);
    EXPECT_EQ(potatoTypeFor("minecraft:sweet_berries")->split, 3);
    auto v = potatoStep({1, 0, 0}, *p);
    EXPECT_NEAR(v.x, 0.99, 1e-6);
    EXPECT_NEAR(v.y, -0.0495, 1e-6);
    EXPECT_DOUBLE_EQ(potatoSpeed(*p), 2.5);
    // Create: no recovery without Potato Recovery, then 25 / 37.5 / 50 %
    EXPECT_DOUBLE_EQ(potatoRecoveryChance(0), 0.0);
    EXPECT_DOUBLE_EQ(potatoRecoveryChance(1), 0.25);
    EXPECT_DOUBLE_EQ(potatoRecoveryChance(3), 0.5);
}

TEST(Potato, RenderModesDropsAndAmmoIndex) {
    EXPECT_EQ(potatoTypeFor("minecraft:potato")->render, PotatoRender::Tumble);
    EXPECT_EQ(potatoTypeFor("minecraft:carrot")->render, PotatoRender::TowardMotion);
    EXPECT_EQ(potatoTypeFor("minecraft:carrot")->spin, 1);
    EXPECT_EQ(potatoTypeFor("minecraft:beetroot")->spin, 2);
    EXPECT_EQ(potatoTypeFor("minecraft:golden_carrot")->spin, 2);
    EXPECT_EQ(potatoTypeFor("minecraft:tropical_fish")->render, PotatoRender::TowardMotion);
    EXPECT_TRUE(potatoTypeFor("minecraft:golden_apple")->consumed);
    EXPECT_TRUE(potatoTypeFor("minecraft:pufferfish")->consumed);
    EXPECT_FALSE(potatoTypeFor("minecraft:poisonous_potato")->consumed);
    EXPECT_EQ(potatoTypeFor("minecraft:suspicious_stew")->dropItem, "minecraft:bowl");
    // the sticky types (Create's sticky()): pies, cakes and fish
    for (auto id : {"minecraft:pumpkin_pie", "minecraft:cake", "levitate:blaze_cake", "minecraft:cod", "minecraft:pufferfish"})
        EXPECT_TRUE(potatoTypeFor(id)->sticky) << id;
    EXPECT_FALSE(potatoTypeFor("minecraft:apple")->sticky);
    // the entity's texture array is indexed in this order (assets/pack/rp/entity/potato_projectile.entity.json)
    EXPECT_EQ(potatoAmmoItems().size(), 28u);
    EXPECT_EQ(potatoAmmoIndex("minecraft:potato"), 0);
    EXPECT_EQ(potatoAmmoIndex("minecraft:melon_block"), 16);
    EXPECT_EQ(potatoAmmoIndex("minecraft:cod"), 21);
    EXPECT_EQ(potatoAmmoIndex("minecraft:suspicious_stew"), 27);
    EXPECT_EQ(potatoAmmoIndex("minecraft:stone"), -1);
}

TEST(Potato, StuckAndRotation) {
    auto const* cake = potatoTypeFor("minecraft:cake");
    double      fall = 0;
    double      y    = potatoStuckStep(1.0, fall, *cake);
    EXPECT_NEAR(fall, 0.007, 1e-12);
    EXPECT_NEAR(y, 0.993, 1e-12);
    y = potatoStuckStep(y, fall, *cake);
    EXPECT_NEAR(y, 0.979, 1e-12);
    EXPECT_FALSE(potatoStuckPops(64.0, 64.0));
    EXPECT_TRUE(potatoStuckPops(63.89, 64.0));
    EXPECT_NEAR(potatoYaw({0, 0, 1}), 0, 1e-9);
    EXPECT_NEAR(potatoYaw({-1, 0, 0}), 90, 1e-9);
    EXPECT_NEAR(potatoPitch({1, -1, 0}), 45, 1e-9);
}

TEST(Enchantments, CostsLevelsAndAir) {
    EXPECT_EQ(enchantMinCost(1), 15);
    EXPECT_EQ(enchantMaxCost(1), 45);
    EXPECT_EQ(enchantMinCost(3), 45);
    EXPECT_EQ(enchantMaxCost(3), 75);
    EXPECT_EQ(enchantLevelForCost(14), 0);
    EXPECT_EQ(enchantLevelForCost(15), 1);
    EXPECT_EQ(enchantLevelForCost(30), 2);
    EXPECT_EQ(enchantLevelForCost(45), 3);
    EXPECT_EQ(enchantLevelForCost(76), 0);
    EXPECT_EQ(enchantLevelName(2), "II");
    EXPECT_EQ(backtankMaxAir(900, 300, 0), 900);
    EXPECT_EQ(backtankMaxAir(900, 300, 3), 1800);
    EXPECT_EQ(mergeEnchantLevel(1, 1), 2);
    EXPECT_EQ(mergeEnchantLevel(3, 3), 3);
    EXPECT_EQ(mergeEnchantLevel(1, 3), 3);
}

TEST(Enchantments, JavaRandomMatchesJava) {
    // new java.util.Random(42): nextInt(10) x3 = 0, 3, 8; nextFloat() after that 0.7275637
    JavaRandom r(42);
    EXPECT_EQ(r.nextInt(10), 0);
    EXPECT_EQ(r.nextInt(10), 3);
    EXPECT_EQ(r.nextInt(10), 8);
    JavaRandom f(42);
    EXPECT_NEAR(f.nextFloat(), 0.7275637f, 1e-6);
}

TEST(Enchantments, SelectionPicksFromThePool) {
    // a pool with only Create's enchantment: the first pick is always it, and it is never picked twice
    auto avail = [](int cost) {
        std::vector<EnchantCandidate> v;
        if (int l = enchantLevelForCost(cost)) v.push_back({"potato_recovery", l, kEnchantWeight});
        return v;
    };
    auto compat = [](EnchantCandidate const&, EnchantCandidate const&) { return true; };
    int  hits   = 0;
    for (int seed = 0; seed < 200; ++seed) {
        JavaRandom r(seed);
        auto       picks = selectEnchantments(r, 1, 30, avail, compat);
        ASSERT_LE(picks.size(), 1u);
        if (!picks.empty()) {
            ++hits;
            EXPECT_EQ(picks[0].key, "potato_recovery");
            EXPECT_GE(picks[0].level, 1);
        }
    }
    EXPECT_EQ(hits, 200); // cost 30 (+1, ±15 %) always lies in a level's range
    JavaRandom r(1);
    EXPECT_TRUE(selectEnchantments(r, 1, 5, avail, compat).empty()); // too cheap for level I
    JavaRandom z(1);
    EXPECT_TRUE(selectEnchantments(z, 0, 30, avail, compat).empty()); // not enchantable
}

TEST(Symmetry, PlaneAndCross) {
    Mirror m;
    m.x = 10, m.z = 10, m.type = MirrorType::Plane, m.orientation = 0;
    auto ps = mirrorPositions(m, 12, 5, 13);
    ASSERT_EQ(ps.size(), 1u);
    EXPECT_EQ(ps[0].x, 8);
    EXPECT_EQ(ps[0].z, 13);
    m.orientation = 1;
    ps            = mirrorPositions(m, 12, 5, 13);
    EXPECT_EQ(ps[0].z, 7);
    m.type = MirrorType::Cross, m.orientation = 0;
    EXPECT_EQ(mirrorPositions(m, 12, 5, 13).size(), 3u);
    m.type = MirrorType::Triple;
    EXPECT_EQ(mirrorPositions(m, 12, 5, 13).size(), 7u);
    // on a diagonal some copies coincide
    EXPECT_EQ(mirrorPositions(m, 12, 5, 12).size(), 3u);
    // directions: flipping X turns east into west
    EXPECT_EQ(transformHorizontal(Mat2{-1, 0, 0, 1}, 3), 2);
    EXPECT_EQ(transformHorizontal(Mat2{-1, 0, 0, 1}, 0), 0);
    EXPECT_EQ(planeOrientationFor(0), 1);
    EXPECT_EQ(planeOrientationFor(90), 0);
    EXPECT_EQ(crossOrientationFor(45), 1);
    auto back = Mirror::fromJson(m.toJson());
    EXPECT_EQ(back, m);
}

TEST(Shaper, Shapes) {
    ShaperSettings s;
    s.params = {3, 3, 3};
    EXPECT_EQ(brushShape(s).size(), 27u);
    s.params = {2, 1, 1};
    auto c   = brushShape(s);
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0].x, 0);
    EXPECT_EQ(c[1].x, 1);
    s.brush  = Brush::Sphere;
    s.params = {0, 0, 0};
    EXPECT_EQ(brushShape(s).size(), 1u);
    s.params = {1, 0, 0};
    EXPECT_EQ(brushShape(s).size(), 19u); // centre distance < 1.5: faces and edges
    s.brush  = Brush::Cylinder;
    s.params = {1, 2, 0};
    EXPECT_EQ(brushShape(s).size(), 18u);
}

TEST(Shaper, PlacementOffsets) {
    ShaperSettings s;
    s.params    = {3, 3, 3};
    s.placement = Placement::Attached;
    auto up     = placementOffset(s, 1);
    EXPECT_EQ(up.y, 2);
    auto down = placementOffset(s, 0);
    EXPECT_EQ(down.y, -2);
    s.placement = Placement::Inserted;
    EXPECT_EQ(placementOffset(s, 1).y, -1);
    s.brush     = Brush::Sphere;
    s.params    = {2, 0, 0};
    s.placement = Placement::Attached;
    EXPECT_EQ(placementOffset(s, 5).x, 3);
}

TEST(Shaper, FloodFillAndTools) {
    ShaperSettings s;
    s.brush  = Brush::Surface;
    s.params = {3, 1, 1};
    // a flat floor at y = 0: everything with y == 0 belongs
    auto out = floodFill(s, {0, 0, 0}, 1, [](IPos const& p) { return p.y == 0; });
    // range 3 (distance < 3), no diagonals: |dx| + ... reachable cells with dx²+dz² < 9
    for (auto const& p : out) EXPECT_LT(p.x * p.x + p.z * p.z, 9);
    EXPECT_EQ(out.size(), 25u);
    EXPECT_EQ(toolAction(ShaperTool::Fill, false), 0);
    EXPECT_EQ(toolAction(ShaperTool::Replace, false), 1);
    EXPECT_EQ(toolAction(ShaperTool::Clear, false), 2);
    ShaperSettings bad;
    bad.brush = Brush::Cluster;
    bad.tool  = ShaperTool::Fill;
    sanitize(bad);
    EXPECT_EQ(bad.tool, ShaperTool::Replace);
    EXPECT_TRUE(patternKeeps(Pattern::Checkered, {1, 0, 0}, [] { return true; }));
    EXPECT_FALSE(patternKeeps(Pattern::Checkered, {1, 1, 0}, [] { return true; }));
    EXPECT_FALSE(patternKeeps(Pattern::Chance50, {}, [] { return true; }));
}

TEST(Shaper, Smooth) {
    std::vector<int> flat(25, 4);
    EXPECT_EQ(smoothHeights(flat, 5, 5), flat);
    std::vector<int> bump(25, 0);
    bump[12] = 10;
    auto sm  = smoothHeights(bump, 5, 5);
    EXPECT_EQ(sm[12], 2);
    EXPECT_EQ(sm[0], 0);
}

TEST(Shaper, ClusterEdgesOutlineTheShape) {
    // one block: its 12 edges
    EXPECT_EQ(clusterEdges({{0, 0, 0}}).size(), 12u);
    // two blocks in a row: a 2x1x1 box, 16 unit edges (the ones across its flat sides vanish)
    auto two = clusterEdges({{0, 0, 0}, {1, 0, 0}});
    EXPECT_EQ(two.size(), 16u);
    // an L of three blocks has a concave edge
    auto l = clusterEdges({{0, 0, 0}, {1, 0, 0}, {0, 0, 1}});
    EXPECT_GT(l.size(), 12u);
    EXPECT_TRUE(clusterEdges({}).empty());
}

TEST(DivingBoots, LavaMultiplier) {
    bool grounded = false;
    // on the ground: 1.75 (1.85 sprinting), sinking damped by 2.5 - 2|y|, and the marker is set
    auto g = lavaBootsMultiplier(-0.1, true, false, false, false, grounded);
    EXPECT_DOUBLE_EQ(g.x, 1.75);
    EXPECT_DOUBLE_EQ(g.y, 2.3);
    EXPECT_TRUE(grounded);
    EXPECT_DOUBLE_EQ(lavaBootsMultiplier(0, true, false, true, false, grounded).z, 1.85);
    // a jump from the ground: the rise becomes 1 (eye in lava) or 0.5 blocks per tick
    auto j = lavaBootsMultiplier(0.2, false, true, true, true, grounded);
    EXPECT_DOUBLE_EQ(j.x, 1.75);
    EXPECT_DOUBLE_EQ(j.y * 0.2, 1.0);
    EXPECT_FALSE(grounded);
    // rising in the air afterwards: x1.3; falling fast: no damping below zero
    EXPECT_DOUBLE_EQ(lavaBootsMultiplier(0.1, false, false, false, false, grounded).y, 1.3);
    EXPECT_DOUBLE_EQ(lavaBootsMultiplier(-2.0, false, false, false, false, grounded).y, 0.0);
}
