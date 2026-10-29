#include <gtest/gtest.h>

#include <map>
#include <set>

#include "core/crafter/Breaking.h"
#include "core/crafter/Crafter.h"
#include "core/crafter/TreeCutter.h"

using namespace lv::core::crafter;
using lv::core::kinetics::Offset;

TEST(Crafter, TargetDirectionFollowsTheArrow) {
    // facing south: the viewer looks north at the front, their right is east
    EXPECT_EQ(targetDirection(Face::South, Pointing::Up), Face::Up);
    EXPECT_EQ(targetDirection(Face::South, Pointing::Down), Face::Down);
    EXPECT_EQ(targetDirection(Face::South, Pointing::Right), Face::East);
    EXPECT_EQ(targetDirection(Face::South, Pointing::Left), Face::West);
    EXPECT_EQ(targetDirection(Face::North, Pointing::Right), Face::West);
    EXPECT_EQ(targetDirection(Face::East, Pointing::Right), Face::North);
    EXPECT_EQ(targetDirection(Face::West, Pointing::Right), Face::South);
    EXPECT_EQ(targetDirection(Face::West, Pointing::Left), Face::North);
}

TEST(Crafter, PlacedCraftersPointAtTheClickedBlock) {
    for (Face facing : {Face::North, Face::South, Face::West, Face::East}) {
        for (Face clicked : lv::core::kinetics::kFaces) {
            if (lv::core::kinetics::axisOf(clicked) == lv::core::kinetics::axisOf(facing)) continue;
            auto p = pointingFromFacing(clicked, facing);
            EXPECT_EQ(targetDirection(facing, p), lv::core::kinetics::opposite(clicked))
                << "facing " << int(facing) << " clicked " << int(clicked);
        }
    }
}

TEST(Crafter, CycleAndOpposition) {
    EXPECT_EQ(cycle(Pointing::Up), Pointing::Left);
    EXPECT_EQ(cycle(Pointing::Right), Pointing::Up);
    EXPECT_TRUE(opposed(Pointing::Up, Pointing::Down));
    EXPECT_TRUE(opposed(Pointing::Left, Pointing::Right));
    EXPECT_FALSE(opposed(Pointing::Up, Pointing::Left));
}

TEST(Crafter, MergeOffsetsBuildAGridWithYUp) {
    // a crafter pointing up sits below its target
    EXPECT_EQ(mergeOffset(Pointing::Up), (Cell{0, -1}));
    EXPECT_EQ(mergeOffset(Pointing::Left), (Cell{1, 0}));
    // 2x2: cells (0,0) bottom-left target... rows top first
    std::vector<Cell> cells{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    auto              m = inputMatrix(cells, {true, true, true, true});
    ASSERT_EQ(m.width, 2);
    ASSERT_EQ(m.height, 2);
    EXPECT_EQ(m.at(0, 0), 2); // (0,1) top-left
    EXPECT_EQ(m.at(1, 1), 1); // (1,0) bottom-right
}

TEST(Crafter, InputMatrixTrimsEmptyEdges) {
    std::vector<Cell> cells{{0, 0}, {1, 0}, {0, 1}};
    auto              m = inputMatrix(cells, {false, true, true});
    EXPECT_EQ(m.width, 2);
    EXPECT_EQ(m.height, 2);
    EXPECT_EQ(m.count(), 2);
    auto one = inputMatrix({{0, 0}, {-1, 0}}, {true, false});
    EXPECT_EQ(one.width, 1);
    EXPECT_EQ(one.height, 1);
}

TEST(Crafter, ShapedMatchingWithMirror) {
    // pattern "AB" / "A " ; ingredient 0 = A, 1 = B
    ShapedPattern p = *parsePattern({"AB", "A "}, {{'A', 0}, {'B', 1}});
    std::vector<Cell>         cells{{0, 1}, {1, 1}, {0, 0}};
    std::vector<std::string>  items{"a", "b", "a"};
    auto                      test = [&](int ing, int cell) { return items[cell] == (ing == 0 ? "a" : "b"); };
    auto                      m    = inputMatrix(cells, {true, true, true});
    EXPECT_TRUE(matchShaped(p, m, false, test));
    // mirrored grid: "BA" / " A"
    std::vector<Cell> mc{{1, 1}, {0, 1}, {1, 0}};
    auto              mm = inputMatrix(mc, {true, true, true});
    EXPECT_FALSE(matchShaped(p, mm, false, test));
    EXPECT_TRUE(matchShaped(p, mm, true, test));
    // pattern with empty edges is trimmed
    ShapedPattern padded = *parsePattern({"   ", " AB", " A "}, {{'A', 0}, {'B', 1}});
    EXPECT_TRUE(matchShaped(padded, m, false, test));
    EXPECT_FALSE(parsePattern({"AX"}, {{'A', 0}}).has_value());
}

TEST(Crafter, ShapelessMatching) {
    std::vector<std::string> items{"x", "y", "x"};
    auto                     m = inputMatrix({{0, 0}, {1, 0}, {2, 0}}, {true, true, true});
    // ingredients: 0 = x, 1 = x or y, 2 = y
    std::vector<std::set<std::string>> ings{{"x"}, {"x", "y"}, {"y"}};
    auto test = [&](int ing, int cell) { return ings[ing].contains(items[cell]); };
    EXPECT_TRUE(matchShapeless(3, m, test));
    EXPECT_FALSE(matchShapeless(2, m, test));
    std::vector<std::set<std::string>> two{{"y"}, {"y"}, {"x"}};
    EXPECT_FALSE(matchShapeless(3, m, [&](int ing, int cell) { return two[ing].contains(items[cell]); }));
}

TEST(Crafter, InventoryOrderReadsLikeText) {
    // facing south: viewer looks north, west is on their left
    std::vector<Offset> o{{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    auto                s = inventoryOrder(o, Face::South);
    EXPECT_EQ(s[0], (Offset{0, 1, 0}));
    EXPECT_EQ(s[1], (Offset{1, 1, 0}));
    EXPECT_EQ(s[2], (Offset{0, 0, 0}));
    auto n = inventoryOrder(o, Face::North); // east is on the left
    EXPECT_EQ(n[0], (Offset{1, 1, 0}));
    std::vector<Offset> z{{0, 0, 1}, {0, 0, 0}};
    EXPECT_EQ(inventoryOrder(z, Face::East)[0], (Offset{0, 0, 1})); // facing east: south is on the left
    EXPECT_EQ(inventoryOrder(z, Face::West)[0], (Offset{0, 0, 0}));
}

TEST(Crafter, EdgeInteraction) {
    std::vector<Face> sides{Face::Down, Face::Up, Face::West, Face::East};
    EXPECT_EQ(edgeAt(Face::North, 0.95, 0.5, 0.0, sides), Face::East);
    EXPECT_EQ(edgeAt(Face::North, 0.05, 0.5, 0.0, sides), Face::West);
    EXPECT_EQ(edgeAt(Face::North, 0.5, 0.9, 0.0, sides), Face::Up);
    EXPECT_FALSE(edgeAt(Face::North, 0.5, 0.5, 0.0, sides).has_value());
}

TEST(Crafter, CountDownSpeed) {
    EXPECT_EQ(countDownSpeed(0), 0);
    EXPECT_EQ(countDownSpeed(1), 4);
    EXPECT_EQ(countDownSpeed(-32), 32);
    EXPECT_EQ(countDownSpeed(256), 250);
    EXPECT_EQ(phaseFromName(phaseName(Phase::Exporting)), Phase::Exporting);
}

TEST(Breaking, Numbers) {
    EXPECT_FLOAT_EQ(stationaryBreakSpeed(-64), 0.64f);
    // stone (1.5) at 64 RPM: every hit adds 1 and waits 2 ticks
    EXPECT_EQ(progressStep(0.64f, 1.5f, 0), 1);
    EXPECT_EQ(ticksUntilNextProgress(1.5f, 0.64f), 2);
    EXPECT_EQ(ticksToBreak(1.5f, 0.64f), 28);
    EXPECT_EQ(progressStep(2.56f, 0.5f, 8), 2);
    EXPECT_EQ(progressStep(1.0f, 0.0f, 3), 7);
    EXPECT_DOUBLE_EQ(drillDamage(0), 0);
    EXPECT_DOUBLE_EQ(drillDamage(32), 2 + 1 + 0.5);
    EXPECT_DOUBLE_EQ(drillDamage(256), 10);
    EXPECT_DOUBLE_EQ(movingDamage(0), 2);
    EXPECT_FLOAT_EQ(animationSpeed(0.1), -200); // Create: (int)(length * 1000 + 100 * sign) / 100 * 100
    EXPECT_FLOAT_EQ(animationSpeed(0.001), 0);
    EXPECT_FLOAT_EQ(movingBreakSpeed(-100), 0.2f);
}

TEST(Breaking, Saw) {
    EXPECT_EQ(directionalAxis(Face::Up, true), Axis::X);
    EXPECT_EQ(directionalAxis(Face::Up, false), Axis::Z);
    EXPECT_EQ(directionalAxis(Face::East, true), Axis::Y);
    EXPECT_EQ(directionalAxis(Face::North, false), Axis::Y);
    EXPECT_FLOAT_EQ(sawProcessingSpeed(12), 1);
    EXPECT_FLOAT_EQ(sawProcessingSpeed(96), 4);
    EXPECT_EQ(sawProcessingTime(50, 1), 50);
    EXPECT_EQ(sawProcessingTime(50, 12), 100);
    EXPECT_EQ(sawItemMovement(false, 32), (Offset{1, 0, 0}));
    EXPECT_EQ(sawItemMovement(true, 32), (Offset{0, 0, -1}));
    EXPECT_EQ(sawItemMovement(true, -32), (Offset{0, 0, 1}));
}

namespace {
struct FakeWorld {
    std::map<TreePos, TreeBlock> blocks;
    TreeReader                   reader() {
        return [this](TreePos const& p) {
            auto it = blocks.find(p);
            return it == blocks.end() ? TreeBlock{} : it->second;
        };
    }
    void set(int x, int y, int z, TreeKind k) { blocks[{x, y, z}] = TreeBlock{k}; }
};
} // namespace

TEST(TreeCutter, CutsAWholeTree) {
    FakeWorld w;
    for (int y = 0; y < 5; ++y) w.set(0, y, 0, TreeKind::Log);
    for (int x = -2; x <= 2; ++x)
        for (int z = -2; z <= 2; ++z)
            if (x || z) w.set(x, 4, z, TreeKind::Leaf);
    w.set(0, 5, 0, TreeKind::Leaf);
    w.set(1, 2, 0, TreeKind::Attachment);
    auto t = findTree(w.reader(), {0, 0, 0}, TreeBlock{TreeKind::Log});
    EXPECT_EQ(t.logs.size(), 4u); // the cut block itself is broken by the saw
    EXPECT_EQ(t.leaves.size(), 25u);
    EXPECT_EQ(t.attachments.size(), 1u);
}

TEST(TreeCutter, RefusesAPartialCut) {
    FakeWorld w;
    for (int y = 0; y < 4; ++y) w.set(0, y, 0, TreeKind::Log);
    // a second trunk next to the cut that grows from the ground
    for (int y = -1; y < 4; ++y) w.set(1, y, 0, TreeKind::Log);
    auto t = findTree(w.reader(), {0, 0, 0}, TreeBlock{TreeKind::Log});
    EXPECT_TRUE(t.empty());
}

TEST(TreeCutter, VerticalPlantsAndLeafDistance) {
    FakeWorld w;
    for (int y = 0; y < 4; ++y) w.set(0, y, 0, TreeKind::VerticalPlant);
    auto t = findTree(w.reader(), {0, 0, 0}, TreeBlock{TreeKind::VerticalPlant});
    ASSERT_EQ(t.logs.size(), 3u);
    EXPECT_EQ(t.logs.front(), (TreePos{0, 3, 0}));
    EXPECT_TRUE(findTree(w.reader(), {0, 3, 0}, TreeBlock{TreeKind::VerticalPlant}).empty());

    FakeWorld l;
    l.set(0, 0, 0, TreeKind::Log);
    l.set(1, 0, 0, TreeKind::Leaf);
    l.set(2, 0, 0, TreeKind::Leaf);
    EXPECT_EQ(computeLeafDistance(l.reader(), {2, 0, 0}), 2);
    l.set(5, 0, 0, TreeKind::Leaf);
    EXPECT_EQ(computeLeafDistance(l.reader(), {5, 0, 0}), 7);
}
