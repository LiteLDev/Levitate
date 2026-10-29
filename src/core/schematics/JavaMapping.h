#pragma once

// Java block states (Create `.nbt` schematics) -> Bedrock block states, best effort (pure C++).
//
// Vanilla ids map to their Bedrock names (renames such as `dirt_path` -> `grass_path`, flattened
// families such as signs, beds, doors and slabs) and their properties to Bedrock states
// (`facing` -> `facing_direction` / `minecraft:cardinal_direction`, stairs -> `weirdo_direction`,
// `half` -> `upside_down_bit`, ...). Several plausible states may be emitted: the game side keeps
// those the block really has (and tries `alternatives` when the name doesn't exist). `create:<path>`
// becomes `levitate:<path>` with Levitate's state conventions (`levitate:facing` 0..5,
// `levitate:axis` 0..2, booleans as `levitate:<name>`).

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/schematics/Schematic.h"

namespace lv::core::schematics {

struct JavaState {
    std::string                                      name; ///< "minecraft:oak_stairs"
    std::vector<std::pair<std::string, std::string>> props;
    [[nodiscard]] std::string get(std::string_view key, std::string_view fallback = "") const;
    [[nodiscard]] bool        has(std::string_view key) const;
};

/// The Bedrock state for a Java state; nullopt for structure voids (nothing placed).
[[nodiscard]] std::optional<BlockState> mapJavaState(JavaState const& state);

/// Parses "minecraft:oak_stairs[facing=east,half=top]".
[[nodiscard]] JavaState parseJavaState(std::string_view text);

} // namespace lv::core::schematics
