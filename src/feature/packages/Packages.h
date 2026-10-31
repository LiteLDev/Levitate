#pragma once

// Package logistics contract (U25, SPEC §3.6.25). Other units (U26 stock network, U27 trains,
// U12 funnels, U24 display links, ...) use this header to read and build packages, match
// addresses, drive packagers and talk to postboxes. Server thread only.
//
//     auto box = packages::containing({diamonds, gold});      // random Create box style
//     packages::setAddress(box, "Storage");
//     if (packages::matchAddress(packages::addressOf(box), "Stor*")) ...
//     for (auto const& port : packages::postboxesForStation(stationPos)) { ... }   // U27
//
// Packages are the Create box items (`levitate:cardboard_package_<w>x<h>`, the ten rare
// styles); their address, 9 content slots and order data live in the item's user data
// (`lv:package`). Everything here mirrors Create 6.0.10's PackageItem / PackagerBlockEntity /
// PackagePortBlockEntity behaviour.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/packages/Address.h"

#include "mc/world/item/ItemStack.h"

class BlockSource;
class BlockPos;

namespace lv::feature::packages {

using Json = nlohmann::json;
using core::GlobalPos;

// ---- ids -------------------------------------------------------------------------------------------
inline constexpr char const* kPackagerId      = "levitate:packager";
inline constexpr char const* kRepackagerId    = "levitate:repackager";
inline constexpr char const* kFrogportId      = "levitate:package_frogport";
inline constexpr char const* kChainConveyorId = "levitate:chain_conveyor";
inline constexpr char const* kPackageFilterId = "levitate:package_filter";
/// The train station a postbox can target (U27's block, referenced by id).
inline constexpr char const* kStationId = "levitate:track_station";
/// Slots of a package (Create's PackageItem.SLOTS) and of a frogport / postbox.
inline constexpr int kPackageSlots = 9;
inline constexpr int kPortSlots    = 18;

/// Every package item id (4 cardboard sizes, then the 10 rare styles).
[[nodiscard]] std::vector<std::string> const& packageIds();
/// The 16 postbox ids (`levitate:<colour>_postbox`).
[[nodiscard]] std::vector<std::string> const& postboxIds();
[[nodiscard]] bool isPostboxId(std::string_view id);

// ---- addresses ---------------------------------------------------------------------------------------
/// Create's PackageItem.matchAddress (see core/packages/Address.h for the glob rules).
[[nodiscard]] inline bool matchAddress(std::string_view boxAddress, std::string_view address) {
    return core::packages::matchAddress(boxAddress, address);
}

// ---- package items -----------------------------------------------------------------------------------
[[nodiscard]] bool isPackage(ItemStack const& stack);
/// A package's address ("" when none). Also reads a package filter's address.
[[nodiscard]] std::string addressOf(ItemStack const& stack);
void                      setAddress(ItemStack& stack, std::string const& address);
void                      clearAddress(ItemStack& stack);
/// The 9 content slots (empty stacks for empty slots).
[[nodiscard]] std::vector<ItemStack> contentsOf(ItemStack const& box);
/// Create's PackageItem.containing: a random box style (1 in 7500 rare) holding `stacks`
/// (inserted stacked into 9 slots; what does not fit is lost, callers pass at most 9 slots).
[[nodiscard]] ItemStack containing(std::vector<ItemStack> const& stacks);
/// The first box style (`levitate:cardboard_package_12x12`), empty.
[[nodiscard]] ItemStack defaultBox();
/// Whether an item may go into a package (Create's canFitInsideContainerItems: not a package,
/// a shulker box, a bundle, or a minecart contraption unless `minecartContraptionInContainers`).
[[nodiscard]] bool fitsInPackage(ItemStack const& stack);

/// Create's PackageOrderData (fragmentation of an order into packages).
struct OrderData {
    int  orderId       = -1;
    int  linkIndex     = 0;
    bool isFinalLink   = false;
    int  fragmentIndex = 0;
    bool isFinal       = false;
    /// Create's PackageOrderWithCrafts (U26's JSON: {"stacks":[{"item":<item json>,"count":n}],
    /// "crafts":[{"pattern":[{"item":..,"count":n}],"count":n}]}), null when absent.
    Json context;
};
[[nodiscard]] std::optional<OrderData> orderOf(ItemStack const& box);
void                                   setOrder(ItemStack& box, OrderData const& order);
/// The order context: the order data's, else the package's own (redstone requests), else null.
[[nodiscard]] Json orderContextOf(ItemStack const& box);
void               setOrderContext(ItemStack& box, Json const& context);

/// Rewrites a package's lore (address in gold, the first 3 content lines, "and N more...").
void updateLore(ItemStack& box);

// ---- package filter ------------------------------------------------------------------------------------
[[nodiscard]] bool isPackageFilter(ItemStack const& stack);
/// Create's PackageFilterItemStack.test: an empty address behaves like a plain filter item (only
/// the filter itself matches); otherwise packages whose address matches pass.
[[nodiscard]] bool filterMatches(ItemStack const& filter, ItemStack const& stack);

// ---- packagers (U26 stock links / requesters) ----------------------------------------------------------
/// Create's PackagingRequest: `count` of `item` to box for `address`, as part of an order.
struct PackagingRequest {
    ItemStack   item;  ///< prototype (count ignored)
    int         count = 0;
    std::string address;
    int         orderId = 0;
    int         linkIndex = 0;
    bool        finalLink = false;
    int         packageCounter = 0; ///< shared by the requests of one link (Create's MutableInt)
    Json        context;
};
enum class RequestType { Player, Redstone, Restock };
/// Whether a packager / repackager is at `pos`.
[[nodiscard]] bool isPackager(GlobalPos const& pos);
/// The items in the packager's target inventory (merged stacks, Create's InventorySummary).
[[nodiscard]] std::vector<ItemStack> availableItems(GlobalPos const& pos);
/// Create's isTooBusyFor: queued packages >= 50 (player), 20 (redstone), 10 (restock).
[[nodiscard]] bool isTooBusyFor(GlobalPos const& pos, RequestType type);
/// Create's attemptToSend(queuedRequests): boxes the next request(s) (one package per call,
/// removes fulfilled requests from the front of `queue`).
void attemptToSend(GlobalPos const& pos, std::vector<PackagingRequest>& queue);
/// U26: tells whether the packager at a position is linked to a stock link (linked packagers
/// ignore redstone). Default: never linked.
void setLinkProvider(std::function<bool(BlockSource&, BlockPos const&)> provider);

/// Create's UnpackingHandler: puts `items` (from a package with `orderContext`) into the block at
/// `target`, which the packager facing `facing` points at. All or nothing; `simulate` only checks.
using UnpackingHandler = std::function<
    bool(BlockSource& region, BlockPos const& target, int facing, std::vector<ItemStack>& items, Json const& orderContext, bool simulate)>;
/// Registers a handler for a block id (basins, mechanical crafters, creative crates, ...).
void registerUnpackingHandler(std::string const& blockId, UnpackingHandler handler);

// ---- frogports and postboxes (U27 trains) ----------------------------------------------------------------
struct PortInfo {
    GlobalPos   pos;
    std::string blockId;
    std::string address;            ///< address filter typed into the port
    bool        acceptsPackages = true;
    std::optional<GlobalPos> station; ///< postbox: the train station it targets
};
/// The port at `pos` (frogport or postbox).
[[nodiscard]] std::optional<PortInfo> portAt(GlobalPos const& pos);
/// Create's GlobalStation.connectedPorts: postboxes targeting the station at `station`.
[[nodiscard]] std::vector<PortInfo> postboxesForStation(GlobalPos const& station);
/// The port's 18 slots (copies).
[[nodiscard]] std::vector<ItemStack> portInventory(GlobalPos const& pos);
/// The packages looping around the chain conveyor at `pos` (Create's getLoopingPackages; the
/// display link's "Read Package Address" source). Empty for other blocks.
[[nodiscard]] std::vector<ItemStack> loopingPackages(GlobalPos const& pos);
/// Puts a package into the port's inventory (as a train unloading: no address check).
/// Returns what did not fit.
[[nodiscard]] ItemStack insertIntoPort(GlobalPos const& pos, ItemStack const& box, bool simulate);
/// Takes the first package from the port's inventory that passes `filter` (a train loading).
[[nodiscard]] ItemStack extractFromPort(GlobalPos const& pos, std::function<bool(ItemStack const&)> const& filter, bool simulate);
/// Postbox flag / particles after a train delivered (Create's spawnParticles).
void notifyDelivered(GlobalPos const& pos);

} // namespace lv::feature::packages
