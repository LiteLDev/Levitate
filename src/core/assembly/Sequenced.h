#pragma once

// Sequenced assembly recipes (Create's SequencedAssemblyRecipe), independent of the game (U15,
// SPEC §3.6.3).
//
//     {"type": "create:sequenced_assembly",
//      "ingredient": {"item": "create:golden_sheet"},
//      "transitional_item": {"id": "create:incomplete_precision_mechanism"},
//      "loops": 5,
//      "sequence": [{"type": "create:deploying", "ingredients": [{...transitional}, {"item": "create:cogwheel"}],
//                    "results": [{"id": "create:incomplete_precision_mechanism"}]}, ...],
//      "results": [{"id": "create:precision_mechanism", "chance": 120}, {"id": "create:golden_sheet", "chance": 8}]}
//
// A transitional item remembers the recipe id and the number of finished steps. Every machine step
// advances it; after `sequence.size() * loops` steps one result is picked by weight (the result
// "chance" values are weights, as Create's rollResult).

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/common/Recipe.h"

namespace lv::core::assembly {

/// Step types a sequence may contain (Create's IAssemblyRecipe implementations).
inline constexpr std::string_view kDeploying = "deploying";
inline constexpr std::string_view kPressing  = "pressing";
inline constexpr std::string_view kCutting   = "cutting";
inline constexpr std::string_view kFilling   = "filling";

/// One step of a sequence: a processing recipe of a machine type. Its first item ingredient is the
/// transitional item (the base ingredient too for the first step); deploying steps hold the item to
/// apply as their second ingredient, filling steps a fluid ingredient.
struct SequenceStep {
    std::string                    type; ///< kDeploying, kPressing, kCutting or kFilling
    common::ProcessingRecipe       recipe;
    bool                           keepHeldItem = false; ///< deploying: the held item is not used up
};

struct SequencedRecipe {
    std::string                                     id; ///< "levitate:sequenced_assembly/precision_mechanism"
    common::ItemIngredient                          ingredient;
    std::string                                     transitionalItem;
    int                                             loops = 1;
    std::vector<SequenceStep>                       sequence;
    std::vector<common::ItemResult>                 results; ///< `chance` is the weight
    std::map<std::string, std::vector<std::string>> inlineTags; ///< Levitate extension: "tags" {tag: [ids]}

    /// Steps until the result: sequence length times loops.
    [[nodiscard]] int totalSteps() const { return static_cast<int>(sequence.size()) * loops; }
    /// The step to run after `finished` steps.
    [[nodiscard]] SequenceStep const& stepAt(int finished) const { return sequence[static_cast<std::size_t>(finished) % sequence.size()]; }
};

/// Maps a step type id ("create:deploying", "levitate:filling", "deploying") to its short name ("" if unknown).
[[nodiscard]] std::string stepTypeName(std::string_view typeId);

/// Parses a sequenced assembly recipe. Returns nullopt and sets `error` for unusable documents.
[[nodiscard]] std::optional<SequencedRecipe>
parseSequenced(nlohmann::json const& doc, std::string const& id, std::string* error = nullptr);

/// Whether finishing step `finished` (0-based count of steps done before it) completes the recipe
/// (Create: `(step + 1) / sequence.size() >= loops`).
[[nodiscard]] bool completesWith(SequencedRecipe const& recipe, int finished);
/// The progress (0..1) shown after `finishedAfter` steps.
[[nodiscard]] float progressAt(SequencedRecipe const& recipe, int finishedAfter);

/// Picks a result by weight: `random01` in [0, 1). Returns the index or -1 (no results / no weight).
[[nodiscard]] int pickWeighted(std::vector<common::ItemResult> const& results, float random01);
/// Each result's share of the total weight (0..1), as Create's tooltip / JEI shows it.
[[nodiscard]] std::vector<float> resultShares(std::vector<common::ItemResult> const& results);

/// One tooltip line of a transitional item (Create's SequencedAssemblyRecipe.addToTooltip), as a
/// translation key, an English fallback, its arguments and a colour (§ code).
struct TooltipLine {
    std::string              key;
    std::string              fallback;
    std::vector<std::string> args;
    char                     colour = '7';
};
/// `stepNames[i]` is the already translated description of sequence step i ("Deploy Cogwheel").
[[nodiscard]] std::vector<TooltipLine>
tooltipLines(SequencedRecipe const& recipe, int finished, std::vector<std::string> const& stepNames);

} // namespace lv::core::assembly
