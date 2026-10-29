#pragma once

// Linear actuators of Create 6.0.10 as pure maths (U20): mechanical pistons, rope / elevator
// pulleys and gantries. Speeds are blocks per tick, offsets in blocks. Directions use the
// contraption geometry (Bedrock facing order: down, up, north, south, west, east).

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/contraption/Geometry.h"

namespace lv::core::pistons {

using contraption::Axis;
using contraption::Dir;
using contraption::Pos;

/// Create's IControlContraption.MovementMode (value box option order).
enum class MovementMode : int { MovePlace = 0, MovePlaceReturned = 1, MoveNeverPlace = 2 };
inline constexpr int kMovementModes = 3;

/// Create's KineticBlockEntity.convertToLinear: RPM -> blocks per tick.
[[nodiscard]] float convertToLinear(float rpm);
/// The linear speed of a rope pulley / generic actuator: clamp(rpm / 512, -0.49, 0.49).
[[nodiscard]] float actuatorSpeed(float rpm);

/// Create's MechanicalPistonBlockEntity.getMovementSpeed: offset change per tick (positive extends)
/// for a piston facing `facing` turning at `rpm`, clamped so that the offset stays in 0..range.
[[nodiscard]] float pistonMovementSpeed(float rpm, Dir facing, float offset, int range);

/// Create's LinearActuatorBlockEntity.getGridOffset: (int)(offset + 0.5) clamped to 0..range.
[[nodiscard]] int gridOffset(float offset, int range);

/// What `tryDisassemble` does at the end of the range / on stop (Create's LinearActuatorBlockEntity).
enum class StopAction { Disassemble, Wait };
[[nodiscard]] StopAction stopAction(MovementMode mode, float offset, int initialOffset, bool removed);

/// Create's ElevatorPulleyBlockEntity.getMovementSpeed: speed towards `targetOffset` with
/// acceleration, RPM limit and deceleration; `arrived` when within half a block.
struct ElevatorStep {
    float speed   = 0;
    bool  arrived = false;
};
[[nodiscard]] ElevatorStep elevatorStep(float rpm, float offset, int targetOffset, float prevSpeed);
/// The elevator's fine positioning once arrived: moves `y` a quarter of the way to `target`
/// (snaps when closer than 1/128).
[[nodiscard]] double elevatorSettle(double y, double target);

/// Create's DirectionalAxisKineticBlock.getRotationAxis (pistons, gantry carriages).
[[nodiscard]] Axis directionalAxis(Dir facing, bool alongFirst);
/// The `alongFirst` value giving `axis` for `facing` (nullopt if `axis` is the facing axis).
[[nodiscard]] std::optional<bool> alongFirstFor(Dir facing, Axis axis);
/// `alongFirst` after the block is turned by quarter turns (the shaft turns with the block).
[[nodiscard]] bool rotateAlongFirst(Dir facing, bool alongFirst, Axis about, int quarterTurns);

/// Create's GantryCarriageBlockEntity.getGantryPinionModifier.
[[nodiscard]] float gantryPinionModifier(Dir shaftFacing, Dir pinionFacing);
/// Create's GantryShaftBlockEntity.getPinionMovementSpeed: clamp(-rpm / 512, -0.49, 0.49).
[[nodiscard]] float pinionMovementSpeed(float rpm);

/// Gantry shaft parts (Create's GantryShaftBlock.Part; state value order).
enum class ShaftPart : int { Start = 0, Middle = 1, End = 2, Single = 3 };
/// The part from the connections: `front` = a shaft with the same facing on the facing side,
/// `back` = on the other side (Create's updateShape).
[[nodiscard]] ShaftPart shaftPart(bool front, bool back);
/// Create's GantryShaftBlockEntity.canAssembleOn.
[[nodiscard]] bool canAssembleOn(ShaftPart part, bool powered, float pinionSpeed);
/// Create's GantryContraptionEntity.checkPinionShaft test: after this tick the carriage centre
/// (`coord` -> `next` along the shaft axis) is past the centre of its current shaft block in its
/// direction of travel (then the shaft must still allow movement, else it stops there).
[[nodiscard]] bool crossesShaftCentre(double coord, double next, float pinionSpeed, Dir shaftFacing);

/// A block of a planned / moving structure for collision tests.
struct ColliderBlock {
    Pos  local;
    bool solid = true; ///< has a collision shape
};
/// Create's TranslatingContraption.createColliders: solid blocks whose neighbour in `dir` is
/// not a solid block of the structure.
[[nodiscard]] std::vector<Pos> colliders(std::vector<ColliderBlock> const& blocks, Dir dir);
/// Create's ContraptionCollider.collideBlocks grid position: the block of the structure origin
/// (floored), one further for a positive movement direction.
[[nodiscard]] Pos collisionGrid(contraption::Vec3d const& origin, Dir dir);

/// Elevator floors (Create's ElevatorColumn.initNames): contacts sorted bottom to top; blank
/// short names are numbered from their neighbours, numbers that would not increase are cleared.
struct Floor {
    int         y = 0;
    std::string shortName;
    std::string longName;
};
void initFloorNames(std::vector<Floor>& floors);

} // namespace lv::core::pistons
