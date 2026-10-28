#pragma once

// Processing recipe loader (U3, SPEC §1.2 / §3.2).
//
// Create's own recipe types are JSON data in `assets/recipes/<type>/<name>.json` (Create's
// recipe format, `create:` ids are mapped to `levitate:`), copied to `<mod>/recipes/` at build
// time. Units read them by type; `/create reload` reloads them.
//
//     for (auto const& r : recipes::ofType("pressing")) ...
//     auto const* r = recipes::findFirst("pressing", stack);           // first ingredient matches
//     if (recipes::matches(r->ingredients[0], stack)) ...
//     auto outputs = recipes::rollResults(*r);                          // Create's chance rolls
//
// Item tags (`{"tag": "c:ingots/iron"}`) resolve through `assets/recipes/tags/*.json`
// fragments ({"c:ingots/iron": ["minecraft:iron_ingot", "#other:tag"]}, merged, owned per unit)
// and then through the item's own Bedrock tags of the same name.

#include <functional>
#include <string>
#include <vector>

#include "core/common/Recipe.h"

class ItemStack;

namespace lv::feature::common::recipes {

using core::common::FluidIngredient;
using core::common::HeatRequirement;
using core::common::ItemIngredient;
using core::common::ProcessingRecipe;

/// Every recipe of `type` ("pressing", "crushing", ...), sorted by id. Loaded on first use.
[[nodiscard]] std::vector<ProcessingRecipe> const& ofType(std::string const& type);
/// Same as `ofType` (SPEC name): loads the type now (e.g. in a unit's `install()`).
std::vector<ProcessingRecipe> const& loadType(std::string const& type);
/// Recipes of `type` whose first item ingredient matches `stack`.
[[nodiscard]] std::vector<ProcessingRecipe const*> withFirstIngredient(std::string const& type, ItemStack const& stack);
[[nodiscard]] ProcessingRecipe const* findFirst(std::string const& type, ItemStack const& stack);
/// A recipe by id ("levitate:pressing/iron_ingot"), nullptr if none.
[[nodiscard]] ProcessingRecipe const* byId(std::string const& type, std::string const& id);

/// Whether `stack` satisfies an ingredient (item id or tag).
[[nodiscard]] bool matches(ItemIngredient const& ingredient, ItemStack const& stack);
/// Whether an item id is in a tag (Levitate tag files, then Bedrock item tags).
[[nodiscard]] bool inTag(std::string const& tag, ItemStack const& stack);
/// The item ids of a Levitate tag (resolved recursively; empty if the tag is only a Bedrock tag).
[[nodiscard]] std::vector<std::string> tagItems(std::string const& tag);
/// Whether a fluid id satisfies a fluid ingredient (amount not checked).
[[nodiscard]] bool matchesFluid(FluidIngredient const& ingredient, std::string const& fluid);

/// Rolls every item result with Create's chance semantics; returns the produced stacks.
[[nodiscard]] std::vector<ItemStack> rollResults(ProcessingRecipe const& recipe);

/// Drops every cached type (next access reloads). Called by `/create reload`. Recipes handed
/// out earlier stay valid (they are retired, not freed), but are no longer listed.
void reload();

} // namespace lv::feature::common::recipes
