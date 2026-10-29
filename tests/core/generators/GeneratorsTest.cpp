#include <gtest/gtest.h>

#include <set>
#include <tuple>

#include "core/generators/Generators.h"

using namespace lv::core::generators;
using lv::core::kinetics::offsetOf;

namespace {
std::tuple<int, int, int> T(Offset const& o) { return {o.x, o.y, o.z}; }
} // namespace

TEST(Generators, Dyes) {
    EXPECT_EQ(dyeColourOf("minecraft:white_dye"), 0);
    EXPECT_EQ(dyeColourOf("minecraft:light_blue_dye"), 3);
    EXPECT_EQ(dyeColourOf("minecraft:black_dye"), 15);
    EXPECT_EQ(dyeColourOf("minecraft:ink_sac"), -1);
    EXPECT_EQ(dyeColourOf("minecraft:dye"), -1);
    EXPECT_EQ(dyeColourOf("levitate:red_dye"), -1);
}

TEST(Generators, Planks) {
    EXPECT_EQ(woodOfPlanks("minecraft:oak_planks"), 0);
    EXPECT_EQ(woodOfPlanks("minecraft:spruce_planks"), kDefaultWood);
    EXPECT_EQ(woodOfPlanks("minecraft:pale_oak_planks"), 11);
    EXPECT_EQ(woodOfPlanks("minecraft:oak_log"), -1);
    EXPECT_EQ(woodOfPlanks("minecraft:stone"), -1);
}

TEST(Generators, Directions) {
    EXPECT_EQ(convertToDirection(32, Face::East), 32);
    EXPECT_EQ(convertToDirection(32, Face::West), -32);
    EXPECT_EQ(convertToDirection(32, Face::Down), -32);
    EXPECT_FLOAT_EQ(convertToAngular(32), 9.6f);
    // clockwise turns visit every in-plane face once
    for (Axis axis : lv::core::kinetics::kAxes) {
        for (Face f : lv::core::kinetics::kFaces) {
            if (lv::core::kinetics::axisOf(f) == axis) {
                EXPECT_EQ(clockWise(f, axis), f);
                continue;
            }
            Face g = f;
            std::set<Face> seen;
            for (int i = 0; i < 4; ++i) {
                seen.insert(g);
                g = clockWise(g, axis);
            }
            EXPECT_EQ(g, f);
            EXPECT_EQ(seen.size(), 4u);
        }
    }
    EXPECT_EQ(clockWise(Face::North, Axis::Y), Face::East);
    EXPECT_EQ(clockWise(Face::Down, Axis::X), Face::South);
    EXPECT_EQ(clockWise(Face::Up, Axis::Z), Face::East);
}

TEST(Generators, Placement) {
    // a shaft below: the crank's shaft points down (facing up), the motor faces down
    auto below = [](Face f) { return f == Face::Down; };
    auto pref  = preferredFacing(below);
    ASSERT_TRUE(pref);
    EXPECT_EQ(*pref, Face::Down);
    EXPECT_EQ(crankFacing(pref, Face::North, false), Face::Up);
    EXPECT_EQ(crankFacing(pref, Face::North, true), Face::North);
    EXPECT_EQ(crankFacing(std::nullopt, Face::West, false), Face::West);
    EXPECT_EQ(motorFacing(pref, Face::North, false), Face::Down);
    EXPECT_EQ(motorFacing(std::nullopt, Face::North, false), Face::South);
    EXPECT_EQ(motorFacing(pref, Face::North, true), Face::North);
    EXPECT_EQ(directionalFacing(pref, Face::North, false), Face::Up);
    EXPECT_EQ(directionalFacing(std::nullopt, Face::East, false), Face::West);
    // shafts on two axes cancel; on one axis the later side wins
    EXPECT_FALSE(preferredFacing([](Face f) { return f == Face::Down || f == Face::East; }));
    EXPECT_EQ(*preferredFacing([](Face f) { return f == Face::West || f == Face::East; }), Face::East);
    EXPECT_EQ(*preferredAxis([](Face f) { return f == Face::North; }), Axis::Z);
    EXPECT_FALSE(preferredAxis([](Face f) { return f == Face::North || f == Face::Up; }));
    EXPECT_EQ(pillarAxis(Axis::Z, Face::Up, Face::East, false), Axis::Z);
    EXPECT_EQ(pillarAxis(Axis::Z, Face::Up, Face::East, true), Axis::Y);
    EXPECT_EQ(pillarAxis(std::nullopt, Face::Up, Face::East, true), Axis::X);
    EXPECT_EQ(nearestLooking(0.2, -0.9, 0.3), Face::Down);
    EXPECT_EQ(nearestLooking(0.8, 0.1, -0.5), Face::East);
    EXPECT_EQ(nearestLooking(0.1, 0.2, -0.9), Face::North);
}

TEST(Generators, Crank) {
    EXPECT_EQ(crankSpeed(false, false, Face::Up), 0);
    EXPECT_EQ(crankSpeed(true, false, Face::Up), 32);
    EXPECT_EQ(crankSpeed(true, true, Face::Up), -32);
    EXPECT_EQ(crankSpeed(true, false, Face::North), -32);
    // the handle chases 10/3 of the speed, a quarter of the gap per tick
    EXPECT_FLOAT_EQ(crankChase(0, 32), 32 * 10.f / 3.f * .25f);
    float v = 0;
    for (int i = 0; i < 200; ++i) v = crankChase(v, 32);
    EXPECT_NEAR(v, 32 * 10.f / 3.f, 1e-3);
}

TEST(Generators, Valve) {
    auto t = valveTurn(45, false);
    EXPECT_EQ(t.useTicks, 7); // ceil(45 / 9.6) + 2
    EXPECT_EQ(t.targetAngle, 90);
    EXPECT_FALSE(t.clockwise);
    t = valveTurn(-180, false);
    EXPECT_EQ(t.useTicks, 21);
    EXPECT_EQ(t.targetAngle, -180);
    EXPECT_TRUE(t.clockwise);
    t = valveTurn(136, true);
    EXPECT_EQ(t.targetAngle, 180);
    EXPECT_TRUE(t.clockwise);
    EXPECT_EQ(valveTurn(1, false).useTicks, 3);
    EXPECT_FLOAT_EQ(valveAngle(90, 10, 5, false, Face::Up), 45);
    EXPECT_FLOAT_EQ(valveAngle(90, 10, 50, false, Face::Up), 90);
    EXPECT_FLOAT_EQ(valveAngle(90, 10, 5, true, Face::Up), -45);
    EXPECT_FLOAT_EQ(valveAngle(90, 10, 10, false, Face::West), -90);
}

TEST(Generators, WheelOffsets) {
    auto small = wheelOffsets(Axis::Z, false);
    EXPECT_EQ(small.size(), 4u);
    for (auto const& o : small) EXPECT_EQ(o.z, 0);
    auto large = wheelOffsets(Axis::Y, true);
    EXPECT_EQ(large.size(), 12u);
    std::set<std::tuple<int, int, int>> unique;
    for (auto const& o : large) {
        EXPECT_EQ(o.y, 0);
        EXPECT_EQ(std::max(std::abs(o.x), std::abs(o.z)), 2);
        EXPECT_LE(std::min(std::abs(o.x), std::abs(o.z)), 1);
        unique.insert(T(o));
    }
    EXPECT_EQ(unique.size(), 12u);
}

TEST(Generators, FlowScore) {
    // Z-axis wheel, water running east over its top: the top moves +x = clockwise seen from +z
    auto topEast = [](Offset const& o) { return o.y == 1 ? Vec3d{1, 0, 0} : Vec3d{}; };
    EXPECT_EQ(flowScore(Axis::Z, false, topEast), -1);
    EXPECT_EQ(wheelSpeed(flowScore(Axis::Z, false, topEast), false), -8);
    // the same flow below it turns it the other way and cancels out
    auto both = [](Offset const& o) { return o.y != 0 ? Vec3d{1, 0, 0} : Vec3d{}; };
    EXPECT_EQ(flowScore(Axis::Z, false, both), 0);
    // falling water on the east side
    auto fallEast = [](Offset const& o) { return o.x == 1 ? Vec3d{0.1, -0.99, 0} : Vec3d{}; };
    EXPECT_EQ(flowScore(Axis::Z, false, fallEast), -1);
    // flow along the axis does nothing
    auto axial = [](Offset const&) { return Vec3d{0, 0, 1}; };
    EXPECT_EQ(flowScore(Axis::Z, false, axial), 0);
    // only the in-plane part of the flow counts
    auto diag = [](Offset const& o) { return o.y == 1 ? Vec3d{1, 0, 1} : Vec3d{}; };
    EXPECT_EQ(flowScore(Axis::Z, false, diag), -1);
    // a large wheel with a bubble column rising on its west side (Y axis: horizontal wheel ignores y flow)
    auto bubbles = [](Offset const& o) { return o.x == -2 ? Vec3d{0, 1, 0} : Vec3d{}; };
    EXPECT_EQ(flowScore(Axis::Y, true, bubbles), 0);
    EXPECT_EQ(flowScore(Axis::Z, true, bubbles), -3);
    EXPECT_EQ(wheelSpeed(-3, true), -4);
    EXPECT_EQ(wheelSpeed(2, false), 8);
    EXPECT_EQ(wheelSpeed(0, true), 0);
}

TEST(Generators, Structure) {
    for (Axis axis : lv::core::kinetics::kAxes) {
        auto                                cells = structureCells(axis);
        std::set<std::tuple<int, int, int>> unique;
        std::set<int>                       segments;
        for (auto const& c : cells) {
            unique.insert(T(c.offset));
            segments.insert(c.segment);
            EXPECT_EQ(lv::core::kinetics::choose(axis, c.offset), 0);
            EXPECT_EQ(T(segmentOffset(c.segment, axis)), T(c.offset));
            // the facing leads to the wheel (edges) or to an edge cell (corners)
            auto f      = offsetOf(c.facing);
            Offset next{c.offset.x + f.x, c.offset.y + f.y, c.offset.z + f.z};
            int    dist = std::abs(next.x) + std::abs(next.y) + std::abs(next.z);
            EXPECT_LE(dist, 1);
        }
        EXPECT_EQ(unique.size(), 8u);
        EXPECT_EQ(segments.size(), 8u);
    }
}
