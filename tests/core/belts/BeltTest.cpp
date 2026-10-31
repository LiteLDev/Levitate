#include <gtest/gtest.h>

#include <map>
#include <set>

#include "core/belts/Belt.h"

using namespace lv::core::belts;

namespace {

/// A tiny world: shafts with an axis, solid blocks, everything else air.
struct World {
    std::map<Vec3i, Axis>  shafts;
    std::set<Vec3i>        solid;
    std::map<Vec3i, float> speeds;
    ConnectWorld           view() {
        ConnectWorld w;
        w.loaded    = [](Vec3i const&) { return true; };
        w.shaftAxis = [this](Vec3i const& p) -> std::optional<Axis> {
            auto it = shafts.find(p);
            return it == shafts.end() ? std::nullopt : std::optional<Axis>(it->second);
        };
        w.speed       = [this](Vec3i const& p) { return speeds.contains(p) ? speeds[p] : 0.f; };
        w.replaceable = [this](Vec3i const& p) { return !solid.contains(p) && !shafts.contains(p); };
        return w;
    }
};

} // namespace

TEST(BeltGeometry, FacingHelpers) {
    EXPECT_EQ(facingNormal(0), (Vec3i{0, 0, 1}));
    EXPECT_EQ(facingNormal(3), (Vec3i{1, 0, 0}));
    EXPECT_EQ(clockwise(2), 3); // north -> east
    EXPECT_EQ(clockwise(3), 0); // east -> south
    EXPECT_EQ(faceOfFacing(1), 4);
    EXPECT_EQ(facingOfFace(5), 3);
    EXPECT_EQ(oppositeFace(2), 3);
    EXPECT_EQ(rotationAxis({0, Slope::Horizontal}), Axis::X); // belt along z turns about x
    EXPECT_EQ(rotationAxis({3, Slope::Sideways}), Axis::Y);
}

TEST(BeltGeometry, VectorsAndNextSegments) {
    SegmentState up{3, Slope::Upward, Part::Middle};
    EXPECT_EQ(beltVector(up), (Vec3i{1, 1, 0}));
    EXPECT_EQ(*nextSegmentOffset(up, true), (Vec3i{1, 1, 0}));
    EXPECT_EQ(*nextSegmentOffset(up, false), (Vec3i{-1, -1, 0}));
    SegmentState vert{2, Slope::Vertical, Part::Start};
    EXPECT_EQ(beltVector(vert), (Vec3i{0, -1, 0}));
    EXPECT_FALSE(nextSegmentOffset(vert, false));
    SegmentState end{0, Slope::Horizontal, Part::End};
    EXPECT_FALSE(nextSegmentOffset(end, true));
}

TEST(BeltConnect, StraightHorizontal) {
    World w;
    w.shafts[{0, 0, 0}] = Axis::X;
    w.shafts[{0, 0, 5}] = Axis::X;
    w.shafts[{0, 0, 3}] = Axis::X; // a shaft in between becomes a pulley
    auto view          = w.view();
    EXPECT_EQ(canConnect(view, {0, 0, 0}, {0, 0, 5}, 20), ConnectError::None);
    auto plan = planBelt(view, {0, 0, 0}, {0, 0, 5});
    ASSERT_EQ(plan.size(), 6u);
    EXPECT_EQ(plan[0].state.part, Part::Start);
    EXPECT_EQ(plan[3].state.part, Part::Pulley);
    EXPECT_EQ(plan[5].state.part, Part::End);
    for (auto const& s : plan) {
        EXPECT_EQ(s.state.facing, 0); // south
        EXPECT_EQ(s.state.slope, Slope::Horizontal);
    }
}

TEST(BeltConnect, RulesOfCreate) {
    World w;
    w.shafts[{0, 0, 0}]  = Axis::X;
    w.shafts[{0, 3, 3}]  = Axis::X; // 45 degrees up
    w.shafts[{0, 2, 3}]  = Axis::X; // not 45 degrees
    w.shafts[{3, 0, 0}]  = Axis::X; // along the shaft axis
    w.shafts[{0, 0, -4}] = Axis::Z; // other axis
    w.shafts[{0, 0, 20}] = Axis::X; // too far (distance must be < 20)
    w.shafts[{0, 0, 19}] = Axis::X;
    auto v               = w.view();
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 3, 3}, 20), ConnectError::None);
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 2, 3}, 20), ConnectError::NotAligned);
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {3, 0, 0}, 20), ConnectError::NotAligned);
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 0, -4}, 20), ConnectError::AxisMismatch);
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 0, 20}, 20), ConnectError::TooFar);
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 0, 19}, 20), ConnectError::None);
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 0, 0}, 20), ConnectError::SamePosition);
    w.solid.insert({0, 0, 10});
    v = w.view();
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 0, 19}, 20), ConnectError::Obstructed);
    w.speeds[{0, 0, 0}]  = 16;
    w.speeds[{0, 3, 3}]  = -16;
    v                    = w.view();
    EXPECT_EQ(canConnect(v, {0, 0, 0}, {0, 3, 3}, 20), ConnectError::SpeedConflict);
}

TEST(BeltConnect, SlopesVerticalAndSideways) {
    World w;
    w.shafts[{0, 0, 0}] = Axis::X;
    w.shafts[{0, 3, 3}] = Axis::X;
    w.shafts[{0, 4, 0}] = Axis::X;
    auto v              = w.view();
    auto up             = planBelt(v, {0, 0, 0}, {0, 3, 3});
    ASSERT_EQ(up.size(), 4u);
    EXPECT_EQ(up[0].state.slope, Slope::Upward);
    EXPECT_EQ(up[1].pos, (Vec3i{0, 1, 1}));
    auto down = planBelt(v, {0, 3, 3}, {0, 0, 0});
    EXPECT_EQ(down[0].state.slope, Slope::Downward);
    EXPECT_EQ(down[0].state.facing, 2); // north
    auto vert = planBelt(v, {0, 0, 0}, {0, 4, 0});
    ASSERT_EQ(vert.size(), 5u);
    EXPECT_EQ(vert[0].state.slope, Slope::Vertical);
    EXPECT_EQ(vert[0].state.facing, 0); // perpendicular to the x shafts, positive = up
    EXPECT_EQ(beltVector(vert[0].state), (Vec3i{0, 1, 0}));

    World s;
    s.shafts[{0, 0, 0}] = Axis::Y;
    s.shafts[{4, 0, 0}] = Axis::Y;
    s.shafts[{2, 0, 2}] = Axis::Y;
    auto sv             = s.view();
    EXPECT_EQ(canConnect(sv, {0, 0, 0}, {4, 0, 0}, 20), ConnectError::None);
    EXPECT_EQ(canConnect(sv, {0, 0, 0}, {2, 0, 2}, 20), ConnectError::VerticalShaftDiagonal);
    auto side = planBelt(sv, {0, 0, 0}, {4, 0, 0});
    EXPECT_EQ(side[2].state.slope, Slope::Sideways);
    EXPECT_FALSE(canTransportObjects(Slope::Sideways));
}

TEST(BeltSpeed, DirectionsMatchCreate) {
    // Belt facing east (axis x): negative RPM moves items towards the end (east).
    EXPECT_GT(directionAwareSpeed(-32, 3), 0);
    EXPECT_EQ(movementFacing(-32, 3), 3);
    EXPECT_EQ(movementFacing(32, 3), 1);
    // Belt facing south (axis z): positive RPM moves items south.
    EXPECT_GT(directionAwareSpeed(32, 0), 0);
    EXPECT_EQ(movementFacing(32, 0), 0);
    EXPECT_FLOAT_EQ(beltMovementSpeed(240), .5f);
}

namespace {
struct TestEnv : InventoryEnv {
    float              s   = .25f;
    int                len = 4;
    Ending             end = Ending::Blocked;
    std::vector<Item>  ejected;
    float              speed() const override { return s; }
    int                length() const override { return len; }
    Ending             resolveEnding() override { return end; }
    void               eject(Item& i) override { ejected.push_back(i); }
};
} // namespace

TEST(BeltInventory, ItemsMoveQueueAndStopAtABlockedEnd) {
    Inventory inv;
    TestEnv   env;
    inv.addItem({1, .5f, .5f});
    inv.addItem({2, .5f, .5f}); // queued behind the first
    for (int i = 0; i < 100; ++i) inv.tick(env);
    ASSERT_EQ(inv.items.size(), 2u);
    EXPECT_NEAR(inv.items[0].beltPosition, 4 - .45f, 1e-4); // stops before the end (Create's margin)
    EXPECT_NEAR(inv.items[1].beltPosition, 4 - .45f - 1, 1e-4); // one block behind
    EXPECT_TRUE(env.ejected.empty());
}

TEST(BeltInventory, ItemsFallOffAnOpenEnd) {
    Inventory inv;
    TestEnv   env;
    env.end = Ending::Eject;
    inv.addItem({7, .5f, .5f});
    int ticks = 0;
    while (!inv.items.empty() || !inv.toInsert.empty()) {
        inv.tick(env);
        ASSERT_LT(++ticks, 100);
    }
    ASSERT_EQ(env.ejected.size(), 1u);
    EXPECT_EQ(env.ejected[0].id, 7u);
    EXPECT_EQ(ticks, 15); // 3.5 blocks at 1/4 per tick, then off the edge
}

TEST(BeltInventory, ReversesWithTheBelt) {
    Inventory inv;
    TestEnv   env;
    inv.addItem({1, 1.5f, 1.5f});
    inv.addItem({2, 2.5f, 2.5f});
    inv.tick(env);
    EXPECT_EQ(inv.items.front().id, 2u); // furthest along first
    env.s = -.25f;
    inv.tick(env);
    EXPECT_FALSE(inv.movementPositive);
    EXPECT_EQ(inv.items.front().id, 1u);
}

TEST(BeltInventory, InsertionBlocksUntilTheItemMovedOn) {
    Inventory inv;
    inv.addItem({1, .5f, .5f, 0, 0, 0, 1});
    EXPECT_FALSE(inv.canInsertAt(0, 3));
    EXPECT_TRUE(inv.canInsertAt(2, 3));
    inv.flushQueues();
    inv.items[0].beltPosition = 2.1f;
    EXPECT_TRUE(inv.canInsertAt(0, 3));
    EXPECT_FALSE(inv.canInsertAtFromSide(0, 2, 3)); // against the movement
    EXPECT_NE(inv.stackAtOffset(2), nullptr);
    EXPECT_EQ(inv.stackAtOffset(1), nullptr);
}

TEST(BeltInventory, ProcessingHoldsItems) {
    struct ProcEnv : TestEnv {
        int  held = 0;
        bool processing(Item& item, float next, bool) override {
            if (item.locked) return ++held >= 5;
            if (item.beltPosition <= 1.5f && next >= 1.5f) {
                item.beltPosition = 1.5f;
                item.locked       = true;
            }
            return false;
        }
    } env;
    Inventory inv;
    inv.addItem({1, .5f, .5f});
    for (int i = 0; i < 4; ++i) inv.tick(env);
    ASSERT_EQ(inv.items.size(), 1u);
    EXPECT_TRUE(inv.items[0].locked);
    EXPECT_FLOAT_EQ(inv.items[0].beltPosition, 1.5f);
    for (int i = 0; i < 10; ++i) inv.tick(env);
    EXPECT_TRUE(inv.items.empty()); // the processor removed it
}

TEST(BeltSlicer, HoveringEnd) {
    SegmentState end{0, Slope::Horizontal, Part::End};
    EXPECT_TRUE(hoveringEnd(end, {0, 0, .3}));
    EXPECT_FALSE(hoveringEnd(end, {0, 0, -.3}));
    SegmentState start{0, Slope::Horizontal, Part::Start};
    EXPECT_TRUE(hoveringEnd(start, {0, 0, -.3}));
    EXPECT_FALSE(hoveringEnd({0, Slope::Horizontal, Part::Middle}, {0, 0, .3}));
    EXPECT_TRUE(statesCompatible({0, Slope::Horizontal}, {2, Slope::Horizontal}));
    EXPECT_TRUE(statesCompatible({0, Slope::Upward}, {2, Slope::Downward}));
    EXPECT_FALSE(statesCompatible({0, Slope::Upward}, {0, Slope::Downward}));
    auto f = flipped({0, Slope::Upward, Part::Start});
    EXPECT_EQ(f.facing, 2);
    EXPECT_EQ(f.slope, Slope::Downward);
    EXPECT_EQ(f.part, Part::End);
}

TEST(Depot, ItemsSlideToTheCentre) {
    Item it{1, 0, 0};
    int  ticks = 0;
    while (!depotSlide(it)) ASSERT_LT(++ticks, 50);
    EXPECT_GT(it.beltPosition, .5f - 1 / 16.f);
    Item top{2, .5f, .5f};
    EXPECT_TRUE(depotSlide(top));
}

TEST(Ejector, TargetsAndLaunches) {
    EXPECT_EQ(ejectorTargetFacing({0, 0, 0}, {0, 3, -5}, 32), 2);
    EXPECT_EQ(ejectorTargetFacing({0, 0, 0}, {4, -2, 0}, 32), 3);
    EXPECT_FALSE(ejectorTargetFacing({0, 0, 0}, {1, 0, 1}, 32));
    EXPECT_FALSE(ejectorTargetFacing({0, 0, 0}, {0, 5, 0}, 32));
    EXPECT_FALSE(ejectorTargetFacing({0, 0, 0}, {33, 0, 0}, 32));
    EXPECT_FLOAT_EQ(ejectorWindUp(256, 1, 0), 1.f);
    EXPECT_FLOAT_EQ(ejectorWindUp(-64, 3, 4), .05f);
    for (auto [h, v] : {std::pair{1, 0}, {4, 0}, {6, 3}, {10, -4}, {32, 8}}) {
        auto l   = planLaunch(h, v);
        auto end = l.at(l.ticks);
        EXPECT_NEAR(end.first, h, 1e-3) << h << "," << v; // pulled onto the target at the end
        // Create's arc only goes half the remaining way towards the target height (getGlobalPos):
        // close for short level shots, up to a few blocks off for long or steep ones
        EXPECT_NEAR(end.second, v, std::abs(v) <= 3 && h <= 6 ? 1.0 : 3.5) << h << "," << v;
        EXPECT_GT(l.vy, 0);
        auto mid = l.at(l.ticks / 2);
        EXPECT_GT(mid.second, std::max(v, 0)); // an arc above the target
        EXPECT_GE(l.itemTicks(), 3.0);
    }
    // Create's EntityLauncher numbers (h 1, v 0): yPeak 1.0625 + ... -> yMotion = sqrt(2 yPeak / 13) + .015 yPeak
    auto one   = planLaunch(1, 0);
    double yPk = std::max(0.0, (1 + .5) / 8.0) + 1;
    EXPECT_NEAR(one.vy, std::sqrt(2 * yPk / 13) + 0.015 * yPk, 1e-9);
}

TEST(BeltPreview, StraightLineAlongTheBelt) {
    auto l = previewLine({0, 0, 0}, {5, 0, 0}, Axis::Z);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->step, (Vec3i{1, 0, 0}));
    EXPECT_FLOAT_EQ(l->length, 5.f);
}

TEST(BeltPreview, ComponentAlongTheShaftIsIgnored) {
    // shafts along z: the z difference doesn't count
    auto l = previewLine({0, 0, 0}, {0, 3, 7}, Axis::Z);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->step, (Vec3i{0, 1, 0}));
    EXPECT_FLOAT_EQ(l->length, 3.f);
}

TEST(BeltPreview, SnapsToTheClosestDiagonal) {
    // (4, 3) in the x-y plane: closer to the 45 degree diagonal than to the x axis
    auto l = previewLine({0, 0, 0}, {4, 3, 0}, Axis::Z);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->step, (Vec3i{1, 1, 0}));
    EXPECT_FLOAT_EQ(l->length, 4.f);
    // (5, 1): closer to straight along x
    auto s = previewLine({0, 0, 0}, {5, -1, 0}, Axis::Z);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->step, (Vec3i{1, 0, 0}));
}

TEST(BeltPreview, VerticalShaftsOnlyStraight) {
    EXPECT_FALSE(previewLine({0, 0, 0}, {3, 0, 3}, Axis::Y).has_value());
    auto l = previewLine({0, 0, 0}, {0, 0, -4}, Axis::Y);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->step, (Vec3i{0, 0, -1}));
    EXPECT_FALSE(previewLine({0, 0, 0}, {0, 0, 0}, Axis::X).has_value());
    // the height difference doesn't count for a vertical shaft
    auto h = previewLine({0, 0, 0}, {1, 1, 0}, Axis::Y);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->step, (Vec3i{1, 0, 0}));
    EXPECT_FLOAT_EQ(h->length, 1.f);
}
