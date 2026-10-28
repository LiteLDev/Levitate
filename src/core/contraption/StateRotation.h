#pragma once

// Rotation of block states (pure C++), used when a rotated contraption is placed back.
//
// Handles the state names shared by many blocks: Bedrock's facing states
// (`facing_direction`, `minecraft:facing_direction`, `minecraft:block_face`,
// `minecraft:cardinal_direction`, `pillar_axis`, `direction`, `weirdo_direction`,
// `ground_sign_direction`, `torch_facing_direction`, `minecraft:vertical_half`, `upside_down_bit`)
// and Levitate's own conventions (`levitate:facing` 0..5 in facing order, `levitate:axis` 0..2,
// chassis sticky sides). Horizontal-only states keep their value when a rotation about X or Z
// would turn them vertical (as Create's StructureTransform). Units with special states register a
// rotation handler with the engine instead.

#include <cstdint>
#include <map>
#include <string>
#include <variant>

#include "core/contraption/Geometry.h"

namespace lv::core::contraption {

using StateValue = std::variant<std::int64_t, std::string>;
using States     = std::map<std::string, StateValue>;

/// Rotates every known state of `states` by `quarterTurns` about `axis`. Returns true if a state changed.
bool rotateStates(States& states, Axis axis, int quarterTurns);

/// Radial chassis side names "north", "south", "east", "west" -> world directions for an axis.
[[nodiscard]] std::array<Dir, 4> radialSides(Axis axis);

} // namespace lv::core::contraption
