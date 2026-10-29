// Fluid registry (U19): Create's fluids, their containers and the filling / emptying rules.

#include "feature/fluids/FluidRegistry.h"

#include <random>

#include "core/fluids/Fluids.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::fluids {

namespace cf    = core::fluids;
namespace items = common::items;
namespace rec   = common::recipes;

namespace {

constexpr char kBucket[]      = "minecraft:bucket";
constexpr char kGlassBottle[] = "minecraft:glass_bottle";
constexpr char kBuildersTea[] = "levitate:builders_tea";

/// Registers the containers of our fluids with the U3 fluid capability (bucket <-> 1000 mB,
/// bottle <-> 250 mB), so every unit (tanks, basins, pipes) knows them.
struct ContainerRegistrar {
    ContainerRegistrar() {
        namespace cfl = common::fluids;
        cfl::registerFluidItem("levitate:honey_bucket", {std::string(cf::kHoney), cf::kBucket}, kBucket);
        cfl::registerFluidItem("levitate:chocolate_bucket", {std::string(cf::kChocolate), cf::kBucket}, kBucket);
        cfl::registerFluidItem("minecraft:honey_bottle", {std::string(cf::kHoney), cf::kBottle}, kGlassBottle);
        cfl::registerFluidItem(kBuildersTea, {std::string(cf::kTea), cf::kBottle}, kGlassBottle);
    }
};
ContainerRegistrar const gContainers;

ItemStack make(std::string_view id, int aux = 0) {
    auto s = items::make(id, 1, aux);
    return s ? *s : items::empty();
}

bool isRecipeType(std::string const& type, std::string_view want) {
    // "create:filling" / "levitate:filling" / "filling"
    auto colon = type.find(':');
    return (colon == std::string::npos ? type : type.substr(colon + 1)) == want;
}

// ---- sequenced assembly (Create's SequencedAssemblyRecipe, filling steps) ----------------------
//
// A transitional item carries {"recipe": "<sequenced assembly recipe id>", "step": n} in its
// Levitate data under "sequenced_assembly" (SPEC §3.6.19); the step after the last one rolls the
// recipe's results by their weights.

struct SequencedStep {
    rec::ProcessingRecipe const* recipe = nullptr;
    nlohmann::json const*        step   = nullptr;
    int                          index  = 0;
};

std::optional<SequencedStep> sequencedFillingStep(ItemStack const& stack) {
    auto const& all = rec::ofType("sequenced_assembly");
    if (all.empty()) return std::nullopt;
    auto data = items::getData(stack, "sequenced_assembly");
    for (auto const& r : all) {
        auto seq = r.raw.find("sequence");
        if (seq == r.raw.end() || !seq->is_array() || seq->empty()) continue;
        int index = -1;
        if (data.is_object() && data.value("recipe", "") == r.id) {
            auto trans = r.raw.value("transitional_item", nlohmann::json::object());
            std::string tid = core::common::mapId(trans.value("item", trans.value("id", "")));
            if (!tid.empty() && items::idOf(stack) != tid) continue;
            index = data.value("step", 0);
        } else if (!data.is_object()) {
            core::common::ItemIngredient ing;
            auto const&                  in = r.raw.value("ingredient", nlohmann::json::object());
            if (in.is_object()) {
                if (in.contains("item")) ing.items.push_back(core::common::mapId(in["item"].get<std::string>()));
                if (in.contains("tag")) ing.tags.push_back(core::common::mapId(in["tag"].get<std::string>()));
            }
            if (!ing.empty() && rec::matches(ing, stack)) index = 0;
        }
        if (index < 0) continue;
        auto const& step = (*seq)[static_cast<std::size_t>(index) % seq->size()];
        if (!step.is_object() || !isRecipeType(step.value("type", ""), "filling")) continue;
        return SequencedStep{&r, &step, index};
    }
    return std::nullopt;
}

std::optional<core::common::FluidIngredient> stepFluid(nlohmann::json const& step) {
    auto parsed = core::common::parseRecipe(step, "filling", "step");
    if (!parsed || parsed->fluidIngredients.empty()) return std::nullopt;
    return parsed->fluidIngredients.front();
}

ItemStack advanceSequence(SequencedStep const& s, ItemStack const& stack) {
    auto const& raw   = s.recipe->raw;
    int         len   = static_cast<int>(raw["sequence"].size());
    int         loops = std::max(1, raw.value("loops", 1));
    int         next  = s.index + 1;
    if (next >= len * loops) {
        // Create's rollResult: one output picked by weight (chance)
        auto const& results = s.recipe->results;
        if (results.empty()) return items::empty();
        float total = 0;
        for (auto const& r : results) total += r.chance;
        static std::mt19937                   rng{std::random_device{}()};
        std::uniform_real_distribution<float> dist(0.f, total);
        float                                 n = dist(rng);
        for (auto const& r : results) {
            n -= r.chance;
            if (n < 0) {
                auto out = items::make(r.id, r.count);
                return out ? *out : items::empty();
            }
        }
        auto out = items::make(results.back().id, results.back().count);
        return out ? *out : items::empty();
    }
    auto        trans = raw.value("transitional_item", nlohmann::json::object());
    std::string tid   = core::common::mapId(trans.value("item", trans.value("id", "")));
    ItemStack   out   = tid.empty() ? items::withCount(stack, 1) : make(tid);
    if (items::isEmpty(out)) return out;
    items::setData(out, "sequenced_assembly", {{"recipe", s.recipe->id}, {"step", next}, {"progress", float(next) / float(len * loops)}});
    return out;
}

/// The filling recipe for one of `stack` with `fluid` (nullptr if none).
rec::ProcessingRecipe const* fillingRecipe(ItemStack const& stack, FluidStack const& fluid) {
    for (auto const* r : rec::withFirstIngredient("filling", stack)) {
        if (r->fluidIngredients.empty()) continue;
        if (rec::matchesFluid(r->fluidIngredients.front(), fluid.fluid)) return r;
    }
    return nullptr;
}

bool anyFillingRecipe(ItemStack const& stack) { return !rec::withFirstIngredient("filling", stack).empty(); }

/// Glass bottles take water, potions and builder's tea directly (Create's
/// canFillGlassBottleInternally).
bool bottleTakes(std::string_view fluid) { return fluid == cf::kWater || cf::isPotionFluid(fluid) || fluid == cf::kTea; }

} // namespace

std::string bucketOf(std::string_view fluid) {
    if (fluid == cf::kWater) return "minecraft:water_bucket";
    if (fluid == cf::kLava) return "minecraft:lava_bucket";
    if (fluid == cf::kMilk) return "minecraft:milk_bucket";
    if (fluid == cf::kHoney) return "levitate:honey_bucket";
    if (fluid == cf::kChocolate) return "levitate:chocolate_bucket";
    return {};
}

bool hasBlockState(std::string_view fluid) {
    return fluid == cf::kWater || fluid == cf::kLava || fluid == cf::kHoney || fluid == cf::kChocolate;
}

std::optional<Emptied> emptyItem(ItemStack const& stack) {
    if (items::isEmpty(stack)) return std::nullopt;
    auto id = items::idOf(stack);
    // potions (and water bottles) -> potion fluid + glass bottle
    if (auto f = cf::fluidOfPotionItem(id, stack.getAuxValue()); !f.empty())
        return Emptied{{f, cf::kBottle}, make(kGlassBottle)};
    // emptying recipes
    if (auto const* r = rec::findFirst("emptying", stack); r && !r->fluidResults.empty()) {
        ItemStack rest = items::empty();
        if (!r->results.empty()) rest = make(r->results.front().id);
        return Emptied{{r->fluidResults.front().fluid, r->fluidResults.front().amount}, rest};
    }
    // fluid containers (buckets): Create's fluid handler items; powder snow is no fluid in Create
    auto f = common::fluids::fluidInItem(stack);
    if (f.empty() || f.fluid == "minecraft:powder_snow") return std::nullopt;
    std::string container = id.ends_with("_bucket") ? kBucket : kGlassBottle;
    return Emptied{f, make(container)};
}

bool canBeEmptied(ItemStack const& stack) { return emptyItem(stack).has_value(); }

bool canBeFilled(ItemStack const& stack, bool bySpout) {
    if (items::isEmpty(stack)) return false;
    auto id = items::idOf(stack);
    if (id == kGlassBottle || id == kBucket) return true;
    if (!bySpout) return false;
    return anyFillingRecipe(stack) || sequencedFillingStep(stack).has_value();
}

int requiredAmount(ItemStack const& stack, FluidStack const& available, bool bySpout) {
    if (items::isEmpty(stack) || available.empty()) return -1;
    auto id = items::idOf(stack);
    if (bySpout) {
        // FillingBySpout: sequenced assembly first, then filling recipes, then generic filling
        if (auto s = sequencedFillingStep(stack)) {
            if (auto f = stepFluid(*s->step); f && rec::matchesFluid(*f, available.fluid)) return f->amount;
        }
        if (auto const* r = fillingRecipe(stack, available)) return r->fluidIngredients.front().amount;
    }
    if (id == kGlassBottle && bottleTakes(available.fluid)) return cf::kBottle;
    if (id == kBucket && !bucketOf(available.fluid).empty()) return cf::kBucket;
    return -1;
}

ItemStack fillItem(ItemStack const& stack, FluidStack const& available, bool bySpout) {
    int need = requiredAmount(stack, available, bySpout);
    if (need < 0 || need > available.amount) return items::empty();
    auto id = items::idOf(stack);
    if (bySpout) {
        if (auto s = sequencedFillingStep(stack)) {
            if (auto f = stepFluid(*s->step); f && rec::matchesFluid(*f, available.fluid)) return advanceSequence(*s, stack);
        }
        if (auto const* r = fillingRecipe(stack, available)) {
            auto out = rec::rollResults(*r);
            return out.empty() ? items::empty() : out.front();
        }
    }
    if (id == kGlassBottle) {
        if (available.fluid == cf::kTea) return make(kBuildersTea);
        if (auto b = cf::bottleFor(available.fluid)) return make(b->id, b->aux);
    }
    if (id == kBucket) {
        auto b = bucketOf(available.fluid);
        if (!b.empty()) return make(b);
    }
    return items::empty();
}

std::string fluidName(std::string_view fluid) {
    auto key = cf::fluidNameKey(fluid);
    if (auto pf = cf::parsePotionFluid(fluid)) {
        std::string name = pf->potion;
        for (auto& c : name)
            if (c == '_') c = ' ';
        return tr(key, "Potion") + " (" + name + ")";
    }
    std::string fallback(fluid.substr(fluid.find(':') + 1));
    if (!fallback.empty()) fallback[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(fallback[0])));
    return tr(key, fallback);
}

} // namespace lv::feature::fluids
