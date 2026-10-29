#pragma once

// Rotating contraptions (U21), pure rules: bearing angles and disassembly, windmill speed, clock
// hands, cart assembler actions, controller rails, cart orientation and the sail placement
// helper. Numbers and formulas follow Create 6.0.10 (see docs/SPEC.md §3.6.1).

#include <optional>
#include <vector>

#include "core/contraption/Geometry.h"

namespace lv::core::bearings {

namespace cc = ::lv::core::contraption;

// ---- bearings ---------------------------------------------------------------------------------

/// Degrees per tick of a block turning at `rpm` (Create's KineticBlockEntity.convertToAngular).
[[nodiscard]] inline constexpr float angularSpeed(float rpm) { return rpm * 3.f / 10.f; }

/// Create's RotationMode of the mechanical bearing (value box order).
enum class RotationMode : int { RotatePlace = 0, RotatePlaceReturned = 1, RotateNeverPlace = 2 };

/// Create's isNearInitialAngle: within 45 degrees of the assembly angle.
[[nodiscard]] bool isNearInitialAngle(double angle);
/// Whether a running bearing whose speed dropped to 0 places its structure (Create's
/// MechanicalBearingBlockEntity#tick).
[[nodiscard]] bool canDisassemble(RotationMode mode, double angle);
/// The next angle (Java float `%` keeps the sign).
[[nodiscard]] double advanceAngle(double angle, double delta);

/// Windmill RPM for `sails` sail-like blocks (Create: clamp(sails / windmillSailsPerRPM, 1, 16)),
/// with the sign of the chosen direction (`clockwise` -> +1, Create's RotationDirection).
[[nodiscard]] float windmillSpeed(int sails, int sailsPerRpm, bool clockwise);

/// Create's AngleHelper.getShortestAngleDiff: signed difference target - current in (-180, 180].
[[nodiscard]] double shortestAngleDiff(double current, double target);
/// Same, with Create's hint for differences close to 180 degrees (turn the way of the hint).
[[nodiscard]] double shortestAngleDiff(double current, double target, double hint);

// ---- clockwork bearing ------------------------------------------------------------------------

/// Create's ClockHands option of the clockwork bearing.
enum class ClockHands : int { HourFirst = 0, MinuteFirst = 1, HourFirst24 = 2 };

/// Angle the hour hand points to at `dayTime` (ticks); `axisStep` is +1 for a bearing facing the
/// positive end of its axis, -1 otherwise.
[[nodiscard]] double hourTarget(long long dayTime, int axisStep, bool twentyFour);
/// Angle the minute hand points to.
[[nodiscard]] double minuteTarget(long long dayTime, int axisStep);
/// Degrees this tick of a hand at `angle` heading to `target` at most `maxSpeed` degrees per tick
/// (Create's getHourArmSpeed / getMinuteArmSpeed; 0 when the bearing doesn't turn).
[[nodiscard]] double handStep(double angle, double target, double maxSpeed);

// ---- cart assembler / controller rail ---------------------------------------------------------

/// The rail the cart assembler was placed on (Create's CartAssembleRailType).
enum class RailType : int { Regular = 0, Powered = 1, Detector = 2, Activator = 3, Controller = 4 };
enum class CartAction : int { Pass, Assemble, Disassemble, AssembleAccelerate, DisassembleBrake, AssembleAccelerateDirectional };

/// Create's CartAssemblerBlock#getActionForCart.
[[nodiscard]] CartAction actionFor(RailType type, bool powered, bool cartHasPassengers);
[[nodiscard]] bool shouldAssemble(CartAction a);
[[nodiscard]] bool shouldDisassemble(CartAction a);

/// Cart movement modes of the cart assembler (Create's CartMovementMode).
enum class CartMovementMode : int { Rotate = 0, RotatePaused = 1, RotationLocked = 2 };

/// The direction a straight rail along `axisX` (east-west when true) "points towards" (Create:
/// west for east-west rails, north otherwise), reversed when `backwards`.
[[nodiscard]] cc::Dir railDirection(bool axisX, bool backwards);
/// Horizontal angle of a vector (degrees, right-hand rule about +Y from +X: east 0, north 90).
[[nodiscard]] double headingOf(double dx, double dz);
[[nodiscard]] double headingOf(cc::Dir d);

/// Velocity a controller rail gives a cart (Create's ControllerRailBlock#onMinecartPass).
/// `motion` is the cart's horizontal motion, `power` 0..15. Returns nullopt to brake (pull to the
/// rail's centre at 1/16 of the offset per tick).
[[nodiscard]] std::optional<cc::Vec3d> controllerVelocity(cc::Vec3d const& motion, cc::Dir accel, int power, double maxSpeed);
/// Create's brake: velocity towards the block centre (`offset` = centre - cart).
[[nodiscard]] inline cc::Vec3d brakeVelocity(cc::Vec3d const& offset) { return {offset.x / 16.0, 0, offset.z / 16.0}; }

/// How far a minecart contraption turns towards its target heading this tick (Create's
/// OrientedContraptionEntity#updateOrientation): `motion` length, `widthX` the structure's x size.
[[nodiscard]] double maxYawApproach(double motionLength, double widthX);

// ---- sails --------------------------------------------------------------------------------------

/// Create's IPlacementHelper.orderedByDistanceExceptAxis: the faces not on `axis`, nearest first
/// to `hit` (block-local, 0..1).
[[nodiscard]] std::vector<cc::Dir> orderedByDistanceExceptAxis(cc::Vec3d const& hit, cc::Axis axis);

} // namespace lv::core::bearings
