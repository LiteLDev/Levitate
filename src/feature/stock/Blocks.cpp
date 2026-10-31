// Stock unit: the blocks' placement and orientation, player interaction (GameMode hooks: they
// see network players and SimulatedPlayers alike), redstone polling, block removal and the
// unit's module (tick, world lifecycle).

#include <algorithm>
#include <array>
#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/actors/Actors.h"
#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/ValueBox.h"
#include "feature/common/Items.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::stock {

namespace items = common::items;
namespace ia    = common::interactions;

namespace {
constexpr char kDbBlocks[] = "lv:stock:blocks";

/// Bedrock FacingID (0 down, 1 up, 2 north, 3 south, 4 west, 5 east) to Java 2D facing.
int facing2dOf(int face) {
    switch (face) {
    case 2: return 2;
    case 3: return 0;
    case 4: return 1;
    case 5: return 3;
    default: return 2;
    }
}

bool ourBlock(std::string_view id) { return isLinkedItem(id); }

void persistRegistry() {
    if (!game::db::available()) return;
    auto  list = [](std::set<GlobalPos> const& s) {
        Json a = Json::array();
        for (auto const& p : s) a.push_back(posJson(p));
        return a;
    };
    auto& r = registry();
    game::db::save(kDbBlocks, {{"links", list(r.links)}, {"tickers", list(r.tickers)}, {"requesters", list(r.requesters)}, {"gauges", list(r.gauges)}, {"shops", list(r.shops)}});
}

void loadRegistry() {
    registry() = {};
    auto j     = game::db::load(kDbBlocks);
    if (!j || !j->is_object()) return;
    auto read = [&](char const* key, std::set<GlobalPos>& out) {
        for (auto const& p : j->value(key, Json::array()))
            if (auto gp = posFrom(p)) out.insert(*gp);
    };
    auto& r = registry();
    read("links", r.links);
    read("tickers", r.tickers);
    read("requesters", r.requesters);
    read("gauges", r.gauges);
    read("shops", r.shops);
}

/// Create's LogisticallyLinkedBlockItem.useOn on a block with a network: tune the held item.
bool tuneHeld(Player& player, std::string const& network) {
    if (network.empty()) return false;
    if (!mayInteract(network, player)) {
        statusKey(player, "create.logistically_linked.protected", "Logistics Network is Protected", "§c");
        return true;
    }
    ItemStack held = player.getSelectedItem();
    tuneItem(held, network);
    player.setSelectedItem(held);
    player.sendInventory(false);
    statusKey(player, "create.logistically_linked.tuned", "Tuned to this network");
    log().info("stock: {} tuned {} to network {}", player.getRealName(), items::idOf(held), network.substr(0, 8));
    return true;
}

/// The network message after placing a linked block (Create's useOn).
void placedMessage(Player* player, bool tuned) {
    if (!player) return;
    if (tuned) statusKey(*player, "create.logistically_linked.connected", "Connected to existing network successfully");
    else statusKey(*player, "create.logistically_linked.new_network_started", "New link network started");
}

/// Orients a face-attached block (stock link, factory gauge) from the clicked face.
void orientAttached(GlobalPos const& p, int face, Player const& player) {
    int attach = face == 1 ? 0 : face == 0 ? 2 : 1;
    int facing = attach == 1 ? facing2dOf(face) : game::playerFacing(player);
    setStates(p, {{kStateFace, attach}, {kStateFacing, facing}});
}

std::uint64_t gLastUseTick = 0;
std::int64_t  gLastUser    = 0;
GlobalPos     gLastUsePos;
std::uint64_t gLastPlaceTick = 0;
std::int64_t  gLastPlacer    = 0;

void afterPlaced(GlobalPos const& p, std::string const& id, ItemStack const& item, Player& player, int face);

/// Create's TableClothBlock placement helper (IPlacementHelper.orderedByDistanceExceptAxis on Y):
/// the held cloth goes into the free horizontal neighbour nearest to the clicked point, as if it
/// were placed there (a shop cloth keeps its order). False: no free neighbour (the click goes on).
bool extendCloth(Player& player, ItemStack const& held, GlobalPos const& p, cs::Vec3d const& hit) {
    if (!player.canUseAbility(AbilitiesIndex::Build)) return false;
    auto* region = regionOf(p.dim);
    if (!region) return false;
    static constexpr int dirs[4][2]{{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    std::array<int, 4>   order{0, 1, 2, 3};
    auto                 dist = [&](int d) {
        double dx = .5 + dirs[d][0] - hit.x, dz = .5 + dirs[d][1] - hit.z;
        return dx * dx + dz * dz;
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return dist(a) < dist(b); });
    for (int d : order) {
        GlobalPos target{p.dim, p.x + dirs[d][0], p.y, p.z + dirs[d][1]};
        if (!game::replaceable(*region, game::blockPos(target))) continue;
        auto id = items::idOf(held);
        auto const* block = game::blockWith(id);
        if (!block || !game::setBlock(*region, game::blockPos(target), *block)) return false;
        ItemStack copy = held;
        afterPlaced(target, id, copy, player, 1);
        if (!player.isCreative()) {
            player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
            player.sendInventory(false);
        }
        playSound(target, "use.cloth", 1.f, .8f);
        log().info("stock: {} extended the table cloth at {} {} {} to {} {} {}", player.getRealName(), p.x, p.y, p.z, target.x, target.y, target.z);
        return true;
    }
    return false;
}

/// Handles a right-click on one of the unit's blocks (or a keeper / shop). True = consumed.
bool useOnBlock(Player& player, ItemStack const& held, GlobalPos const& p, int face, cs::Vec3d const& hit, std::string const& tid, bool sneaking) {
    std::string hid      = items::isEmpty(held) ? std::string{} : items::idOf(held);

    if (tid == kStockLink || tid == kStockTicker || tid == kRedstoneRequester) {
        if (isLinkedItem(hid) && !sneaking) return tuneHeld(player, networkAt(p));
        if (tid == kStockLink) return false;
        if (sneaking && !hid.empty()) return false; // sneak with an item: place against it
        if (tid == kStockTicker) {
            if (hid == kShoppingList) {
                useKeeper(player, p);
                return true;
            }
            useTicker(player, p);
            return true;
        }
        if (sneaking) return false;
        useRequester(player, p);
        return true;
    }
    if (tid == kFactoryGauge) {
        int  slot   = targetedSlotAt(p, hit.x, hit.y, hit.z);
        auto info   = gaugeInfo(p, slot);
        bool active = info.value("active", false);
        if (!active) {
            if (hid != kFactoryGauge) return false;
            auto network = networkOfItem(held);
            if (network.empty()) {
                playSound(p, "levitate.stock.deny", 0.5f, 1.0f);
                statusKey(player, "create.factory_panel.tune_before_placing", "Tune to a stock link before placing");
                return true;
            }
            if (placeGaugePanel(p, slot, network, &player)) {
                statusKey(player, "create.logistically_linked.connected", "Connected to existing network successfully");
                if (!player.isCreative()) {
                    ItemStack h = held;
                    player.setSelectedItem(items::withCount(h, items::countOf(h) - 1));
                    player.sendInventory(false);
                }
            }
            return true;
        }
        if (hid == kWrench && sneaking) {
            // Create's FactoryPanelBlock.onSneakWrenched: the targeted panel comes off and its gauge
            // goes back into the inventory (the block goes with the last panel)
            if (!mayInteract(info.value("freq", std::string{}), player)) {
                statusKey(player, "create.logistically_linked.protected", "Logistics Network is Protected", "§c");
                return true;
            }
            goggles::wrench::playRemoveSound(p);
            removeGaugePanel(p, slot, &player);
            log().info("stock: {} wrenched off the gauge panel at {} {} {} slot {}", player.getRealName(), p.x, p.y, p.z, slot);
            return true;
        }
        useGauge(player, p, slot, sneaking);
        return true;
    }
    if (isTableCloth(tid)) {
        // Create's TableClothBlock.useItemOn
        if (face == 0 || !player.canUseAbility(AbilitiesIndex::Build)) return false;
        // the placement helper: a table cloth in hand extends the cloth sideways
        if (isTableCloth(hid) && !sneaking) return extendCloth(player, held, p, hit);
        bool shop = isShop(p);
        // sneaking or an empty hand on a cloth without items: the default interaction (placing)
        if ((sneaking || hid.empty()) && !shop && !hasClothItems(p)) return false;
        if (shop) return useShop(player, p, hit.x, hit.y, hit.z);
        return useClothItems(player, p);
    }
    if (tid == kBlazeBurner) {
        // Create's BlazeBurnerBlock.useItemOn: a burner beside a stock ticker is its keeper
        if (hid == "levitate:goggles" || stateOr(p, "levitate:blaze", 0) == 0) return false;
        auto ticker = tickerOfKeeper(p, true);
        if (!ticker) return false;
        if (hid == kShoppingList) checkout(player, *ticker, held);
        else useKeeper(player, *ticker);
        return true;
    }
    return false;
}

/// After a placement of one of the unit's items succeeded.
void afterPlaced(GlobalPos const& p, std::string const& id, ItemStack const& item, Player& player, int face) {
    if (id == kStockLink) {
        orientAttached(p, face, player);
        onLinkPlaced(p, &player, item);
    } else if (id == kStockTicker) {
        int f = game::opposite(game::playerFacing(player));
        if (player.isSneaking()) f = game::opposite(f);
        setStates(p, {{kStateFacing, f}});
        onTickerPlaced(p, &player, item);
    } else if (id == kRedstoneRequester) {
        int f = game::playerFacing(player);
        setStates(p, {{kStateAxis, (f == 0 || f == 2) ? 1 : 0}});
        onRequesterPlaced(p, &player, item);
    } else if (id == kFactoryGauge) {
        orientAttached(p, face, player);
        // the first panel goes where the player aimed on the new block's plane: the centre slot
        // nearest to the player's look is not known, Create picks the targeted one (bottom left
        // by default for a fresh block)
        placeGaugePanel(p, static_cast<int>(cs::PanelSlot::BottomLeft), networkOfItem(item), &player);
        placedMessage(&player, true);
    } else if (isTableCloth(id)) {
        if (items::getData(item, kRequestData).is_object()) onClothPlaced(p, player, item);
    }
}
} // namespace

Registry& registry() {
    static Registry r;
    return r;
}
void saveRegistry() { persistRegistry(); }

void onLinkPlaced(GlobalPos const& p, Player* player, ItemStack const& item) {
    auto network = networkOfItem(item);
    bool tuned   = !network.empty();
    if (!tuned) network = newNetworkId();
    auto  owner = player ? uuidOf(*player) : std::string{};
    auto& d     = editData(p);
    d["freq"]   = network;
    d["placedBy"] = owner;
    d["power"]  = receivedPower(p, -1);
    registry().links.insert(p);
    persistRegistry();
    linkAdded(network, p, owner);
    setStates(p, {{kStatePowered, d.value("power", 0) > 0 ? 1 : 0}});
    placedMessage(player, tuned);
    log().info("stock: stock link placed at {} {} {} on network {}", p.x, p.y, p.z, network.substr(0, 8));
}

void onTickerPlaced(GlobalPos const& p, Player* player, ItemStack const& item) {
    auto network = networkOfItem(item);
    bool tuned   = !network.empty();
    if (!tuned) network = newNetworkId();
    auto& d = editData(p);
    d["freq"] = network;
    registry().tickers.insert(p);
    persistRegistry();
    placedMessage(player, tuned);
    log().info("stock: stock ticker placed at {} {} {} on network {}", p.x, p.y, p.z, network.substr(0, 8));
}

void onRequesterPlaced(GlobalPos const& p, Player* player, ItemStack const& item) {
    auto network = networkOfItem(item);
    bool tuned   = !network.empty();
    if (!tuned) network = newNetworkId();
    auto& d       = editData(p);
    d["freq"]     = network;
    d["powered"]  = receivedPower(p) > 0;
    d["partial"]  = false;
    d["success"]  = false;
    d["order"]    = Json::array();
    d["address"]  = "";
    placedMessage(player, tuned);
    // Create's AutoRequestData.readFromItem: the order programmed at a stock ticker
    auto req = items::getData(item, kRequestData);
    if (req.is_object()) {
        auto target = posFrom(req.value("ticker", Json()));
        bool valid  = target && target->dim == p.dim
                  && std::hypot(double(target->x - p.x), double(target->y - p.y), double(target->z - p.z)) < 128;
        d["order"]   = req.value("order", Json::array());
        d["address"] = req.value("address", std::string{});
        if (player) {
            if (valid) statusKey(*player, "create.redstone_requester.keeper_connected", "Connected to Stock keeper successfully", "§f");
            else statusKey(*player, "create.redstone_requester.keeper_too_far_away", "Requester too far away from Stock keeper", "§c");
        }
    }
    setStates(p, {{kStatePowered, d.value("powered", false) ? 1 : 0}});
    registry().requesters.insert(p);
    persistRegistry();
    log().info("stock: redstone requester placed at {} {} {}", p.x, p.y, p.z);
}

// ---- hooks ---------------------------------------------------------------------------------------

// U36: all of the unit's clicks go through the shared interaction dispatch.

namespace {

/// The held stack of a use-on that may place one of the unit's blocks (its tuning / order is
/// read once the block exists).
struct PendingPlace {
    std::int64_t  player = 0;
    std::uint64_t tick   = 0;
    ItemStack     item;
};
PendingPlace gPendingPlace;

/// Right-clicks on the unit's blocks, and the checks before placing a linked block.
ia::Result stockUseOn(ia::UseOnContext const& c) {
    Player&            player  = c.player;
    std::string const& tid     = c.blockId;
    std::string const& hid     = c.itemId;
    GlobalPos          clicked = c.pos;
    bool candidate = ourBlock(tid) || tid == kBlazeBurner || isTableCloth(tid);
    if (candidate && !player.isSpectator()) {
        // Bedrock repeats the use while the button is held (and clients may send it twice):
        // one action per click and block
        auto uid = player.getOrCreateUniqueID().rawID;
        if (uid == gLastUser && clicked == gLastUsePos && currentTick() - gLastUseTick < 5) return ia::Result::Consume;
        try {
            if (useOnBlock(player, c.item, clicked, c.face, cs::Vec3d{c.hit[0], c.hit[1], c.hit[2]}, tid, c.sneaking)) {
                gLastUser    = uid;
                gLastUseTick = currentTick();
                gLastUsePos  = clicked;
                player.sendInventory(false);
                return ia::Result::Consume;
            }
        } catch (std::exception const& e) {
            log().error("stock: interaction failed: {}", e.what());
        }
    }
    bool ourItem = isLinkedItem(hid) || (isTableCloth(hid) && common::items::getData(c.item, kRequestData).is_object());
    if (!ourItem) return ia::Result::Pass;
    if (hid == kFactoryGauge && networkOfItem(c.item).empty()) {
        // Create's FactoryPanelBlockItem.place: an untuned gauge can't be placed
        playSound(clicked, "levitate.stock.deny", 0.5f, 1.0f);
        statusKey(player, "create.factory_panel.tune_before_placing", "Tune to a stock link before placing");
        player.sendInventory(false);
        return ia::Result::Deny;
    }
    gPendingPlace = {player.getOrCreateUniqueID().rawID, currentTick(), c.item};
    return ia::Result::Pass;
}

/// A linked block / shop cloth placed by vanilla: tune it from the stack it was placed with.
void stockPlaced(ia::PlaceContext const& c) {
    if (c.blockId != c.itemId) return;
    auto uid = c.player.getOrCreateUniqueID().rawID;
    if (gPendingPlace.player != uid || gPendingPlace.tick != currentTick() || items::idOf(gPendingPlace.item) != c.itemId)
        return;
    ItemStack copy = gPendingPlace.item;
    gPendingPlace  = {};
    gLastPlacer    = uid;
    gLastPlaceTick = currentTick();
    try {
        afterPlaced(c.pos, c.blockId, copy, c.player, c.face);
    } catch (std::exception const& e) {
        log().error("stock: placement failed: {}", e.what());
    }
}

/// Linked items in the air clear their tuning; sneak-using a shopping list discards it.
ia::Result stockUse(ia::UseContext const& c) {
    Player&            player = c.player;
    std::string const& hid    = c.itemId;
    if (hid.empty()) return ia::Result::Pass;
    // the use of a click that already placed / tuned on a block is not a click in the air
    auto uid        = player.getOrCreateUniqueID().rawID;
    bool justPlaced = (gLastPlacer == uid && currentTick() - gLastPlaceTick < 2) || (gLastUser == uid && currentTick() - gLastUseTick < 2);
    if (isLinkedItem(hid) && !networkOfItem(c.item).empty() && !justPlaced) {
        // Create's LogisticallyLinkedBlockItem.use: right-click in the air clears the tuning
        ItemStack item = c.item;
        clearTuning(item);
        player.setSelectedItem(item);
        player.sendInventory(false);
        statusKey(player, "create.logistically_linked.cleared", "Cleared connection to network");
        playSound(game::globalPos(player.getDimensionBlockSource(), BlockPos{player.getPosition()}), "levitate.stock.clear", 0.75f, 1.0f);
        log().info("stock: {} cleared the tuning of {}", player.getRealName(), hid);
        return ia::Result::Consume;
    }
    if (hid == kShoppingList && c.sneaking) {
        // Create's ShoppingListItem.use: sneak-use discards the list
        player.setSelectedItem(ItemStack::EMPTY_ITEM());
        player.sendInventory(false);
        statusKey(player, "create.table_cloth.shopping_list_discarded", "Shopping list discarded");
        log().info("stock: {} discarded a shopping list", player.getRealName());
        return ia::Result::Consume;
    }
    return ia::Result::Pass;
}

/// Create's StockTickerInteractionHandler: a seated mob next to exactly one stock ticker.
ia::Result stockInteract(ia::EntityContext const& c) {
    Player& player = c.player;
    Actor&  entity = c.entity;
    // Create's getStockTickerPosition: a living entity whose (root) vehicle is a seat
    auto const* seat = entity.getVehicle();
    if (player.isSpectator() || entity.isPlayer() || !seat || !actors::isSeatEntityId(seat->getTypeName()) || entity.getTypeName() == "levitate:package") return ia::Result::Pass;
    auto const& pos = entity.getPosition();
    GlobalPos   at{static_cast<int>(entity.getDimensionId()), static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y + 0.01)), static_cast<int>(std::floor(pos.z))};
    auto        ticker = tickerOfKeeper(at, false);
    if (!ticker) return ia::Result::Pass;
    try {
        auto const& held = c.item;
        if (!items::isEmpty(held) && items::idOf(held) == kShoppingList) checkout(player, *ticker, held);
        else useKeeper(player, *ticker);
    } catch (std::exception const& e) {
        log().error("stock: keeper interaction failed: {}", e.what());
    }
    return ia::Result::Consume;
}

/// Breaking a gauge with several panels removes one panel; a shop drops its encoded cloth.
ia::Result stockDestroy(ia::DestroyContext const& c) {
    Player&     player = c.player;
    auto&       region = player.getDimensionBlockSource();
    GlobalPos   p      = c.pos;
    BlockPos    pos{p.x, p.y, p.z};
    std::string id = c.blockId;
    if (id == kFactoryGauge) {
        // Create: breaking a gauge with several panels removes the targeted panel only
        int panels = stateOr(p, kStatePanels, 0);
        int count  = 0;
        for (int s = 0; s < 4; ++s) count += (panels >> s) & 1;
        if (count > 1) {
            auto eye  = player.getEyePos();
            auto look = player.getViewVector();
            auto mid  = cs::panelCentre(faceOf(p), facingOf(p), cs::PanelSlot::TopLeft);
            auto n    = cs::supportOffset(faceOf(p), facingOf(p));
            // intersect the view ray with the panel plane
            double    denom = look.x * n[0] + look.y * n[1] + look.z * n[2];
            cs::Vec3d hit{0.5, 0.5, 0.5};
            if (std::abs(denom) > 1e-6) {
                double px = p.x + mid.x, py = p.y + mid.y, pz = p.z + mid.z;
                double t  = ((px - eye.x) * n[0] + (py - eye.y) * n[1] + (pz - eye.z) * n[2]) / denom;
                hit       = {eye.x + look.x * t - p.x, eye.y + look.y * t - p.y, eye.z + look.z * t - p.z};
            }
            removeGaugePanel(p, targetedSlotAt(p, hit.x, hit.y, hit.z), &player);
            return ia::Result::Deny;
        }
    }
    if (isTableCloth(id) && isShop(p)) {
        // Create's TableClothBlock.getDrops: a shop drops its cloth with the encoded order
        if (player.isCreative()) return ia::Result::Pass;
        auto const* d     = dataOf(p);
        Json        shop  = d ? d->value("shop", Json::object()) : Json::object();
        auto        cloth = items::make(id, 1);
        if (!cloth) return ia::Result::Pass;
        ItemStack drop = *cloth;
        items::setData(drop, kRequestData, {{"order", shop.value("order", Json::array())}, {"address", shop.value("address", std::string{})}, {"ticker", shop.value("ticker", Json())}});
        updateLore(drop);
        // the cloth's own loot would drop a plain cloth: remove the block without drops instead
        if (!game::clearBlock(region, pos)) return ia::Result::Pass;
        dropAt(p, drop);
        log().info("stock: shop at {} {} {} broken by {}", p.x, p.y, p.z, player.getRealName());
        return ia::Result::Consume;
    }
    return ia::Result::Pass;
}

} // namespace

// ---- polling ------------------------------------------------------------------------------------

namespace {
void pollLinks() {
    for (auto const& p : std::vector<GlobalPos>(registry().links.begin(), registry().links.end())) {
        if (!blockAt(p)) continue;
        if (idAt(p) != kStockLink) continue;
        // Create's PackagerLinkBlock.getPower: every side but the support
        auto o      = cs::supportOffset(faceOf(p), facingOf(p));
        int  except = o[1] < 0 ? 0 : o[1] > 0 ? 1 : o[2] < 0 ? 2 : o[2] > 0 ? 3 : o[0] < 0 ? 4 : 5;
        int  power  = receivedPower(p, except);
        auto const* d = dataOf(p);
        int  before = d ? d->value("power", 0) : 0;
        if (power == before) continue;
        editData(p)["power"] = power;
        setStates(p, {{kStatePowered, power > 0 ? 1 : 0}});
        if ((before >= 15) != (power >= 15)) invalidateSummaries();
    }
}

class StockModule {
public:
    void install() {
        static bool once = false;
        if (!once) {
            once = true;
            game::db::onWorldReady([] {
                loadNetworks();
                loadRegistry();
                restoreClothItemDisplays();
            });
            game::db::onWorldSave([] { saveNetworks(); });
            game::db::onWorldClosed([] {
                resetNetworks();
                resetGaugeRuntime();
                resetTickerRuntime();
                registry() = {};
            });
            common::blockdata::onErase([](GlobalPos const& p, Json const& doc, std::string const& prev) {
                auto it = doc.find(kDataKey);
                if (it == doc.end() || !it->is_object()) return;
                Json d = *it;
                if (prev == kStockLink || (prev.empty() && registry().links.contains(p))) {
                    linkRemoved(d.value("freq", std::string{}), p);
                    registry().links.erase(p);
                    persistRegistry();
                    log().info("stock: stock link removed at {} {} {}", p.x, p.y, p.z);
                } else if (prev == kStockTicker || registry().tickers.contains(p)) {
                    tickerRemoved(p, d);
                    registry().tickers.erase(p);
                    persistRegistry();
                } else if (prev == kRedstoneRequester || registry().requesters.contains(p)) {
                    registry().requesters.erase(p);
                    persistRegistry();
                } else if (prev == kFactoryGauge || registry().gauges.contains(p)) {
                    gaugeRemoved(p, d);
                    registry().gauges.erase(p);
                    persistRegistry();
                } else if (isTableCloth(prev) || registry().shops.contains(p)) {
                    shopRemoved(p, d);
                    registry().shops.erase(p);
                    persistRegistry();
                }
            });
        }
        registerTickerScreens();
        registerRequesterScreens();
        registerGaugeScreens();
        registerShopValueBoxes();
        installGaugeLinks();
        mHandlers.push_back(ia::onUseOn({}, stockUseOn, ia::priority::High));
        mHandlers.push_back(ia::onPlace({}, stockPlaced));
        mHandlers.push_back(ia::onUse({}, stockUse));
        mHandlers.push_back(ia::onInteract({}, stockInteract));
        mHandlers.push_back(ia::onDestroy({}, stockDestroy, ia::priority::High));
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); });
        if (game::db::available()) {
            loadNetworks();
            loadRegistry();
        }
    }

    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mTick);
        uninstallGaugeLinks();
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
        saveNetworks();
    }

private:
    static void tick() {
        try {
            tickNetworks();
            // Create reacts to neighbour updates at once: links and requesters are read every
            // tick (custom blocks get no neighbour updates), so short pulses are never missed
            pollLinks();
            for (auto const& p : std::vector<GlobalPos>(registry().requesters.begin(), registry().requesters.end())) pollRequester(p);
            tickGauges();
        } catch (std::exception const& e) {
            log().error("stock: tick failed: {}", e.what());
        }
    }
    ll::event::ListenerPtr     mTick;
    std::vector<ia::HandlerId> mHandlers;
};
} // namespace

} // namespace lv::feature::stock

LV_REGISTER_MODULE(lv::feature::stock::StockModule);
