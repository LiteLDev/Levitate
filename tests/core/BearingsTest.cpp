#include <gtest/gtest.h>

#include <cmath>

#include "core/bearings/BearingMath.h"

using namespace lv::core::bearings;

TEST(Bearings, AngularSpeed) {
    EXPECT_FLOAT_EQ(angularSpeed(32), 9.6f);
    EXPECT_FLOAT_EQ(angularSpeed(-256), -76.8f);
}

TEST(Bearings, DisassemblyModes) {
    EXPECT_TRUE(canDisassemble(RotationMode::RotatePlace, 123));
    EXPECT_TRUE(canDisassemble(RotationMode::RotatePlaceReturned, 30));
    EXPECT_TRUE(canDisassemble(RotationMode::RotatePlaceReturned, -340));
    EXPECT_FALSE(canDisassemble(RotationMode::RotatePlaceReturned, 90));
    EXPECT_FALSE(canDisassemble(RotationMode::RotatePlaceReturned, 45));
    EXPECT_FALSE(canDisassemble(RotationMode::RotateNeverPlace, 0));
}

TEST(Bearings, AngleWraps) {
    EXPECT_NEAR(advanceAngle(350, 20), 10, 1e-4);
    EXPECT_NEAR(advanceAngle(-350, -20), -10, 1e-4);
    EXPECT_NEAR(advanceAngle(10, -20), -10, 1e-4);
}

TEST(Bearings, WindmillSpeed) {
    EXPECT_EQ(windmillSpeed(8, 8, true), 1.f);
    EXPECT_EQ(windmillSpeed(15, 8, true), 1.f);
    EXPECT_EQ(windmillSpeed(16, 8, true), 2.f);
    EXPECT_EQ(windmillSpeed(2000, 8, false), -16.f);
    EXPECT_EQ(windmillSpeed(3, 8, true), 1.f); // at least 1 RPM once assembled
}

TEST(Bearings, ShortestAngleDiff) {
    EXPECT_NEAR(shortestAngleDiff(0, 90), 90, 1e-4);
    EXPECT_NEAR(shortestAngleDiff(0, 270), -90, 1e-4);
    EXPECT_NEAR(shortestAngleDiff(350, 10), 20, 1e-4);
    EXPECT_NEAR(shortestAngleDiff(10, -350), 0, 1e-4);
    EXPECT_NEAR(std::abs(shortestAngleDiff(0, 180)), 180, 1e-4);
    EXPECT_NEAR(shortestAngleDiff(0, 180, 5), 180, 1e-4);
    EXPECT_NEAR(shortestAngleDiff(0, 180, -5), -180, 1e-4);
}

TEST(Bearings, ClockTargets) {
    // day time 0 is 6:00, 6000 is noon, 18000 midnight
    EXPECT_NEAR(hourTarget(0, 1, false), -180, 1e-4);
    EXPECT_NEAR(hourTarget(6000, 1, false), 0, 1e-4);
    EXPECT_NEAR(hourTarget(9000, 1, false), -90, 1e-4);
    EXPECT_NEAR(hourTarget(9000, -1, false), 90, 1e-4);
    EXPECT_NEAR(hourTarget(6000, 1, true), -180, 1e-4);
    EXPECT_NEAR(hourTarget(24000 + 6000, 1, false), 0, 1e-4);
    EXPECT_NEAR(minuteTarget(500, 1), -180, 1e-4);
    EXPECT_NEAR(minuteTarget(6250, 1), -90, 1e-4);
}

TEST(Bearings, HandStepApproachesTarget) {
    EXPECT_NEAR(handStep(0, -90, 9.6), -9.6, 1e-4);
    EXPECT_NEAR(handStep(0, 90, 9.6), 9.6, 1e-4);
    EXPECT_NEAR(handStep(-85, -90, 9.6), -5, 1e-4);
    EXPECT_NEAR(handStep(-90, -90, 9.6), 0, 1e-4);
    EXPECT_EQ(handStep(0, 90, 0), 0);
}

TEST(Bearings, CartActions) {
    EXPECT_EQ(actionFor(RailType::Regular, true, false), CartAction::Assemble);
    EXPECT_EQ(actionFor(RailType::Regular, false, false), CartAction::Disassemble);
    EXPECT_EQ(actionFor(RailType::Powered, true, false), CartAction::AssembleAccelerate);
    EXPECT_EQ(actionFor(RailType::Powered, false, false), CartAction::DisassembleBrake);
    EXPECT_EQ(actionFor(RailType::Detector, false, false), CartAction::AssembleAccelerate);
    EXPECT_EQ(actionFor(RailType::Detector, false, true), CartAction::Disassemble);
    EXPECT_EQ(actionFor(RailType::Activator, true, false), CartAction::Disassemble);
    EXPECT_EQ(actionFor(RailType::Activator, false, false), CartAction::Pass);
    EXPECT_EQ(actionFor(RailType::Controller, true, false), CartAction::AssembleAccelerateDirectional);
    EXPECT_EQ(actionFor(RailType::Controller, false, false), CartAction::DisassembleBrake);
    EXPECT_TRUE(shouldAssemble(CartAction::AssembleAccelerate));
    EXPECT_TRUE(shouldDisassemble(CartAction::DisassembleBrake));
    EXPECT_FALSE(shouldAssemble(CartAction::Pass));
    EXPECT_FALSE(shouldDisassemble(CartAction::Pass));
}

TEST(Bearings, ControllerRail) {
    using lv::core::contraption::Dir;
    EXPECT_EQ(railDirection(true, false), Dir::West);
    EXPECT_EQ(railDirection(true, true), Dir::East);
    EXPECT_EQ(railDirection(false, false), Dir::North);
    EXPECT_EQ(railDirection(false, true), Dir::South);
    auto v = controllerVelocity({0, 0, 0}, Dir::East, 15, 0.4);
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(v->x, 0.4, 1e-9);
    v = controllerVelocity({0.1, 0, 0}, Dir::East, 5, 0.4);
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(v->x, 0.4 / 3, 1e-9);
    EXPECT_FALSE(controllerVelocity({-0.3, 0, 0}, Dir::East, 15, 0.4).has_value()); // moving against: brake
    EXPECT_FALSE(controllerVelocity({0, 0, 0}, Dir::East, 0, 0.4).has_value());     // no signal: brake
    auto b = brakeVelocity({1.6, 0, -0.8});
    EXPECT_NEAR(b.x, 0.1, 1e-9);
    EXPECT_NEAR(b.z, -0.05, 1e-9);
}

TEST(Bearings, Headings) {
    using lv::core::contraption::Dir;
    EXPECT_NEAR(headingOf(Dir::East), 0, 1e-9);
    EXPECT_NEAR(headingOf(Dir::North), 90, 1e-9);
    EXPECT_NEAR(std::abs(headingOf(Dir::West)), 180, 1e-9);
    EXPECT_NEAR(headingOf(Dir::South), -90, 1e-9);
    EXPECT_NEAR(maxYawApproach(0.4, 3), 4.8, 1e-9);
    EXPECT_NEAR(maxYawApproach(0.4, 12), 2.4, 1e-9);
}

TEST(Bearings, SailPlacementOrder) {
    using lv::core::contraption::Axis;
    using lv::core::contraption::Dir;
    // a sail facing up: the helper only offers horizontal neighbours, nearest to the hit first
    auto order = orderedByDistanceExceptAxis({0.95, 1.0, 0.5}, Axis::Y);
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], Dir::East);
    EXPECT_EQ(order[3], Dir::West);
    order = orderedByDistanceExceptAxis({0.5, 0.5, 0.0}, Axis::X);
    EXPECT_EQ(order[0], Dir::North);
}
