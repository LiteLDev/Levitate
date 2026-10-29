#include <gtest/gtest.h>

#include "core/pistons/Linear.h"

using namespace lv::core::pistons;
using lv::core::contraption::Vec3d;

TEST(Pistons, LinearSpeed) {
    EXPECT_FLOAT_EQ(convertToLinear(256), 0.5f);
    EXPECT_FLOAT_EQ(actuatorSpeed(256), 0.49f);
    EXPECT_FLOAT_EQ(actuatorSpeed(-256), -0.49f);
    EXPECT_FLOAT_EQ(actuatorSpeed(64), 0.125f);
}

TEST(Pistons, PistonDirection) {
    // Create: positive speed extends pistons facing south / west / down, retracts the others
    EXPECT_GT(pistonMovementSpeed(64, Dir::South, 1, 4), 0);
    EXPECT_GT(pistonMovementSpeed(64, Dir::West, 1, 4), 0);
    EXPECT_GT(pistonMovementSpeed(64, Dir::Down, 1, 4), 0);
    EXPECT_LT(pistonMovementSpeed(64, Dir::North, 1, 4), 0);
    EXPECT_LT(pistonMovementSpeed(64, Dir::East, 1, 4), 0);
    EXPECT_LT(pistonMovementSpeed(64, Dir::Up, 1, 4), 0);
    // clamped to the range
    EXPECT_FLOAT_EQ(pistonMovementSpeed(256, Dir::South, 3.8f, 4), 0.2f);
    EXPECT_FLOAT_EQ(pistonMovementSpeed(-256, Dir::South, 0.1f, 4), -0.1f);
}

TEST(Pistons, GridAndStop) {
    EXPECT_EQ(gridOffset(2.49f, 5), 2);
    EXPECT_EQ(gridOffset(2.5f, 5), 3);
    EXPECT_EQ(gridOffset(7.f, 5), 5);
    EXPECT_EQ(stopAction(MovementMode::MovePlace, 2, 0, false), StopAction::Disassemble);
    EXPECT_EQ(stopAction(MovementMode::MoveNeverPlace, 0, 0, false), StopAction::Wait);
    EXPECT_EQ(stopAction(MovementMode::MoveNeverPlace, 0, 0, true), StopAction::Disassemble);
    EXPECT_EQ(stopAction(MovementMode::MovePlaceReturned, 2.4f, 2, false), StopAction::Disassemble);
    EXPECT_EQ(stopAction(MovementMode::MovePlaceReturned, 3, 2, false), StopAction::Wait);
}

TEST(Pistons, ElevatorAcceleratesAndArrives) {
    float offset = 0, prev = 0;
    int   ticks  = 0;
    bool  arrived = false;
    float top     = 0;
    while (ticks < 2000) {
        auto s = elevatorStep(64, offset, 20, prev);
        prev   = s.speed;
        top    = std::max(top, s.speed);
        offset += s.speed;
        ++ticks;
        if (s.arrived) {
            arrived = true;
            break;
        }
    }
    EXPECT_TRUE(arrived);
    EXPECT_LE(top, 0.25f + 1e-6f); // 64 RPM * 2 / 512
    EXPECT_NEAR(offset, 20, 0.5);
    auto first = elevatorStep(64, 0, 20, 0);
    EXPECT_LT(first.speed, 0.02f); // acceleration limited
    EXPECT_DOUBLE_EQ(elevatorSettle(5.0, 5.001), 5.001);
    EXPECT_DOUBLE_EQ(elevatorSettle(5.0, 6.0), 5.25);
}

TEST(Pistons, DirectionalAxis) {
    EXPECT_EQ(directionalAxis(Dir::East, true), Axis::Y);
    EXPECT_EQ(directionalAxis(Dir::East, false), Axis::Z);
    EXPECT_EQ(directionalAxis(Dir::Up, true), Axis::X);
    EXPECT_EQ(directionalAxis(Dir::Up, false), Axis::Z);
    EXPECT_EQ(directionalAxis(Dir::North, true), Axis::X);
    EXPECT_EQ(directionalAxis(Dir::North, false), Axis::Y);
    EXPECT_FALSE(alongFirstFor(Dir::North, Axis::Z).has_value());
    // facing north, shaft along x; a quarter turn about y: facing west/east, shaft along z
    bool af = rotateAlongFirst(Dir::North, true, Axis::Y, 1);
    EXPECT_EQ(directionalAxis(lv::core::contraption::rotate(Dir::North, Axis::Y, 1), af), Axis::Z);
    // about the facing axis: the shaft turns too
    af = rotateAlongFirst(Dir::Up, true, Axis::Y, 1);
    EXPECT_EQ(directionalAxis(Dir::Up, af), Axis::Z);
}

TEST(Pistons, Gantry) {
    EXPECT_FLOAT_EQ(gantryPinionModifier(Dir::Up, Dir::North), -1);
    EXPECT_FLOAT_EQ(gantryPinionModifier(Dir::Up, Dir::South), 1);
    EXPECT_FLOAT_EQ(gantryPinionModifier(Dir::Down, Dir::South), -1);
    EXPECT_FLOAT_EQ(gantryPinionModifier(Dir::East, Dir::Down), -1);
    EXPECT_FLOAT_EQ(gantryPinionModifier(Dir::West, Dir::Up), -1);
    EXPECT_FLOAT_EQ(gantryPinionModifier(Dir::South, Dir::West), -1);
    EXPECT_FLOAT_EQ(pinionMovementSpeed(64), -0.125f);
    EXPECT_EQ(shaftPart(true, false), ShaftPart::Start);
    EXPECT_EQ(shaftPart(false, true), ShaftPart::End);
    EXPECT_EQ(shaftPart(true, true), ShaftPart::Middle);
    EXPECT_EQ(shaftPart(false, false), ShaftPart::Single);
    EXPECT_TRUE(canAssembleOn(ShaftPart::Start, false, 0.1f));
    EXPECT_FALSE(canAssembleOn(ShaftPart::Start, false, -0.1f));
    EXPECT_TRUE(canAssembleOn(ShaftPart::End, false, -0.1f));
    EXPECT_TRUE(canAssembleOn(ShaftPart::Middle, false, -0.1f));
    EXPECT_FALSE(canAssembleOn(ShaftPart::Middle, true, -0.1f));
    EXPECT_FALSE(canAssembleOn(ShaftPart::Single, false, 0.1f));
    // moving east (positive) past the centre of block 4
    EXPECT_TRUE(crossesShaftCentre(4.3, 4.6, 0.3f, Dir::East));
    EXPECT_FALSE(crossesShaftCentre(4.1, 4.3, 0.2f, Dir::East));
    // moving west (negative) past the centre
    EXPECT_TRUE(crossesShaftCentre(4.7, 4.4, -0.3f, Dir::East));
    EXPECT_FALSE(crossesShaftCentre(4.9, 4.7, -0.2f, Dir::East));
}

TEST(Pistons, Colliders) {
    std::vector<ColliderBlock> blocks{{{0, 0, 0}, true}, {{1, 0, 0}, true}, {{0, 1, 0}, false}};
    auto east = colliders(blocks, Dir::East);
    ASSERT_EQ(east.size(), 1u);
    EXPECT_EQ(east[0], (Pos{1, 0, 0}));
    auto up = colliders(blocks, Dir::Up);
    EXPECT_EQ(up.size(), 2u);
    EXPECT_EQ(collisionGrid(Vec3d{2.3, 5, -0.5}, Dir::East), (Pos{3, 5, -1}));
    EXPECT_EQ(collisionGrid(Vec3d{2.3, 5, -0.5}, Dir::West), (Pos{2, 5, -1}));
}

TEST(Pistons, FloorNames) {
    std::vector<Floor> f{{10, "", ""}, {20, "", ""}, {30, "", ""}};
    initFloorNames(f);
    EXPECT_EQ(f[0].shortName, "0");
    EXPECT_EQ(f[1].shortName, "1");
    EXPECT_EQ(f[2].shortName, "2");

    std::vector<Floor> g{{10, "", ""}, {20, "5", ""}, {30, "", ""}};
    initFloorNames(g);
    EXPECT_EQ(g[0].shortName, "4");
    EXPECT_EQ(g[1].shortName, "5");
    EXPECT_EQ(g[2].shortName, "6");

    std::vector<Floor> h{{10, "B", ""}, {20, "", ""}};
    initFloorNames(h);
    EXPECT_EQ(h[0].shortName, "B");
    EXPECT_EQ(h[1].shortName, "0");
}
