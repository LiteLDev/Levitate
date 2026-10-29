#pragma once

// Create's wrench rotation (IWrenchable.getRotatedBlockState) on Levitate's block state
// conventions (pure C++, unit-tested). See docs/SPEC.md §3.6 (U8).
//
// State conventions (integer block states):
//   levitate:facing / minecraft:facing_direction  0..5 in Create's Direction order
//                                                  (down, up, north, south, west, east)
//   levitate:axis                                  0 x, 1 y, 2 z   (RotatedPillarKineticBlock.AXIS)
//   levitate:horizontal_facing                     0 south, 1 west, 2 north, 3 east (Java 2D value)
//   levitate:horizontal_axis                       0 x, 2 z        (HorizontalAxisKineticBlock)
//   levitate:axis_along_first                      0 / 1           (DirectionalAxisKineticBlock)

#include <array>
#include <map>
#include <string>

namespace lv::core::goggles {

/// The rotation-relevant states of a block: name -> integer value.
using States = std::map<std::string, int>;

inline constexpr char const* kFacing           = "levitate:facing";
inline constexpr char const* kVanillaFacing    = "minecraft:facing_direction";
inline constexpr char const* kAxis             = "levitate:axis";
inline constexpr char const* kHorizontalFacing = "levitate:horizontal_facing";
inline constexpr char const* kHorizontalAxis   = "levitate:horizontal_axis";
inline constexpr char const* kAxisAlongFirst   = "levitate:axis_along_first";

/// The state names `rotatedStates` reads.
inline constexpr std::array<char const*, 6> kRotationStates{
    kFacing, kVanillaFacing, kAxis, kHorizontalFacing, kHorizontalAxis, kAxisAlongFirst
};

/// Axis of a direction (0..5, Create order): 0 x, 1 y, 2 z.
[[nodiscard]] int axisOfDirection(int dir);
/// The positive direction of an axis (Create's VoxelShaper.axisAsFace): x -> east, y -> up, z -> south.
[[nodiscard]] int positiveDirection(int axis);
/// Minecraft's Direction.getClockWise(Axis): a quarter turn about `axis`; directions on the
/// axis stay unchanged.
[[nodiscard]] int clockwise(int dir, int axis);
/// Java 2D data value (0 S, 1 W, 2 N, 3 E) <-> direction (2..5).
[[nodiscard]] int directionFromHorizontal(int value);
[[nodiscard]] int horizontalFromDirection(int dir);

/// Create's IWrenchable.getRotatedBlockState: the states after a wrench click on `face`
/// (0..5). Only the states listed above change; a block without any of them is returned as is.
[[nodiscard]] States rotatedStates(States const& states, int face);

} // namespace lv::core::goggles
