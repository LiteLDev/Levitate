#pragma once

// Create's advancement tree (AllAdvancements of Create 6.0.10), independent of the game: the
// entries in Create's order with their parent, icon, task type and collection criteria, the
// visibility rule of the advancement screen (vanilla's AdvancementVisibilityEvaluator) and the
// tree layout (columns by depth, rows in order, a parent centred on its children).
//
// Titles and descriptions are translations: `advancement.create.<id>` / `advancement.create.<id>.desc`.

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lv::core::advancements {

/// Create's CreateAdvancement.TaskType.
enum class Type { Silent, Normal, Noisy, Expert, Secret };

struct Def {
    std::string_view id;     ///< Create's id ("andesite_alloy")
    std::string_view parent; ///< "" for the root
    std::string_view icon;   ///< item id of the icon
    Type             type = Type::Normal;
    /// Inventory criteria (Create's whenIconCollected / whenItemCollected): every listed item
    /// must have been in the inventory; empty: awarded by a trigger.
    std::array<std::string_view, 2> collect{};
    bool free = false; ///< Create's awardedForFree (the root)
};

[[nodiscard]] std::span<Def const> all();
[[nodiscard]] Def const*           find(std::string_view id);
[[nodiscard]] int                  indexOf(std::string_view id);

/// The display flags of a type (Create's TaskType: toast, announceChat, hidden, frame).
[[nodiscard]] constexpr bool toast(Type t) { return t != Type::Silent; }
[[nodiscard]] constexpr bool announce(Type t) { return t == Type::Noisy || t == Type::Expert || t == Type::Secret; }
[[nodiscard]] constexpr bool hidden(Type t) { return t == Type::Secret; }
/// Goal frame (rounded) instead of the task frame (square).
[[nodiscard]] constexpr bool goal(Type t) { return t == Type::Expert || t == Type::Secret; }

/// Whether each entry (index of `all()`) is shown on the advancement screen given what is done
/// (vanilla: done, or a done descendant, or the nearest of itself / parent / grandparent that is
/// done or hidden is done).
[[nodiscard]] std::vector<bool> visibility(std::function<bool(std::string_view)> const& done);

struct Placement {
    float x = 0; ///< column (depth)
    float y = 0; ///< row
};
/// Position of each entry (index of `all()`) in the tree.
[[nodiscard]] std::vector<Placement> const& layout();

/// Parent index of an entry (-1 for the root).
[[nodiscard]] int parentOf(std::size_t index);

} // namespace lv::core::advancements
