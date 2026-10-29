#pragma once

// Sequenced assembly engine (U15, SPEC §3.6.15): the contract other units' machines use.
//
// Create's sequenced assembly recipes (`assets/recipes/sequenced_assembly/*.json`) are chains of
// deploying / pressing / cutting / filling steps on a transitional ("incomplete") item. Every
// machine that can run a step asks this engine before its own recipes, then lets it advance the
// item:
//
//     #include "feature/assembly/SequencedAssembly.h"
//     namespace sa = lv::feature::assembly::sequenced;
//
//     // a press (U14) pressing `stack` on a depot / belt / in the world:
//     if (auto step = sa::findStep(sa::kPressing, stack)) {
//         ItemStack out = sa::advance(*step, oneItemOf(stack));   // per item
//         ...
//     }
//     // a spout (U19) filling `stack` with the fluid in its tank:
//     if (auto step = sa::findStep(sa::kFilling, stack); step && sa::acceptsFluid(*step, tankFluid)) {
//         int amount = sa::fluidAmount(*step);                   // mB to drain
//         ItemStack out = sa::advance(*step, oneItemOf(stack));
//     }
//     // a saw (U16): sa::kCutting; the deployer (this unit): sa::kDeploying + acceptsHeld().
//
// `advance` returns the transitional item with one more step done (its progress and tooltip in
// the item's data / lore), or, after the last step of the last loop, one result picked by weight.
// A machine processes as many items as it would for its own recipes (Create applies a step per
// item) and uses `processingTime(step)` when it has a duration (0 = the machine's default).
//
// Server thread only. Recipes are loaded on first use and dropped by `/create reload`.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/assembly/Sequenced.h"
#include "feature/common/Capabilities.h"

class ItemStack;

namespace lv::feature::assembly::sequenced {

using core::assembly::SequencedRecipe;
using core::assembly::SequenceStep;
using FluidStack = common::fluids::FluidStack;

inline constexpr std::string_view kDeploying = core::assembly::kDeploying;
inline constexpr std::string_view kPressing  = core::assembly::kPressing;
inline constexpr std::string_view kCutting   = core::assembly::kCutting;
inline constexpr std::string_view kFilling   = core::assembly::kFilling;

/// The item data key holding the progress of a transitional item: {"recipe", "step", "progress"}
/// (shared with U19's spout, SPEC §3.6.19).
inline constexpr std::string_view kDataKey = "sequenced_assembly";

/// A step that a machine of some type can run on an item now.
struct StepMatch {
    SequencedRecipe const* recipe   = nullptr;
    int                    finished = 0; ///< steps done before this one
    SequenceStep const*    step     = nullptr;
};

/// Every loaded sequenced assembly recipe (sorted by id).
[[nodiscard]] std::vector<SequencedRecipe> const& all();
/// A recipe by id ("levitate:sequenced_assembly/track").
[[nodiscard]] SequencedRecipe const* byId(std::string const& id);

/// Whether `stack` is the start item of `recipe` or its transitional item carrying its progress
/// (Create's SequencedAssemblyRecipe.appliesTo).
[[nodiscard]] bool appliesTo(SequencedRecipe const& recipe, ItemStack const& stack);
/// The step of type `machineType` (kPressing, ...) that comes next for `stack`, if any (first
/// recipe that applies and whose next step is of that type and accepts the item).
[[nodiscard]] std::optional<StepMatch> findStep(std::string_view machineType, ItemStack const& stack);
/// Deploying steps: whether `held` is the item the step applies.
[[nodiscard]] bool acceptsHeld(StepMatch const& match, ItemStack const& held);
/// The next deploying step for `stack` with `held` in the deployer's hand.
[[nodiscard]] std::optional<StepMatch> findDeployingStep(ItemStack const& stack, ItemStack const& held);
/// Whether a deploying step keeps the held item (Create's keep_held_item).
[[nodiscard]] bool keepsHeldItem(StepMatch const& match);
/// Filling steps: whether `fluid` matches (amount not checked) and the amount (mB) the step uses.
[[nodiscard]] bool acceptsFluid(StepMatch const& match, FluidStack const& fluid);
[[nodiscard]] int  fluidAmount(StepMatch const& match);
/// The step's processing time in ticks (0: the machine's default).
[[nodiscard]] int processingTime(StepMatch const& match);

/// Runs the step on ONE item: returns the advanced transitional item, or the final result.
[[nodiscard]] ItemStack advance(StepMatch const& match, ItemStack const& input);

/// Whether `stack` carries sequenced assembly progress.
[[nodiscard]] bool isTransitional(ItemStack const& stack);
/// Steps done and progress (0..1) of a transitional item.
struct Progress {
    std::string recipeId;
    int         step     = 0;
    float       progress = 0;
};
[[nodiscard]] std::optional<Progress> progressOf(ItemStack const& stack);
/// Create's tooltip lines of a transitional item (translated, with § colours); empty otherwise.
[[nodiscard]] std::vector<std::string> tooltip(ItemStack const& stack, std::string const& locale = {});
/// Text of one step ("Deploy Cogwheel", "Process in Press", ...).
[[nodiscard]] std::string describeStep(SequenceStep const& step, std::string const& locale = {});

/// Whether `stack` satisfies an ingredient of `recipe` (its inline tags first, then the loader's tags).
[[nodiscard]] bool matches(SequencedRecipe const& recipe, core::common::ItemIngredient const& ingredient, ItemStack const& stack);

/// Drops the loaded recipes (next use reloads). `/create reload` calls it.
void reload();

} // namespace lv::feature::assembly::sequenced
