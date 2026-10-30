// Recipes in a basin (Create's BasinRecipe, the press's and mixer's static recipe filters,
// automated crafting) and the two basin operators: the mechanical press (Create's
// MechanicalPressBlockEntity + PressingBehaviour: in the world, on depots / belts, compacting in a
// basin) and the mechanical mixer (Create's MechanicalMixerBlockEntity).

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/item/ItemDescriptor.h"
#include "mc/world/item/alchemy/PotionBrewing.h"
#include "mc/world/item/crafting/Recipe.h"
#include "mc/world/item/crafting/RecipeIngredient.h"
#include "mc/world/item/crafting/Recipes.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/phys/AABB.h"

#include "core/fluids/Fluids.h"
#include "feature/assembly/SequencedAssembly.h"
#include "feature/basin/BasinInternal.h"
#include "feature/belts/BeltApi.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::basin {

namespace items  = common::items;
namespace fluids = common::fluids;
namespace cr     = common::recipes;
namespace bd     = common::blockdata;
namespace k      = lv::kinetics;
namespace sa     = assembly::sequenced;

// ---- recipes ---------------------------------------------------------------------------------------------

namespace recipes {

namespace {

struct Candidates {
    std::vector<BasinRecipe>                       list;
    std::vector<core::common::ProcessingRecipe> const* source = nullptr;
    std::size_t                                    sourceSize = 0;
    bool                                           built      = false;
};

Candidates& candidateCache(bool press) {
    static Candidates press_, mixer_;
    return press ? press_ : mixer_;
}

ItemStack fromInstance(ItemInstance const& inst) {
    auto tag = inst.save(*SaveContextFactory::createCloneSaveContext());
    if (!tag) return items::empty();
    return ItemStack::fromTag(*tag);
}

/// The key of one crafting ingredient for Create's "all ingredients equal" test.
std::string ingredientKey(RecipeIngredient const& ing) {
    if (ing.isNull()) return {};
    return ing.getSerializedNameAndAux(false);
}

/// The crafting table recipes of the world with their expanded ingredient lists.
void addCraftingRecipes(bool press, std::vector<BasinRecipe>& out) {
    auto const& cfg   = config::get<BasinConfig>();
    auto        level = ll::service::getLevel();
    if (!level) return;
    auto& all = level->getRecipes();
    for (auto const& [tag, byId] : *all.mRecipes) {
        if (tag.getString() != "crafting_table") continue;
        for (auto const& [id, recipe] : byId) {
            if (!recipe || recipe->isMultiRecipe()) continue;
            if (recipe->getResultItems().empty()) continue;
            if (cb::ignoredInAutomation(id)) continue;
            std::vector<std::string> keys;
            for (auto const& ing : *recipe->mMyIngredients) {
                int n = std::max<int>(1, static_cast<int>(ing.mStackSize));
                for (int i = 0; i < n; ++i) keys.push_back(ingredientKey(ing));
            }
            bool compress = cb::canCompress(keys);
            if (press) {
                if (!cfg.allowShapedSquareInPress || !compress) continue;
            } else {
                if (!cfg.allowShapelessInMixer || !recipe->isShapeless() || keys.size() <= 1 || compress) continue;
            }
            BasinRecipe r;
            r.id              = id;
            r.crafting        = true;
            r.vanilla         = recipe;
            r.ingredientCount = static_cast<int>(keys.size());
            out.push_back(std::move(r));
        }
    }
}

/// One ingredient test (processing ingredient or crafting descriptor).
struct Ingredient {
    core::common::ItemIngredient const* processing = nullptr;
    RecipeIngredient const*             crafting   = nullptr;
    ::Recipe const*                     recipe     = nullptr;
    [[nodiscard]] bool test(ItemStack const& stack) const {
        if (items::isEmpty(stack)) return false;
        if (processing) return cr::matches(*processing, stack);
        return recipe->itemValidForRecipe(*crafting, stack);
    }
};

std::vector<Ingredient> ingredientsOf(BasinRecipe const& r) {
    std::vector<Ingredient> out;
    if (r.processing) {
        for (auto const& ing : r.processing->ingredients) out.push_back({&ing, nullptr, nullptr});
    } else if (r.vanilla) {
        for (auto const& ing : *r.vanilla->mMyIngredients) {
            if (ing.isNull()) continue;
            int n = std::max<int>(1, static_cast<int>(ing.mStackSize));
            for (int i = 0; i < n; ++i) out.push_back({nullptr, &ing, r.vanilla.get()});
        }
    }
    return out;
}

ItemStack firstResult(BasinRecipe const& r) {
    if (r.processing) {
        if (r.processing->results.empty()) return items::empty();
        auto const& res = r.processing->results.front();
        return items::make(res.id, res.count).value_or(items::empty());
    }
    auto const& results = r.vanilla->getResultItems();
    return results.empty() ? items::empty() : fromInstance(results.front());
}

bool fluidOk(core::common::FluidIngredient const& ing, cb::Fluid const& f) {
    // NeoForge's SizedFluidIngredient.test: the fluid matches and there is at least the amount
    return !f.empty() && cr::matchesFluid(ing, f.id) && f.amount >= ing.amount;
}

bool applyImpl(BlockSource& region, GlobalPos const& basinPos, BasinState& b, BasinRecipe const& r, bool test) {
    if (r.processing && !core::common::heatSatisfies(r.processing->heat, basinops::heatUnder(region, basinPos))) return false;
    auto                    ingredients = ingredientsOf(r);
    std::vector<ItemStack>  outItems;
    std::vector<cb::Fluid>  outFluids;
    auto                    tanks = basinops::combinedTanks(b);
    static std::vector<core::common::FluidIngredient> const noFluids;
    auto const& fluidIngredients = r.processing ? r.processing->fluidIngredients : noFluids;

    for (bool simulate : {true, false}) {
        if (!simulate && test) return true;
        std::array<int, 2 * cb::kSlotCount> extracted{};
        std::array<int, 4>                  extractedFluid{};
        std::vector<ItemStack>              consumed;
        for (auto const& ing : ingredients) {
            bool found = false;
            for (int slot = 0; slot < 2 * cb::kSlotCount; ++slot) {
                auto& s = basinops::slot(b, slot);
                if (simulate && items::countOf(s) <= extracted[slot]) continue;
                if (!ing.test(s)) continue;
                consumed.push_back(items::withCount(s, 1));
                if (!simulate) s = items::withCount(s, items::countOf(s) - 1);
                ++extracted[slot];
                found = true;
                break;
            }
            if (!found) return false;
        }
        for (auto const& fi : fluidIngredients) {
            int  required = fi.amount;
            bool found    = false;
            for (int t = 0; t < static_cast<int>(tanks.size()); ++t) {
                auto& tank = *tanks[t];
                if (simulate && tank.amount <= extractedFluid[t]) continue;
                if (!fluidOk(fi, tank)) continue;
                int drained = std::min(required, tank.amount);
                if (!simulate) {
                    tank.amount -= drained;
                    if (tank.amount <= 0) tank = {};
                }
                required -= drained;
                if (required != 0) continue;
                extractedFluid[t] += drained;
                found = true;
                break;
            }
            if (!found) return false;
        }
        if (simulate) {
            if (r.processing) {
                outItems = cr::rollResults(*r.processing);
                for (auto const& f : r.processing->fluidResults)
                    if (f.amount > 0) outFluids.push_back({f.fluid, f.amount});
            } else {
                for (auto const& res : r.vanilla->getResultItems())
                    if (auto st = fromInstance(res); !items::isEmpty(st)) outItems.push_back(st);
            }
            // containers left by the consumed items (Bedrock's crafting recipes list them among
            // their results already: cake -> buckets, honey block -> bottles)
            if (r.processing)
                for (auto const& c : consumed)
                    if (auto rest = remainderOf(c); !items::isEmpty(rest)) outItems.push_back(rest);
        }
        if (!basinops::acceptOutputs(region, basinPos, b, outItems, outFluids, simulate)) return false;
    }
    return true;
}

// ---- potion brewing in the mixer (Create's PotionMixingRecipes) ----
//
// Create turns every brewing stand mix into a mixing recipe: the reagent + 1000 mB of the input
// potion -> 1000 mB of the output potion, heated (potion mixes for each bottle type, container
// mixes such as gunpowder -> splash for every potion). Bedrock keeps its mixes in PotionBrewing,
// whose lists have no usable layout, so the recipes are made on demand from what the basin holds
// and Bedrock's own PotionBrewing::mix decides the result.

struct Brewing {
    std::map<std::string, core::common::ProcessingRecipe> recipes; // stable addresses
    std::map<std::string, BasinRecipe>                    basin;
    std::map<std::string, std::string>                    outcomes; ///< "<fluid>/<reagent>#<aux>" -> brewed fluid ("" none)
};

Brewing& brewing() {
    static Brewing b;
    return b;
}

/// The potion fluid brewed from `fluid` with `reagent` ("" when there is no such mix).
std::string brewedFluid(std::string const& fluid, ItemStack const& reagent) {
    if (items::isEmpty(reagent) || !reagent.mItem.get()) return {};
    auto key = fluid + "/" + items::idOf(reagent) + "#" + std::to_string(reagent.getAuxValue());
    auto& outcomes = brewing().outcomes;
    if (auto it = outcomes.find(key); it != outcomes.end()) return it->second;
    auto& result = outcomes[key];
    auto  bottle = core::fluids::bottleFor(fluid);
    if (!bottle) return result;
    auto source = items::make(bottle->id, 1, bottle->aux);
    if (!source) return {};
    ItemInstance   in(*source);
    ItemDescriptor ingredient(*reagent.mItem.get(), reagent.getAuxValue());
    if (!PotionBrewing::hasMix(in, ingredient)) return {};
    auto out = fromInstance(PotionBrewing::mix(ingredient, in));
    if (items::isEmpty(out)) return result;
    auto brewed = core::fluids::fluidOfPotionItem(items::idOf(out), out.getAuxValue());
    if (brewed != fluid) result = brewed;
    return result;
}

BasinRecipe const* brewingRecipe(std::string const& fluid, ItemStack const& reagent) {
    auto to = brewedFluid(fluid, reagent);
    if (to.empty()) return nullptr;
    auto  reagentId = items::idOf(reagent);
    auto  id        = "levitate:potion_mixing/" + fluid + "/" + reagentId;
    auto& br        = brewing();
    if (auto it = br.basin.find(id); it != br.basin.end()) return &it->second;
    core::common::ProcessingRecipe p;
    p.id   = id;
    p.type = "mixing";
    p.ingredients.push_back({{reagentId}, {}});
    p.fluidIngredients.push_back({fluid, {}, 1000});
    p.fluidResults.push_back({to, 1000});
    p.heat              = core::common::HeatRequirement::Heated;
    auto const& stored  = br.recipes.emplace(id, std::move(p)).first->second;
    BasinRecipe r;
    r.id              = id;
    r.processing      = &stored;
    r.ingredientCount = 1;
    return &br.basin.emplace(id, std::move(r)).first->second;
}

} // namespace

std::vector<BasinRecipe> const& candidates(bool press) {
    auto& c      = candidateCache(press);
    auto const& source = cr::ofType(press ? "compacting" : "mixing");
    if (c.built && c.source == &source && c.sourceSize == source.size()) return c.list;
    c.list.clear();
    for (auto const& p : source) {
        BasinRecipe r;
        r.id              = p.id;
        r.processing      = &p;
        r.ingredientCount = static_cast<int>(p.ingredients.size());
        c.list.push_back(std::move(r));
    }
    addCraftingRecipes(press, c.list);
    c.source     = &source;
    c.sourceSize = source.size();
    c.built      = true;
    log().info("basin: {} {} recipes available", c.list.size(), press ? "compacting" : "mixing");
    return c.list;
}

BasinRecipe const* byId(bool press, std::string const& id) {
    if (id.empty()) return nullptr;
    for (auto const& r : candidates(press))
        if (r.id == id) return &r;
    if (!press) {
        if (auto it = brewing().basin.find(id); it != brewing().basin.end()) return &it->second;
        // a brewing recipe saved by a mixer before a reload: "levitate:potion_mixing/<fluid>/<reagent>"
        constexpr std::string_view kPrefix = "levitate:potion_mixing/";
        if (id.starts_with(kPrefix)) {
            auto rest  = id.substr(kPrefix.size());
            auto slash = rest.rfind('/');
            if (slash != std::string::npos)
                if (auto reagent = items::make(rest.substr(slash + 1))) return brewingRecipe(rest.substr(0, slash), *reagent);
        }
    }
    return nullptr;
}

bool match(BlockSource& region, GlobalPos const& basinPos, BasinState& b, BasinRecipe const& r) {
    // the (cheaper) ingredient test first; same result as Create's filter-first order
    if (!applyImpl(region, basinPos, b, r, true)) return false;
    // Create's FilteringBehaviour.test on the output (the first fluid when there are only fluids)
    auto filter = common::filterslot::get(basinPos, "filter");
    if (!items::isEmpty(filter)) {
        bool pass;
        if (r.processing && r.processing->results.empty() && !r.processing->fluidResults.empty()) {
            auto const& f = r.processing->fluidResults.front();
            pass          = common::filter::matchesFluid(filter, {f.fluid, f.amount});
        } else {
            pass = common::filter::matches(filter, firstResult(r));
        }
        if (!pass) return false;
    }
    return true;
}

bool apply(BlockSource& region, GlobalPos const& basinPos, BasinState& b, BasinRecipe const& r) {
    return applyImpl(region, basinPos, b, r, false);
}

std::vector<BasinRecipe const*> matching(BlockSource& region, GlobalPos const& basinPos, BasinState& b, bool press) {
    std::vector<BasinRecipe const*> out;
    if (basinops::isEmpty(b)) return out;
    int itemCount = 0;
    for (int i = 0; i < 2 * cb::kSlotCount; ++i) itemCount += items::countOf(basinops::slot(b, i));
    for (auto const& r : candidates(press)) {
        if (r.ingredientCount > itemCount) continue; // can't match: fewer items than ingredients
        if (match(region, basinPos, b, r)) out.push_back(&r);
    }
    if (!press && config::get<BasinConfig>().allowBrewingInMixer) {
        for (auto* tank : basinops::combinedTanks(b)) {
            if (tank->empty() || tank->amount < 1000) continue;
            if (tank->id != core::fluids::kWater && !core::fluids::isPotionFluid(tank->id)) continue;
            for (int i = 0; i < cb::kSlotCount; ++i) {
                auto const* r = brewingRecipe(tank->id, b.in[i]);
                if (r && std::find(out.begin(), out.end(), r) == out.end() && match(region, basinPos, b, *r)) out.push_back(r);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](auto const* a, auto const* c) { return a->ingredientCount > c->ingredientCount; });
    return out;
}

core::common::ProcessingRecipe const* pressing(ItemStack const& stack) { return cr::findFirst("pressing", stack); }

bool canPress(ItemStack const& stack) {
    if (items::isEmpty(stack)) return false;
    // Create's getRecipe: SequencedAssemblyRecipe.getRecipe(PRESSING) before the pressing recipes
    return sa::findStep(sa::kPressing, stack).has_value() || pressing(stack) != nullptr;
}

std::vector<ItemStack> press(ItemStack const& stack, int count, std::string* recipeId) {
    std::vector<ItemStack> results;
    if (items::isEmpty(stack) || count <= 0) return results;
    auto        step = sa::findStep(sa::kPressing, stack);
    auto const* r    = step ? nullptr : pressing(stack);
    if (!step && !r) return results;
    if (recipeId) *recipeId = step ? step->recipe->id : r->id;
    // RecipeApplier.applyRecipeOn: every item rolls its results, merged into earlier stacks
    auto add = [&](ItemStack out) {
        for (auto& have : results) {
            if (items::isEmpty(out)) return;
            if (!items::sameItemSameData(have, out)) continue;
            int amount = std::min(maxStackOf(have) - items::countOf(have), items::countOf(out));
            if (amount <= 0) continue;
            have = items::withCount(have, items::countOf(have) + amount);
            out  = items::withCount(out, items::countOf(out) - amount);
        }
        if (!items::isEmpty(out)) results.push_back(out);
    };
    for (int i = 0; i < count; ++i) {
        if (step) add(sa::advance(*step, items::withCount(stack, 1)));
        else
            for (auto const& out : roll(*r)) add(out);
    }
    return results;
}

std::vector<ItemStack> roll(core::common::ProcessingRecipe const& r) { return cr::rollResults(r); }

ItemStack remainderOf(ItemStack const& ingredient) {
    static std::map<std::string, std::string> const containers{
        {"minecraft:milk_bucket",        "minecraft:bucket"      },
        {"minecraft:water_bucket",       "minecraft:bucket"      },
        {"minecraft:lava_bucket",        "minecraft:bucket"      },
        {"minecraft:powder_snow_bucket", "minecraft:bucket"      },
        {"levitate:honey_bucket",        "minecraft:bucket"      },
        {"levitate:chocolate_bucket",    "minecraft:bucket"      },
        {"minecraft:honey_bottle",       "minecraft:glass_bottle"},
        {"minecraft:dragon_breath",      "minecraft:glass_bottle"},
    };
    auto it = containers.find(items::idOf(ingredient));
    if (it == containers.end()) return items::empty();
    return items::make(it->second).value_or(items::empty());
}

void reset() {
    candidateCache(true)  = {};
    candidateCache(false) = {};
    brewing()             = {};
}

} // namespace recipes

// ---- operators ---------------------------------------------------------------------------------------------

namespace operators {

namespace {

std::map<GlobalPos, OperatorState>& cache() {
    static std::map<GlobalPos, OperatorState> c;
    return c;
}

char const* keyOf(bool press) { return press ? "press" : "mixer"; }

GlobalPos basinPosOf(GlobalPos const& pos) { return {pos.dim, pos.x, pos.y - 2, pos.z}; }

/// Create's isSpeedRequirementFulfilled (the mixer needs medium speed).
bool speedOk(GlobalPos const& pos, bool press) {
    float speed = k::speedAt(pos);
    if (press) return true;
    return static_cast<int>(k::speedLevelOf(speed)) >= static_cast<int>(k::SpeedLevel::Medium);
}

BasinState* basinOf(BlockSource& region, GlobalPos const& pos) {
    auto bp = basinPosOf(pos);
    if (idAt(region, game::blockPos(bp)) != kBasin) return nullptr;
    return basinops::get(region, bp);
}

void basinChanged(GlobalPos const& basinPos, BasinState& b) {
    basinops::notifyChangeOfContents(b);
    basinops::save(basinPos, b);
    sync::dirty(basinPos);
}

int processingTimeOf(BasinRecipe const* r) { return r && r->processing ? r->processing->processingTime : 0; }

// ---- shared (BasinOperatingBlockEntity) ----

void startPress(GlobalPos const& pos, OperatorState& s, cb::PressMode mode) {
    s.mode             = static_cast<int>(mode);
    s.running          = true;
    s.prevRunningTicks = 0;
    s.runningTicks     = 0;
    sync::dirty(pos);
}

void startProcessingBasin(GlobalPos const& pos, OperatorState& s, bool press) {
    if (press) {
        if (s.running && s.runningTicks <= cb::kPressCycle / 2) return;
        startPress(pos, s, cb::PressMode::Basin);
    } else {
        if (s.running && s.runningTicks <= 20) return;
        s.running      = true;
        s.runningTicks = 0;
        sync::dirty(pos);
    }
}

void updateBasin(BlockSource& region, GlobalPos const& pos, OperatorState& s, bool press) {
    if (!speedOk(pos, press)) return;
    if (k::speedAt(pos) == 0) return;
    if (s.running) return;
    auto* b = basinOf(region, pos);
    if (!b || !basinops::canContinueProcessing(*b)) return;
    auto list = recipes::matching(region, basinPosOf(pos), *b, press);
    if (list.empty()) return;
    s.recipeId = list.front()->id;
    startProcessingBasin(pos, s, press);
}

bool matchBasinRecipe(BlockSource& region, GlobalPos const& pos, OperatorState const& s, bool press) {
    auto const* r = recipes::byId(press, s.recipeId);
    auto*       b = basinOf(region, pos);
    return r && b && recipes::match(region, basinPosOf(pos), *b, *r);
}

void applyBasinRecipe(BlockSource& region, GlobalPos const& pos, OperatorState& s, bool press) {
    auto const* r = recipes::byId(press, s.recipeId);
    if (!r) return;
    auto* b = basinOf(region, pos);
    if (!b) return;
    auto basinPos = basinPosOf(pos);
    bool wasEmpty = basinops::canContinueProcessing(*b);
    if (!recipes::apply(region, basinPos, *b, *r)) return;
    log().info("{} at {} {} {} in the basin", press ? "press" : "mixer", pos.key(), press ? "compacted" : "mixed", r->id);
    // continue mixing
    if (wasEmpty && recipes::match(region, basinPos, *b, *r)) {
        if (!press) s.runningTicks = 20;
    }
    basinChanged(basinPos, *b);
}

// ---- press ----

bool beltLike(std::string const& id) { return id == "levitate:depot" || id == "levitate:belt"; }

std::vector<ItemActor*> itemsIn(BlockSource& region, AABB const& box) {
    std::vector<ItemActor*> out;
    for (auto* a : region.getEntities(box, 0.f, ActorType::ItemEntity)) {
        if (!a || !a->isAlive()) continue;
        out.push_back(static_cast<ItemActor*>(a));
    }
    return out;
}

/// Create's `onGround()` for item entities: Bedrock item actors don't keep the flag up to date
/// while lying still, so an item resting on a block top (no vertical travel) counts too.
bool resting(ItemActor& e) {
    if (e.isOnGround()) return true;
    // lying still: no vertical travel (positions sit a little above the block top on Bedrock)
    return std::abs(e.getPosDelta().y) < 0.01f;
}

AABB blockBox(BlockPos const& p, float deflate) {
    return AABB{
        // the query matches entity positions (feet), so the bottom isn't deflated: an item lying
        // on the floor has its position exactly at the block's bottom
        Vec3{p.x + deflate,      p.y - .01f,         p.z + deflate     },
        Vec3{p.x + 1.f - deflate, p.y + 1.f - deflate, p.z + 1.f - deflate}
    };
}

void spawnAt(BlockSource& region, Vec3 const& at, ItemStack const& stack) {
    auto level = ll::service::getLevel();
    if (!level || items::isEmpty(stack)) return;
    auto* e = level->getSpawner().spawnItem(region, stack, nullptr, at, 10);
    // Create: results get a tiny random motion (VecHelper.offsetRandomly(ZERO, .05)), not a throw
    if (e) const_cast<Vec3&>(e->getPosDelta()) = Vec3{(random01() - .5f) * .05f, 0.f, (random01() - .5f) * .05f};
}

/// Create's tryProcessInWorld (simulate: only whether a recipe exists).
bool tryProcessInWorld(BlockSource& region, GlobalPos const& pos, ItemActor& entity, bool simulate) {
    auto& stack = entity.item();
    if (!recipes::canPress(stack)) return false;
    if (simulate) return true;
    auto const& cfg   = config::get<BasinConfig>();
    Vec3        at    = entity.getPosition();
    std::string input = items::idOf(stack);
    int         count = items::countOf(stack);
    std::string recipeId;
    if (cfg.bulkPressing || count == 1) {
        // RecipeApplier.applyRecipeOn(entity): every item of the stack is processed
        auto results = recipes::press(stack, count, &recipeId);
        entity.remove();
        for (auto const& out : results) spawnAt(region, at, out);
    } else {
        auto results = recipes::press(stack, 1, &recipeId);
        for (auto const& out : results) spawnAt(region, at, out);
        // the rest as a fresh entity: clients only learn a changed count from a new entity
        auto rest = items::withCount(stack, count - 1);
        entity.remove();
        spawnAt(region, at, rest);
    }
    log().info("press at {} pressed {} ({})", pos.key(), input, recipeId);
    return true;
}

void applyInWorld(BlockSource& region, GlobalPos const& pos, OperatorState& s) {
    auto bp   = game::blockPos(pos);
    bool bulk = config::get<BasinConfig>().bulkPressing;
    for (auto* e : itemsIn(region, blockBox(offsetOf(bp, kDown), 0.f))) {
        if (!resting(*e)) continue;
        s.scanCooldown = 0;
        // Create stops at the first item entity it finds; Bedrock lists entities in another order,
        // so items without a recipe (e.g. the results of the last cycle) are skipped first
        if (!tryProcessInWorld(region, pos, *e, true)) continue;
        tryProcessInWorld(region, pos, *e, false);
        if (!bulk) break;
    }
}

/// Create's PressingBehaviour.whenItemHeld at half cycle (BELT mode): the item held under the
/// press on the belt segment / depot two below is pressed (tryProcessOnBelt). The belts unit calls
/// the processor below while the item travels; the conversion runs here, in the press's own tick.
void applyOnBelt(GlobalPos const& pos) {
    GlobalPos below2{pos.dim, pos.x, pos.y - 2, pos.z};
    bool      bulk = config::get<BasinConfig>().bulkPressing;
    belts::handleCenteredProcessing(below2, .5f, [&](belts::TransportedItem& item) {
        if (!item.locked || !recipes::canPress(item.stack)) return belts::TransportedResult::doNothing();
        int         count = items::countOf(item.stack);
        bool        all   = bulk || count == 1;
        std::string recipeId;
        auto        outs  = recipes::press(item.stack, all ? count : 1, &recipeId);
        log().info("press at {} pressed {} on the {} ({})", pos.key(), items::idOf(item.stack),
                   belts::beltAt(below2) ? "belt" : "depot", recipeId);
        if (all) {
            if (outs.empty()) return belts::TransportedResult::removeItem();
            // the results stay under the press until it lifts (Create: outputs locked)
            ItemStack first = outs.front();
            outs.erase(outs.begin());
            return belts::TransportedResult::convertToAndLeaveHeld(std::move(outs), first);
        }
        // one item pressed, the rest stays held for the next cycle
        return belts::TransportedResult::convertToAndLeaveHeld(std::move(outs), items::withCount(item.stack, count - 1));
    });
}

void onPressingCompleted(BlockSource& region, GlobalPos const& pos, OperatorState& s) {
    auto* b = basinOf(region, pos);
    if (s.mode == static_cast<int>(cb::PressMode::Basin) && matchBasinRecipe(region, pos, s, true) && b
        && basinops::canContinueProcessing(*b)) {
        startProcessingBasin(pos, s, true);
    } else {
        s.basinCheck = true;
    }
}

} // namespace

OperatorState* get(GlobalPos const& pos, bool press) {
    auto& c = cache();
    if (auto it = c.find(pos); it != c.end()) return &it->second;
    OperatorState s;
    if (auto const* doc = bd::get(pos)) {
        if (auto it = doc->find(keyOf(press)); it != doc->end() && it->is_object()) {
            auto const& j      = *it;
            s.running          = j.value("running", false);
            s.runningTicks     = j.value("ticks", 0);
            s.prevRunningTicks = j.value("prev", 0);
            s.mode             = j.value("mode", 0);
            s.processingTicks  = j.value("proc", -1);
            s.recipeId         = j.value("recipe", std::string{});
        }
    }
    return &c.emplace(pos, std::move(s)).first->second;
}

void save(GlobalPos const& pos, OperatorState const& s, bool press) {
    bd::edit(pos)[keyOf(press)] = Json{
        {"running", s.running         },
        {"ticks",   s.runningTicks    },
        {"prev",    s.prevRunningTicks},
        {"mode",    s.mode            },
        {"proc",    s.processingTicks },
        {"recipe",  s.recipeId        },
    };
}

void forget(GlobalPos const& pos) { cache().erase(pos); }
void forgetAll() { cache().clear(); }

void registerBeltProcessor() {
    // Create's PressingBehaviour.onItemReceived / whenItemHeld
    belts::registerProcessor(kPress, belts::Processor{
        .received = [](belts::ProcessorContext const& c, belts::TransportedItem& item) {
            if (k::speedAt(c.processor) == 0) return belts::ProcessingResult::Pass;
            auto* s = get(c.processor, true);
            if (s->running) return belts::ProcessingResult::Hold;
            if (!recipes::canPress(item.stack)) return belts::ProcessingResult::Pass;
            startPress(c.processor, *s, cb::PressMode::Belt);
            save(c.processor, *s, true);
            return belts::ProcessingResult::Hold;
        },
        .held = [](belts::ProcessorContext const& c, belts::TransportedItem&) {
            if (k::speedAt(c.processor) == 0) return belts::ProcessingResult::Pass;
            auto* s = get(c.processor, true);
            if (!s->running) return belts::ProcessingResult::Pass;
            return belts::ProcessingResult::Hold;
        },
    });
}

void onSpeedChanged(GlobalPos const& pos) {
    auto& c = cache();
    if (auto it = c.find(pos); it != c.end()) it->second.basinCheck = true;
    sync::dirty(pos);
}

void tickPress(BlockSource& region, GlobalPos const& pos, OperatorState& s) {
    bool was = s.running;
    int  wasTicks = s.runningTicks;
    if (s.basinRemoved) {
        s.basinRemoved = false;
        s.running      = false;
        s.runningTicks = 0;
        save(pos, s, true);
        sync::dirty(pos);
        return;
    }
    if (s.basinCheck) {
        s.basinCheck = false;
        updateBasin(region, pos, s, true);
    }
    float speed = k::speedAt(pos);
    auto  bp    = game::blockPos(pos);
    if (!s.running) {
        if (speed == 0) return;
        if (s.scanCooldown > 0) --s.scanCooldown;
        auto below2 = offsetOf(bp, kDown, 2);
        auto id2    = idAt(region, below2);
        // belts and depots call the press when an item reaches the centre (registerBeltProcessor)
        if (belts::isTransporter({pos.dim, below2.x, below2.y, below2.z})) return;
        if (s.scanCooldown <= 0) {
            s.scanCooldown = cb::kPressEntityScan;
            if (id2 == kBasin) return;
            // Create: the block below deflated by 1/8 against the item box (1/4 wide) = item positions anywhere in the block
            for (auto* e : itemsIn(region, blockBox(offsetOf(bp, kDown), 0.f))) {
                if (!resting(*e)) continue;
                if (!tryProcessInWorld(region, pos, *e, true)) continue;
                startPress(pos, s, cb::PressMode::World);
                save(pos, s, true);
                return;
            }
        }
        return;
    }
    if (s.runningTicks == cb::kPressCycle / 2 && speed != 0) {
        auto mode = static_cast<cb::PressMode>(s.mode);
        if (mode == cb::PressMode::World) applyInWorld(region, pos, s);
        if (mode == cb::PressMode::Basin) applyBasinRecipe(region, pos, s, true);
        if (mode == cb::PressMode::Belt) applyOnBelt(pos);
        auto below2 = offsetOf(bp, kDown, 2);
        if (beltLike(idAt(region, below2)) && idAt(region, below2) == "levitate:belt") {
            playSound(pos.dim, bp, "dig.cloth", .5f, 1.f);
            playSound(pos.dim, bp, "random.break", .25f, 1.f);
        } else {
            float pitch = .75f + std::abs(speed) / 1024.f;
            playSound(pos.dim, bp, "random.anvil_land", .0625f, pitch);
            playSound(pos.dim, bp, "random.break", .25f, pitch);
        }
        sync::dirty(pos);
    }
    if (s.runningTicks > cb::kPressCycle) {
        s.finished = true;
        s.running  = false;
        onPressingCompleted(region, pos, s);
        save(pos, s, true);
        sync::dirty(pos);
        return;
    }
    s.prevRunningTicks = s.runningTicks;
    s.runningTicks += cb::pressRunningTickSpeed(speed);
    if (s.prevRunningTicks < cb::kPressCycle / 2 && s.runningTicks >= cb::kPressCycle / 2) s.runningTicks = cb::kPressCycle / 2;
    // saved on start / finish (above) and every second while running
    if (was != s.running || (wasTicks != s.runningTicks && now() % 20 == 0)) save(pos, s, true);
}

void tickMixer(BlockSource& region, GlobalPos const& pos, OperatorState& s) {
    bool was      = s.running;
    int  wasTicks = s.runningTicks;
    if (s.basinRemoved) {
        s.basinRemoved = false;
        if (s.running) {
            s.runningTicks = 40;
            s.running      = false;
        }
        save(pos, s, false);
        sync::dirty(pos);
        return;
    }
    if (s.basinCheck) {
        s.basinCheck = false;
        updateBasin(region, pos, s, false);
    }
    if (s.runningTicks >= 40) {
        s.running      = false;
        s.runningTicks = 0;
        s.basinCheck   = true;
        save(pos, s, false);
        sync::dirty(pos);
        return;
    }
    float speed = std::abs(k::speedAt(pos));
    auto  bp    = game::blockPos(pos);
    if (s.running) {
        if (speed == 0 || !speedOk(pos, false)) {
            if (s.runningTicks < 20) s.runningTicks = 40 - s.runningTicks;
            else if (s.runningTicks == 20) ++s.runningTicks;
        }
        if (s.runningTicks == 20) {
            if (s.processingTicks < 0) {
                s.processingTicks = cb::mixerProcessingTicks(speed, processingTimeOf(recipes::byId(false, s.recipeId)));
                if (auto* b = basinOf(region, pos)) {
                    bool wet = std::any_of(b->tankIn.begin(), b->tankIn.end(), [](auto const& t) { return !t.empty(); })
                            || std::any_of(b->tankOut.begin(), b->tankOut.end(), [](auto const& t) { return !t.empty(); });
                    if (wet) playSound(pos.dim, bp, "bubble.down", .75f, speed < 65 ? .75f : 1.5f);
                }
            } else {
                --s.processingTicks;
                if (s.processingTicks == 0) {
                    ++s.runningTicks;
                    s.processingTicks = -1;
                    applyBasinRecipe(region, pos, s, false);
                }
            }
            // Create's MIXING sound while the whisk is down (every 4 ticks from the server)
            if (now() % 4 == 0) playSound(pos.dim, bp, "dig.netherrack", .1f, .5f);
        }
        if (s.runningTicks != 20) ++s.runningTicks;
    }
    if (was != s.running || wasTicks != s.runningTicks) {
        if (was != s.running || s.runningTicks == 20 || s.runningTicks == 21 || now() % 20 == 0) save(pos, s, false);
        if (was != s.running || s.runningTicks == 20 || s.runningTicks == 21 || s.runningTicks == 1) sync::dirty(pos);
    }
}

Json renderJson(GlobalPos const& pos, OperatorState const& s, bool press) {
    return {
        {"running", s.running              },
        {"ticks",   s.runningTicks         },
        {"prev",    s.prevRunningTicks     },
        {"mode",    s.mode                 },
        {"speed",   k::speedAt(pos)        },
        {"press",   press                  },
    };
}

} // namespace operators

} // namespace lv::feature::basin
