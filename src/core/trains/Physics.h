#pragma once

// Train physics (pure C++): Create 6.0.10's speed model of trains (Train / Navigation /
// ContraptionCollider), in blocks and ticks.
//
// Config values are in blocks per second (speeds) and blocks per second² (acceleration), as
// Create's CTrains; a train works with blocks per tick: speeds / 20, acceleration / 400.

#include <optional>

#include "core/tracks/Math.h"

namespace lv::core::trains {

using tracks::Vec3d;

/// Create's CTrains speed keys (defaults are Create's).
struct SpeedConfig {
    double trainTopSpeed               = 28;
    double trainTurningTopSpeed        = 14;
    double trainAcceleration           = 3;
    double poweredTrainTopSpeed        = 40;
    double poweredTrainTurningTopSpeed = 20;
    double poweredTrainAcceleration    = 3;
    double manualTrainSpeedModifier    = 0.75;
};

/// A train's limits in blocks per tick (Train.maxSpeed / maxTurnSpeed / acceleration).
struct Stats {
    double maxSpeed     = 1.4;
    double maxTurnSpeed = 0.7;
    double acceleration = 0.0075;
};

/// `powered`: the train burns fuel (Create's fuelTicks > 0).
[[nodiscard]] Stats statsOf(SpeedConfig const& cfg, bool powered);

/// Train.approachTargetSpeed: moves `speed` towards `target` by `acceleration * mod`.
[[nodiscard]] double approach(double speed, double target, double acceleration, double mod);
/// Train.tickPassiveSlowdown: an unguided train coasts to a stop at `acceleration` per tick.
[[nodiscard]] double passiveSlowdown(double speed, double acceleration);
/// A derailed train: speed / 3 per tick, snapped to 0 (Mth.equal).
[[nodiscard]] double derailedSlowdown(double speed);
/// Distance needed to stop: speed² / (2 · acceleration).
[[nodiscard]] double brakingDistance(double speed, double acceleration);

/// What Navigation.tick knows about the way ahead.
struct NavigationInput {
    double distanceToTarget    = 0;  ///< to the destination (or the signal it waits for), >= 0
    double distanceToNextCurve = -1; ///< -1: no curve ahead
    double throttle            = 1;  ///< 0..1 (schedule throttle / HUD)
    double direction           = 1;  ///< +1 forward, -1 when the destination is behind the train
};
struct NavigationStep {
    double speed       = 0;     ///< the new speed (signed)
    bool   finalApproach = false; ///< snapped onto the destination this tick
};
/// Navigation.tick's speed control (after the conductor and signal checks): final approach
/// snap, approach clamp in the last 10 blocks, braking distance, curve slow-down.
[[nodiscard]] NavigationStep navigate(double speed, Stats const& stats, NavigationInput const& in);

/// Manual driving (ControlsHandler / Train.control): the target speed for a throttle key
/// direction (`keys` -1, 0, +1), `onTurn` when a bogey is on a curve, `slow` when driving
/// against the controls' facing (backwards: a quarter of the speed).
[[nodiscard]] double manualTargetSpeed(int keys, Stats const& stats, double manualModifier, double throttle, bool onTurn, bool slow);
/// One tick of manual driving: braking against the motion uses twice the acceleration.
[[nodiscard]] double manualStep(double speed, double target, Stats const& stats);

/// An entity hit by a moving carriage (ContraptionCollider.handleDamageFromTrain).
struct EntityHit {
    int   damage = 0;   ///< hit points
    Vec3d motion;       ///< the entity's new motion (knockback)
};
/// `trainMotion` / `entityMotion` in blocks per tick. Nullopt below Create's thresholds.
[[nodiscard]] std::optional<EntityHit> entityHit(Vec3d const& trainMotion, Vec3d const& entityMotion, bool monster);

/// Train-train collision: explosion power when the combined speed is above 0.2 b/t
/// (min(3 · combined, 5)), nullopt otherwise.
[[nodiscard]] std::optional<double> collisionExplosion(double speedA, double speedB);
/// Train.crash: the train bounces back with its speed clamped to ±0.5 b/t.
[[nodiscard]] double crashBounce(double speed);

/// Station summary / ETA formatting helpers.
/// Navigation prediction of the ticks to travel `distance` (Create's submitPredictions).
[[nodiscard]] int predictTransitTicks(double distance, Stats const& stats, double throttle);

} // namespace lv::core::trains
