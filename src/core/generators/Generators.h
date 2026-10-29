#pragma once

// Generators (U7): the game-independent rules of Create 6.0.10's hand crank, valve handles, water
// wheels and creative motor (pure C++, unit-tested in tests/core/generators/).
//
// Speeds are RPM along the positive axis (Create's convention: the sign is the direction of a
// right-hand rotation about the axis' positive direction).

#include <array>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "core/kinetics/Types.h"

namespace lv::core::generators {

using kinetics::Axis;
using kinetics::Face;
using kinetics::Offset;

// ---- Create's constants ----------------------------------------------------------------------------------------
inline constexpr int   kCrankSpeed        = 32; ///< HandCrankBlock.getRotationSpeed (valve handles too)
inline constexpr int   kCrankUseTicks     = 10; ///< HandCrankBlockEntity.turn: inUse = 10
inline constexpr float kCrankHunger       = 0.01f; ///< CKinetics.crankHungerMultiplier default
inline constexpr int   kCrankSoundPeriod  = 10; ///< HandCrankBlockEntity.tickAudio: every 10 ticks
inline constexpr int   kValveDefaultAngle = 45; ///< ValveHandleBlockEntity: angleInput.setValue(45)
inline constexpr int   kValveMaxAngle     = 180;
inline constexpr int   kValveMilestone    = 45;
inline constexpr int   kValveCooldown     = 4;
inline constexpr int   kWheelSpeed        = 8; ///< WaterWheelBlockEntity.getGeneratedSpeed: 8 / size
inline constexpr int   kWheelLazyTicks    = 60; ///< WaterWheelBlockEntity: setLazyTickRate(60)
inline constexpr int   kMotorDefaultSpeed = 16; ///< CreativeMotorBlockEntity.DEFAULT_SPEED
inline constexpr int   kMotorMaxSpeed     = 256; ///< CreativeMotorBlockEntity.MAX_SPEED
inline constexpr int   kMotorMilestone    = 32; ///< KineticScrollValueBehaviour's board

/// The 16 dye colours in Minecraft's DyeColor order (ids `<colour>_valve_handle`).
inline constexpr std::array<std::string_view, 16> kDyeColours{
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
};
/// The colour index of a dye id ("minecraft:red_dye", also Bedrock's legacy ink sac, lapis,
/// cocoa beans and bone meal are *not* dyes for Create), -1 when it is not a dye.
[[nodiscard]] int dyeColourOf(std::string_view itemId);

/// Wood types a water wheel can take (block state `levitate:wood`), with their planks ids.
/// Index 1 (spruce) is Create's default material.
inline constexpr std::array<std::string_view, 12> kWoods{
    "oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry", "bamboo", "crimson", "warped", "pale_oak",
};
inline constexpr int kDefaultWood = 1;
/// The wood index of a planks item / block id ("minecraft:oak_planks", ...), -1 for anything else.
[[nodiscard]] int woodOfPlanks(std::string_view id);

// ---- directions ----------------------------------------------------------------------------------------------------
/// Create's KineticBlockEntity.convertToDirection: `axisSpeed` for a positive facing, negated otherwise.
[[nodiscard]] float convertToDirection(float axisSpeed, Face facing);
/// Create's KineticBlockEntity.convertToAngular: degrees per tick at `speed` RPM.
[[nodiscard]] constexpr float convertToAngular(float speed) { return speed * 360.f / 60.f / 20.f; }
/// Create's Direction.getClockWise(axis) (unchanged for faces on `axis`).
[[nodiscard]] Face clockWise(Face face, Axis axis);

// ---- placement (Create's getStateForPlacement) ----------------------------------------------------------------------
/// Create's getPreferredFacing: the side whose neighbour has a shaft towards the placed block;
/// sides on two different axes cancel out (nullopt), on one axis the last one in Direction order wins.
[[nodiscard]] std::optional<Face> preferredFacing(std::function<bool(Face side)> const& neighbourShaftTowardsUs);
/// RotatedPillarKineticBlock.getPreferredAxis (the same rule for axes).
[[nodiscard]] std::optional<Axis> preferredAxis(std::function<bool(Face side)> const& neighbourShaftTowardsUs);
/// The face the player looks along the most (Create's getNearestLookingDirection), from a view vector.
[[nodiscard]] Face nearestLooking(double dx, double dy, double dz);

/// HandCrankBlock (and valve handles): facing = the clicked face, or away from a preferred shaft.
[[nodiscard]] Face crankFacing(std::optional<Face> preferred, Face clickedFace, bool sneaking);
/// DirectionalKineticBlock (water wheel): towards the player, or away from a preferred shaft.
[[nodiscard]] Face directionalFacing(std::optional<Face> preferred, Face nearestLook, bool sneaking);
/// CreativeMotorBlock: like DirectionalKineticBlock, but the shaft faces the preferred side.
[[nodiscard]] Face motorFacing(std::optional<Face> preferred, Face nearestLook, bool sneaking);
/// RotatedPillarKineticBlock (large water wheel).
[[nodiscard]] Axis pillarAxis(std::optional<Axis> preferred, Face clickedFace, Face nearestLook, bool sneaking);

// ---- hand crank / valve handle ---------------------------------------------------------------------------------------
/// HandCrankBlockEntity.getGeneratedSpeed: +-32 RPM while in use, along the facing.
[[nodiscard]] float crankSpeed(bool inUse, bool clockwise, Face facing);

/// ValveHandleBlockEntity.activate: how long the valve turns and its rendered target angle.
struct ValveTurn {
    int  useTicks    = 0; ///< ticks the valve generates (ceil(angle / degrees per tick) + 2)
    int  targetAngle = 0; ///< degrees the handle model turns (+-90 or +-180, before direction)
    bool clockwise   = false;
};
/// `angle` is the value box value (-180..180, never 0; negative = clockwise row), `sneaking`
/// reverses (Create: `clockwise = value < 0 ^ backwards`).
[[nodiscard]] ValveTurn valveTurn(int angle, bool sneaking);
/// The handle's rendered angle (degrees) `elapsed` ticks (with partial ticks) into a turn of
/// `total` ticks (Create's ValveHandleBlockEntity.getIndependentAngle).
[[nodiscard]] float valveAngle(int targetAngle, int total, float elapsed, bool backwards, Face facing);

/// The client-side chase of a hand crank's handle (HandCrankBlockEntity.tick): returns the new
/// chasing velocity; the handle angle advances by it every tick (radians = angle / 360).
[[nodiscard]] float crankChase(float chasingVelocity, float speed);

// ---- water wheels -----------------------------------------------------------------------------------------------------
/// Positions (relative to the wheel) whose fluid flow drives it (Create's SMALL_OFFSETS /
/// LARGE_OFFSETS): the 4 in-plane neighbours of a small wheel, the 12 cells around a large one.
[[nodiscard]] std::vector<Offset> wheelOffsets(Axis axis, bool large);

struct Vec3d {
    double x = 0, y = 0, z = 0;
};
/// Create's WaterWheelBlockEntity.determineAndApplyFlowScore: each checked position whose flow
/// (projected onto the wheel's plane) runs along the wheel's positive tangent (|dot| > .5) adds
/// its sign.
[[nodiscard]] int flowScore(Axis axis, bool large, std::function<Vec3d(Offset const&)> const& flowAt);
/// WaterWheelBlockEntity.getGeneratedSpeed: clamp(score, -1, 1) * 8 / size.
[[nodiscard]] float wheelSpeed(int flowScore, bool large);

// ---- large water wheel structure ----------------------------------------------------------------------------------------
/// One of the 8 structure cells around a large water wheel (Create's LargeWaterWheelBlock.tick order).
struct StructureCell {
    Offset offset;  ///< from the wheel
    Face   facing;  ///< Create's WaterWheelStructuralBlock.FACING: towards its parent cell
    int    segment; ///< `levitate:segment` of the structure block (0..7)
};
[[nodiscard]] std::array<StructureCell, 8> structureCells(Axis axis);
/// The plane offset of a segment in the canonical model (wheel in the XZ plane): (a, b) in -1..1.
[[nodiscard]] std::pair<int, int> segmentCell(int segment);
/// Offset from the wheel to the structure block of `segment` for `axis`.
[[nodiscard]] Offset segmentOffset(int segment, Axis axis);

} // namespace lv::core::generators
