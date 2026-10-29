// Stock unit: the stock ticker (Create's StockTickerBlock / StockTickerBlockEntity /
// StockTickerInteractionHandler): keepers, received payments, the category editor and the stock
// keeper request screen (native screen for companions, forms for vanilla players).

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/net/Net.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::stock {

namespace items   = common::items;
namespace screens = common::screens;

namespace {
constexpr char kRequestScreen[]  = "stock.request";
constexpr char kCategoryScreen[] = "stock.categories";
constexpr int  kPaymentSlots     = 27;

constexpr int kHx[4]{0, -1, 0, 1}, kHz[4]{1, 0, -1, 0}; // S W N E

std::string const& freqOf(GlobalPos const& p) {
    static std::string empty;
    auto const*        d = dataOf(p);
    if (!d) return empty;
    auto it = d->find("freq");
    return it != d->end() && it->is_string() ? it->get_ref<std::string const&>() : empty;
}

bool isTickerAt(GlobalPos const& p) { return idAt(p) == kStockTicker; }

std::vector<ItemStack> readStacks(Json const& arr) {
    std::vector<ItemStack> out;
    if (!arr.is_array()) return out;
    for (auto const& j : arr) out.push_back(stackFromJson(j));
    return out;
}

Json writeStacks(std::vector<ItemStack> const& v) {
    Json a = Json::array();
    for (auto const& s : v) a.push_back(stackToJson(s));
    return a;
}

/// The player holds an item an order can be programmed into (Create: a redstone requester or a
/// table cloth).
bool holdsProgrammable(Player const& player) {
    auto const& held = player.getSelectedItem();
    if (items::isEmpty(held)) return false;
    auto id = items::idOf(held);
    return id == kRedstoneRequester || isTableCloth(id);
}

/// Category filters with their names: [{"filter": stack json, "name": ""}].
Json categoriesOf(GlobalPos const& p) {
    auto const* d = dataOf(p);
    return d ? d->value("categories", Json::array()) : Json::array();
}

/// The search text of each player's request screen (Create keeps it in the client screen).
std::map<std::string, std::string>& searches() {
    static std::map<std::string, std::string> m;
    return m;
}

std::string lowerCase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool matchesSearch(ItemStack const& s, std::string const& search) {
    if (search.empty()) return true;
    auto q = lowerCase(search);
    return lowerCase(items::displayName(s)).find(q) != std::string::npos || items::idOf(s).find(q) != std::string::npos;
}

/// The network stock split into the ticker's categories (Create's receiveStockPacket), hidden
/// categories of the player removed. The last group is "Unsorted".
Json stockGroups(GlobalPos const& ticker, Player& player, std::string const& search = {}) {
    auto const& stock = summaryOf(freqOf(ticker), false);
    auto        all   = stock.byCount();
    std::erase_if(all, [&](BigStack const& e) { return !matchesSearch(e.stack, search); });
    auto        cats  = categoriesOf(ticker);
    auto const* d     = dataOf(ticker);
    Json        hidden = d ? d->value("hidden", Json::object()).value(uuidOf(player), Json::array()) : Json::array();
    Json        groups = Json::array();
    std::vector<bool> taken(all.size(), false);
    int index = 0;
    for (auto const& c : cats) {
        auto filter = stackFromJson(c.value("filter", Json()));
        Json list   = Json::array();
        if (!items::isEmpty(filter)) {
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (taken[i] || !common::filter::matches(filter, all[i].stack)) continue;
                taken[i] = true;
                list.push_back({{"item", stackToJson(all[i].stack)}, {"count", all[i].count}});
            }
        }
        auto name = c.value("name", std::string{});
        if (name.empty() && !items::isEmpty(filter)) name = items::displayName(filter);
        bool hide = std::find(hidden.begin(), hidden.end(), Json(index)) != hidden.end();
        groups.push_back({{"name", name}, {"items", list}, {"index", index}, {"hidden", hide}});
        ++index;
    }
    Json unsorted = Json::array();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (!taken[i]) unsorted.push_back({{"item", stackToJson(all[i].stack)}, {"count", all[i].count}});
    groups.push_back({{"name", trFor(player, "create.gui.stock_keeper.unsorted_category", "Unsorted")}, {"items", unsorted}, {"index", -1}, {"hidden", false}});
    return groups;
}

std::optional<GlobalPos> tickerFrom(Player& player, Json const& ctx) {
    auto p = posFrom(ctx.value("pos", Json()));
    if (!p || !isTickerAt(*p) || !inReach(player, *p, 16)) return std::nullopt;
    return p;
}

Json requestData(Player& player, GlobalPos const& ticker) {
    auto const& network = freqOf(ticker);
    auto const& stock   = summaryOf(network, false);
    auto const* d       = dataOf(ticker);
    return {
        {"pos",          posJson(ticker)                                                          },
        {"groups",       stockGroups(ticker, player, searches()[uuidOf(player)])                  },
        {"search",       searches()[uuidOf(player)]                                               },
        {"address",      d ? d->value("prevAddress", std::string{}) : std::string{}               },
        {"links",        stock.summary.contributingLinks                                          },
        {"canLock",      mayAdministrate(network, player) && isLockable(network)                  },
        {"locked",       isLocked(network)                                                        },
        {"program",      holdsProgrammable(player)                                                },
        {"maxSlots",     cs::kMaxOrderSlots                                                       },
    };
}

/// The vanilla request screen: search / address / category first, then amounts.
void requestForm(Player& player, Json const& ctx);

void amountsForm(Player& player, GlobalPos ticker, std::string search, std::string address, int category) {
    auto groups = stockGroups(ticker, player);
    std::vector<BigStack> shown;
    std::string lowered = search;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto const& g : groups) {
        if (category >= 0 && g.value("index", -2) != category) continue;
        if (category < 0 && g.value("hidden", false)) continue;
        for (auto const& e : g.value("items", Json::array())) {
            auto s    = stackFromJson(e.value("item", Json()));
            auto name = items::displayName(s);
            auto low  = name;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto idLow = items::idOf(s);
            if (!lowered.empty() && low.find(lowered) == std::string::npos && idLow.find(lowered) == std::string::npos) continue;
            shown.push_back({s, e.value("count", 0)});
            if (shown.size() >= 48) break;
        }
    }
    bool program = holdsProgrammable(player);
    ll::form::CustomForm form(trFor(player, "create.gui.stock_keeper.title", "Stock Keeper"));
    if (shown.empty()) {
        form.appendLabel(trFor(player, "create.gui.stock_keeper.no_search_results", "No matching items found"));
    } else {
        form.appendLabel(trFor(player, "lv.stock.form.amounts", "Amounts to order (at most {} kinds of item):", cs::kMaxOrderSlots));
        for (std::size_t i = 0; i < shown.size(); ++i) {
            auto count = shown[i].count >= kInfinite ? std::string("∞") : std::to_string(shown[i].count);
            form.appendInput("n" + std::to_string(i), items::displayName(shown[i].stack) + " §7(" + count + ")", "0", "");
        }
    }
    form.appendToggle("send", program ? trFor(player, "create.gui.stock_keeper.program_requester", "Program Requester") : trFor(player, "create.gui.stock_keeper.send", "Send"), true);
    form.sendTo(player, [ticker, shown, address, program](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        if (auto it = result->find("send"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second); v && !*v) return;
        }
        std::vector<BigStack> order;
        bool                  full = false;
        for (std::size_t i = 0; i < shown.size(); ++i) {
            auto it = result->find("n" + std::to_string(i));
            if (it == result->end()) continue;
            auto const* text = std::get_if<std::string>(&it->second);
            if (!text || text->empty()) continue;
            int n = 0;
            try {
                n = std::stoi(*text);
            } catch (...) {
                continue;
            }
            n = std::min(n, shown[i].count);
            if (n <= 0) continue;
            if (static_cast<int>(order.size()) >= cs::kMaxOrderSlots) {
                full = true;
                continue;
            }
            order.push_back({shown[i].stack, n});
        }
        if (full) statusKey(p, "create.gui.stock_keeper.slots_full", "Order slots already full", "§c");
        if (order.empty()) return;
        placeOrder(p, ticker, order, address, program);
    });
}

void requestForm(Player& player, Json const& ctx) {
    auto ticker = tickerFrom(player, ctx);
    if (!ticker) return;
    auto data    = requestData(player, *ticker);
    auto network = freqOf(*ticker);
    ll::form::CustomForm form(trFor(player, "create.gui.stock_keeper.title", "Stock Keeper"));
    if (data.value("links", 0) == 0) {
        form.appendLabel("§c" + trFor(player, "create.gui.stock_keeper.no_packagers_linked", "No Packagers Linked"));
    } else {
        int total = 0;
        for (auto const& g : data["groups"]) total += static_cast<int>(g.value("items", Json::array()).size());
        if (total == 0) form.appendLabel(trFor(player, "create.gui.stock_keeper.inventories_empty", "Linked Inventories are Empty"));
    }
    std::vector<std::string> cats{trFor(player, "lv.stock.form.all_categories", "(all categories)")};
    std::vector<int>         catIndex{-2};
    for (auto const& g : data["groups"]) {
        auto name = g.value("name", std::string{});
        if (name.empty()) name = trFor(player, "create.gui.stock_ticker.empty_category_name_placeholder", "(Empty)");
        cats.push_back(name + " (" + std::to_string(g.value("items", Json::array()).size()) + ")");
        catIndex.push_back(g.value("index", -1));
    }
    form.appendInput("search", trFor(player, "create.gui.stock_keeper.search_items", "Search Items"), "", "");
    form.appendDropdown("category", trFor(player, "lv.stock.form.category", "Category"), cats, 0);
    form.appendInput("address", trFor(player, "create.gui.stock_keeper.package_address", "Package Address"), "", data.value("address", std::string{}));
    bool canLock = data.value("canLock", false);
    if (canLock) {
        form.appendToggle(
            "lock",
            trFor(player, "create.gui.stock_keeper.network_lock_tip", "Locking prevents other Players") + " "
                + trFor(player, "create.gui.stock_keeper.network_lock_tip_1", "from ordering items directly"),
            data.value("locked", false)
        );
    }
    GlobalPos t = *ticker;
    form.sendTo(player, [t, catIndex, canLock, network](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result || !isTickerAt(t)) return;
        auto str = [&](char const* k) {
            auto it = result->find(k);
            if (it == result->end()) return std::string{};
            auto const* s = std::get_if<std::string>(&it->second);
            return s ? *s : std::string{};
        };
        int cat = -2;
        if (auto it = result->find("category"); it != result->end())
            if (auto const* v = std::get_if<uint64>(&it->second); v && *v < catIndex.size()) cat = catIndex[*v];
        if (canLock) {
            if (auto it = result->find("lock"); it != result->end()) {
                if (auto const* v = std::get_if<uint64>(&it->second); v && (*v != 0) != isLocked(network)) {
                    setLocked(network, *v != 0);
                    statusKey(p, *v ? "create.gui.stock_keeper.network_locked" : "create.gui.stock_keeper.network_open", *v ? "Network is locked" : "Network is open");
                    log().info("stock: {} {} network {}", p.getRealName(), *v ? "locked" : "opened", network.substr(0, 8));
                }
            }
        }
        amountsForm(p, t, str("search"), str("address"), cat == -2 ? -1 : cat);
    });
}

void requestAction(Player& player, Json const& ctx, Json const& action) {
    auto ticker = tickerFrom(player, ctx);
    if (!ticker) return;
    auto op      = action.value("op", std::string{});
    auto network = freqOf(*ticker);
    if (op == "order") {
        auto order = orderFromJson(action.value("items", Json::array()));
        if (static_cast<int>(order.size()) > cs::kMaxOrderSlots) order.resize(cs::kMaxOrderSlots);
        placeOrder(player, *ticker, order, action.value("address", std::string{}), action.value("encode", false));
    } else if (op == "lock") {
        if (!mayAdministrate(network, player) || !isLockable(network)) return;
        bool now = !isLocked(network);
        setLocked(network, now);
        log().info("stock: {} {} network {}", player.getRealName(), now ? "locked" : "opened", network.substr(0, 8));
    } else if (op == "hide") {
        int   index  = action.value("index", -1);
        auto& d      = editData(*ticker);
        auto& hidden = d["hidden"];
        if (!hidden.is_object()) hidden = Json::object();
        auto& list = hidden[uuidOf(player)];
        if (!list.is_array()) list = Json::array();
        auto it = std::find(list.begin(), list.end(), Json(index));
        if (it == list.end()) list.push_back(index);
        else list.erase(it);
    } else if (op == "search") {
        searches()[uuidOf(player)] = action.value("text", std::string{}).substr(0, 50);
    } else if (op == "search_form") {
        ll::form::CustomForm form(trFor(player, "create.gui.stock_keeper.search_items", "Search Items"));
        form.appendInput("text", trFor(player, "create.gui.stock_keeper.search_items", "Search Items"), "", searches()[uuidOf(player)]);
        GlobalPos t = *ticker;
        form.sendTo(player, [t](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
            if (!result || !isTickerAt(t)) return;
            if (auto it = result->find("text"); it != result->end())
                if (auto const* s = std::get_if<std::string>(&it->second)) searches()[uuidOf(p)] = s->substr(0, 50);
            screens::open(p, kRequestScreen, {{"pos", posJson(t)}});
        });
    } else if (op == "address_form") {
        // the native screen has no text field: the address is typed in a form
        ll::form::CustomForm form(trFor(player, "create.gui.stock_keeper.package_address", "Package Address"));
        auto const* d = dataOf(*ticker);
        form.appendInput("address", trFor(player, "create.gui.stock_keeper.package_address", "Package Address"), "", d ? d->value("prevAddress", std::string{}) : std::string{});
        GlobalPos t = *ticker;
        form.sendTo(player, [t](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
            if (!result || !isTickerAt(t)) return;
            auto it = result->find("address");
            if (it == result->end()) return;
            if (auto const* s = std::get_if<std::string>(&it->second)) editData(t)["prevAddress"] = *s;
            screens::open(p, kRequestScreen, {{"pos", posJson(t)}});
        });
    }
}

// ---- category editor --------------------------------------------------------------------------

Json categoryData(Player& player, GlobalPos const& ticker) {
    Json list = Json::array();
    for (auto const& c : categoriesOf(ticker)) {
        auto filter = stackFromJson(c.value("filter", Json()));
        auto name   = c.value("name", std::string{});
        list.push_back({{"filter", c.value("filter", Json())}, {"name", name.empty() && !items::isEmpty(filter) ? items::displayName(filter) : name}});
    }
    (void)player;
    return {{"pos", posJson(ticker)}, {"categories", list}};
}

/// Takes a filter from the player's inventory slot into a new category (Create: categories are
/// list / attribute filters; an empty category is allowed as a divider).
void addCategory(Player& player, GlobalPos const& ticker, int slot) {
    auto& d    = editData(ticker);
    auto& cats = d["categories"];
    if (!cats.is_array()) cats = Json::array();
    Json entry{{"filter", nullptr}, {"name", ""}};
    if (slot >= 0) {
        auto& inv = player.getInventory();
        if (slot >= inv.getContainerSize()) return;
        ItemStack s = inv.getItem(slot);
        if (!common::filter::isFilterItem(s)) return;
        entry["filter"] = stackToJson(items::withCount(s, 1));
        if (!player.isCreative()) {
            inv.setItem(slot, items::withCount(s, items::countOf(s) - 1));
            player.sendInventory(false);
        }
    }
    cats.push_back(entry);
    log().info("stock: {} added a category to the stock ticker at {} {} {}", player.getRealName(), ticker.x, ticker.y, ticker.z);
}

void removeCategory(Player& player, GlobalPos const& ticker, int index) {
    auto& d    = editData(ticker);
    auto& cats = d["categories"];
    if (!cats.is_array() || index < 0 || index >= static_cast<int>(cats.size())) return;
    auto filter = stackFromJson(cats[static_cast<std::size_t>(index)].value("filter", Json()));
    cats.erase(cats.begin() + index);
    if (!items::isEmpty(filter) && !player.isCreative()) giveOrDrop(player, filter);
    log().info("stock: {} removed a category of the stock ticker at {} {} {}", player.getRealName(), ticker.x, ticker.y, ticker.z);
}

void moveCategory(GlobalPos const& ticker, int index, int delta, bool toEnd) {
    auto& cats = editData(ticker)["categories"];
    if (!cats.is_array() || index < 0 || index >= static_cast<int>(cats.size())) return;
    Json e = cats[static_cast<std::size_t>(index)];
    cats.erase(cats.begin() + index);
    int to = toEnd ? (delta < 0 ? 0 : static_cast<int>(cats.size())) : std::clamp(index + delta, 0, static_cast<int>(cats.size()));
    cats.insert(cats.begin() + to, e);
}

void renameCategory(GlobalPos const& ticker, int index, std::string const& name) {
    auto& cats = editData(ticker)["categories"];
    if (!cats.is_array() || index < 0 || index >= static_cast<int>(cats.size())) return;
    cats[static_cast<std::size_t>(index)]["name"] = name.substr(0, 64);
}

void categoryAction(Player& player, Json const& ctx, Json const& action) {
    auto ticker = tickerFrom(player, ctx);
    if (!ticker || !mayInteract(freqOf(*ticker), player)) return;
    auto op = action.value("op", std::string{});
    if (op == "add") addCategory(player, *ticker, action.value("slot", -1));
    else if (op == "remove") removeCategory(player, *ticker, action.value("index", -1));
    else if (op == "move") moveCategory(*ticker, action.value("index", -1), action.value("delta", 0), action.value("toEnd", false));
    else if (op == "rename") renameCategory(*ticker, action.value("index", -1), action.value("name", std::string{}));
    else if (op == "rename_form") {
        int  index = action.value("index", -1);
        auto cats  = categoriesOf(*ticker);
        if (index < 0 || index >= static_cast<int>(cats.size())) return;
        ll::form::CustomForm form(trFor(player, "create.gui.stock_ticker.category_name", "Category Name"));
        form.appendInput("name", trFor(player, "create.gui.stock_ticker.category_name", "Category Name"), "", cats[static_cast<std::size_t>(index)].value("name", std::string{}));
        GlobalPos t = *ticker;
        form.sendTo(player, [t, index](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
            if (!result || !isTickerAt(t)) return;
            if (auto it = result->find("name"); it != result->end())
                if (auto const* s = std::get_if<std::string>(&it->second)) renameCategory(t, index, *s);
            screens::open(p, kCategoryScreen, {{"pos", posJson(t)}});
        });
    }
}

void categoryForm(Player& player, Json const& ctx) {
    auto ticker = tickerFrom(player, ctx);
    if (!ticker) return;
    auto cats = categoriesOf(*ticker);
    ll::form::SimpleForm form(trFor(player, "create.gui.stock_ticker.category_editor", "Category Editor"));
    form.setContent(trFor(player, "create.gui.stock_ticker.category_filter_tip", "Place a List or Attribute Filter") + " "
                    + trFor(player, "create.gui.stock_ticker.category_filter_tip_1", "to specify which items are included"));
    GlobalPos t = *ticker;
    for (std::size_t i = 0; i < cats.size(); ++i) {
        auto filter = stackFromJson(cats[i].value("filter", Json()));
        auto name   = cats[i].value("name", std::string{});
        if (name.empty()) name = items::isEmpty(filter) ? trFor(player, "create.gui.stock_ticker.empty_category_name_placeholder", "(Empty)") : items::displayName(filter);
        int index = static_cast<int>(i);
        form.appendButton(name, [t, index](Player& p) {
            ll::form::CustomForm edit(trFor(p, "create.gui.stock_ticker.category_editor", "Category Editor"));
            auto c = categoriesOf(t);
            if (index >= static_cast<int>(c.size())) return;
            edit.appendInput("name", trFor(p, "create.gui.stock_ticker.category_name", "Category Name"), "", c[static_cast<std::size_t>(index)].value("name", std::string{}));
            edit.appendDropdown(
                "move",
                trFor(p, "lv.stock.form.move", "Move"),
                {trFor(p, "lv.common.form.keep", "(keep)"), "↑", "↓", trFor(p, "create.gui.stock_ticker.shift_moves_top", "Shift moves to top"),
                 trFor(p, "create.gui.stock_ticker.shift_moves_bottom", "Shift moves to bottom")},
                0
            );
            edit.appendToggle("delete", trFor(p, "create.gui.stock_ticker.delete_category", "Delete Category"), false);
            edit.sendTo(p, [t, index](Player& q, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
                if (!r || !isTickerAt(t)) return;
                if (auto it = r->find("delete"); it != r->end())
                    if (auto const* v = std::get_if<uint64>(&it->second); v && *v) {
                        removeCategory(q, t, index);
                        return;
                    }
                if (auto it = r->find("name"); it != r->end())
                    if (auto const* s = std::get_if<std::string>(&it->second)) renameCategory(t, index, *s);
                if (auto it = r->find("move"); it != r->end())
                    if (auto const* v = std::get_if<uint64>(&it->second)) {
                        if (*v == 1) moveCategory(t, index, -1, false);
                        if (*v == 2) moveCategory(t, index, 1, false);
                        if (*v == 3) moveCategory(t, index, -1, true);
                        if (*v == 4) moveCategory(t, index, 1, true);
                    }
            });
        });
    }
    form.appendButton("+ " + trFor(player, "create.gui.stock_ticker.new_category", "New Category"), [t](Player& p) {
        // offer the filters in the inventory
        std::vector<int>         slots{-1};
        std::vector<std::string> names{trFor(p, "create.gui.stock_ticker.empty_category_name_placeholder", "(Empty)")};
        auto&                    inv = p.getInventory();
        for (int i = 0; i < inv.getContainerSize(); ++i) {
            auto const& s = inv.getItem(i);
            if (!common::filter::isFilterItem(s)) continue;
            slots.push_back(i);
            names.push_back(items::displayName(s));
        }
        ll::form::CustomForm add(trFor(p, "create.gui.stock_ticker.new_category", "New Category"));
        add.appendDropdown("filter", trFor(p, "create.gui.stock_ticker.category_filter", "Category Filter"), names, 0);
        add.sendTo(p, [t, slots](Player& q, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
            if (!r || !isTickerAt(t)) return;
            auto it = r->find("filter");
            if (it == r->end()) return;
            if (auto const* v = std::get_if<uint64>(&it->second); v && *v < slots.size()) addCategory(q, t, slots[*v]);
        });
    });
    form.sendTo(player);
}
} // namespace

// ---- keepers -------------------------------------------------------------------------------------

bool keeperPresent(GlobalPos const& ticker) {
    auto* region = regionOf(ticker.dim);
    if (!region) return false;
    for (int y : {0, 1}) {
        for (int s = 0; s < 4; ++s) {
            GlobalPos seat = offset(ticker, kHx[s], -y, kHz[s]);
            if (!blockAt(seat)) continue;
            AABB box{Vec3{float(seat.x), float(seat.y), float(seat.z)}, Vec3{seat.x + 1.0f, seat.y + 1.0f, seat.z + 1.0f}};
            for (auto* a : region->getEntities(box, 0.0f)) {
                if (!a || a->isPlayer() || !a->getVehicle()) continue;
                return true;
            }
            if (y == 0 && idAt(seat) == kBlazeBurner) return true;
        }
    }
    return false;
}

std::optional<GlobalPos> tickerOfKeeper(GlobalPos const& keeperPos, bool burner) {
    if (burner) {
        for (int s = 0; s < 4; ++s) {
            auto q = offset(keeperPos, kHx[s], 0, kHz[s]);
            if (isTickerAt(q)) return q;
        }
        return std::nullopt;
    }
    std::optional<GlobalPos> found;
    int                      stations = 0;
    for (int s = 0; s < 4; ++s)
        for (int y : {0, 1}) {
            auto q = offset(keeperPos, kHx[s], y, kHz[s]);
            if (!isTickerAt(q)) continue;
            found = q;
            ++stations;
        }
    return stations == 1 ? found : std::nullopt;
}

void useTicker(Player& player, GlobalPos const& ticker) {
    auto const& network = freqOf(ticker);
    if (!mayInteract(network, player)) {
        statusKey(player, "create.logistically_linked.protected", "Logistics Network is Protected", "§c");
        return;
    }
    // Create: the stock ticker hands out the payments of its shops first
    auto pays = payments(ticker);
    bool any  = std::any_of(pays.begin(), pays.end(), [](ItemStack const& s) { return !items::isEmpty(s); });
    if (any) {
        for (auto const& s : pays) giveOrDrop(player, s);
        editData(ticker)["payments"] = Json::array();
        playSound(ticker, "random.pop", 0.3f, 1.0f);
        log().info("stock: {} collected the payments of the stock ticker at {} {} {}", player.getRealName(), ticker.x, ticker.y, ticker.z);
        return;
    }
    if (!keeperPresent(ticker)) {
        statusKey(player, "create.stock_ticker.keeper_missing", "Seat a mob or blaze burner next to this block to use it");
        return;
    }
    screens::open(player, kCategoryScreen, {{"pos", posJson(ticker)}});
}

void useKeeper(Player& player, GlobalPos const& ticker) {
    auto const& held = player.getSelectedItem();
    if (!items::isEmpty(held) && items::idOf(held) == kShoppingList) {
        checkout(player, ticker, held);
        return;
    }
    auto const& network = freqOf(ticker);
    if (!mayInteract(network, player)) {
        statusKey(player, "create.stock_keeper.locked", "Interact using a Shopping list, direct access has been locked", "§c");
        return;
    }
    screens::open(player, kRequestScreen, {{"pos", posJson(ticker)}});
}

bool placeOrder(Player& player, GlobalPos const& ticker, std::vector<BigStack> const& order, std::string const& address, bool encode) {
    auto network = freqOf(ticker);
    if (!mayInteract(network, player)) return false;
    if (encode) {
        // Create's RedstoneRequesterBlock.programRequester: the order goes into the held item
        if (order.empty() || !holdsProgrammable(player)) return false;
        ItemStack held = player.getSelectedItem();
        items::setData(held, kRequestData, {{"order", orderToJson(order)}, {"address", address}, {"ticker", posJson(ticker)}});
        if (items::idOf(held) == kRedstoneRequester) tuneItem(held, network);
        updateLore(held);
        player.setSelectedItem(held);
        player.sendInventory(false);
        log().info("stock: {} programmed {} with an order of {} stack(s) to '{}'", player.getRealName(), items::idOf(held), order.size(), address);
        return true;
    }
    editData(ticker)["prevAddress"] = address;
    bool ok = requestOrder(network, RequestType::Player, order, address);
    if (ok) {
        playSound(ticker, "levitate.stock.request", 1.0f, 1.0f);
        statusKey(player, "create.gui.stock_keeper.request_sent", "Request Sent!", "§a");
    } else {
        statusKey(player, "create.gui.stock_keeper.not_in_stock", "Required items are not in Stock", "§c");
    }
    log().info("stock: {} ordered {} stack(s) to '{}' at the stock ticker at {} {} {}: {}", player.getRealName(), order.size(), address, ticker.x, ticker.y, ticker.z, ok ? "sent" : "refused");
    return ok;
}

std::vector<ItemStack> payments(GlobalPos const& ticker) {
    auto const* d = dataOf(ticker);
    return d ? readStacks(d->value("payments", Json::array())) : std::vector<ItemStack>{};
}

/// Inserts into the 27 payment slots (stacking first). Returns what did not fit.
ItemStack insertPayment(GlobalPos const& ticker, ItemStack stack) {
    auto pays = payments(ticker);
    pays.resize(kPaymentSlots, items::empty());
    int left = items::countOf(stack);
    int max  = std::max<int>(1, stack.getMaxStackSize());
    for (auto& s : pays) {
        if (left <= 0) break;
        if (items::isEmpty(s) || !items::sameItemSameData(s, stack)) continue;
        int add = std::min(left, max - items::countOf(s));
        if (add <= 0) continue;
        s = items::withCount(s, items::countOf(s) + add);
        left -= add;
    }
    for (auto& s : pays) {
        if (left <= 0) break;
        if (!items::isEmpty(s)) continue;
        int add = std::min(left, max);
        s       = items::withCount(stack, add);
        left -= add;
    }
    editData(ticker)["payments"] = writeStacks(pays);
    return items::withCount(stack, left);
}

int freePaymentSlots(GlobalPos const& ticker) {
    auto pays = payments(ticker);
    pays.resize(kPaymentSlots, items::empty());
    return static_cast<int>(std::count_if(pays.begin(), pays.end(), [](ItemStack const& s) { return items::isEmpty(s); }));
}

void tickerRemoved(GlobalPos const& p, Json const& doc) {
    // Create's destroy(): payments and category filters drop
    for (auto const& s : readStacks(doc.value("payments", Json::array()))) dropAt(p, s);
    for (auto const& c : doc.value("categories", Json::array())) dropAt(p, stackFromJson(c.value("filter", Json())));
    log().info("stock: stock ticker removed at {} {} {}", p.x, p.y, p.z);
}

void registerTickerScreens() {
    screens::registerType({
        .id   = kRequestScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            auto t = tickerFrom(p, ctx);
            if (!t || !mayInteract(freqOf(*t), p)) return std::nullopt;
            return requestData(p, *t);
        },
        .action = requestAction,
        .form   = requestForm,
    });
    screens::registerType({
        .id   = kCategoryScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            auto t = tickerFrom(p, ctx);
            if (!t) return std::nullopt;
            return categoryData(p, *t);
        },
        .action = categoryAction,
        .form   = categoryForm,
    });
}

} // namespace lv::feature::stock
