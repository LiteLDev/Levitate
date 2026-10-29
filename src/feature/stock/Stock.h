#pragma once

// Stock network (U26, SPEC §2.22 / §3.6.26): Create 6's logistics networks. This header is the
// unit's public, contract-style API: other units (U25 packages, U24 display links, ...) use it
// without including the unit's implementation.
//
// - A *network* is a frequency (a UUID string). Stock links, stock tickers, redstone requesters
//   and factory gauge panels are tuned to one. Stock links sit on packagers: their packagers'
//   target inventories make up the network's stock.
// - Packagers belong to U25. U25 (or a test) plugs them in with `registerPackagerProvider`: the
//   stock network asks the provider what a packager can take out of its inventory and hands it
//   packaging requests (item, count, address, order context); the packager boxes them.
//
//     stock::registerPackagerProvider({ .isPackager = ..., .availableItems = ..., .send = ... });
//     auto summary = stock::summaryOf(network, /*accurate*/ false);
//     stock::requestOrder(network, stock::RequestType::Player, {{iron, 64}}, "Factory");

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "mc/world/item/ItemStack.h"

#include "core/GlobalPos.h"
#include "core/stock/Logistics.h"

class BlockPos;
class BlockSource;
class Player;

namespace lv::feature::stock {

using Json = nlohmann::json;
using core::GlobalPos;
using core::stock::RequestType;

inline constexpr char kStockLink[]         = "levitate:stock_link";
inline constexpr char kStockTicker[]       = "levitate:stock_ticker";
inline constexpr char kFactoryGauge[]      = "levitate:factory_gauge";
inline constexpr char kRedstoneRequester[] = "levitate:redstone_requester";
inline constexpr char kShoppingList[]      = "levitate:shopping_list";
inline constexpr char kPackager[]          = "levitate:packager";  ///< U25
inline constexpr char kRepackager[]        = "levitate:repackager"; ///< U25

/// An item with a count beyond stack sizes (Create's BigItemStack). `count` >= kInfinite is
/// infinite (creative crates).
struct BigStack {
    ItemStack stack;
    int       count = 0;
};
using core::stock::kInfinite;

/// The identity of an item for summaries: item id, aux value and user data (count ignored).
[[nodiscard]] std::string itemKey(ItemStack const& stack);

// ---- packagers (U25 seam) ------------------------------------------------------------------------

/// One packaging request (Create's PackagingRequest): pack `count` of `item` into packages
/// addressed to `address`. Requests of one order share `orderId`; `linkIndex` / `finalLink` let
/// the receiver re-assemble orders split across packagers; `orderContext` is set on the first
/// request of an order only ({"stacks": [{"item", "count"}], "crafts": [...]}, Create's
/// PackageOrderWithCrafts).
struct PackagingRequest {
    ItemStack   item;
    int         count = 0;
    std::string address;
    int         orderId   = 0;
    int         linkIndex = 0;
    bool        finalLink = false;
    Json        orderContext;
};

/// How the stock network talks to packagers. Every function gets the packager's position.
struct PackagerProvider {
    /// A packager stock links can use (Create: a packager, not a repackager).
    std::function<bool(BlockSource&, BlockPos const&)> isPackager;
    /// What the packager could take out of its target inventory (nullopt: no inventory).
    std::function<std::optional<std::vector<BigStack>>(BlockSource&, BlockPos const&)> availableItems;
    /// Identity of the target inventory ("" = none): links of packagers on the same inventory
    /// are counted once (Create's InventoryIdentifier).
    std::function<std::string(BlockSource&, BlockPos const&)> inventoryId;
    /// Packages queued to leave the packager (Create's queuedExitingPackages).
    std::function<int(BlockSource&, BlockPos const&)> queuedPackages;
    /// Queue requests (they are boxed and sent in order).
    std::function<void(BlockSource&, BlockPos const&, std::vector<PackagingRequest> const&)> send;
    /// Optional: Create's isTooBusyFor (used instead of `queuedPackages` when set).
    std::function<bool(BlockSource&, BlockPos const&, RequestType)> tooBusy;
};

/// Plugs packagers in (a later registration replaces an earlier one; U25 calls it in install()).
void registerPackagerProvider(PackagerProvider provider);
/// Removes the provider (selftests restore the previous one with `currentPackagerProvider`).
[[nodiscard]] std::optional<PackagerProvider> currentPackagerProvider();
void                                          setPackagerProvider(std::optional<PackagerProvider> provider);

/// The stock link attached to a packager (Create's getLinkPos: a link on any side whose support
/// is the packager). Packagers with a link don't react to redstone (Create's LINKED state).
[[nodiscard]] std::optional<GlobalPos> linkOfPackager(GlobalPos const& packager);

// ---- networks ------------------------------------------------------------------------------------

/// The network of a stock link, ticker, requester or gauge (its first panel) at `pos` ("" none).
[[nodiscard]] std::string networkAt(GlobalPos const& pos);

/// The stock of a network: every loaded, unpowered (power < 15) stock link's packager inventory,
/// each inventory once. Cached for 20 ticks (`accurate`: 1 tick), as Create's LogisticsManager.
struct Stock {
    core::stock::Summary                       summary;
    std::unordered_map<std::string, ItemStack> stacks; ///< key -> one item of that kind
    [[nodiscard]] std::vector<BigStack>        byCount() const;
    [[nodiscard]] int                          countOf(ItemStack const& stack) const { return summary.countOf(itemKey(stack)); }
};
[[nodiscard]] Stock const& summaryOf(std::string const& network, bool accurate);

/// Sends an order through the network's packagers (Create's broadcastPackageRequest). Returns
/// false when the order is empty or a packager has too many packages queued for `type`.
/// `ignoredInventory`: links whose packager targets that inventory don't take part.
bool requestOrder(
    std::string const&           network,
    RequestType                  type,
    std::vector<BigStack> const& order,
    std::string const&           address,
    std::string const&           ignoredInventory = {},
    Json const&                  crafts           = Json::array()
);

/// Locking (Create's GlobalLogisticsManager): the owner (the player who placed the first link)
/// may lock a network; then only the owner interacts with its blocks.
[[nodiscard]] bool mayInteract(std::string const& network, Player const& player);
[[nodiscard]] bool mayAdministrate(std::string const& network, Player const& player);
[[nodiscard]] bool isLocked(std::string const& network);

// ---- factory gauge panels (for redstone / display links: Create's FactoryPanelSupportBehaviour) --

struct PanelState {
    std::string network;
    ItemStack   item;              ///< the monitored item (empty: none set)
    int         inStorage = 0;     ///< level in the network (restocker: in the packager's inventory)
    int         promised  = 0;
    int         target    = 0;     ///< target amount (items or stacks, see upTo)
    bool        upTo      = true;  ///< target counts items (true) or stacks
    bool        satisfied = false;
    bool        promisedSatisfied = false;
    bool        waiting   = false; ///< some links are not loaded
    bool        paused    = false; ///< paused by redstone
    std::string address;
};
/// A gauge panel (slot 0 top left, 1 top right, 2 bottom left, 3 bottom right), nullopt if none.
[[nodiscard]] std::optional<PanelState> panelAt(GlobalPos const& gauge, int slot);
/// A redstone link powering a panel pauses its requests (Create's redstonePowered).
void setPanelPaused(GlobalPos const& gauge, int slot, bool paused);

} // namespace lv::feature::stock
