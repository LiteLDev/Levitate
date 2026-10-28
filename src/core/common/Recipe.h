#pragma once

// Create processing recipes (crushing, milling, pressing, mixing, ...), parsed from Create's
// recipe JSON (1.20 and 1.21 spellings), independent of the game (U3, SPEC §1.2 / §3.2).
//
//     {"type": "create:pressing", "ingredients": [{"item": "minecraft:iron_ingot"}],
//      "results": [{"id": "create:iron_sheet"}], "processing_time": 100,
//      "heat_requirement": "heated"}
//
// Ids in the `create:` namespace are mapped to `levitate:`. Unit-specific fields stay in `raw`.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::core::common {

/// One ingredient slot: any of `items` or any item in one of `tags` (Create's alternatives).
struct ItemIngredient {
    std::vector<std::string> items;
    std::vector<std::string> tags;
    [[nodiscard]] bool       empty() const { return items.empty() && tags.empty(); }
};

/// A fluid ingredient: a fluid id or a fluid tag, with an amount in mB.
struct FluidIngredient {
    std::string fluid;
    std::string tag;
    int         amount = 0;
};

/// An item output with Create's chance semantics (each of the `count` items is rolled).
struct ItemResult {
    std::string id;
    int         count  = 1;
    float       chance = 1.0f;
};

struct FluidResult {
    std::string fluid;
    int         amount = 0;
};

/// Create's HeatCondition.
enum class HeatRequirement : int { None = 0, Heated = 1, Superheated = 2 };

/// Blaze burner heat levels (Create's BlazeBurnerBlock.HeatLevel).
enum class HeatLevel : int { None = 0, Smouldering = 1, Fading = 2, Kindled = 3, Seething = 4 };

[[nodiscard]] std::string_view heatRequirementName(HeatRequirement h);
[[nodiscard]] std::string_view heatLevelName(HeatLevel h);
/// Create's HeatCondition.testBlazeBurner: superheated needs seething; heated needs at least
/// fading (passive heaters give smouldering, which is not enough); none accepts anything.
[[nodiscard]] bool heatSatisfies(HeatRequirement required, HeatLevel level);

struct ProcessingRecipe {
    std::string                  id;   ///< "levitate:pressing/iron_sheet" (namespace + type + file stem)
    std::string                  type; ///< "pressing"
    std::vector<ItemIngredient>  ingredients;
    std::vector<FluidIngredient> fluidIngredients;
    std::vector<ItemResult>      results;
    std::vector<FluidResult>     fluidResults;
    HeatRequirement              heat           = HeatRequirement::None;
    int                          processingTime = 0; ///< ticks; 0 = the machine's default
    nlohmann::json               raw;                ///< the whole source document
};

/// Maps Create's namespace to ours ("create:x" -> "levitate:x"); other ids unchanged.
[[nodiscard]] std::string mapId(std::string_view id);

/// Parses one recipe document. `type` is the folder name ("pressing"); `id` the recipe id.
/// Returns nullopt and sets `error` when the document is not a usable recipe.
[[nodiscard]] std::optional<ProcessingRecipe>
parseRecipe(nlohmann::json const& doc, std::string const& type, std::string const& id, std::string* error = nullptr);

/// Rolls an output: every one of `result.count` items is kept with probability `chance`
/// (`random01()` returns uniform floats in [0, 1)). Returns the number of items produced.
[[nodiscard]] int rollResult(ItemResult const& result, std::function<float()> const& random01);

} // namespace lv::core::common
