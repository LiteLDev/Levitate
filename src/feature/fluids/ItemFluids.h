#pragma once

// Fluid registry of the fluids unit (U19, SPEC §2.14 / §3.6.19), contract style: the fluids Create
// adds (honey, chocolate, builder's tea, potion fluids) and how items hold them. Other units may
// use these functions through this header once U19 is merged (they work on fluid ids only, as the
// U3 fluid capability does); until then they read the same ids from `core/fluids/Fluids.h`.
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

/// The bucket item of a fluid ("" when it has none).
[[nodiscard]] std::string bucketOf(std::string_view fluid);
/// Fluid ids Create can place in the world as a block (water, lava, honey, chocolate).
[[nodiscard]] bool hasBlockState(std::string_view fluid);

/// Display name of a fluid in the server's default locale ("Honey", "Potion of Swiftness").
[[nodiscard]] std::string fluidName(std::string_view fluid);

} // namespace lv::feature::fluids
