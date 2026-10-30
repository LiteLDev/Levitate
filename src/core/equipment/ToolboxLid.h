#pragma once

// Toolbox lid / drawers and dyeing (U31 / U45, SPEC §3.6.31), independent of the game.
//
// Create's ToolboxBlockEntity: while a player has the toolbox open, the lid chases 1 linearly
// (0.2 per tick) and the drawers chase 1 exponentially (a fifth of the rest per tick); both chase
// 0 when nobody has it open. ToolboxRenderer turns the lid by 135° × lid about its back edge and
// slides the lower drawer out by 0.35 × drawers blocks, the upper one by half of that.
// Create's ToolboxDyeingRecipe: exactly one toolbox and one dye in the grid; the result keeps the
// toolbox's data.

#include <optional>
#include <string>
#include <vector>

namespace lv::core::equipment {

/// Create's LerpedFloat chases of the lid (LINEAR) and the drawers (EXP), speed 0.2.
struct ToolboxLid {
    float lid     = 0; ///< 0 closed .. 1 open
    float drawers = 0; ///< 0 in .. 1 out

    /// One tick towards open (someone has the toolbox open) or closed.
    void tick(bool open);
    /// Both values at their target.
    [[nodiscard]] bool settled(bool open) const;
};

/// The lid's turn in degrees (0 .. 135).
[[nodiscard]] inline float lidDegrees(float lid) { return 135.f * lid; }
/// How far a drawer is out in blocks: the lower drawer (`upper` false) 0.35 × value, the upper
/// drawer half of that.
[[nodiscard]] inline float drawerOut(float drawers, bool upper) { return drawers * .175f * (upper ? 1.f : 2.f); }

/// Create's ToolboxDyeingRecipe.matches on the crafting grid (item ids, "" = empty slot): the
/// index of the toolbox when the grid holds exactly one toolbox and one dye and nothing else.
[[nodiscard]] std::optional<int> dyeingToolboxSlot(std::vector<std::string> const& grid);

} // namespace lv::core::equipment
