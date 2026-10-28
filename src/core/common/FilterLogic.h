#pragma once

// Create's filter semantics, independent of the game (U3, SPEC §3.2).
//
// The game layer (feature/common/Filter) turns item stacks into callbacks; the decisions about
// how list entries and attribute tests combine live here so they are unit-tested.

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace lv::core::common {

/// Attribute filter modes, in Create's order (`create.gui.attribute_filter.allow_list_disjunctive` ...).
enum class AttributeMode : int {
    WhitelistAny = 0, ///< "Allow-List (Any)": passes when any attribute applies
    WhitelistAll = 1, ///< "Allow-List (All)": passes when every attribute applies
    Blacklist    = 2, ///< "Deny-List": passes when no attribute applies
};

[[nodiscard]] std::string_view attributeModeName(AttributeMode mode);
[[nodiscard]] AttributeMode    attributeModeFromName(std::string_view name, AttributeMode fallback = AttributeMode::WhitelistAny);

/// One attribute test of an attribute filter: does the attribute apply (before inversion)?
struct AttributeTest {
    bool applies  = false;
    bool inverted = false;
};

/// Create's AttributeFilterItemStack.test for a non-empty attribute list.
[[nodiscard]] bool combineAttributeTests(std::span<AttributeTest const> tests, AttributeMode mode);

/// Create's ListFilterItemStack.test for a non-empty entry list: `entryMatches(i)` tells whether
/// list entry `i` matches. Whitelist passes on the first match; deny-list fails on it.
[[nodiscard]] bool combineListFilter(std::size_t entries, std::function<bool(std::size_t)> const& entryMatches, bool blacklist);

/// Slots of a list filter (2 rows of 9, as Create's filter menu).
inline constexpr int kListFilterSlots = 18;

} // namespace lv::core::common
