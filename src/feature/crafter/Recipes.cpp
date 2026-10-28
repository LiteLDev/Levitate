// Crafter unit: recipes. The mechanical crafter applies vanilla crafting recipes (the game's own
// "crafting_table" recipes) and then Create's mechanical_crafting recipes (assets/recipes/
// mechanical_crafting, up to 9x9); the saw uses Create's cutting recipes and the game's
// stonecutter recipes (RecipeGridHandler.tryToApplyRecipe, SawBlockEntity.getRecipes).

#include <map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/item/crafting/Recipe.h"
#include "mc/world/item/crafting/RecipeIngredient.h"
#include "mc/world/item/crafting/Recipes.h"
#include "mc/world/level/Level.h"

#include "core/common/Recipe.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/common/ValueBox.h"
#include "feature/crafter/Internal.h"
#include "mod/Log.h"

namespace lv::feature::crafter {

namespace ci = common::items;
namespace rc = common::recipes;
using Json   = nlohmann::json;

namespace {

ItemStack toStack(ItemInstance const& inst) {
    auto tag = inst.save(*SaveContextFactory::createCloneSaveContext());
    if (!tag) return ci::empty();
    return ItemStack::fromTag(*tag);
}

/// The game's recipes with a tag ("crafting_table", "stonecutter"), sorted by id.
template <class F>
void forEachGameRecipe(char const* tag, F&& fn) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto&       recipes = level->getRecipes();
    auto const& byTag   = recipes.mRecipes.get();
    auto        it      = byTag.find(HashedString(tag));
    if (it == byTag.end()) return;
    for (auto const& [id, recipe] : it->second) {
        if (!recipe) continue;
        if (fn(id, *recipe)) return;
    }
}

bool ingredientEmpty(RecipeIngredient const& ing) { return ing.isNull() || ing.mStackSize == 0; }

struct GridInput {
    std::vector<cr::Cell>  cells;
    std::vector<ItemStack> stacks;
    cr::InputMatrix        matrix;
};

GridInput gridInput(std::map<cr::Cell, ItemStack> const& grid) {
    GridInput         in;
    std::vector<bool> present;
    for (auto const& [cell, stack] : grid) {
        in.cells.push_back(cell);
        in.stacks.push_back(stack);
        present.push_back(!ci::isEmpty(stack));
    }
    in.matrix = cr::inputMatrix(in.cells, present);
    return in;
}

std::optional<ItemStack> vanillaResult(GridInput const& in) {
    std::optional<ItemStack> result;
    forEachGameRecipe("crafting_table", [&](std::string const& id, Recipe const& r) {
        if (r.isMultiRecipe()) return false;
        auto const& results = r.getResultItems();
        if (results.empty()) return false;
        bool ok = false;
        if (r.isShapeless()) {
            std::vector<RecipeIngredient const*> ings;
            for (auto const& ing : r.mMyIngredients.get()) {
                if (ingredientEmpty(ing)) continue;
                for (int n = 0; n < std::max<int>(1, ing.mStackSize); ++n) ings.push_back(&ing);
            }
            ok = cr::matchShapeless(static_cast<int>(ings.size()), in.matrix, [&](int ing, int cell) {
                return r.itemValidForRecipe(*ings[static_cast<std::size_t>(ing)], in.stacks[static_cast<std::size_t>(cell)]);
            });
        } else {
            int w = r.mWidth, h = r.mHeight;
            if (w <= 0 || h <= 0 || w > 3 || h > 3) return false;
            cr::ShapedPattern p;
            p.width  = w;
            p.height = h;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) p.cells.push_back(ingredientEmpty(r.getIngredient(x, y)) ? -1 : y * w + x);
            ok = cr::matchShaped(p, in.matrix, true, [&](int ing, int cell) {
                return r.itemValidForRecipe(r.getIngredient(ing % w, ing / w), in.stacks[static_cast<std::size_t>(cell)]);
            });
        }
        if (!ok) return false;
        auto out = toStack(results.front());
        if (ci::isEmpty(out)) return false;
        // Create's isRecipeAllowed: firework rockets take at most maxFireworkIngredientsInCrafter items
        if (ci::idOf(out) == "minecraft:firework_rocket" && in.matrix.count() > config().maxFireworkIngredientsInCrafter) return false;
        result = out;
        log().debug("crafter: vanilla recipe {}", id);
        return true;
    });
    return result;
}

/// A mechanical crafting recipe parsed from its raw JSON.
struct MechanicalRecipe {
    std::string                              id;
    cr::ShapedPattern                        pattern;
    std::vector<core::common::ItemIngredient> ingredients;
    bool                                     mirror = true;
    std::string                              resultId;
    int                                      count = 1;
};

std::vector<MechanicalRecipe> const& mechanicalRecipes() {
    static std::vector<core::common::ProcessingRecipe> const* source = nullptr;
    static std::vector<MechanicalRecipe>                       parsed;
    auto const&                                                all = rc::ofType("mechanical_crafting");
    if (source == &all && !parsed.empty()) return parsed;
    source = &all;
    parsed.clear();
    for (auto const& r : all) {
        auto const& raw = r.raw;
        if (!raw.contains("pattern") || !raw.contains("key") || r.results.empty()) continue;
        MechanicalRecipe m;
        m.id = r.id;
        std::map<char, int> key;
        bool                bad = false;
        for (auto const& [k, v] : raw["key"].items()) {
            if (k.size() != 1) {
                bad = true;
                break;
            }
            // reuse the processing recipe parser for one ingredient (item / tag / alternatives)
            Json        doc{{"ingredients", Json::array({v})}, {"results", Json::array({{{"id", "minecraft:air"}}})}};
            std::string err;
            auto        one = core::common::parseRecipe(doc, "mechanical_crafting", m.id, &err);
            if (!one || one->ingredients.empty()) {
                bad = true;
                break;
            }
            key[k[0]] = static_cast<int>(m.ingredients.size());
            m.ingredients.push_back(one->ingredients[0]);
        }
        std::vector<std::string> rows;
        for (auto const& row : raw["pattern"])
            if (row.is_string()) rows.push_back(row.get<std::string>());
        auto pattern = cr::parsePattern(rows, key);
        if (bad || !pattern) {
            log().warn("crafter: skipping mechanical crafting recipe {} (bad key or pattern)", m.id);
            continue;
        }
        m.pattern  = *pattern;
        m.mirror   = raw.value("accept_mirrored", raw.value("acceptMirrored", true));
        m.resultId = r.results[0].id;
        m.count    = r.results[0].count;
        parsed.push_back(std::move(m));
    }
    return parsed;
}

std::optional<ItemStack> mechanicalResult(GridInput const& in) {
    for (auto const& m : mechanicalRecipes()) {
        bool ok = cr::matchShaped(m.pattern, in.matrix, m.mirror, [&](int ing, int cell) {
            return rc::matches(m.ingredients[static_cast<std::size_t>(ing)], in.stacks[static_cast<std::size_t>(cell)]);
        });
        if (!ok) continue;
        if (auto out = ci::make(m.resultId, m.count)) {
            log().debug("crafter: mechanical crafting recipe {}", m.id);
            return out;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<ItemStack> crafterlogic::tryToApplyRecipe(std::map<cr::Cell, ItemStack> const& grid) {
    auto in = gridInput(grid);
    if (in.matrix.count() == 0) return std::nullopt;
    if (config().allowRegularCraftingInCrafter && in.matrix.width <= 3 && in.matrix.height <= 3) {
        if (auto r = vanillaResult(in)) return r;
    }
    return mechanicalResult(in);
}

std::vector<sawrecipes::Option> sawrecipes::forInput(GlobalPos const& pos, ItemStack const& input) {
    std::vector<Option> out;
    if (ci::isEmpty(input)) return out;
    for (auto const* r : rc::withFirstIngredient("cutting", input)) {
        if (r->results.empty()) continue;
        auto display = ci::make(r->results[0].id, r->results[0].count);
        if (!display) continue;
        Option o;
        o.id      = r->id;
        o.time    = r->processingTime > 0 ? r->processingTime : 50;
        o.display = *display;
        o.roll    = [r] { return rc::rollResults(*r); };
        out.push_back(std::move(o));
    }
    if (config().allowStonecuttingOnSaw) {
        forEachGameRecipe("stonecutter", [&](std::string const& id, Recipe const& r) {
            auto const& results = r.getResultItems();
            if (results.empty()) return false;
            auto const& ings = r.mMyIngredients.get();
            if (ings.empty() || !r.itemValidForRecipe(ings.front(), input)) return false;
            auto result = toStack(results.front());
            if (ci::isEmpty(result)) return false;
            Option o;
            o.id      = id;
            o.time    = 50;
            o.display = result;
            o.roll    = [result] { return std::vector<ItemStack>{result}; };
            out.push_back(std::move(o));
            return false;
        });
    }
    // Create's outputMatchesFilter
    std::erase_if(out, [&](Option const& o) { return !common::filterslot::matches(pos, o.display); });
    return out;
}

} // namespace lv::feature::crafter
