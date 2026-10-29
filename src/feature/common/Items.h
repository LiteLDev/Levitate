#pragma once

// Item stack helpers shared by every unit (U3). Server thread unless noted.

#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

class ItemStack;
class CompoundTag;

namespace lv::feature::common::items {

/// A stack of `id` (nullopt when the item is not registered).
[[nodiscard]] std::optional<ItemStack> make(std::string_view id, int count = 1, int aux = 0);
/// "minecraft:stone" ("" for an empty stack).
[[nodiscard]] std::string idOf(ItemStack const& stack);
/// Empty or air.
[[nodiscard]] bool isEmpty(ItemStack const& stack);
[[nodiscard]] int  countOf(ItemStack const& stack);
/// Copy of `stack` with `count` items (empty when count <= 0).
[[nodiscard]] ItemStack withCount(ItemStack const& stack, int count);
/// The empty stack.
[[nodiscard]] ItemStack const& empty();

/// Same item (and aux value), ignoring data / count (Create's `ItemStack.isSameItem`).
[[nodiscard]] bool sameItem(ItemStack const& a, ItemStack const& b);
/// Same item and the same data (Create's `isSameItemSameComponents`): aux, user data.
[[nodiscard]] bool sameItemSameData(ItemStack const& a, ItemStack const& b);

/// Serialises a stack (item, count, aux, user data) to JSON for BlockData / packets:
/// {"id": "...", "count": n, "aux": n, "nbt": "<snbt of the full stack>"}.
[[nodiscard]] nlohmann::json toJson(ItemStack const& stack);
/// `toJson` of `stack` as if it held `count` items, without copying the stack (safe for stacks
/// the game owns, e.g. the client's inventory: copying them on the client asserts).
[[nodiscard]] nlohmann::json toJsonWithCount(ItemStack const& stack, int count);
/// The stack back (empty stack for null / unreadable input).
[[nodiscard]] ItemStack fromJson(nlohmann::json const& j);

/// The item's user data (custom NBT), nullptr when it has none.
[[nodiscard]] CompoundTag const* userData(ItemStack const& stack);
/// A Levitate JSON document stored in the stack's user data under `key` (null when absent).
[[nodiscard]] nlohmann::json getData(ItemStack const& stack, std::string_view key);
/// Stores (or with null removes) a Levitate JSON document in the stack's user data.
void setData(ItemStack& stack, std::string_view key, nlohmann::json const& value);

/// Display name of the stack (custom name or the item's localised name, server side default
/// language).
[[nodiscard]] std::string displayName(ItemStack const& stack);

} // namespace lv::feature::common::items
