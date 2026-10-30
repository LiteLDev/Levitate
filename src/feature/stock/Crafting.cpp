// Stock unit: "Use Mechanical Crafting" of a factory gauge (U42; Create's FactoryPanelScreen
// availableCraftingRecipe + convertRecipeToPackageOrderContext). A gauge whose connected inputs
// are exactly the ingredients of a crafting-table recipe for its item may send them as one
// package per craft, laid out in the recipe's 3x3 pattern, so mechanical crafters receiving it
// craft the item.

#include <set>

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

#include "feature/common/Items.h"
#include "feature/stock/StockInternal.h"

namespace lv::feature::stock {

namespace {

namespace items = common::items;

ItemStack toStack(ItemInstance const& inst) {
    auto tag = inst.save(*SaveContextFactory::createCloneSaveContext());
    if (!tag) return items::empty();
    return ItemStack::fromTag(*tag);
}

bool ingredientEmpty(RecipeIngredient const& ing) { return ing.isNull() || ing.mStackSize == 0; }

} // namespace

std::optional<CraftingArrangement> craftingArrangement(ItemStack const& output, std::vector<ItemStack> const& inputs) {
    if (items::isEmpty(output) || inputs.empty()) return std::nullopt;
    auto level = ll::service::getLevel();
    if (!level) return std::nullopt;
    auto const& byTag = level->getRecipes().mRecipes.get();
    auto        it    = byTag.find(HashedString("crafting_table"));
    if (it == byTag.end()) return std::nullopt;
    auto outId = items::idOf(output);
    for (auto const& [id, recipe] : it->second) {
        if (!recipe || recipe->isMultiRecipe()) continue;
        auto const& results = recipe->getResultItems();
        if (results.empty()) continue;
        auto result = toStack(results.front());
        if (items::isEmpty(result) || items::idOf(result) != outId) continue;
        // every ingredient matches one of the inputs, and every input is used
        std::set<std::size_t> used;
        auto                  pick = [&](RecipeIngredient const& ing) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < inputs.size(); ++i)
                if (recipe->itemValidForRecipe(ing, inputs[i])) return i;
            return std::nullopt;
        };
        CraftingArrangement out;
        out.pattern.fill(-1);
        out.resultCount = std::max(1, items::countOf(result));
        bool ok         = true;
        if (recipe->isShapeless()) {
            std::size_t slot = 0;
            for (auto const& ing : recipe->mMyIngredients.get()) {
                if (ingredientEmpty(ing)) continue;
                auto i = pick(ing);
                if (!i) {
                    ok = false;
                    break;
                }
                used.insert(*i);
                for (int n = 0; n < std::max<int>(1, ing.mStackSize) && slot < 9; ++n) out.pattern[slot++] = static_cast<int>(*i);
            }
        } else {
            int w = recipe->mWidth, h = recipe->mHeight;
            if (w <= 0 || h <= 0 || w > 3 || h > 3) continue;
            for (int y = 0; y < h && ok; ++y) {
                for (int x = 0; x < w; ++x) {
                    auto const& ing = recipe->getIngredient(x, y);
                    if (ingredientEmpty(ing)) continue;
                    auto i = pick(ing);
                    if (!i) {
                        ok = false;
                        break;
                    }
                    used.insert(*i);
                    out.pattern[static_cast<std::size_t>(y * 3 + x)] = static_cast<int>(*i);
                }
            }
        }
        if (!ok || used.size() < inputs.size()) continue;
        out.recipeId = id;
        return out;
    }
    return std::nullopt;
}

} // namespace lv::feature::stock
