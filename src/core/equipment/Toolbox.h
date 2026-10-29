#pragma once

// Toolbox inventory model (U31, SPEC §3.6.31), independent of the game.
//
// Create's ToolboxInventory: 8 compartments of 4 stacks (32 slots). A compartment is locked to the
// first item put into it (its *filter*, kept when the compartment runs empty); items share a
// compartment when they match the filter (same item and data, or the same damageable tool).
// Insertion follows Forge's insertItemStacked: first merge into stacks of the same item, then
// fill empty slots in slot order. In *limited* mode (returning items) only compartments with a
// matching filter take items.

#include <array>
#include <string>

#include "nlohmann/json.hpp"

namespace lv::core::equipment {

inline constexpr int kCompartments           = 8;
inline constexpr int kStacksPerCompartment    = 4;
inline constexpr int kToolboxSlots            = kCompartments * kStacksPerCompartment;

/// A stack as the toolbox sees it. `match` decides the compartment (Create's
/// canItemsShareCompartment), `payload` identifies the exact item (stacks merge when equal).
struct TStack {
    std::string match;   ///< "" = empty
    std::string payload; ///< serialised item (game layer), compared for merging
    int         count = 0;
    int         max   = 64; ///< max stack size of the item
    [[nodiscard]] bool empty() const { return match.empty() || count <= 0; }
    [[nodiscard]] TStack withCount(int n) const {
        TStack s = *this;
        s.count  = n;
        if (n <= 0) s = TStack{};
        return s;
    }
    friend bool operator==(TStack const&, TStack const&) = default;
};

struct ToolboxInv {
    std::array<TStack, kToolboxSlots>  slots{};
    std::array<TStack, kCompartments> filters{}; ///< count 1 copies

    /// Inserts with insertItemStacked semantics; returns what did not fit.
    TStack insert(TStack stack, bool limited, bool simulate);
    /// Takes up to `amount` items from a compartment, last stacks first (Create's
    /// distributeExtract). Returns the items.
    TStack extract(int compartment, int amount, bool simulate);
    /// Items in a compartment.
    [[nodiscard]] int count(int compartment) const;
    /// The first stack of a compartment (what the radial menu shows), empty if none.
    [[nodiscard]] TStack const& first(int compartment) const;
    /// Moves a compartment's stacks to the front and merges partial stacks (Create's settle).
    void settle(int compartment);
    /// Clears a compartment's filter when it is empty (clicking an empty slot with an empty cursor).
    bool clearFilter(int compartment);
    /// Whether the compartment accepts `stack`.
    [[nodiscard]] bool accepts(int compartment, TStack const& stack, bool limited) const;
    /// Number of non-empty compartments (comparator output uses the fill level).
    [[nodiscard]] float fillLevel() const;

    [[nodiscard]] nlohmann::json toJson() const;
    static ToolboxInv            fromJson(nlohmann::json const& j);
};

/// Create's ToolboxHandler: a linked hotbar slot is kept at (maxStack + 1) / 2 items.
[[nodiscard]] constexpr int hotbarTarget(int maxStack) { return (maxStack + 1) / 2; }

/// Create's toolbox range check: squared distance from the player to (x+0.5, y, z+0.5) < range².
[[nodiscard]] bool inToolboxRange(double px, double py, double pz, int bx, int by, int bz, double range);

} // namespace lv::core::equipment
