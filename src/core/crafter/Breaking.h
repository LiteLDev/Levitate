#pragma once

// Block breaking and saw processing numbers (pure C++): Create 6.0.10's
// BlockBreakingKineticBlockEntity, BlockBreakingMovementBehaviour, DrillBlock, SawBlockEntity and
// DirectionalAxisKineticBlock.

#include <optional>

#include "core/kinetics/Types.h"

namespace lv::core::crafter {

using kinetics::Axis;
using kinetics::Face;

// ---- breaking ------------------------------------------------------------------------------------

/// Stationary breakers (drill, horizontal saw): |rpm| / 100.
[[nodiscard]] float stationaryBreakSpeed(float rpm);
/// Create's MovementContext.getAnimationSpeed for a linear motion of `motionLength` blocks/tick
/// (rounded to hundreds, 0 below 1/512).
[[nodiscard]] float animationSpeed(double motionLength);
/// Moving breakers: clamp(|animationSpeed| / 500, lowerLimit, 16); lowerLimit 1/128 (1 for
/// minecart contraptions, 2 for trains).
[[nodiscard]] float movingBreakSpeed(float animSpeed, float lowerLimit = 1.0f / 128.0f);
/// Progress added by one hit: clamp((int)(speed / hardness), 1, 10 - progress).
[[nodiscard]] int progressStep(float breakSpeed, float hardness, int progress);
/// Ticks until the next hit: (int)(hardness / speed).
[[nodiscard]] int ticksUntilNextProgress(float hardness, float breakSpeed);
/// Create's isBreakable minus the block checks the caller does (air, liquids, unbreakable).
[[nodiscard]] bool breakableHardness(float hardness);
/// Total ticks to break a block of `hardness` at a constant break speed (selftests, SPEC).
[[nodiscard]] int ticksToBreak(float hardness, float breakSpeed);

/// DrillBlock.getDamage: min(|s|/16, 2) + min(|s|/32, 4) + min(|s|/64, 4), clamped to 0..20.
[[nodiscard]] double drillDamage(float rpm);
/// Moving breakers hitting entities: clamp(6 * |relativeMotion|^0.4 + 1, 2, 10).
[[nodiscard]] double movingDamage(double relativeMotionLength);

// ---- saw ------------------------------------------------------------------------------------------

/// DirectionalAxisKineticBlock.getRotationAxis(facing, axisAlongFirstCoordinate).
[[nodiscard]] Axis directionalAxis(Face facing, bool alongFirst);
/// Saw processing: remaining time drops by clamp(|rpm| / 24, 1, 128) per tick.
[[nodiscard]] float sawProcessingSpeed(float rpm);
/// Saw start: time * max(1, count / 5) (cutting recipes: their processing time; others 50).
[[nodiscard]] int sawProcessingTime(int recipeTime, int count);
/// SawBlockEntity.getItemMovementVec: (±1, 0, 0) along x when !alongFirst, else (0, 0, ∓1);
/// the sign follows the speed.
[[nodiscard]] kinetics::Offset sawItemMovement(bool alongFirst, float rpm);

} // namespace lv::core::crafter
