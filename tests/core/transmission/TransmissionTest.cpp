#include <gtest/gtest.h>

#include "nlohmann/json.hpp"

#include "core/transmission/Transmission.h"

using namespace lv::core::transmission;
using lv::core::kinetics::Face;

TEST(TransmissionChain, PlacedNextToAChainStartsAConnection) {
    // two chain drives along y side by side on x: the new one (west) sees the old one east
    ChainState fresh{Axis::Y, ChainPart::None, false};
    ChainState other{Axis::Y, ChainPart::None, false};
    auto       s = chainUpdateShape(fresh, Face::East, other);
    EXPECT_EQ(s.part, ChainPart::Start);
    EXPECT_TRUE(s.alongFirst); // axis y: along first = x
    EXPECT_EQ(chainConnectionAxis(s), Axis::X);
    // the old one sees the new one on its west side
    auto o = chainUpdateShape(other, Face::West, s);
    EXPECT_EQ(o.part, ChainPart::End);
    EXPECT_TRUE(chainConnected(s, o, Face::East));
    EXPECT_TRUE(chainConnected(o, s, Face::West));
    EXPECT_FALSE(chainConnected(s, o, Face::West));
    // a third one east of the end turns it into a middle
    auto m = chainUpdateShape(o, Face::East, ChainState{Axis::Y, ChainPart::None, false});
    EXPECT_EQ(m.part, ChainPart::Middle);
    // removing the neighbour east of a middle makes it an end again
    EXPECT_EQ(chainUpdateShape(m, Face::East, std::nullopt).part, ChainPart::End);
    // an end losing its start neighbour becomes single
    EXPECT_EQ(chainUpdateShape(o, Face::West, std::nullopt).part, ChainPart::None);
    // faces along the axis never change the shape
    EXPECT_EQ(chainUpdateShape(fresh, Face::Up, other), fresh);
    // a neighbour with a different axis along the face is ignored
    EXPECT_EQ(chainUpdateShape(fresh, Face::East, ChainState{Axis::X, ChainPart::None, false}), fresh);
}

TEST(TransmissionChain, GearshiftModifier) {
    EXPECT_FLOAT_EQ(chainGearshiftModifier(0), 1.f);
    EXPECT_FLOAT_EQ(chainGearshiftModifier(15), 2.f);
    EXPECT_FLOAT_EQ(chainGearshiftModifier(7), 1.5f);
}

TEST(TransmissionGauge, RotationAxisAndHeads) {
    EXPECT_EQ(gaugeRotationAxis(Face::North, true), Axis::X);
    EXPECT_EQ(gaugeRotationAxis(Face::North, false), Axis::Y);
    EXPECT_EQ(gaugeRotationAxis(Face::East, false), Axis::Z);
    EXPECT_EQ(gaugeRotationAxis(Face::Up, true), Axis::X);
    // horizontal facing, horizontal axis: one head on the facing side
    EXPECT_TRUE(gaugeHeadOnFace(Face::North, true, Face::North));
    EXPECT_FALSE(gaugeHeadOnFace(Face::North, true, Face::South));
    EXPECT_FALSE(gaugeHeadOnFace(Face::North, true, Face::East));
    // vertical axis: only the facing side
    EXPECT_TRUE(gaugeHeadOnFace(Face::North, false, Face::North));
    EXPECT_FALSE(gaugeHeadOnFace(Face::North, false, Face::West));
    // facing up, axis x: heads on north and south
    EXPECT_TRUE(gaugeHeadOnFace(Face::Up, true, Face::North));
    EXPECT_TRUE(gaugeHeadOnFace(Face::Up, true, Face::South));
    EXPECT_FALSE(gaugeHeadOnFace(Face::Up, true, Face::Up));
}

TEST(TransmissionGauge, Dials) {
    EXPECT_FLOAT_EQ(speedDialTarget(0, 30, 100, 256), 0.f);
    EXPECT_FLOAT_EQ(speedDialTarget(15, 30, 100, 256), .225f);
    EXPECT_FLOAT_EQ(speedDialTarget(-30, 30, 100, 256), .45f);
    EXPECT_FLOAT_EQ(speedDialTarget(100, 30, 100, 256), .75f);
    EXPECT_FLOAT_EQ(speedDialTarget(256, 30, 100, 256), 1.125f);
    EXPECT_FLOAT_EQ(stressDialTarget(true, 16, false, 64, 128), .5f);
    EXPECT_FLOAT_EQ(stressDialTarget(true, 16, true, 192, 128), 1.125f);
    EXPECT_FLOAT_EQ(stressDialTarget(true, 0, false, 64, 128), 0.f);
    EXPECT_FLOAT_EQ(stressDialTarget(false, 16, false, 64, 128), 0.f);
    EXPECT_EQ(gaugeComparatorSignal(0), 0);
    EXPECT_EQ(gaugeComparatorSignal(.5f), 7);
    EXPECT_EQ(gaugeComparatorSignal(.51f), 8);
    EXPECT_EQ(gaugeComparatorSignal(1.125f), 15);
    EXPECT_EQ(dialStateOf(0), 0);
    EXPECT_EQ(dialStateOf(1.125f), kDialStates - 1);
    EXPECT_NEAR(dialOfState(dialStateOf(.5f)), .5f, .08f);
}

TEST(TransmissionSequencer, Durations) {
    Instruction turn{Instr::TurnAngle, 1, 90};
    // 16 RPM -> 4.8 degrees per tick: ceil(90 / 4.8) + 2 = 21
    EXPECT_EQ(instructionDuration(turn, 0, 16), 21);
    Instruction fast{Instr::TurnAngle, 2, 90};
    EXPECT_EQ(instructionDuration(fast, 0, 16), 12);
    Instruction piston{Instr::TurnDistance, -1, 5};
    // 64 RPM -> 0.125 m per tick: ceil(40) + 2
    EXPECT_EQ(instructionDuration(piston, 0, 64), 42);
    EXPECT_EQ(instructionDuration(Instruction{Instr::Delay, 1, 30}, 0, 16), 30);
    EXPECT_EQ(instructionDuration(Instruction{Instr::Await, 1, 1}, 0, 16), -1);
    EXPECT_FLOAT_EQ(instructionTickProgress(turn, 16), 4.8f);
    EXPECT_EQ(instructionSpeedModifier(piston), -1);
    EXPECT_EQ(instructionSpeedModifier(Instruction{Instr::Delay, 2, 10}), 0);
    EXPECT_EQ(formatInstructionValue(Instr::Delay, 40), "2s");
    EXPECT_EQ(formatInstructionValue(Instr::Delay, 10), "10t");
    EXPECT_EQ(formatInstructionValue(Instr::TurnDistance, 5), "5m");
    for (int m : {0, 1, 2, -1, -2}) EXPECT_EQ(stateToModifier(modifierToState(m)), m);
}

TEST(TransmissionSequencer, ListEditing) {
    auto list = defaultInstructions();
    ASSERT_EQ(list.size(), 2u);
    setInstructionType(list, 1, Instr::Delay);
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[1].value, 10);
    EXPECT_EQ(list[2].type, Instr::End);
    setInstructionType(list, 0, Instr::End);
    EXPECT_EQ(list.size(), 1u);
    for (int i = 0; i < 6; ++i) setInstructionType(list, static_cast<int>(list.size()) - 1, Instr::Await);
    EXPECT_EQ(list.size(), static_cast<std::size_t>(kMaxInstructions));
    auto round = instructionsFromJson(instructionsToJson(list));
    EXPECT_EQ(round, list);
    EXPECT_EQ(instructionsFromJson(nlohmann::json::array()), defaultInstructions());
    auto clamped = instructionsFromJson(nlohmann::json::parse(R"([{"type": 0, "speed": 7, "value": 9999}])"));
    EXPECT_EQ(clamped[0].value, 360);
    EXPECT_EQ(clamped[0].speedModifier, 1);
}

TEST(TransmissionController, ConveyedSpeed) {
    // cog drives an unpowered controller: the controller turns at the target speed
    EXPECT_FLOAT_EQ(controllerConveyedSpeed(0, 32, 16, false, false, true), 16.f);
    // the controller (driven by its shaft) drives the cog: target speed
    EXPECT_FLOAT_EQ(controllerConveyedSpeed(32, 0, 16, true, false, false), 16.f);
    EXPECT_FLOAT_EQ(controllerConveyedSpeed(32, 0, -48, true, false, false), -48.f);
    // target 0 stops it
    EXPECT_FLOAT_EQ(controllerConveyedSpeed(32, 0, 0, true, false, false), 0.f);
    // the faster existing speed of the same direction wins
    EXPECT_FLOAT_EQ(controllerConveyedSpeed(32, 64, 16, true, false, false), 64.f);
}

TEST(TransmissionPlacement, OrderedByDistance) {
    auto all = orderedByDistance(.4, .1, 0, [](Face) { return true; });
    EXPECT_EQ(all.front(), Face::East);
    auto noX = orderedByDistance(.4, .1, -.05, [](Face f) { return lv::core::kinetics::axisOf(f) != Axis::X; });
    EXPECT_EQ(noX.front(), Face::Up);
    EXPECT_EQ(noX.size(), 4u);
}
