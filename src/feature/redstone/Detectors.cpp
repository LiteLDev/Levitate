// Redstone unit (U23): smart observer and threshold switch (filter slots, inventory / tank
// reading through the U3 capabilities, the threshold screen and its form fallback).

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>

#include "ll/api/form/CustomForm.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/common/ValueBox.h"
#include "feature/redstone/Internal.h"
#include "feature/redstone/Redstone.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::redstone {

namespace {

constexpr char kThresholdScreen[] = "redstone.threshold_switch";
constexpr int  kObserverDelay     = 6; ///< Create's SmartObserverBlockEntity.DEFAULT_DELAY
constexpr int  kThresholdLazyRate = 10;
constexpr int  kThresholdPowerDelay = 2;

std::string posText(GP const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

std::unordered_map<std::string, threshold::Observable>& observables() {
    static std::unordered_map<std::string, threshold::Observable> m;
    return m;
}

bool fluidPasses(GP const& pos, common::fluids::FluidStack const& fluid) {
    auto f = common::filterslot::get(pos);
    if (common::items::isEmpty(f)) return true;
    return common::filter::matchesFluid(f, fluid);
}

// ---- filter slots (Create's FilteredDetectorFilterSlot) ----------------------------------------

/// Whether a face of the detector shows its filter slot.
bool filterSideActive(Block const& block, int side, bool slotAtBottom) {
    int target = targetDirection(block);
    if (side == target) return false;
    if (side == opposite(target)) return true;
    if (isHorizontal(target)) return side == kUp || (side == kDown && slotAtBottom);
    if (target == kUp) side = opposite(side);
    int facing = state(block, sFacing, kNorth);
    if (!slotAtBottom) return side == facing;
    int cw = clockwise(facing);
    return side == cw || side == opposite(cw);
}

void registerDetectorSlot(char const* id, bool slotAtBottom) {
    common::filterslot::SlotSpec slot;
    slot.id     = "filter";
    slot.centre = [slotAtBottom](Block const& block, int face) -> std::optional<core::common::Vec> {
        if (!filterSideActive(block, face, slotAtBottom)) return std::nullopt;
        // Create: (8, 8, 15.5) on the south face, i.e. half a pixel inside the face
        auto o = offsetOf(face);
        double k = 7.5 / 16.0;
        return core::common::Vec{0.5 + o[0] * k, 0.5 + o[1] * k, 0.5 + o[2] * k};
    };
    slot.scale = 0.5;
    if (std::string(id) == kThresholdSwitch) {
        slot.onChanged = [](GP const& pos) {
            if (auto* region = regionOf(pos.dim)) {
                auto bp = game::blockPos(pos);
                if (loadedAt(*region, bp)) updateThresholdLevel(*region, pos, region->getBlock(bp));
            }
        };
    }
    common::filterslot::registerSlot(id, std::move(slot));
}

} // namespace

void registerFilterSlots() {
    registerDetectorSlot(kSmartObserver, false);
    registerDetectorSlot(kThresholdSwitch, true);
}

// ---- smart observer ----------------------------------------------------------------------------

void activateObserver(BlockSource& region, GP const& pos, int ticks) {
    if (auto* rt = runtimeAt(pos)) counterOf(pos, *rt) = ticks;
    else dataFor(pos)["turnOff"] = ticks;
    auto bp = game::blockPos(pos);
    if (state(region.getBlock(bp), sPowered)) return;
    setStates(region, bp, {{sPowered, 1}});
}

void tickSmartObserver(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    auto bp = game::blockPos(pos);
    // scheduled block tick: switch off
    if (rt.blockTick >= 0 && --rt.blockTick < 0) {
        if (state(region.getBlock(bp), sPowered)) setStates(region, bp, {{sPowered, 0}});
    }
    int& turnOff = counterOf(pos, rt);
    if (turnOff > 0) {
        if (--turnOff == 0) rt.blockTick = 1;
    }

    int  target = targetDirection(block);
    auto tp     = step(bp, target);
    if (!loadedAt(region, tp)) return;
    auto filter = common::filterslot::get(pos);

    // the observed block itself matches the filter
    if (!common::items::isEmpty(filter)) {
        auto const& observed = region.getBlock(tp);
        if (!observed.isAir()) {
            if (auto item = common::items::make(observed.getTypeName()); item && common::filter::matches(filter, *item)) {
                activateObserver(region, pos, 3);
                return;
            }
        }
    }
    // items in an inventory (belts, depots, ... register item handlers too)
    common::Access at{region, tp, -1};
    if (auto h = common::items::find(at)) {
        auto found = h->extract(at, [&](ItemStack const& s) { return common::filterslot::matches(pos, s); }, 64, true);
        if (!common::items::isEmpty(found)) {
            activateObserver(region, pos, kObserverDelay);
            return;
        }
    }
    // fluids in a tank
    for (auto const& tank : common::fluids::tanks(region, tp, -1)) {
        if (!tank.contents.empty() && fluidPasses(pos, tank.contents)) {
            activateObserver(region, pos, kObserverDelay);
            return;
        }
    }
}

namespace observer {
void onFunnelTransfer(BlockSource& region, BlockPos const& funnelPos, ItemStack const& stack) {
    int dim = static_cast<int>(region.getDimensionId());
    for (int d = 0; d < 6; ++d) {
        auto p = step(funnelPos, d);
        if (!loadedAt(region, p)) continue;
        auto const& b = region.getBlock(p);
        if (b.getTypeName() != kSmartObserver || targetDirection(b) != opposite(d)) continue;
        GP gp{dim, p.x, p.y, p.z};
        if (!common::filterslot::matches(gp, stack)) continue;
        activateObserver(region, gp, 4);
    }
}
} // namespace observer

// ---- threshold switch --------------------------------------------------------------------------

namespace threshold {
void registerObservable(std::string const& blockId, Observable observable) { observables()[blockId] = std::move(observable); }
} // namespace threshold

namespace {

struct Measure {
    std::string type = "none"; ///< item, fluid, custom, none
    int         min = -1, level = -1, max = -1;
};

Measure measure(BlockSource& region, GP const& pos, Block const& block) {
    Measure m;
    auto    tp = step(game::blockPos(pos), targetDirection(block));
    if (!loadedAt(region, tp)) return m;
    auto const& target = region.getBlock(tp);
    if (auto it = observables().find(target.getTypeName()); it != observables().end()) {
        auto const& o = it->second;
        m.type  = "custom";
        m.min   = o.min ? o.min(region, tp) : 0;
        m.max   = o.max ? o.max(region, tp) : 0;
        m.level = o.current ? o.current(region, tp) : 0;
        return m;
    }
    common::Access at{region, tp, -1};
    auto           items = common::items::find(at);
    auto           tanks = common::fluids::tanks(region, tp, -1);
    bool           fluids = common::fluids::find(at).has_value();
    if (!items && !fluids) return m;
    m.type = items ? "item" : "fluid";
    m.min = 0, m.level = 0, m.max = 0;
    if (items) {
        int slots = items->slots ? items->slots(at) : 0;
        for (int i = 0; i < slots; ++i) {
            auto stack = items->getSlot(at, i);
            int  space = common::items::isEmpty(stack) ? 64 : std::max<int>(1, stack.getMaxStackSize());
            m.max += space;
            if (common::filterslot::matches(pos, stack)) m.level += common::items::countOf(stack);
        }
    }
    for (auto const& t : tanks) {
        if (t.capacity <= 0) continue;
        m.max += t.capacity;
        if (!t.contents.empty() && fluidPasses(pos, t.contents)) m.level += t.contents.amount;
    }
    m.level = std::clamp(m.level, m.min, std::max(m.min, m.max));
    return m;
}

void scheduleSwitchTick(GP const& pos) {
    if (auto* rt = runtimeAt(pos); rt && rt->blockTick < 0) rt->blockTick = kThresholdPowerDelay;
}

/// Create's updatePowerAfterDelay: the output follows the state (inverted if set).
void updatePowerAfterDelay(BlockSource& region, GP const& pos) {
    auto const* d        = dataOf(pos);
    bool        on       = d && d->value("state", false);
    bool        inverted = d && d->value("inverted", false);
    bool        powered  = on != inverted;
    dataFor(pos)["poweredAfterDelay"] = powered;
    setStates(region, game::blockPos(pos), {{sPowered, powered ? 1 : 0}});
}

std::string formatValue(std::string const& type, int value, bool stacks, std::string const& locale, BlockSource* region, BlockPos const* target) {
    if (type == "custom" && region && target) {
        auto const& id = region->getBlock(*target).getTypeName();
        if (auto it = observables().find(id); it != observables().end() && it->second.format) return it->second.format(value);
    }
    auto unit = type == "item" ? (stacks ? translate("create.schedule.condition.threshold.stacks", "Stacks", locale)
                                         : translate("create.schedule.condition.threshold.items", "Items", locale))
                               : translate("create.schedule.condition.threshold.buckets", "Buckets", locale);
    return std::to_string(value) + " " + std::string(unit);
}

} // namespace

void updateThresholdLevel(BlockSource& region, GP const& pos, Block const& block) {
    auto m  = measure(region, pos, block);
    auto bp = game::blockPos(pos);
    auto const* d = dataOf(pos);
    int  prevLevel = d ? d->value("current", -1) : -1;
    auto& w        = dataFor(pos);
    w["type"]      = m.type;
    w["min"]       = m.min;
    w["max"]       = m.max;
    if (m.type == "none") {
        // no compatible block: level 0, off (Create keeps the thresholds)
        if (prevLevel == -1) return;
        w["current"] = -1;
        w["state"]   = false;
        setStates(region, bp, {{sLevel, 0}});
        scheduleSwitchTick(pos);
        return;
    }
    w["current"]   = m.level;
    int  offBelow  = w.value("offBelow", 64);
    int  onAbove   = w.value("onAbove", 128);
    bool before    = w.value("state", false);
    bool now       = cr::thresholdState(before, m.level, offBelow, onAbove);
    w["state"]     = now;
    setStates(region, bp, {{sLevel, cr::thresholdDisplayLevel(m.level, m.min, m.max)}});
    if (before != now) scheduleSwitchTick(pos);
}

void tickThresholdSwitch(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    if (rt.blockTick >= 0 && --rt.blockTick < 0) updatePowerAfterDelay(region, pos);
    if (--rt.lazy <= 0) {
        rt.lazy = kThresholdLazyRate;
        updateThresholdLevel(region, pos, block);
    }
}

void configureThreshold(GP const& pos, int offBelow, int onAbove, bool inverted, bool inStacks) {
    auto const* d    = dataOf(pos);
    auto        type = d ? d->value("type", std::string("none")) : std::string("none");
    int         min  = d ? std::max(0, d->value("min", 0)) : 0;
    int         max  = d ? d->value("max", 0) : 0;
    int         step = cr::thresholdStep(type == "fluid", type == "item" && inStacks);
    if (max > min) std::tie(offBelow, onAbove) = cr::clampThresholds(offBelow, onAbove, min, max, step, true);
    else onAbove = std::max(onAbove, offBelow + 1);
    bool wasInverted = d && d->value("inverted", false);
    auto& w          = dataFor(pos);
    w["offBelow"]    = std::max(0, offBelow);
    w["onAbove"]     = std::max(1, onAbove);
    w["inverted"]    = inverted;
    w["inStacks"]    = inStacks;
    log().info("threshold switch at {} set to {}..{}{}", posText(pos), w["offBelow"].get<int>(), w["onAbove"].get<int>(), inverted ? " inverted" : "");
    if (wasInverted != inverted) {
        if (auto* region = regionOf(pos.dim)) updatePowerAfterDelay(*region, pos);
    }
}

// ---- screen ------------------------------------------------------------------------------------

namespace {

std::optional<GP> contextPos(Json const& ctx) {
    auto p = ctx.value("pos", Json());
    if (!p.is_array() || p.size() != 4) return std::nullopt;
    for (auto const& v : p)
        if (!v.is_number_integer()) return std::nullopt;
    return GP{p[0].get<int>(), p[1].get<int>(), p[2].get<int>(), p[3].get<int>()};
}

/// The switch at the context position if the player may use it (same dimension, within 8 blocks).
Block const* reachableSwitch(Player& player, GP const& pos) {
    if (static_cast<int>(player.getDimensionId()) != pos.dim) return nullptr;
    auto const& p  = player.getPosition();
    double      dx = p.x - (pos.x + 0.5), dy = p.y - (pos.y + 0.5), dz = p.z - (pos.z + 0.5);
    if (dx * dx + dy * dy + dz * dz > 64.0) return nullptr;
    auto& region = player.getDimensionBlockSource();
    auto  bp     = game::blockPos(pos);
    if (!loadedAt(region, bp)) return nullptr;
    auto const& b = region.getBlock(bp);
    return b.getTypeName() == kThresholdSwitch ? &b : nullptr;
}

std::optional<Json> screenData(Player& player, Json const& ctx) {
    auto pos = contextPos(ctx);
    if (!pos) return std::nullopt;
    auto const* block = reachableSwitch(player, *pos);
    if (!block) return std::nullopt;
    auto& region = player.getDimensionBlockSource();
    updateThresholdLevel(region, *pos, *block);
    auto const* d      = dataOf(*pos);
    auto        locale = localeOf(player);
    auto        tp     = step(game::blockPos(*pos), targetDirection(*block));
    auto const& target = region.getBlock(tp);
    auto        type   = d ? d->value("type", std::string("none")) : std::string("none");
    bool        stacks = d && d->value("inStacks", false);
    Json        out{
        {"type",      type                                                       },
        {"min",       d ? d->value("min", -1) : -1                               },
        {"max",       d ? d->value("max", -1) : -1                               },
        {"current",   d ? d->value("current", -1) : -1                           },
        {"onAbove",   d ? d->value("onAbove", 128) : 128                         },
        {"offBelow",  d ? d->value("offBelow", 64) : 64                          },
        {"inverted",  d && d->value("inverted", false)                           },
        {"inStacks",  stacks                                                     },
        {"powered",   state(*block, sPowered) != 0                               },
        {"target",    target.isAir() ? "" : target.getTypeName()                 },
        {"unitItems", std::string(translate("create.schedule.condition.threshold.items", "Items", locale))},
        {"unitStacks", std::string(translate("create.schedule.condition.threshold.stacks", "Stacks", locale))},
        {"unitBuckets", std::string(translate("create.schedule.condition.threshold.buckets", "Buckets", locale))},
    };
    if (type == "custom") {
        // labels of custom values, so the client needs no knowledge of the observable
        Json labels = Json::object();
        for (char const* k : {"onAbove", "offBelow", "current", "min", "max"}) labels[k] = formatValue(type, out[k].get<int>(), stacks, locale, &region, &tp);
        out["labels"] = labels;
    }
    return out;
}

void screenAction(Player& player, Json const& ctx, Json const& action) {
    auto pos = contextPos(ctx);
    if (!pos || !reachableSwitch(player, *pos)) return;
    configureThreshold(*pos, action.value("offBelow", 64), action.value("onAbove", 128), action.value("inverted", false), action.value("inStacks", false));
}

int formInt(ll::form::CustomFormResult const& r, std::string const& key, int fallback) {
    auto it = r->find(key);
    if (it == r->end()) return fallback;
    if (auto const* u = std::get_if<uint64>(&it->second)) return static_cast<int>(*u);
    if (auto const* f = std::get_if<double>(&it->second)) return static_cast<int>(std::lround(*f));
    return fallback;
}

void screenForm(Player& player, Json const& ctx) {
    auto pos = contextPos(ctx);
    if (!pos) return;
    auto data = screenData(player, ctx);
    if (!data) return;
    auto locale   = localeOf(player);
    auto type     = (*data)["type"].get<std::string>();
    bool stacks   = (*data)["inStacks"].get<bool>();
    int  min      = std::max(0, (*data)["min"].get<int>());
    int  max      = (*data)["max"].get<int>();
    int  step     = cr::thresholdStep(type == "fluid", type == "item" && stacks);
    auto fmt      = [&](int v) { return formatValue(type, v / step, stacks, locale, nullptr, nullptr); };
    ll::form::CustomForm form(trFor(player, "create.gui.threshold_switch.title", "Threshold Switch"));
    if (type == "none") {
        form.appendLabel(trFor(player, "create.gui.threshold_switch.not_attached", "Not attached to a block"));
    } else {
        form.appendLabel(trFor(player, "create.gui.threshold_switch.currently", "Currently {0}", fmt((*data)["current"].get<int>())));
        form.appendLabel(trFor(player, "create.gui.threshold_switch.range_max", "(max. {0})", fmt(max)));
    }
    if (type == "item") {
        form.appendDropdown(
            "measure",
            trFor(player, "create.schedule.condition.threshold.item_measure", "Item Measure"),
            {trFor(player, "create.schedule.condition.threshold.items", "Items"), trFor(player, "create.schedule.condition.threshold.stacks", "Stacks")},
            stacks ? 1 : 0
        );
    }
    int top = std::max(1, max / step); // the scroll inputs never go past the capacity
    form.appendSlider(
        "onAbove",
        trFor(player, "create.gui.threshold_switch.power_on_when", "Power on when...") + " " + trFor(player, "create.gui.threshold_switch.upper_threshold", "...at or above:"),
        min / step + 1,
        std::max(min / step + 1, top),
        1,
        std::clamp((*data)["onAbove"].get<int>() / step, min / step + 1, std::max(min / step + 1, top))
    );
    form.appendSlider(
        "offBelow",
        trFor(player, "create.gui.threshold_switch.power_off_when", "Power off when...") + " " + trFor(player, "create.gui.threshold_switch.lower_threshold", "...at or below:"),
        min / step,
        std::max(min / step, top - 1),
        1,
        std::clamp((*data)["offBelow"].get<int>() / step, min / step, std::max(min / step, top - 1))
    );
    form.appendToggle("inverted", trFor(player, "create.gui.threshold_switch.invert_signal", "Invert Signal"), (*data)["inverted"].get<bool>());
    GP p = *pos;
    form.sendTo(player, [p, step, stacks](Player& pl, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r || !reachableSwitch(pl, p)) return;
        bool inStacks = r->contains("measure") ? formInt(r, "measure", stacks ? 1 : 0) == 1 : stacks;
        // the sliders count in the unit shown when the form was opened
        int on  = formInt(r, "onAbove", 2) * step;
        int off = formInt(r, "offBelow", 1) * step;
        bool inverted = formInt(r, "inverted", 0) != 0;
        configureThreshold(p, off, on, inverted, inStacks);
    });
}

} // namespace

void registerThresholdScreen() {
    common::screens::registerType({
        .id     = kThresholdScreen,
        .data   = screenData,
        .action = screenAction,
        .form   = screenForm,
    });
}

void openThresholdScreen(Player& player, GP const& pos) {
    common::screens::open(player, kThresholdScreen, {{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}});
}

} // namespace lv::feature::redstone
