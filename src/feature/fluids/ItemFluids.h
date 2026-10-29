#pragma once

// How items hold fluids (U19, SPEC §2.14 / §3.6.19): filling and emptying items. The fluids
// themselves (honey, chocolate, builder's tea, potion fluids, their buckets / bottles, names,
// world blocks) are in the one fluid registry, `feature/common/FluidRegistry.h`; U19 registers
// Create's fluids there.
//
// Fluid ids: "minecraft:water", "minecraft:lava", "minecraft:milk", "levitate:honey",
// "levitate:chocolate", "levitate:tea", potion fluids "levitate:potion/<bottle>/<potion>"
// (bottle regular / splash / lingering, potion = Java potion name, e.g. "long_swiftness").
//
// Containers (Create's GenericItemEmptying / GenericItemFilling / FillingBySpout):
//     bucket  <-> 1000 mB  (water, lava, milk, honey, chocolate)
//     bottle  <-> 250 mB   (water bottle = water; potions keep their type and bottle; honey bottle;
//                           builder's tea)
// plus the `filling` / `emptying` processing recipes (assets/recipes/{filling,emptying}).

#include <optional>
#include <string>
#include <string_view>

#include "mc/world/item/ItemStack.h"

#include "feature/common/Capabilities.h"

namespace lv::feature::fluids {

using common::fluids::FluidStack;

/// What emptying one item gives: the fluid and the item left behind (Create's
/// GenericItemEmptying.emptyItem).
struct Emptied {
    FluidStack fluid;
    ItemStack  container;
};

/// Whether one of `stack` can be emptied (buckets, bottles, potions, honey bottles, builder's tea,
/// emptying recipes).
[[nodiscard]] bool canBeEmptied(ItemStack const& stack);
/// Empties one item of `stack` (count ignored); nullopt when it holds no fluid.
[[nodiscard]] std::optional<Emptied> emptyItem(ItemStack const& stack);

/// Whether one of `stack` can be filled at all (Create's GenericItemFilling.canItemBeFilled, plus
/// filling recipes and sequenced assembly steps when `bySpout`).
[[nodiscard]] bool canBeFilled(ItemStack const& stack, bool bySpout);
/// mB needed to fill one of `stack` with `available` (Create's getRequiredAmountForItem), -1 when
/// that fluid can't fill it. Filling recipes and sequenced assembly steps only when `bySpout`.
[[nodiscard]] int requiredAmount(ItemStack const& stack, FluidStack const& available, bool bySpout);
/// Fills one of `stack` with `available` (which must hold at least `requiredAmount`); returns the
/// filled item (empty on failure).
[[nodiscard]] ItemStack fillItem(ItemStack const& stack, FluidStack const& available, bool bySpout);

} // namespace lv::feature::fluids
