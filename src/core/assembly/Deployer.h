#pragma once

// Deployer maths (Create's DeployerBlockEntity / DeployerRenderer / DirectionalAxisKineticBlock),
// independent of the game (U15, SPEC §3.6.1).

#include "core/kinetics/Types.h"

namespace lv::core::assembly {

using kinetics::Axis;
using kinetics::Face;

enum class DeployerState : int { Waiting = 0, Expanding = 1, Retracting = 2, Dumping = 3 };
enum class DeployerMode : int { Punch = 0, Use = 1 };
enum class HandPose : int { Pointing = 0, Punching = 1, Holding = 2 };

/// Timer units per tick (Create's getTimerSpeed): 0 when stopped, else clamp(|speed| * 2, 8, 512).
[[nodiscard]] int deployerTimerSpeed(float speed);
/// A full extension / retraction is 1000 timer units, the pause between cycles 500.
inline constexpr int kDeployerCycle = 1000;
inline constexpr int kDeployerPause = 500;

/// The shaft axis of a deployer (Create's DirectionalAxisKineticBlock.getRotationAxis).
[[nodiscard]] Axis deployerShaftAxis(Face facing, bool alongFirst);
/// `alongFirst` that gives `shaft` for `facing` (shaft must be perpendicular to facing).
[[nodiscard]] bool deployerAlongFirstFor(Face facing, Axis shaft);

/// What a placement sees around the new block: whether the neighbour on a face has a shaft end
/// towards it (Create's prefersConnectionTo).
struct PlacementNeighbours {
    bool shaftTowards[6] = {false, false, false, false, false, false};
};
/// Create's getStateForPlacement of directional axis blocks: `alongFirst` from the facing, the
/// player's horizontal look axis and the shafts around.
[[nodiscard]] bool deployerPlacementAlongFirst(Face facing, bool horizontalLookAlongX, PlacementNeighbours const& n);

/// The hand pose (Create's getHandPose).
[[nodiscard]] HandPose deployerHandPose(DeployerMode mode, bool holding);
/// How far the hand is out of the block (blocks), Create's DeployerRenderer.getHandOffset.
/// `timer` is the timer at the start of the tick, `partial` the frame's partial tick.
[[nodiscard]] float deployerHandOffset(DeployerState state, float timer, int timerSpeed, float partial, float reach, HandPose pose);
/// The reach from the distance between the ray origin (1.5 blocks out) and the first hit (Create's start()).
[[nodiscard]] float deployerReach(float hitDistance);

/// Creates's destroy progress per deployer activation: the per-tick progress times 16.
inline constexpr float kDeployerBreakFactor = 16.f;

} // namespace lv::core::assembly
