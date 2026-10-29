#pragma once

// Mechanical crafter geometry and grid logic (pure C++, Create 6.0.10's MechanicalCrafterBlock,
// RecipeGridHandler and ConnectedInputHandler behaviour).
//
// Faces use the kinetic network's `Face` (Create's Direction order). A crafter has a horizontal
// `facing` (its front, where the slot is) and a `Pointing` (the arrow on the front: where the
// grid's items travel). Grid coordinates: x grows to the right as seen by someone looking at the
// front, y grows upwards; the crafter whose arrow points nowhere (the last one) is (0, 0).

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "core/kinetics/Types.h"

namespace lv::core::crafter {

using kinetics::Axis;
using kinetics::Face;
using kinetics::Offset;

/// Create's `Pointing` in its declaration order (the wrench cycles through it).
enum class Pointing : std::uint8_t { Up = 0, Left = 1, Down = 2, Right = 3 };

/// Create's `Pointing.getXRotation()` (degrees): up 0, left 270, down 180, right 90.
[[nodiscard]] int xRotation(Pointing p);
/// The next pointing in declaration order (Create's `state.cycle(POINTING)`).
[[nodiscard]] Pointing cycle(Pointing p);
/// Create's `MechanicalCrafterBlock.getTargetDirection`: the face the items leave through.
[[nodiscard]] Face targetDirection(Face facing, Pointing p);
/// Create's `pointingFromFacing(clickedFace, blockFacing)`: the arrow of a crafter placed on the
/// `clickedFace` of another block points back at that block.
[[nodiscard]] Pointing pointingFromFacing(Face clickedFace, Face blockFacing);
/// Create's `HorizontalKineticBlock` placement: facing the player (`lookFacing` = the horizontal
/// face the player looks towards, the crafter faces the opposite way).
[[nodiscard]] Face horizontalOpposite(Face lookFacing);
/// Two crafters point exactly at each other's opposite (Create's isValidTarget: |xRot1-xRot2| == 180).
[[nodiscard]] bool opposed(Pointing a, Pointing b);

// ---- grouped items (RecipeGridHandler.GroupedItems) ---------------------------------------------

using Cell = std::pair<int, int>; ///< (x, y)

/// Offset applied to a crafter's grid when it is merged onto the crafter it points at.
[[nodiscard]] Cell mergeOffset(Pointing p);

struct GridStats {
    int minX = 0, minY = 0, maxX = 0, maxY = 0, width = 1, height = 1;
};
/// Create's calcStats: bounds of the cells, (0, 0) always included.
[[nodiscard]] GridStats statsOf(std::vector<Cell> const& cells);

/// A crafting input matrix built from grid cells (row 0 = the top row, i.e. the highest y), as
/// indices into the caller's cell list (-1 = empty). Empty outer rows / columns are trimmed
/// (vanilla CraftingInput.ofPositioned).
struct InputMatrix {
    int              width = 0, height = 0;
    std::vector<int> cells; ///< width * height, row major
    [[nodiscard]] int at(int x, int y) const { return cells[static_cast<std::size_t>(y * width + x)]; }
    [[nodiscard]] int count() const;
};
/// `present[i]` = whether cell i holds an item (empty / covered cells still widen the grid but
/// count as empty).
[[nodiscard]] InputMatrix inputMatrix(std::vector<Cell> const& cells, std::vector<bool> const& present);

// ---- recipe matching ----------------------------------------------------------------------------

/// A shaped pattern: `width * height` cells, -1 = empty, otherwise an ingredient index.
struct ShapedPattern {
    int              width = 0, height = 0;
    std::vector<int> cells;
    [[nodiscard]] int at(int x, int y) const { return cells[static_cast<std::size_t>(y * width + x)]; }
};
/// Whether `input` matches `pattern` (same size; also mirrored when `mirror`). `test(ingredient,
/// inputCell)` tells whether the item of the caller's cell satisfies an ingredient.
[[nodiscard]] bool
matchShaped(ShapedPattern const& pattern, InputMatrix const& input, bool mirror, std::function<bool(int, int)> const& test);
/// Shapeless: every non-empty input cell matches exactly one of `ingredientCount` ingredients and
/// every ingredient is used once (bipartite matching).
[[nodiscard]] bool matchShapeless(int ingredientCount, InputMatrix const& input, std::function<bool(int, int)> const& test);

/// Parses Create's mechanical crafting `pattern` rows with a key map from character to ingredient
/// index (unknown non-space characters give nullopt).
[[nodiscard]] std::optional<ShapedPattern> parsePattern(std::vector<std::string> const& rows, std::map<char, int> const& key);

// ---- connected inputs (ConnectedInputHandler) ---------------------------------------------------

/// Create's insertion order of a connected input group: top row first, then left to right as seen
/// from the front (Create's invOrdering). Returns the offsets sorted.
[[nodiscard]] std::vector<Offset> inventoryOrder(std::vector<Offset> offsets, Face facing);

/// Create's EdgeInteractionHandler: the direction of the edge a click at block-local `hit` on
/// `face` activates (within 3/16 of an edge perpendicular to the face), among `candidates`.
[[nodiscard]] std::optional<Face> edgeAt(Face face, double hx, double hy, double hz, std::vector<Face> const& candidates);

// ---- timing (MechanicalCrafterBlockEntity) ------------------------------------------------------

/// Create's getCountDownSpeed: 0 when stopped, else clamp(|rpm|, 4, 250).
[[nodiscard]] int countDownSpeed(float rpm);

enum class Phase : std::uint8_t { Idle = 0, Accepting, Assembling, Exporting, Waiting, Crafting, Inserting };
[[nodiscard]] char const* phaseName(Phase p);
[[nodiscard]] Phase       phaseFromName(std::string_view name);

} // namespace lv::core::crafter
