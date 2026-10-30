#pragma once

// Internals shared by the stock unit's files (not a contract; other units use Stock.h).

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/stock/Logistics.h"
#include "feature/stock/Stock.h"

class Block;
class Player;
class Vec3;

namespace lv::feature::stock {

namespace cs = core::stock;

// ---- ids, states -----------------------------------------------------------------------------------

inline constexpr char kStateFace[]    = "levitate:face";    ///< 0 floor, 1 wall, 2 ceiling
inline constexpr char kStateFacing[]  = "levitate:facing";  ///< Java 2D facing 0 S, 1 W, 2 N, 3 E
inline constexpr char kStatePowered[] = "levitate:powered"; ///< 0 / 1
inline constexpr char kStateAxis[]    = "levitate:axis";    ///< requester: 0 x, 1 z
inline constexpr char kStatePanels[]  = "levitate:panels";  ///< gauge: bitmask of active panels
inline constexpr char kBlazeBurner[]  = "levitate:blaze_burner";
inline constexpr char kWrench[]       = "levitate:wrench";
inline constexpr char kDataKey[]      = "stock";            ///< BlockData top-level key
inline constexpr char kFreqData[]     = "stock_freq";       ///< item user data: the tuned network
inline constexpr char kRequestData[]  = "stock_request";    ///< item user data: an encoded order
inline constexpr char kListData[]     = "shopping_list";    ///< item user data: a shopping list

[[nodiscard]] bool isTableCloth(std::string_view id);
[[nodiscard]] bool isLinkedItem(std::string_view id); ///< Create's LogisticallyLinkedBlockItem

// ---- world helpers -----------------------------------------------------------------------------------

[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] Block const* blockAt(GlobalPos const& p);
[[nodiscard]] std::string  idAt(GlobalPos const& p);
[[nodiscard]] int          stateOr(GlobalPos const& p, char const* state, int fallback);
bool                       setStates(GlobalPos const& p, std::vector<std::pair<std::string_view, int>> const& states);
[[nodiscard]] GlobalPos    offset(GlobalPos const& p, int dx, int dy, int dz);
[[nodiscard]] Json         posJson(GlobalPos const& p);
[[nodiscard]] std::optional<GlobalPos> posFrom(Json const& j);
[[nodiscard]] std::uint64_t currentTick();
/// Redstone power reaching `p` from its neighbours (polled; Create's getSignal), skipping `except`
/// (an offset index 0..5 = down, up, north, south, west, east; -1 none).
[[nodiscard]] int receivedPower(GlobalPos const& p, int except = -1);

/// The unit's part of the BlockData document at `p` (created when `create`).
[[nodiscard]] Json const* dataOf(GlobalPos const& p);
[[nodiscard]] Json&       editData(GlobalPos const& p);

/// Face-attached blocks: the face, facing and support position.
[[nodiscard]] cs::AttachFace faceOf(GlobalPos const& p);
[[nodiscard]] int            facingOf(GlobalPos const& p);
[[nodiscard]] GlobalPos      supportOf(GlobalPos const& p);

// ---- players -------------------------------------------------------------------------------------

[[nodiscard]] std::string uuidOf(Player const& player);
[[nodiscard]] Player*     playerByUuid(std::string const& uuid);
/// Create's sendStatus / displayClientMessage(…, true): the action bar.
void status(Player& player, std::string const& text);
void statusKey(Player& player, char const* key, char const* fallback, char const* colour = "");
void playSound(GlobalPos const& p, char const* sound, float volume = 1.0f, float pitch = 1.0f);
void giveOrDrop(Player& player, ItemStack const& stack);
/// Spawns an item entity at the centre of a block.
void dropAt(GlobalPos const& p, ItemStack const& stack);
/// Within 8 blocks of the block centre, same dimension.
[[nodiscard]] bool inReach(Player& player, GlobalPos const& p, double range = 8.0);

/// Runs `fn` on the next server tick (world edits after an interaction).
void defer(std::function<void()> fn);

// ---- items -------------------------------------------------------------------------------------------

[[nodiscard]] std::string networkOfItem(ItemStack const& stack);
void                      tuneItem(ItemStack& stack, std::string const& network);
void                      clearTuning(ItemStack& stack);
/// Rewrites the lore of a tuned / programmed item (Create's tooltips).
void                      updateLore(ItemStack& stack);
[[nodiscard]] std::string newNetworkId();
[[nodiscard]] ItemStack   stackFromJson(Json const& j);
[[nodiscard]] Json        stackToJson(ItemStack const& s);

/// An order as JSON: [{"item": <stack json>, "count": n}].
[[nodiscard]] Json                  orderToJson(std::vector<BigStack> const& order);
[[nodiscard]] std::vector<BigStack> orderFromJson(Json const& j);

// ---- networks (Network.cpp) -----------------------------------------------------------------------

struct NetworkInfo {
    std::string                 owner;  ///< uuid ("" none)
    bool                        locked = false;
    std::set<GlobalPos>         links;  ///< every stock link ever added (Create's totalLinks)
    cs::PromiseQueue            promises;
};

[[nodiscard]] NetworkInfo*            networkInfo(std::string const& id);
NetworkInfo&                          ensureNetwork(std::string const& id);
void                                  linkAdded(std::string const& network, GlobalPos const& pos, std::string const& placedBy);
void                                  linkRemoved(std::string const& network, GlobalPos const& pos);
void                                  setLocked(std::string const& network, bool locked);
void                                  markNetworksDirty();
/// Links of `network` that are loaded and placed (total minus these = Create's unloaded links).
[[nodiscard]] int                     unloadedLinkCount(std::string const& network);
[[nodiscard]] bool                    isLockable(std::string const& network);
void                                  invalidateSummaries();
/// Create's getStockOf: the count of `stack` across the network, without `ignoredInventory`.
[[nodiscard]] int stockOfItem(std::string const& network, ItemStack const& stack, std::string const& ignoredInventory);
/// The packager the link at `link` sits on (nullopt: none or the link is powered with 15).
[[nodiscard]] std::optional<GlobalPos> packagerOfLink(GlobalPos const& link);
/// What a packager can take out of its inventory (also feeds promises with new arrivals, as
/// Create's PackagerBlockEntity.submitNewArrivals).
[[nodiscard]] std::optional<Stock> packagerStock(GlobalPos const& packager);
[[nodiscard]] std::string          packagerInventory(GlobalPos const& packager);
/// A packager (per the provider) stands at `p`.
[[nodiscard]] bool                 packagerAt(GlobalPos const& p);
/// Drops per-world runtime state (gauge timers, pending connections, searches).
void resetGaugeRuntime();
void resetTickerRuntime();
[[nodiscard]] bool                 packagerTooBusy(GlobalPos const& packager, RequestType type);
void                               loadNetworks();
void                               saveNetworks();
void                               resetNetworks();
void                               tickNetworks();
/// A wireless pulse at a link (Create's WiFiEffectPacket: sound + particles).
void linkEffect(GlobalPos const& link);

// ---- blocks (Blocks.cpp) --------------------------------------------------------------------------

/// Positions of the unit's blocks, kept so they can be ticked / polled (custom blocks don't tick).
struct Registry {
    std::set<GlobalPos> links, tickers, requesters, gauges, shops;
};
[[nodiscard]] Registry& registry();
void                    saveRegistry();

/// Called after one of the unit's blocks was placed by a player (or a selftest).
void onLinkPlaced(GlobalPos const& p, Player* player, ItemStack const& item);
void onTickerPlaced(GlobalPos const& p, Player* player, ItemStack const& item);
void onRequesterPlaced(GlobalPos const& p, Player* player, ItemStack const& item);

// ---- stock ticker (Ticker.cpp) ------------------------------------------------------------------

/// Create's isKeeperPresent: a seated mob or a blaze burner beside the ticker.
[[nodiscard]] bool keeperPresent(GlobalPos const& ticker);
/// The ticker a keeper (a seated mob / a blaze burner at `keeperPos`) serves (exactly one).
[[nodiscard]] std::optional<GlobalPos> tickerOfKeeper(GlobalPos const& keeperPos, bool burner);
/// Right-click on the ticker block (Create's StockTickerBlock.useItemOn).
void useTicker(Player& player, GlobalPos const& ticker);
/// Right-click on the keeper (Create's interactWithLogisticsManagerAt).
void useKeeper(Player& player, GlobalPos const& ticker);
/// Create's PackageOrderRequestPacket: orders (or programs a held requester / table cloth).
bool placeOrder(Player& player, GlobalPos const& ticker, std::vector<BigStack> const& order, std::string const& address, bool encode);
void tickerRemoved(GlobalPos const& p, Json const& doc);
void registerTickerScreens();
[[nodiscard]] std::vector<ItemStack> payments(GlobalPos const& ticker);
/// Puts a payment into the ticker's 27 slots; returns what did not fit.
ItemStack          insertPayment(GlobalPos const& ticker, ItemStack stack);
[[nodiscard]] int  freePaymentSlots(GlobalPos const& ticker);

// ---- redstone requester (Requester.cpp) ---------------------------------------------------------

void useRequester(Player& player, GlobalPos const& p);
void pollRequester(GlobalPos const& p);
/// Create's triggerRequest. Returns whether an order was sent.
bool triggerRequest(GlobalPos const& p);
void registerRequesterScreens();

// ---- factory gauge (Gauge.cpp) --------------------------------------------------------------------

/// Places a panel (first panel: the block is placed by the caller). Returns false when refused.
bool placeGaugePanel(GlobalPos const& p, int slot, std::string const& network, Player* player);
/// Right-click on a gauge panel.
void useGauge(Player& player, GlobalPos const& p, int slot, bool sneaking);
/// Breaking one panel (the block stays while other panels remain).
void removeGaugePanel(GlobalPos const& p, int slot, Player* player);
void gaugeRemoved(GlobalPos const& p, Json const& doc);
void tickGauges();

/// (U42) Create's "Use Mechanical Crafting": the crafting-table recipe making `output` from
/// exactly `inputs`, laid out in its 3x3 grid (`pattern[slot]` = index into `inputs`, -1 empty).
struct CraftingArrangement {
    std::string        recipeId;
    std::array<int, 9> pattern{};
    int                resultCount = 1;
};
[[nodiscard]] std::optional<CraftingArrangement> craftingArrangement(ItemStack const& output, std::vector<ItemStack> const& inputs);
void registerGaugeScreens();
/// Items that newly arrived in a packager's inventory: fulfils the promises of restocker gauges
/// on that packager.
void restockerArrivals(GlobalPos const& packager, std::vector<cs::Entry> const& arrivals);
/// Starts / ends a connection from the player's panel (screen "Add new connection").
void startConnection(Player& player, GlobalPos const& p, int slot);
[[nodiscard]] bool connectPanels(Player* player, GlobalPos const& target, int targetSlot, GlobalPos const& input, int inputSlot);
[[nodiscard]] int  targetedSlotAt(GlobalPos const& p, double lx, double ly, double lz);
/// Gauge state for selftests / hover: {"item", "amount", "upTo", "inStorage", "promised", ...}.
[[nodiscard]] Json gaugeInfo(GlobalPos const& p, int slot);

// ---- table cloth shops (Shop.cpp) ---------------------------------------------------------------

/// A table cloth item carrying an encoded order was placed: it becomes a shop.
void onClothPlaced(GlobalPos const& p, Player& player, ItemStack const& item);
/// Right-click on a shop cloth. Returns false when the cloth is not a shop (deco behaviour).
bool useShop(Player& player, GlobalPos const& p, double lx, double ly, double lz);
/// Checkout at the keeper with a shopping list in hand (Create's interactWithShop).
void checkout(Player& player, GlobalPos const& ticker, ItemStack const& list);
[[nodiscard]] bool isShop(GlobalPos const& p);
/// Create's getStockLevelForTrade.
[[nodiscard]] int stockLevelForTrade(GlobalPos const& cloth, Json const* otherPurchases);
/// The shopping list's items and price: {order: [...], payment: [...]} (Create's bakeEntries).
struct Baked {
    Stock order, payment;
};
[[nodiscard]] Baked bakeList(Json const& list, std::optional<GlobalPos> ignore);
void                shopRemoved(GlobalPos const& p, Json const& doc);
void                registerShopValueBoxes();

} // namespace lv::feature::stock
