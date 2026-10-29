#pragma once

// Pure logic of the stock network (U26, SPEC §3.6.26): Create 6's logistics bookkeeping without
// game types. Items are identified by a *key* string (the game side uses the serialised stack
// without its count), so everything here is unit-testable.
//
// - Summary: Create's InventorySummary (counts per item, insertion order, "infinite" counts).
// - PromiseQueue: Create's RequestPromiseQueue (items promised to arrive, with expiry).
// - distributeOrder: Create's LogisticsManager.findPackagersForRequest (which link sends what).
// - Gauge maths: demand, restock amounts, promise expiry, panel slots and connection checks.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::core::stock {

/// Create's BigItemStack.INF: counts at or above this are infinite (creative crates).
inline constexpr int kInfinite = 1'000'000'000;

struct Entry {
    std::string key;
    int         count = 0;
    friend bool operator==(Entry const&, Entry const&) = default;
};

/// Create's InventorySummary: one entry per distinct item (same item and data), in the order
/// items were first added. Entries are kept when their count drops to 0 (as Create).
class Summary {
public:
    /// Adds `count` (may be negative) of `key`. Zero counts and empty keys are ignored. Counts
    /// saturate at kInfinite.
    void add(std::string const& key, int count);
    void add(Summary const& other);
    [[nodiscard]] int  countOf(std::string const& key) const;
    [[nodiscard]] bool empty() const { return mEntries.empty(); }
    [[nodiscard]] int  total() const { return mTotal; }
    /// Entries in insertion order.
    [[nodiscard]] std::vector<Entry> const& entries() const { return mEntries; }
    /// Entries sorted by count, largest first (stable: equal counts keep insertion order).
    [[nodiscard]] std::vector<Entry> byCount() const;
    /// Removes an entry. Returns whether it existed.
    bool erase(std::string const& key);

    /// Number of stock links that contributed (Create's contributingLinks).
    int contributingLinks = 0;

    [[nodiscard]] nlohmann::json toJson() const;
    static Summary               fromJson(nlohmann::json const& j);

private:
    std::vector<Entry> mEntries;
    int                mTotal = 0;
};

/// Create's RequestPromise / RequestPromiseQueue: items requested from the network that are
/// expected to arrive somewhere. Promises age by one per `tick()`.
class PromiseQueue {
public:
    struct Promise {
        std::string key;
        int         count = 0;
        int         ticks = 0;
    };

    void add(std::string const& key, int count, int ticks = 0);
    /// Sum of the promised counts of `key`; promises at least `expiryTicks` old are removed first
    /// (-1 = never expire).
    [[nodiscard]] int totalPromisedAndRemoveExpired(std::string const& key, int expiryTicks);
    /// Removes every promise of `key`.
    void forceClear(std::string const& key);
    /// `amount` of `key` arrived: fulfils promises oldest-first.
    void itemEnteredSystem(std::string const& key, int amount);
    void tick();
    [[nodiscard]] bool                        empty() const { return mPromises.empty(); }
    [[nodiscard]] std::vector<Promise> const& all() const { return mPromises; }
    /// Calls `fn` after every change (persistence).
    std::function<void()> onChanged;

    [[nodiscard]] nlohmann::json toJson() const;
    static PromiseQueue          fromJson(nlohmann::json const& j);

private:
    void changed() const {
        if (onChanged) onChanged();
    }
    std::vector<Promise> mPromises;
};

// ---- order distribution --------------------------------------------------------------------------

/// A stock link as the order distribution sees it.
struct LinkView {
    int         id = 0;        ///< caller's handle
    std::string inventory;     ///< identity of its packager's target inventory ("" = none)
    int         power = 0;     ///< redstone power at the link (Create sorts links by it)
    Summary     available;     ///< what its packager can take out
};

/// One packaging request: link `link` packs `count` of `key`.
struct LinkRequest {
    int         link = 0;
    std::string key;
    int         count     = 0;
    int         linkIndex = 0;     ///< order of the link in this order (for re-assembly)
    bool        finalLink = false; ///< the last link of the order
    bool        context   = false; ///< carries the order context (first package of the order)
};

/// Create's findPackagersForRequest: links sorted by redstone power (lowest first); links sharing
/// one inventory are reduced to one picked by `pick(n)` (returns 0..n-1); each ordered entry is
/// taken from the links in turn until satisfied.
[[nodiscard]] std::vector<LinkRequest>
distributeOrder(std::vector<Entry> const& order, std::vector<LinkView> const& links, std::function<int(int)> const& pick);

/// Create's packager busy limits (queued packages): player 50, redstone 20, restock 10.
enum class RequestType { Restock, Redstone, Player };
[[nodiscard]] int busyLimit(RequestType type);

// ---- factory gauge ---------------------------------------------------------------------------------

/// Items wanted by a gauge: `amount` items (up to) or stacks.
[[nodiscard]] int demandOf(int amount, bool upTo, int maxStackSize);
/// Create's tryRestock: what a restocker orders (clamped to 9 stacks and to what's available).
[[nodiscard]] int restockAmount(int demand, int promised, int inStorage, int maxStackSize, int availableOnNetwork);
/// Create's getPromiseExpiryTimeInTicks: -1 never, 0 = 30 s, n = n minutes.
[[nodiscard]] int promiseExpiryTicks(int interval);

/// Face-attached blocks (stock link, factory gauge): Create's AttachFace.
enum class AttachFace : int { Floor = 0, Wall = 1, Ceiling = 2 };

struct Vec3d {
    double x = 0, y = 0, z = 0;
};

/// The 4 panels of a factory gauge block (Create's PanelSlot order).
enum class PanelSlot : int { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

/// Java 2D facing (0 south, 1 west, 2 north, 3 east) as a unit vector (dx, dz).
[[nodiscard]] std::pair<int, int> facingVector(int facing2d);
/// The direction from the block towards its support (Create's getConnectedDirection().getOpposite()),
/// as (dx, dy, dz).
[[nodiscard]] std::array<int, 3> supportOffset(AttachFace face, int facing2d);
/// Block-local centre of a panel slot (on the panel plane, 1/16 off the support face).
[[nodiscard]] Vec3d panelCentre(AttachFace face, int facing2d, PanelSlot slot);
/// Create's getTargetedSlot: the slot nearest to a block-local click position.
[[nodiscard]] PanelSlot targetedSlot(AttachFace face, int facing2d, Vec3d const& local);

/// Create's FactoryPanelConnectionHandler.checkForIssues for two gauge panels: the translation
/// key of the problem or nullopt. `fromInputs` = connections the receiving panel already has.
struct PanelRef {
    int        x = 0, y = 0, z = 0;
    AttachFace face     = AttachFace::Floor;
    int        facing2d = 0;
    bool       restocker = false;
    bool       hasItem   = false;
};
[[nodiscard]] std::optional<std::string>
connectionIssue(PanelRef const& target, PanelRef const& input, bool alreadyConnected, int targetInputs);

/// Maximum inputs of a gauge (Create: 9) and maximum gauge distance (Create: < 16 blocks).
inline constexpr int kMaxPanelInputs = 9;
inline constexpr int kMaxOrderSlots  = 9;

} // namespace lv::core::stock
