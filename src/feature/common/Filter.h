#pragma once

// Filters (U3, SPEC §2.26 / §3.2): `levitate:filter` (list filter) and
// `levitate:attribute_filter`, matched with Create's semantics (FilterItemStack):
//
// - an empty filter slot matches everything;
// - a plain item matches the same item (data ignored unless `matchData`);
// - a list filter matches when any of its 18 entries matches (recursively, entries may be
//   filters themselves), honouring "respect data"; a deny-list inverts the result; an empty list
//   matches only filter items;
// - an attribute filter combines its attribute tests in allow-list (any) / allow-list (all) /
//   deny-list mode; an empty attribute list matches only attribute filter items.
//
// Filter contents live in the item's user data (Levitate keys "lv:filter" / "lv:attribute_filter").
// Units with filter slots use `matches()` / `predicate()`; the slots themselves are FilterSlot.h.

#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/common/FilterLogic.h"
#include "feature/common/Capabilities.h"

class ItemStack;
class Player;

namespace lv::feature::common::filter {

using Json = nlohmann::json;

inline constexpr char const* kListFilterId      = "levitate:filter";
inline constexpr char const* kAttributeFilterId = "levitate:attribute_filter";

[[nodiscard]] bool isListFilter(ItemStack const& stack);
[[nodiscard]] bool isAttributeFilter(ItemStack const& stack);
[[nodiscard]] bool isFilterItem(ItemStack const& stack);

// ---- matching ----------------------------------------------------------------------------------

/// Create's FilterItemStack.test(world, stack, matchData).
[[nodiscard]] bool matches(ItemStack const& filter, ItemStack const& stack, bool matchData = false);
/// Create's FilterItemStack.test(world, fluidStack): the filter item's fluid (buckets, bottles),
/// recursively through list filters. Attribute filters never match fluids.
[[nodiscard]] bool matchesFluid(ItemStack const& filter, fluids::FluidStack const& fluid);
/// A reusable predicate (pre-parses the filter's contents once).
[[nodiscard]] items::ItemPredicate predicate(ItemStack const& filter, bool matchData = false);

/// Filter items of other units (Create's FilterItem subclasses, e.g. U25's package filter):
/// `matches` / `predicate` hand a filter of `itemId` to `fn(filter, stack, matchData)`, and
/// `isFilterItem` counts it as a filter item. The unit registers itself (common never includes it).
using Matcher = std::function<bool(ItemStack const& filter, ItemStack const& stack, bool matchData)>;
void registerMatcher(std::string itemId, Matcher fn);

// ---- list filter contents ----------------------------------------------------------------------

struct ListFilter {
    std::vector<ItemStack> items; ///< kListFilterSlots entries (empty stacks for empty slots)
    bool                   blacklist   = false;
    bool                   respectData = false;
};
[[nodiscard]] ListFilter readList(ItemStack const& filter);
void                     writeList(ItemStack& filter, ListFilter const& data);

// ---- attribute filter contents -----------------------------------------------------------------

/// One configured attribute: type id ("in_tag"), its value ("minecraft:logs", "" when the type
/// has none) and whether it is inverted ("is not ...").
struct AttributeEntry {
    std::string type;
    std::string value;
    bool        inverted = false;
    friend bool operator==(AttributeEntry const&, AttributeEntry const&) = default;
};
struct AttributeFilter {
    core::common::AttributeMode mode = core::common::AttributeMode::WhitelistAny;
    std::vector<AttributeEntry> attributes;
};
[[nodiscard]] AttributeFilter readAttributes(ItemStack const& filter);
void                          writeAttributes(ItemStack& filter, AttributeFilter const& data);

/// An item attribute type (Create's ItemAttributeType).
struct AttributeType {
    std::string id; ///< "placeable", "in_tag", ...
    /// The attribute values that apply to `stack` (one "" entry for value-less attributes).
    std::function<std::vector<std::string>(ItemStack const&)> valuesFor;
    /// Whether the attribute with `value` applies to `stack`.
    std::function<bool(ItemStack const&, std::string const& value)> test;
};
/// Adds an attribute type (units add theirs, e.g. compat ones); replaces one with the same id.
void registerAttribute(AttributeType type);
/// Every attribute (type, value) that applies to `stack`, in registration order (what the
/// attribute filter screen offers for a reference item).
[[nodiscard]] std::vector<AttributeEntry> attributesOf(ItemStack const& stack);
/// Whether `entry` (ignoring `inverted`) applies to `stack`.
[[nodiscard]] bool attributeApplies(AttributeEntry const& entry, ItemStack const& stack);
/// Create's text for an attribute ("is tagged minecraft:logs" / "is not ..."), in `locale`.
[[nodiscard]] std::string describe(AttributeEntry const& entry, std::string const& locale = {});

/// Rewrites the filter's lore with Create's summary (mode line + up to 4 entries).
void updateLore(ItemStack& filter);


} // namespace lv::feature::common::filter
