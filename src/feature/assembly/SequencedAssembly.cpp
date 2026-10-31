#include "feature/assembly/SequencedAssembly.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

#include "mc/world/item/ItemStack.h"

#include "feature/common/FluidRegistry.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::assembly::sequenced {

namespace items   = common::items;
namespace recipes = common::recipes;
using Json        = nlohmann::json;

namespace {
struct Cache {
    bool                                                     loaded = false;
    std::vector<SequencedRecipe>                             list;
    std::vector<std::unique_ptr<std::vector<SequencedRecipe>>> retired; ///< matches handed out stay valid
};
Cache& cache() {
    static Cache c;
    static bool  hooked = [] {
        config::onReload([] { reload(); });
        return true;
    }();
    (void)hooked;
    return c;
}

std::vector<SequencedRecipe> const& load() {
    auto& c = cache();
    if (c.loaded) return c.list;
    c.loaded = true;
    c.list.clear();
    for (auto const& r : recipes::ofType("sequenced_assembly")) {
        std::string err;
        if (auto s = core::assembly::parseSequenced(r.raw, r.id, &err)) c.list.push_back(std::move(*s));
        else log().warn("sequenced assembly recipe {} skipped: {}", r.id, err);
    }
    std::sort(c.list.begin(), c.list.end(), [](auto const& a, auto const& b) { return a.id < b.id; });
    log().debug("loaded {} sequenced assembly recipes", c.list.size());
    return c.list;
}

float random01() {
    static std::mt19937                   rng{std::random_device{}()};
    std::uniform_real_distribution<float> dist(0.f, 1.f);
    return dist(rng);
}

bool firstStepAccepts(SequencedRecipe const& recipe, SequenceStep const& step, int finished, ItemStack const& stack) {
    // Create's initFromSequencedAssembly: the first step takes the transitional item or the base
    // ingredient, the others only the transitional item
    if (finished == 0 && matches(recipe, recipe.ingredient, stack)) return true;
    return items::idOf(stack) == recipe.transitionalItem || matches(recipe, step.recipe.ingredients.front(), stack);
}

std::string itemName(std::string const& id, std::string const& locale) {
    if (auto s = items::make(id)) return items::displayName(*s);
    (void)locale;
    return id;
}

std::string ingredientName(SequencedRecipe const& recipe, core::common::ItemIngredient const& ing, std::string const& locale) {
    if (!ing.items.empty()) return itemName(ing.items.front(), locale);
    for (auto const& t : ing.tags) {
        if (auto it = recipe.inlineTags.find(t); it != recipe.inlineTags.end() && !it->second.empty())
            return itemName(it->second.front(), locale);
        if (auto ids = recipes::tagItems(t); !ids.empty()) return itemName(ids.front(), locale);
        return "#" + t;
    }
    return "?";
}

std::string fluidName(std::string const& fluid, std::string const& locale) {
    return common::fluidtypes::displayName(fluid, locale);
}

void writeLore(ItemStack& stack) {
    auto lines = tooltip(stack);
    stack.setCustomLore(lines);
}
} // namespace

std::vector<SequencedRecipe> const& all() { return load(); }

SequencedRecipe const* byId(std::string const& id) {
    for (auto const& r : load())
        if (r.id == id) return &r;
    return nullptr;
}

bool matches(SequencedRecipe const& recipe, core::common::ItemIngredient const& ingredient, ItemStack const& stack) {
    if (items::isEmpty(stack)) return false;
    auto id = items::idOf(stack);
    for (auto const& t : ingredient.tags) {
        if (auto it = recipe.inlineTags.find(t); it != recipe.inlineTags.end()) {
            if (std::find(it->second.begin(), it->second.end(), id) != it->second.end()) return true;
        }
    }
    return recipes::matches(ingredient, stack);
}

std::optional<Progress> progressOf(ItemStack const& stack) {
    if (items::isEmpty(stack)) return std::nullopt;
    auto d = items::getData(stack, kDataKey);
    if (!d.is_object() || !d.contains("recipe")) return std::nullopt;
    Progress p;
    p.recipeId = d.value("recipe", std::string{});
    p.step     = d.value("step", 0);
    p.progress = d.value("progress", 0.f);
    return p;
}

bool isTransitional(ItemStack const& stack) { return progressOf(stack).has_value(); }

bool appliesTo(SequencedRecipe const& recipe, ItemStack const& stack) {
    if (matches(recipe, recipe.ingredient, stack)) return true;
    if (items::idOf(stack) != recipe.transitionalItem) return false;
    auto p = progressOf(stack);
    return p && p->recipeId == recipe.id;
}

std::optional<StepMatch> findStep(std::string_view machineType, ItemStack const& stack) {
    if (items::isEmpty(stack)) return std::nullopt;
    for (auto const& r : load()) {
        if (r.sequence.empty() || !appliesTo(r, stack)) continue;
        auto p        = progressOf(stack);
        int  finished = p && p->recipeId == r.id && items::idOf(stack) == r.transitionalItem ? p->step : 0;
        auto const& step = r.stepAt(finished);
        if (step.type != machineType) continue;
        if (!firstStepAccepts(r, step, finished, stack)) continue;
        return StepMatch{&r, finished, &step};
    }
    return std::nullopt;
}

bool acceptsHeld(StepMatch const& match, ItemStack const& held) {
    if (!match.step || match.step->type != kDeploying || match.step->recipe.ingredients.size() < 2) return false;
    return matches(*match.recipe, match.step->recipe.ingredients[1], held);
}

std::optional<StepMatch> findDeployingStep(ItemStack const& stack, ItemStack const& held) {
    if (items::isEmpty(stack) || items::isEmpty(held)) return std::nullopt;
    for (auto const& r : load()) {
        if (r.sequence.empty() || !appliesTo(r, stack)) continue;
        auto p        = progressOf(stack);
        int  finished = p && p->recipeId == r.id && items::idOf(stack) == r.transitionalItem ? p->step : 0;
        auto const& step = r.stepAt(finished);
        if (step.type != kDeploying || !firstStepAccepts(r, step, finished, stack)) continue;
        StepMatch m{&r, finished, &step};
        if (acceptsHeld(m, held)) return m;
    }
    return std::nullopt;
}

bool keepsHeldItem(StepMatch const& match) { return match.step && match.step->keepHeldItem; }

bool acceptsFluid(StepMatch const& match, FluidStack const& fluid) {
    if (!match.step || match.step->recipe.fluidIngredients.empty() || fluid.empty()) return false;
    return recipes::matchesFluid(match.step->recipe.fluidIngredients.front(), fluid.fluid);
}

int fluidAmount(StepMatch const& match) {
    if (!match.step || match.step->recipe.fluidIngredients.empty()) return 0;
    return match.step->recipe.fluidIngredients.front().amount;
}

int processingTime(StepMatch const& match) { return match.step ? match.step->recipe.processingTime : 0; }

ItemStack advance(StepMatch const& match, ItemStack const& input) {
    if (!match.recipe) return input;
    auto const& r = *match.recipe;
    if (core::assembly::completesWith(r, match.finished)) {
        int idx = core::assembly::pickWeighted(r.results, random01());
        if (idx < 0) return items::empty();
        auto const& res = r.results[static_cast<std::size_t>(idx)];
        if (auto out = items::make(res.id, res.count)) return *out;
        log().warn("sequenced assembly {}: unknown result item {}", r.id, res.id);
        return items::empty();
    }
    auto out = items::make(r.transitionalItem, 1);
    if (!out) {
        log().warn("sequenced assembly {}: unknown transitional item {}", r.id, r.transitionalItem);
        return input;
    }
    int step = match.finished + 1;
    float progress = core::assembly::progressAt(r, step);
    items::setData(*out, kDataKey, Json{{"recipe", r.id}, {"step", step}, {"progress", progress}});
    // the progress in percent for the client mod's item bar (its item renderer sees no item data)
    out->mAuxValue = static_cast<short>(std::clamp(std::lround(progress * 100.f), 0l, 100l));
    writeLore(*out);
    return *out;
}

namespace {
std::string describe(SequencedRecipe const* recipe, SequenceStep const& step, std::string const& locale) {
    if (step.type == kDeploying) {
        std::string name = "?";
        if (step.recipe.ingredients.size() > 1) {
            auto const& ing = step.recipe.ingredients[1];
            if (recipe) name = ingredientName(*recipe, ing, locale);
            else if (!ing.items.empty()) name = itemName(ing.items.front(), locale);
            else if (!ing.tags.empty()) {
                auto ids = recipes::tagItems(ing.tags.front());
                name     = ids.empty() ? "#" + ing.tags.front() : itemName(ids.front(), locale);
            }
        }
        return core::formatMessageArgs(translate("create.recipe.assembly.deploying_item", "Deploy {0}", locale), name);
    }
    if (step.type == kPressing) return std::string(translate("create.recipe.assembly.pressing", "Process in Press", locale));
    if (step.type == kCutting) return std::string(translate("create.recipe.assembly.cutting", "Cut with Saw", locale));
    if (step.type == kFilling) {
        std::string fluid = "?";
        if (!step.recipe.fluidIngredients.empty()) {
            auto const& f = step.recipe.fluidIngredients.front();
            fluid         = f.fluid.empty() ? f.tag : f.fluid;
        }
        return core::formatMessageArgs(translate("create.recipe.assembly.spout_filling_fluid", "Spout {0}", locale), fluidName(fluid, locale));
    }
    return step.type;
}
} // namespace

std::string describeStep(SequenceStep const& step, std::string const& locale) { return describe(nullptr, step, locale); }

std::vector<std::string> tooltip(ItemStack const& stack, std::string const& locale) {
    auto p = progressOf(stack);
    if (!p) return {};
    auto const* r = byId(p->recipeId);
    if (!r) return {};
    std::vector<std::string> names;
    for (auto const& s : r->sequence) names.push_back(describe(r, s, locale));
    std::vector<std::string> out;
    for (auto const& line : core::assembly::tooltipLines(*r, p->step, names)) {
        std::string text = line.key.empty() ? core::formatMessage(line.fallback, line.args)
                                            : core::formatMessage(translate(line.key, line.fallback, locale), line.args);
        out.push_back(std::string("§r§") + line.colour + text);
    }
    return out;
}

void reload() {
    auto& c = cache();
    if (c.loaded) c.retired.push_back(std::make_unique<std::vector<SequencedRecipe>>(std::move(c.list)));
    c.list.clear();
    c.loaded = false;
}

} // namespace lv::feature::assembly::sequenced
