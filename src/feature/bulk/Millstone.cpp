// Millstone (Create's MillstoneBlock / MillstoneBlockEntity): a small cog with a shaft below that
// grinds the item in its input slot with milling recipes into 9 output slots. Items go in from
// the top (thrown on it) or through the item capability (hoppers, funnels, chutes, belts);
// outputs come out through the capability or a right-click with an empty hand.
//
// BlockData `data["bulk"]`: {"in": stack, "out": [9 stacks], "timer": int}.

#include <algorithm>

#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "feature/bulk/Bulk.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "mod/Log.h"

namespace lv::feature::bulk {

namespace blockdata = common::blockdata;
namespace items     = common::items;

namespace {

constexpr int kOutputs = 9;

int durationOf(core::common::ProcessingRecipe const& r) {
    return r.processingTime > 0 ? r.processingTime : cb::kDefaultProcessingTime;
}

MillstoneView load(GlobalPos const& pos) {
    MillstoneView v;
    v.outputs.assign(kOutputs, items::empty());
    auto const* d = blockdata::get(pos);
    if (!d || !d->contains("bulk")) return v;
    auto const& j = (*d)["bulk"];
    v.input       = stackOf(j.value("in", Json(nullptr)));
    v.timer       = j.value("timer", 0);
    if (auto it = j.find("out"); it != j.end() && it->is_array()) {
        for (std::size_t i = 0; i < it->size() && i < kOutputs; ++i) v.outputs[i] = stackOf((*it)[i]);
    }
    return v;
}

void save(GlobalPos const& pos, MillstoneView const& v) {
    bool empty = items::isEmpty(v.input) && v.timer == 0
              && std::all_of(v.outputs.begin(), v.outputs.end(), [](ItemStack const& s) { return items::isEmpty(s); });
    if (empty) {
        if (auto const* d = blockdata::get(pos); d && d->contains("bulk")) {
            auto& e = blockdata::edit(pos);
            e.erase("bulk");
        }
        return;
    }
    Json out = Json::array();
    for (auto const& s : v.outputs) out.push_back(stackJson(s));
    auto& e     = blockdata::edit(pos);
    e["bulk"]   = Json{{"in", stackJson(v.input)}, {"out", std::move(out)}, {"timer", v.timer}};
}

bool canProcess(ItemStack const& stack) { return findRecipe("milling", stack) != nullptr; }

void process(GlobalPos const& pos, MillstoneView& v) {
    auto const* recipe = findRecipe("milling", v.input);
    if (!recipe) return;
    std::string inputId = items::idOf(v.input);
    v.input             = items::withCount(v.input, items::countOf(v.input) - 1);
    int produced        = 0;
    for (auto const& s : rollRecipe(*recipe, 1)) {
        produced += items::countOf(s);
        (void)insertStacked(v.outputs, s); // Create voids what does not fit
    }
    log().info("millstone at {} {} {}: milled {} into {} item(s)", pos.x, pos.y, pos.z, inputId, produced);
}

void spawnParticles(GlobalPos const& pos, MillstoneView const& v, float speed) {
    if (items::isEmpty(v.input)) return;
    // Create: item particles from a random point on a circle of radius .5, swirling with the stone
    float     angle = random01() * 6.2831853f;
    cb::Vec3d at{pos.x + .5 + std::sin(angle) * .5, pos.y + .5, pos.z + .5 + std::cos(angle) * .5};
    (void)speed;
    legacyParticle(pos.dim, at, 14 /* ParticleType::IconCrack */, (int(v.input.getId()) << 16) | (v.input.getAuxValue() & 0xffff));
}

} // namespace

MillstoneView millstoneAt(GlobalPos const& pos) { return load(pos); }

void tickMillstone(BlockSource& region, GlobalPos const& pos) {
    millstoneCatchItems(region, pos);
    float speed = kinetics::speedAt(pos);
    if (speed == 0) return;
    auto const* d = blockdata::get(pos);
    if (!d || !d->contains("bulk")) return;
    auto v = load(pos);
    // Create stops while an output slot is at its slot limit (64)
    for (auto const& s : v.outputs)
        if (items::countOf(s) == 64) return;
    if (v.timer > 0) {
        v.timer -= cb::millstoneProcessingSpeed(speed);
        if (currentTick() % 2 == 0) spawnParticles(pos, v, speed);
        if (v.timer <= 0) process(pos, v);
        save(pos, v);
        return;
    }
    if (items::isEmpty(v.input)) return;
    auto const* recipe = findRecipe("milling", v.input);
    v.timer            = recipe ? durationOf(*recipe) : 100;
    save(pos, v);
}

void millstoneCatchItems(BlockSource& region, GlobalPos const& pos) {
    cb::Box top{
        {pos.x + .0, pos.y + .9,  pos.z + .0},
        {pos.x + 1., pos.y + 1.3, pos.z + 1.}
    };
    for (auto* a : entitiesIn(region, top)) {
        auto* stack = itemOf(*a);
        if (!stack || items::isEmpty(*stack)) continue;
        if (!canProcess(*stack)) continue;
        auto v    = load(pos);
        int  room = 0;
        if (items::isEmpty(v.input)) room = slotLimit(*stack);
        else if (sameItem(v.input, *stack)) room = slotLimit(v.input) - items::countOf(v.input);
        int move = std::min(room, items::countOf(*stack));
        if (move <= 0) continue;
        v.input = items::isEmpty(v.input) ? items::withCount(*stack, move) : items::withCount(v.input, items::countOf(v.input) + move);
        save(pos, v);
        int left = items::countOf(*stack) - move;
        if (left <= 0) a->remove();
        else *stack = items::withCount(*stack, left);
    }
}

bool millstoneTakeOut(Player& player, GlobalPos const& pos) {
    auto v       = load(pos);
    bool emptyOut = true;
    int  given    = 0;
    for (auto& s : v.outputs) {
        if (items::isEmpty(s)) continue;
        emptyOut = false;
        given += items::countOf(s);
        giveOrDrop(player, s);
        s = items::empty();
    }
    if (emptyOut && !items::isEmpty(v.input)) {
        given += items::countOf(v.input);
        giveOrDrop(player, v.input);
        v.input = items::empty();
    }
    save(pos, v);
    if (given) log().info("millstone at {} {} {}: {} took {} item(s)", pos.x, pos.y, pos.z, player.getRealName(), given);
    return true;
}

// ---- capability -------------------------------------------------------------------------------------------------

namespace millstone_cap {

int slots() { return 1 + kOutputs; }

ItemStack getSlot(GlobalPos const& pos, int slot) {
    auto v = load(pos);
    if (slot == 0) return v.input;
    if (slot >= 1 && slot <= kOutputs) return v.outputs[slot - 1];
    return items::empty();
}

ItemStack insert(GlobalPos const& pos, ItemStack const& stack, bool simulate) {
    if (items::isEmpty(stack) || !canProcess(stack)) return stack;
    auto v    = load(pos);
    int  room = 0;
    if (items::isEmpty(v.input)) room = slotLimit(stack);
    else if (sameItem(v.input, stack)) room = slotLimit(v.input) - items::countOf(v.input);
    int move = std::min(room, items::countOf(stack));
    if (move <= 0) return stack;
    if (!simulate) {
        v.input = items::isEmpty(v.input) ? items::withCount(stack, move) : items::withCount(v.input, items::countOf(v.input) + move);
        save(pos, v);
    }
    return items::withCount(stack, items::countOf(stack) - move);
}

ItemStack extract(GlobalPos const& pos, std::function<bool(ItemStack const&)> const& filter, int amount, bool simulate) {
    auto v = load(pos);
    for (auto& s : v.outputs) {
        if (items::isEmpty(s) || (filter && !filter(s))) continue;
        int take = std::min(amount, items::countOf(s));
        if (take <= 0) continue;
        auto out = items::withCount(s, take);
        if (!simulate) {
            s = items::withCount(s, items::countOf(s) - take);
            save(pos, v);
        }
        return out;
    }
    return items::empty();
}

} // namespace millstone_cap

} // namespace lv::feature::bulk
