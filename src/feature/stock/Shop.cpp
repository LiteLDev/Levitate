// Stock unit: table cloth shops (Create's TableClothBlockEntity shop part, TableClothFilteringBehaviour,
// ShoppingListItem and StockTickerInteractionHandler.interactWithShop). The table cloths
// themselves (decoration) belong to U10; a cloth becomes a shop when it is placed carrying an
// order programmed at a stock ticker.

#include <algorithm>
#include <cmath>

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::stock {

namespace items = common::items;

namespace {
constexpr char const* kClothColours[] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan",
                                         "purple", "blue", "brown", "green", "red", "black", "andesite", "brass", "copper"};

Json shopOf(GlobalPos const& p) {
    auto const* d = dataOf(p);
    if (!d) return Json::object();
    return d->value("shop", Json::object());
}

void writeShop(GlobalPos const& p, Json const& shop) { editData(p)["shop"] = shop; }

ItemStack priceItem(Json const& shop) { return stackFromJson(shop.value("price", Json())); }
/// Create's getPaymentAmount: 1 without a price item.
int priceAmount(Json const& shop) { return items::isEmpty(priceItem(shop)) ? 1 : std::max(1, shop.value("priceCount", 1)); }

/// The price tag sits on the top of the cloth, towards its facing (Create's TableClothFilterSlot).
bool hitsPriceTag(GlobalPos const& p, Json const& shop, double lx, double ly, double lz) {
    auto [fx, fz] = cs::facingVector(shop.value("facing", 0));
    double cx = 0.5 + fx * 0.3, cz = 0.5 + fz * 0.3;
    (void)p;
    return ly < 0.5 && std::abs(lx - cx) <= 0.2 && std::abs(lz - cz) <= 0.2;
}

Json listOf(ItemStack const& stack) {
    auto j = items::getData(stack, kListData);
    return j.is_object() ? j : Json();
}

int purchasesAt(Json const& list, GlobalPos const& cloth) {
    for (auto const& e : list.value("purchases", Json::array()))
        if (posFrom(e.value("pos", Json())) == cloth) return e.value("n", 0);
    return 0;
}

void setPurchases(Json& list, GlobalPos const& cloth, int n) {
    auto& purchases = list["purchases"];
    if (!purchases.is_array()) purchases = Json::array();
    for (auto& e : purchases)
        if (posFrom(e.value("pos", Json())) == cloth) {
            e["n"] = n;
            return;
        }
    purchases.push_back({{"pos", posJson(cloth)}, {"n", n}});
}

/// The shopping list's lore (Create's appendHoverText).
void listLore(ItemStack& stack) {
    auto                     list  = listOf(stack);
    std::vector<std::string> lines;
    if (list.is_object()) {
        auto baked = bakeList(list, std::nullopt);
        for (auto const& e : baked.order.byCount()) lines.push_back("§7" + items::displayName(e.stack) + " x" + std::to_string(e.count));
        auto pay = baked.payment.byCount();
        lines.push_back("");
        if (pay.size() == 1) {
            lines.push_back("§6" + tr("create.table_cloth.total_cost", "Total cost: ") + "§e" + items::displayName(pay[0].stack) + " x" + std::to_string(pay[0].count));
        } else {
            lines.push_back("§6" + tr("create.table_cloth.total_cost", "Total cost: "));
            for (auto const& e : pay) lines.push_back("§e" + items::displayName(e.stack) + " x" + std::to_string(e.count));
        }
    }
    lines.push_back("§7" + tr("create.table_cloth.hand_to_shop_keeper", "Hand this to a shop keeper"));
    lines.push_back("§8" + tr("create.table_cloth.sneak_click_discard", "Sneak-Click to discard"));
    stack.setCustomLore(lines);
}

ItemStack makeList(Json const& list) {
    auto made = items::make(kShoppingList, 1);
    if (!made) return items::empty();
    ItemStack s = *made;
    items::setData(s, kListData, list);
    listLore(s);
    return s;
}
} // namespace

bool isShop(GlobalPos const& p) {
    if (!isTableCloth(idAt(p))) return false;
    auto shop = shopOf(p);
    return shop.is_object() && !shop.value("order", Json::array()).empty();
}

void onClothPlaced(GlobalPos const& p, Player& player, ItemStack const& item) {
    auto req = items::getData(item, kRequestData);
    if (!req.is_object()) return;
    auto ticker = posFrom(req.value("ticker", Json()));
    bool valid  = ticker && ticker->dim == p.dim && std::hypot(double(ticker->x - p.x), double(ticker->y - p.y), double(ticker->z - p.z)) < 128;
    if (valid) statusKey(player, "create.redstone_requester.keeper_connected", "Connected to Stock keeper successfully", "§f");
    else statusKey(player, "create.redstone_requester.keeper_too_far_away", "Requester too far away from Stock keeper", "§c");
    Json shop{
        {"order", req.value("order", Json::array())},
        {"address", req.value("address", std::string{})},
        {"ticker", ticker ? posJson(*ticker) : Json()},
        {"owner", uuidOf(player)},
        {"facing", game::opposite(game::playerFacing(player))},
        {"price", nullptr},
        {"priceCount", 1},
    };
    writeShop(p, shop);
    registry().shops.insert(p);
    saveRegistry();
    advancements::award(player, "table_cloth_shop"); // Create's TableClothBlock#setPlacedBy
    log().info("stock: table cloth shop placed at {} {} {} by {}", p.x, p.y, p.z, player.getRealName());
}

int stockLevelForTrade(GlobalPos const& cloth, Json const* otherPurchases) {
    auto shop   = shopOf(cloth);
    auto ticker = posFrom(shop.value("ticker", Json()));
    if (!ticker || idAt(*ticker) != kStockTicker) return 0;
    auto const& stock = summaryOf(networkAt(*ticker), false);
    Stock       modifier;
    if (otherPurchases && otherPurchases->is_object()) modifier = bakeList(*otherPurchases, cloth).order;
    int smallest = kInfinite;
    for (auto const& e : orderFromJson(shop.value("order", Json::array()))) {
        if (e.count <= 0) continue;
        int have = stock.countOf(e.stack) - modifier.countOf(e.stack);
        smallest = std::min(smallest, have / e.count);
    }
    return smallest;
}

Baked bakeList(Json const& list, std::optional<GlobalPos> ignore) {
    Baked out;
    auto  add = [](Stock& s, ItemStack const& item, int n) {
        auto key = itemKey(item);
        if (key.empty() || n == 0) return;
        s.summary.add(key, n);
        s.stacks.try_emplace(key, items::withCount(item, 1));
    };
    for (auto const& e : list.value("purchases", Json::array())) {
        auto pos = posFrom(e.value("pos", Json()));
        int  n   = e.value("n", 0);
        if (!pos || (ignore && *ignore == *pos) || !isShop(*pos)) continue;
        auto shop = shopOf(*pos);
        add(out.payment, priceItem(shop), priceAmount(shop) * n);
        for (auto const& o : orderFromJson(shop.value("order", Json::array()))) add(out.order, o.stack, o.count * n);
    }
    return out;
}

bool useShop(Player& player, GlobalPos const& p, double lx, double ly, double lz) {
    auto shop  = shopOf(p);
    auto owner = shop.value("owner", std::string{});
    // the owner edits the price tag (Create's TableClothFilteringBehaviour, owner only)
    if ((owner.empty() || owner == uuidOf(player)) && hitsPriceTag(p, shop, lx, ly, lz)) {
        auto const& held = player.getSelectedItem();
        if (items::isEmpty(held)) {
            auto clear = [p](Player& q) {
                auto s     = shopOf(p);
                s["price"] = nullptr;
                writeShop(p, s);
                log().info("stock: {} cleared the price of the shop at {} {} {}", q.getRealName(), p.x, p.y, p.z);
            };
            if (player.isSneaking()) {
                clear(player);
            } else if (!common::valuebox::awaitHold(player, p, clear, [p](Player& q) { common::valuebox::openEditor(q, p, "price"); })) {
                // vanilla players: the click opens the price board (Create's hold), sneaking clears
                common::valuebox::openEditor(player, p, "price");
            }
            return true;
        }
        auto hid = items::idOf(held);
        if (common::filter::isFilterItem(held) || hid == kShoppingList) {
            statusKey(player, "create.logistics.filter.invalid_item", "Cannot use this item here.", "§c");
            return true;
        }
        shop["price"] = stackToJson(items::withCount(held, 1));
        writeShop(p, shop);
        statusKey(player, "create.table_cloth.shop_configured", "Item shop configured");
        log().info("stock: {} set the price of the shop at {} {} {} to {} x{}", player.getRealName(), p.x, p.y, p.z, hid, priceAmount(shop));
        return true;
    }

    // Create's TableClothBlockEntity.useShop
    ItemStack inHand   = player.getSelectedItem();
    ItemStack prevList = items::empty();
    bool      addOnto  = false;
    auto&     inv      = player.getInventory();
    for (int i = 0; i < 9; ++i) {
        auto const& s = inv.getItem(i);
        if (items::isEmpty(s) || items::idOf(s) != kShoppingList) continue;
        prevList = s;
        addOnto  = true;
        inv.setItem(i, items::empty());
    }
    if (!items::isEmpty(inHand) && items::idOf(inHand) == kShoppingList) {
        prevList = inHand;
        addOnto  = true;
    }
    inHand = player.getSelectedItem();
    if (!items::isEmpty(inHand) && !addOnto) {
        statusKey(player, "create.stock_keeper.shopping_list_empty_hand", "Empty hand required to start a shopping list");
        playSound(p, "levitate.stock.deny", 0.5f, 1.0f);
        player.sendInventory(false);
        return true;
    }
    auto giveBack = [&](ItemStack const& list) {
        if (items::isEmpty(list)) return;
        if (items::isEmpty(player.getSelectedItem())) player.setSelectedItem(list);
        else giveOrDrop(player, list);
        player.sendInventory(false);
    };
    if (items::isEmpty(priceItem(shop))) {
        statusKey(player, "create.stock_keeper.no_price_set", "Shop owner must set a price first");
        playSound(p, "levitate.stock.deny", 0.5f, 1.0f);
        giveBack(prevList);
        return true;
    }
    std::string network;
    auto        ticker = posFrom(shop.value("ticker", Json()));
    if (ticker && idAt(*ticker) == kStockTicker && keeperPresent(*ticker)) network = networkAt(*ticker);
    Json prev       = listOf(prevList);
    int  stockLevel = stockLevelForTrade(p, prev.is_object() ? &prev : nullptr);
    if (network.empty()) {
        statusKey(player, "create.stock_keeper.keeper_missing", "Stock keeper missing", "§c");
        playSound(p, "levitate.stock.deny", 0.5f, 1.0f);
        giveBack(prevList);
        return true;
    }
    if (stockLevel == 0) {
        statusKey(player, "create.stock_keeper.out_of_stock", "Out of Stock", "§c");
        playSound(p, "levitate.stock.deny", 0.5f, 1.0f);
        giveBack(prevList);
        return true;
    }
    Json list{{"purchases", Json::array()}, {"owner", owner}, {"network", network}};
    if (addOnto && prev.is_object()) {
        if (prev.value("owner", std::string{}) == owner && prev.value("network", std::string{}) == network) list = prev;
        else addOnto = false;
    }
    if (purchasesAt(list, p) >= stockLevel) {
        setPurchases(list, p, std::min(stockLevel, purchasesAt(list, p)));
        statusKey(player, "create.stock_keeper.limited_stock", "Stock limit reached", "§c");
    } else {
        playSound(p, "levitate.stock.confirm", 0.5f, 1.0f);
        setPurchases(list, p, purchasesAt(list, p) + 1);
        if (!addOnto) statusKey(player, "create.stock_keeper.use_list_to_add_purchases", "Use this list to add more to your purchase", "§f");
    }
    list["address"] = shop.value("address", std::string{});
    auto item       = makeList(list);
    if (items::isEmpty(player.getSelectedItem())) player.setSelectedItem(item);
    else giveOrDrop(player, item);
    player.sendInventory(false);
    log().info("stock: {} added a purchase at the shop at {} {} {} ({} on the list)", player.getRealName(), p.x, p.y, p.z, purchasesAt(list, p));
    return true;
}

void checkout(Player& player, GlobalPos const& ticker, ItemStack const& listItem) {
    // Create's StockTickerInteractionHandler.interactWithShop
    auto list = listOf(listItem);
    if (!list.is_object()) return;
    auto network = networkAt(ticker);
    if (network != list.value("network", std::string{})) {
        playSound(ticker, "levitate.stock.deny", 1.0f, 1.0f);
        statusKey(player, "create.stock_keeper.wrong_network", "This list is for another shop", "§c");
        return;
    }
    auto baked = bakeList(list, std::nullopt);
    (void)summaryOf(network, true);
    auto const& recent = summaryOf(network, false);
    for (auto const& e : baked.order.byCount()) {
        if (recent.countOf(e.stack) >= e.count) continue;
        playSound(ticker, "levitate.stock.deny", 1.0f, 1.0f);
        statusKey(player, "create.stock_keeper.stock_level_too_low", "Purchase cancelled: Stock levels lower than expected", "§c");
        log().info("stock: checkout by {} refused: stock too low", player.getRealName());
        return;
    }
    int occupied = 0;
    for (auto const& e : baked.payment.byCount()) occupied += (e.count + std::max<int>(1, e.stack.getMaxStackSize()) - 1) / std::max<int>(1, e.stack.getMaxStackSize());
    if (occupied > freePaymentSlots(ticker)) {
        playSound(ticker, "levitate.stock.deny", 1.0f, 1.0f);
        statusKey(player, "create.stock_keeper.cash_register_full", "Purchase cancelled: Not enough space in Cash Register", "§c");
        return;
    }
    // payment: simulate, then take from the main inventory (Create: inventory.items only)
    auto& inv = player.getInventory();
    for (bool simulate : {true, false}) {
        cs::Summary tally = baked.payment.summary;
        for (int i = 0; i < 36 && i < inv.getContainerSize(); ++i) {
            ItemStack item = inv.getItem(i);
            if (items::isEmpty(item)) continue;
            auto key   = itemKey(item);
            int  owed  = tally.countOf(key);
            if (owed <= 0) continue;
            int take = std::min(items::countOf(item), owed);
            tally.add(key, -take);
            if (simulate) continue;
            inv.setItem(i, items::withCount(item, items::countOf(item) - take));
            auto rest = insertPayment(ticker, items::withCount(item, take));
            if (!items::isEmpty(rest)) dropAt(ticker, rest);
        }
        if (simulate) {
            bool broke = false;
            for (auto const& e : tally.entries())
                if (e.count > 0) broke = true;
            if (broke) {
                playSound(ticker, "levitate.stock.deny", 1.0f, 1.0f);
                statusKey(player, "create.stock_keeper.too_broke", "You cannot afford this purchase", "§c");
                return;
            }
        }
    }
    requestOrder(network, RequestType::Player, baked.order.byCount(), list.value("address", std::string{}));
    player.setSelectedItem(items::empty());
    player.sendInventory(false);
    if (!baked.order.summary.empty()) playSound(ticker, "levitate.stock.trade", 1.0f, 1.0f);
    log().info("stock: {} checked out a shopping list at the stock ticker at {} {} {}", player.getRealName(), ticker.x, ticker.y, ticker.z);
}

void shopRemoved(GlobalPos const& p, Json const&) { log().info("stock: table cloth shop removed at {} {} {}", p.x, p.y, p.z); }

void registerShopValueBoxes() {
    for (auto const* colour : kClothColours) {
        std::string id = std::string("levitate:") + colour + "_table_cloth";
        common::valuebox::registerBox(
            id,
            {
                .id            = "price",
                .label         = "create.table_cloth.price_per_order",
                .labelFallback = "Price per Order",
                .board =
                    [](Block const&) {
                        core::common::BoardSpec b;
                        b.rows              = {tr("create.table_cloth.amount", "Amount")};
                        b.maxValue          = 100;
                        b.milestoneInterval = 10;
                        return b;
                    },
                .centre = [](Block const&, int) -> std::optional<core::common::Vec> { return std::nullopt; },
                .get    = [](GlobalPos const& p) { return core::common::ValueSettings{0, priceAmount(shopOf(p))}; },
                .set =
                    [](GlobalPos const& p, core::common::ValueSettings s, Player& editor) {
                        auto shop  = shopOf(p);
                        auto owner = shop.value("owner", std::string{});
                        if (!owner.empty() && owner != uuidOf(editor)) return;
                        shop["priceCount"] = std::max(1, s.value);
                        writeShop(p, shop);
                        log().info("stock: {} set the price amount of the shop at {} {} {} to {}", editor.getRealName(), p.x, p.y, p.z, std::max(1, s.value));
                    },
                .format = [](Block const&, core::common::ValueSettings s) { return std::to_string(std::max(1, s.value)); },
            }
        );
    }
}

} // namespace lv::feature::stock
