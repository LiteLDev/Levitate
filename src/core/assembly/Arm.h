#pragma once

// Mechanical arm maths (Create's ArmBlockEntity / ArmAngleTarget), independent of the game
// (U15, SPEC §3.6.15).

#include <array>

#include "core/kinetics/Types.h"

namespace lv::core::assembly {

/// Create's ArmBlockEntity.SelectionMode, in its order (the value box columns).
enum class ArmSelectionMode : int { RoundRobin = 0, ForcedRoundRobin = 1, PreferFirst = 2 };
/// Create's ArmBlockEntity.Phase.
enum class ArmPhase : int { SearchInputs = 0, MoveToInput = 1, SearchOutputs = 2, MoveToOutput = 3, Dancing = 4 };

/// The interaction points a search looks at: indices [begin, end).
struct ArmScan {
    int begin = 0;
    int end   = 0;
};
/// Create's searchForItem / searchForDestination window: prefer-first scans from 0, the round robin
/// modes after the last used index; forced round robin only looks at the next one.
[[nodiscard]] ArmScan armScanRange(ArmSelectionMode mode, int lastIndex, int count);
/// The last index after a search (Create: round robin forgets it when nothing was found; any mode
/// wraps after the last point).
[[nodiscard]] int armLastIndexAfter(ArmSelectionMode mode, int lastIndex, bool found, int count);

/// Movement progress per tick towards the chased point: min(256, |speed|) / 1024.
[[nodiscard]] float armProgressPerTick(float speed);
/// Points within this many blocks of the arm can be used (squared distance < range²).
[[nodiscard]] bool armInRange(int dx, int dy, int dz, int range);

/// Angles of the arm's parts in degrees (Create's ArmAngleTarget).
struct ArmAngles {
    float base  = 0;
    float lower = 135;
    float upper = 45;
    float head  = 0;
};
/// The resting pose (Create's ArmAngleTarget.NO_TARGET).
[[nodiscard]] ArmAngles armRestAngles();

using Vec3f = std::array<double, 3>;
/// Inverse kinematics towards `target` (world coordinates of the interaction point) for an arm at
/// block `armPos`; `clawFacing` is the side the claw reaches the point from (Create:
/// getInteractionDirection, default down).
[[nodiscard]] ArmAngles armAnglesFor(std::array<int, 3> armPos, Vec3f target, kinetics::Face clawFacing, bool ceiling);
/// The pose while moving from `previous` to `target` at `progress` (0..1): the base turns all the
/// way, the joints first fold back to rest and then reach out (Create's tickMovementProgress).
[[nodiscard]] ArmAngles armPoseAt(ArmAngles const& previous, ArmAngles const& target, bool hasTarget, float progress);

/// Lengths of the arm's segments (blocks).
inline constexpr double kArmLowerLength = 18 / 16.0;
inline constexpr double kArmUpperLength = 17 / 16.0;

} // namespace lv::core::assembly
