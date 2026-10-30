#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "core/bulk/Bulk.h"

using namespace lv::core::bulk;

TEST(Bulk, FanMaxDistance) {
    FanConfig c;
    EXPECT_FLOAT_EQ(fanMaxDistance(256, c), 20);
    EXPECT_FLOAT_EQ(fanMaxDistance(512, c), 20);
    EXPECT_FLOAT_EQ(fanMaxDistance(128, c), 11.5f);
    EXPECT_FLOAT_EQ(fanMaxDistance(-32, c), 3 + 17 / 8.f);
    c.pullDistance = 10;
    EXPECT_FLOAT_EQ(fanMaxDistance(-256, c), 10);
}

TEST(Bulk, FanPushing) {
    EXPECT_TRUE(fanPushing(Face::Up, 16));
    EXPECT_FALSE(fanPushing(Face::Up, -16));
    EXPECT_TRUE(fanPushing(Face::North, -16));
    EXPECT_FALSE(fanPushing(Face::West, 16));
}

TEST(Bulk, FanProcessingTime) {
    EXPECT_EQ(fanProcessingTime(1, 150), 151);
    EXPECT_EQ(fanProcessingTime(16, 150), 151);
    EXPECT_EQ(fanProcessingTime(17, 150), 301);
    EXPECT_EQ(fanProcessingTime(64, 150), 601);
}

TEST(Bulk, PriorityAndNames) {
    EXPECT_EQ(bestType({FanType::Smoking, FanType::Blasting}), FanType::Smoking);
    EXPECT_EQ(bestType({FanType::Blasting, FanType::Splashing}), FanType::Splashing);
    EXPECT_EQ(bestType({}), FanType::None);
    EXPECT_EQ(fanTypeByName("haunting"), FanType::Haunting);
    EXPECT_EQ(fanTypeName(FanType::Blasting), "blasting");
}

TEST(Bulk, FlowLimit) {
    auto air = [](int) { return FlowBlock{}; };
    EXPECT_FLOAT_EQ(flowLimit(20, Face::East, air), 20);
    auto wallAt5 = [](int i) { return i == 5 ? FlowBlock{FlowBlock::Kind::Full} : FlowBlock{}; };
    EXPECT_FLOAT_EQ(flowLimit(20, Face::East, wallAt5), 4);
    auto unloadedAt3 = [](int i) { return i == 3 ? FlowBlock{FlowBlock::Kind::Unloaded} : FlowBlock{}; };
    EXPECT_FLOAT_EQ(flowLimit(20, Face::East, unloadedAt3), 2);
    // a bottom slab two blocks above an upward fan: the current reaches the slab's bottom face
    FlowBlock slab{FlowBlock::Kind::Partial, {0, 0, 0}, {1, .5, 1}};
    auto      slabAt2 = [&](int i) { return i == 2 ? slab : FlowBlock{}; };
    EXPECT_NEAR(flowLimit(20, Face::Up, slabAt2), 1 + 1 / 32.0, 1e-5);
    // a top slab: the current goes up to its bottom (half a block in)
    FlowBlock top{FlowBlock::Kind::Partial, {0, .5, 0}, {1, 1, 1}};
    auto      topAt2 = [&](int i) { return i == 2 ? top : FlowBlock{}; };
    EXPECT_NEAR(flowLimit(20, Face::Up, topAt2), 1.5 + 1 / 32.0, 1e-5);
    // a fence post (holes at the test points) lets the current through
    FlowBlock post{FlowBlock::Kind::Partial, {.375, 0, .375}, {.625, 1, .625}};
    auto      postAt2 = [&](int i) { return i == 2 ? post : FlowBlock{}; };
    EXPECT_FLOAT_EQ(flowLimit(20, Face::Up, postAt2), 20);
    // the nozzle in front of a fan stops it almost at once
    FlowBlock nozzle{FlowBlock::Kind::Partial, {1 / 16.0, 0, 1 / 16.0}, {15 / 16.0, 15 / 16.0, 15 / 16.0}};
    EXPECT_NEAR(flowLimit(20, Face::Up, [&](int i) { return i == 1 ? nozzle : FlowBlock{}; }), 1 / 32.0, 1e-6);
    // the depth when travelling down uses the box's top
    EXPECT_NEAR(findMaxDepth(slab, Face::Down), .5, 1e-9);
}

TEST(Bulk, FlowLimitMultiBox) {
    // stairs (bottom half + back quarter on the north side): every test point hits the bottom box
    FlowBlock stairs{FlowBlock::Kind::Partial, {0, 0, 0}, {1, .5, 1}, {{{0, .5, 0}, {1, 1, .5}}}};
    EXPECT_NEAR(findMaxDepth(stairs, Face::Up), 0, 1e-9);
    // travelling down, the test points under the step stop at its top, the others at .5 below
    EXPECT_NEAR(findMaxDepth(stairs, Face::Down), .5, 1e-9);
    // travelling south (+z) the points in the upper half meet the step at z = 0, the lower half
    // the bottom box at z = 0 too
    EXPECT_NEAR(findMaxDepth(stairs, Face::South), 0, 1e-9);
    // two posts covering the left and the right quarter test points: no hole for the current
    FlowBlock posts{FlowBlock::Kind::Partial, {.2, 0, .2}, {.3, 1, .8}, {{{.7, 0, .2}, {.8, 1, .8}}}};
    EXPECT_TRUE(std::isinf(findMaxDepth(posts, Face::Up)));
    // ... but with a third one at the centre, the current stops at the block
    posts.more.push_back({{.45, 0, .45}, {.55, 1, .55}});
    EXPECT_NEAR(findMaxDepth(posts, Face::Up), 0, 1e-9);
    // the first box alone would let the current through (hole at the test points)
    FlowBlock oneSide{FlowBlock::Kind::Partial, {0, 0, 0}, {1, 1, .5}};
    EXPECT_TRUE(std::isinf(findMaxDepth(oneSide, Face::Up)));
    oneSide.more.push_back({{0, .5, .5}, {1, 1, 1}});
    EXPECT_NEAR(findMaxDepth(oneSide, Face::Up), .5, 1e-9);
    EXPECT_NEAR(flowLimit(20, Face::Up, [&](int i) { return i == 3 ? oneSide : FlowBlock{}; }), 2.5 + 1 / 32.0, 1e-5);
}

TEST(Bulk, LimitOf) {
    EXPECT_EQ(limitOf(20), 20);
    EXPECT_EQ(limitOf(4.5f), 5);
    EXPECT_EQ(limitOf(0), 0);
}

TEST(Bulk, SegmentsPushing) {
    AirCurrent c;
    c.direction   = Face::East;
    c.pushing     = true;
    c.maxDistance = 8;
    // water at 3: splashing from there on
    c.buildSegments([](int i) { return i == 3 ? FanType::Splashing : FanType::None; });
    ASSERT_EQ(c.segments.size(), 2u);
    EXPECT_EQ(c.segments[0].type, FanType::None);
    EXPECT_EQ(c.segments[0].endOffset, 2);
    EXPECT_EQ(c.segments[1].type, FanType::Splashing);
    EXPECT_EQ(c.segments[1].endOffset, 8);
    EXPECT_EQ(c.typeAt(1.5f), FanType::None);
    EXPECT_EQ(c.typeAt(2.5f), FanType::Splashing);
    EXPECT_EQ(c.typeAt(7.9f), FanType::Splashing);
    EXPECT_EQ(c.typeAt(8.5f), FanType::None); // beyond the current
    EXPECT_EQ(c.typeAt(-.1f), FanType::None);
}

TEST(Bulk, SegmentsPulling) {
    AirCurrent c;
    c.direction   = Face::East;
    c.pushing     = false;
    c.maxDistance = 8;
    // lava at 5: the air is drawn from the far end towards the fan, blasting from 5 to the fan
    c.buildSegments([](int i) { return i == 5 ? FanType::Blasting : FanType::None; });
    ASSERT_EQ(c.segments.size(), 2u);
    EXPECT_EQ(c.segments[0].type, FanType::None);
    EXPECT_EQ(c.segments[0].endOffset, 5);
    EXPECT_EQ(c.segments[1].type, FanType::Blasting);
    EXPECT_EQ(c.segments[1].endOffset, 0);
    EXPECT_EQ(c.typeAt(6.f), FanType::None);
    EXPECT_EQ(c.typeAt(4.f), FanType::Blasting);
    EXPECT_EQ(c.typeAt(.5f), FanType::Blasting);
}

TEST(Bulk, SegmentsSecondCatalystReplaces) {
    AirCurrent c;
    c.pushing     = true;
    c.maxDistance = 10;
    c.buildSegments([](int i) { return i == 2 ? FanType::Smoking : i == 6 ? FanType::Splashing : FanType::None; });
    EXPECT_EQ(c.typeAt(3), FanType::Smoking);
    EXPECT_EQ(c.typeAt(6), FanType::Splashing);
}

TEST(Bulk, Bounds) {
    auto b = currentBounds(0, 64, 0, Face::East, 5);
    EXPECT_DOUBLE_EQ(b.min.x, 1);
    EXPECT_DOUBLE_EQ(b.max.x, 6);
    EXPECT_DOUBLE_EQ(b.min.y, 64);
    EXPECT_DOUBLE_EQ(b.max.y, 65);
    auto w = currentBounds(0, 64, 0, Face::West, 3);
    EXPECT_DOUBLE_EQ(w.min.x, -3);
    EXPECT_DOUBLE_EQ(w.max.x, 0);
    auto shortEast = currentBounds(0, 64, 0, Face::East, .5f);
    EXPECT_DOUBLE_EQ(shortEast.min.x, 1);
    EXPECT_DOUBLE_EQ(shortEast.max.x, 1.5);
    auto shortWest = currentBounds(0, 64, 0, Face::West, .5f);
    EXPECT_DOUBLE_EQ(shortWest.min.x, -.5);
    EXPECT_DOUBLE_EQ(shortWest.max.x, 0);
    EXPECT_TRUE(currentBounds(0, 0, 0, Face::Up, .1f).empty());
}

TEST(Bulk, AlignedDistance) {
    EXPECT_DOUBLE_EQ(alignedDistanceToFace({5.5, 0, 0}, 0, 0, 0, Face::East), 4.5);
    EXPECT_DOUBLE_EQ(alignedDistanceToFace({-3, 0, 0}, 0, 0, 0, Face::West), 3);
}

TEST(Bulk, PushedMotion) {
    AirCurrent c;
    c.direction   = Face::Up;
    c.pushing     = true;
    c.maxDistance = 20;
    auto m        = pushedMotion({0, 0, 0}, c, 256, false, 2);
    // acceleration 256/512/(2/20) = 5 -> +5/8 upwards
    EXPECT_NEAR(m.y, 5 / 8.0, 1e-9);
    EXPECT_NEAR(m.x, 0, 1e-12);
    auto pulled = pushedMotion({0, 0, 0}, AirCurrent{Face::Up, false, 20, {}}, 64, false, 10);
    EXPECT_LT(pulled.y, 0);
    auto sneaking = pushedMotion({0, 0, 0}, c, 256, true, 2);
    EXPECT_NEAR(sneaking.y, 256 / 4096.0 / .1 / 8, 1e-9);
}

TEST(Bulk, Nozzle) {
    auto p = nozzlePush({3, 0, 4}, 10, true, false);
    ASSERT_TRUE(p.has_value());
    EXPECT_NEAR(p->x, .6 * 5 / 32, 1e-9);
    EXPECT_NEAR(p->z, .8 * 5 / 32, 1e-9);
    EXPECT_FALSE(nozzlePush({0, 11, 0}, 10, true, false).has_value());
    EXPECT_FALSE(nozzlePush({0, 1, 0}, 10, false, false).has_value());
    auto pull = nozzlePush({0, 4, 0}, 10, false, true);
    ASSERT_TRUE(pull.has_value());
    EXPECT_NEAR(pull->y, -6 / 128.0, 1e-9);
}

TEST(Bulk, Millstone) {
    EXPECT_EQ(millstoneProcessingSpeed(0), 1);
    EXPECT_EQ(millstoneProcessingSpeed(15), 1);
    EXPECT_EQ(millstoneProcessingSpeed(-64), 4);
    EXPECT_EQ(millstoneProcessingSpeed(256), 16);
}

TEST(Bulk, CrusherSpeeds) {
    EXPECT_FLOAT_EQ(crushingSpeedOf(-100), 2);
    EXPECT_FLOAT_EQ(crusherProcessingSpeed(2, 1, false), 20);   // single items: as fast as allowed
    EXPECT_FLOAT_EQ(crusherProcessingSpeed(2, 64, false), 8 / 6.f);
    EXPECT_FLOAT_EQ(crusherProcessingSpeed(2, 64, true), 8);
    EXPECT_FLOAT_EQ(crusherProcessingSpeed(.01f, 64, false), .25f);
    EXPECT_FLOAT_EQ(crusherProcessingSpeed(0, 64, false), 0);
}

TEST(Bulk, ControllerFacing) {
    // wheels along z (axis x), partner to the south: the wheel turning positive pulls down
    EXPECT_EQ(controllerFacing(Axis::X, Face::South, 64), Face::Down);
    // the partner (side north, opposite speed) agrees
    EXPECT_EQ(controllerFacing(Axis::X, Face::North, -64), Face::Down);
    // reversed: up
    EXPECT_EQ(controllerFacing(Axis::X, Face::South, -64), Face::Up);
    // wheels along x (axis z), partner to the east
    EXPECT_EQ(controllerFacing(Axis::Z, Face::East, 64), Face::Up);
    EXPECT_EQ(controllerFacing(Axis::Z, Face::West, -64), Face::Up);
    // stacked wheels (axis x, partner above): a horizontal crusher
    EXPECT_EQ(controllerFacing(Axis::X, Face::Up, 64), Face::South);
    // vertical wheels side by side
    EXPECT_EQ(controllerFacing(Axis::Y, Face::East, 64), Face::North);
}

TEST(Bulk, CrusherOutput) {
    auto out = crusherOutPos(0, 10, 0, Face::Down);
    EXPECT_NEAR(out.y, 10 + .5 - .55, 1e-9);
    auto sp = crusherOutSpeed(Face::Down);
    EXPECT_DOUBLE_EQ(sp.y, 0);
    EXPECT_DOUBLE_EQ(crusherOutSpeed(Face::Up).y, .5);
    EXPECT_DOUBLE_EQ(crusherOutSpeed(Face::North).z, -.25);
    EXPECT_TRUE(crusherIntakeReached(Face::Down, {0.5, 10.2, 0.5}, 0, 10, 0));
    EXPECT_FALSE(crusherIntakeReached(Face::Down, {0.5, 10.8, 0.5}, 0, 10, 0));
    EXPECT_TRUE(crusherIntakeReached(Face::Up, {0.5, 10.8, 0.5}, 0, 10, 0));
    auto m = crusherEntityMotion(Face::Down, 2, {0.2, 10.5, 0.9}, 0, 0, false);
    EXPECT_NEAR(m.y, -.5, 1e-9);
    EXPECT_NEAR(m.x, .15, 1e-9);
    EXPECT_NEAR(m.z, -.2, 1e-9);
}
