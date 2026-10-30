// Stock unit: the factory gauge (Create's FactoryPanelBlock / FactoryPanelBehaviour): up to 4
// panels per block, each watching one item of its network (or, on a packager, of the packager's
// inventory: restocker mode), connected panels as recipe inputs, requests of missing items,
// promises, the configuration screen and the client hover label.

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/level/Level.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/common/ValueBox.h"
#include "feature/display/Display.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/net/Net.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::stock {

namespace items   = common::items;
namespace screens = common::screens;
namespace display = feature::display;

/// config/stock.json: Create's CLogistics keys used by the stock network.
struct StockConfig {
    int version           = 1;
    int factoryGaugeTimer = 100; ///< ticks between a gauge's requests (Create's default, min 5)
};
LV_REGISTER_CONFIG("stock", StockConfig);

namespace {
constexpr char kScreen[]     = "stock.gauge";
constexpr char kItemScreen[] = "stock.gauge_item";

int requestInterval() { return std::max(5, config::get<StockConfig>().factoryGaugeTimer); }

std::string slotKey(int slot) { return std::to_string(std::clamp(slot, 0, 3)); }

Json readPanel(GlobalPos const& p, int slot) {
    auto const* d = dataOf(p);
    if (!d) return Json::object();
    auto it = d->find("panels");
    if (it == d->end() || !it->is_object()) return Json::object();
    auto s = it->find(slotKey(slot));
    return s == it->end() || !s->is_object() ? Json::object() : *s;
}

void writePanel(GlobalPos const& p, int slot, Json const& panel) {
    auto before = readPanel(p, slot);
    bool itemChanged = before.value("filter", Json()) != panel.value("filter", Json()) || before.value("active", false) != panel.value("active", false);
    auto& d      = editData(p);
    auto& panels = d["panels"];
    if (!panels.is_object()) panels = Json::object();
    if (panel.is_null() || panel.empty()) panels.erase(slotKey(slot));
    else panels[slotKey(slot)] = panel;
    if (itemChanged) itemrender::refresh(p); // the client mod draws the new monitored item
}

bool active(Json const& panel) { return panel.value("active", false); }
bool isGauge(GlobalPos const& p) { return idAt(p) == kFactoryGauge; }

/// Runtime state per panel (not saved: Create's timer and last reports are sent to clients only).
struct Runtime {
    int              timer = 0;
    cs::PromiseQueue restock;
    bool             restockLoaded = false;
    bool             restockDirty  = false;
};
std::map<std::pair<GlobalPos, int>, Runtime>& runtime() {
    static std::map<std::pair<GlobalPos, int>, Runtime> m;
    return m;
}
Runtime& rt(GlobalPos const& p, int slot) {
    auto& r = runtime()[{p, slot}];
    if (!r.restockLoaded) {
        r.restock           = cs::PromiseQueue::fromJson(readPanel(p, slot).value("restockPromises", Json::array()));
        r.restock.onChanged = [&r] { r.restockDirty = true; };
        r.restockLoaded     = true;
    }
    return r;
}

/// Pending gauge connections per player (Create's FactoryPanelConnectionHandler.connectingFrom).
std::map<std::string, std::pair<GlobalPos, int>>& connecting() {
    static std::map<std::string, std::pair<GlobalPos, int>> m;
    return m;
}

std::optional<GlobalPos> restockedPackager(GlobalPos const& gauge) {
    auto support = supportOf(gauge);
    if (!packagerAt(support)) return std::nullopt;
    return support;
}
bool isRestocker(GlobalPos const& gauge) { return restockedPackager(gauge).has_value(); }

int maxStackOf(ItemStack const& s) { return std::max<int>(1, s.getMaxStackSize()); }

int levelInStorage(GlobalPos const& p, Json const& panel) {
    auto filter = stackFromJson(panel.value("filter", Json()));
    if (items::isEmpty(filter)) return 0;
    if (auto pkg = restockedPackager(p)) {
        auto s = packagerStock(*pkg);
        return s ? s->countOf(filter) : 0;
    }
    return summaryOf(panel.value("freq", std::string{}), false).countOf(filter);
}

int promisedOf(GlobalPos const& p, int slot, Json const& panel, bool forceClear) {
    auto filter = stackFromJson(panel.value("filter", Json()));
    if (items::isEmpty(filter)) return 0;
    auto key    = itemKey(filter);
    int  expiry = cs::promiseExpiryTicks(panel.value("promiseInterval", -1));
    if (isRestocker(p)) {
        auto& r = rt(p, slot);
        if (forceClear) r.restock.forceClear(key);
        return r.restock.totalPromisedAndRemoveExpired(key, expiry);
    }
    auto* net = networkInfo(panel.value("freq", std::string{}));
    if (!net) return 0;
    if (forceClear) net->promises.forceClear(key);
    return net->promises.totalPromisedAndRemoveExpired(key, expiry);
}

int unloadedLinks(GlobalPos const& p, Json const& panel) {
    if (isRestocker(p)) return restockedPackager(p) ? 0 : 1;
    return unloadedLinkCount(panel.value("freq", std::string{}));
}

void updatePanelsState(GlobalPos const& p) {
    int mask = 0;
    for (int s = 0; s < 4; ++s)
        if (active(readPanel(p, s))) mask |= 1 << s;
    setStates(p, {{kStatePanels, mask}});
}

std::string itemName(Json const& j) {
    auto s = stackFromJson(j);
    return items::isEmpty(s) ? std::string{} : items::displayName(s);
}

/// Create's FactoryPanelBehaviour.getLabel (in `player`'s language).
std::string labelOf(Player const& player, GlobalPos const& p, Json const& panel) {
    auto filter    = stackFromJson(panel.value("filter", Json()));
    int  count     = panel.value("count", 0);
    bool hasInputs = !panel.value("targetedBy", Json::array()).empty();
    bool missingAddress = (hasInputs || isRestocker(p)) && count != 0 && panel.value("address", std::string{}).find_first_not_of(' ') == std::string::npos;
    if (hasInputs && count == 0) return "§c" + trFor(player, "create.gui.factory_panel.no_target_amount_set", "Inactive: no target amount set");
    if (missingAddress) return "§c" + trFor(player, "create.gui.factory_panel.address_missing", "Inactive: missing a target address");
    if (items::isEmpty(filter)) return trFor(player, "create.factory_panel.new_factory_task", "New factory task");
    if (panel.value("waiting", false)) return trFor(player, "create.factory_panel.some_links_unloaded", "Some links are not loaded");
    auto name = items::displayName(filter);
    if (count == 0 || !hasInputs) return name;
    if (panel.value("paused", false)) return name + " " + trFor(player, "create.factory_panel.redstone_paused", "(Redstone Paused)");
    if (!panel.value("satisfied", false)) return name + " " + trFor(player, "create.factory_panel.in_progress", "(In Progress)");
    return name;
}

/// Create's getCountLabelForValueBox: "in storage / target".
std::string countLabel(GlobalPos const& p, int slot, Json const& panel) {
    auto filter = stackFromJson(panel.value("filter", Json()));
    if (items::isEmpty(filter)) return {};
    if (panel.value("waiting", false)) return "?";
    bool upTo    = panel.value("upTo", true);
    int  level   = levelInStorage(p, panel);
    bool inf     = level >= kInfinite;
    int  shown   = level / (upTo ? 1 : maxStackOf(filter));
    int  count   = panel.value("count", 0);
    std::string stacks = upTo ? "" : "▤";
    if (count == 0) return inf ? "∞" : std::to_string(shown) + stacks;
    int  promised = promisedOf(p, slot, panel, false);
    auto colour   = panel.value("satisfied", false) ? "§a" : panel.value("promisedSatisfied", false) ? "§6" : "§c";
    return std::string(colour) + (inf ? "∞" : std::to_string(shown) + stacks) + (promised ? "⏶" : "") + "§f/" + std::to_string(count) + stacks;
}

/// The items of a panel's inputs, in connection order.
std::vector<ItemStack> inputItems(Json const& panel) {
    std::vector<ItemStack> out;
    for (auto const& c : panel.value("targetedBy", Json::array())) {
        auto from = posFrom(c.value("pos", Json()));
        out.push_back(from ? stackFromJson(readPanel(*from, c.value("slot", 0)).value("filter", Json())) : items::empty());
    }
    return out;
}

/// The crafting recipe a panel could use (non-restockers with inputs only).
std::optional<CraftingArrangement> panelRecipe(GlobalPos const& p, Json const& panel) {
    if (isRestocker(p)) return std::nullopt;
    auto inputs = inputItems(panel);
    for (auto const& s : inputs)
        if (items::isEmpty(s)) return std::nullopt;
    return craftingArrangement(stackFromJson(panel.value("filter", Json())), inputs);
}
// ---- ticking (Create's tickStorageMonitor / tickRequests / tryRestock) -------------------------

void tickStorageMonitor(GlobalPos const& p, int slot, Json& panel, bool& changed) {
    auto filter   = stackFromJson(panel.value("filter", Json()));
    int  unloaded = unloadedLinks(p, panel);
    if (!isRestocker(p) && unloaded == 0 && panel.value("lastUnloaded", 0) != 0) invalidateSummaries();
    int  inStorage = levelInStorage(p, panel);
    int  promised  = promisedOf(p, slot, panel, false);
    int  demand    = items::isEmpty(filter) ? 0 : cs::demandOf(panel.value("count", 0), panel.value("upTo", true), maxStackOf(filter));
    bool satisfy   = items::isEmpty(filter) || inStorage >= demand;
    bool promiseSatisfy = items::isEmpty(filter) || static_cast<long long>(inStorage) + promised >= demand;
    bool wait      = unloaded > 0;
    if (panel.value("lastLevel", -1) == inStorage && panel.value("lastPromised", -1) == promised && panel.value("lastUnloaded", -1) == unloaded
        && panel.value("satisfied", false) == satisfy && panel.value("promisedSatisfied", false) == promiseSatisfy && panel.value("waiting", false) == wait)
        return;
    if (!panel.value("satisfied", false) && satisfy && demand > 0) {
        playSound(p, "levitate.stock.confirm", 0.075f, 1.0f);
        log().info("stock: factory gauge at {} {} {} panel {} satisfied ({} of {})", p.x, p.y, p.z, slot, inStorage, demand);
    }
    panel["lastLevel"]         = inStorage;
    panel["lastPromised"]      = promised;
    panel["lastUnloaded"]      = unloaded;
    panel["satisfied"]         = satisfy;
    panel["promisedSatisfied"] = promiseSatisfy;
    panel["waiting"]           = wait;
    changed                    = true;
}

void tryRestock(GlobalPos const& p, int slot, Json& panel) {
    auto filter = stackFromJson(panel.value("filter", Json()));
    if (items::isEmpty(filter)) return;
    auto pkg = restockedPackager(p);
    if (!pkg) return;
    auto inv       = packagerInventory(*pkg);
    auto network   = panel.value("freq", std::string{});
    int  available = stockOfItem(network, filter, inv);
    if (available == 0) return;
    int inStorage = levelInStorage(p, panel);
    int promised  = promisedOf(p, slot, panel, false);
    int demand    = cs::demandOf(panel.value("count", 0), panel.value("upTo", true), maxStackOf(filter));
    int amount    = cs::restockAmount(demand, promised, inStorage, maxStackOf(filter), available);
    if (amount <= 0) return;
    if (!requestOrder(network, RequestType::Restock, {{filter, amount}}, panel.value("address", std::string{}), inv)) return;
    rt(p, slot).restock.add(itemKey(filter), amount);
    log().info("stock: factory gauge at {} {} {} restocks {} x{} to '{}'", p.x, p.y, p.z, items::idOf(filter), amount, panel.value("address", std::string{}));
}

void tickRequests(GlobalPos const& p, int slot, Json& panel) {
    bool restocker = isRestocker(p);
    auto inputs    = panel.value("targetedBy", Json::array());
    if (inputs.empty() && !restocker) return;
    if (restocker) rt(p, slot).restock.tick();
    if (panel.value("satisfied", false) || panel.value("promisedSatisfied", false) || panel.value("waiting", false) || panel.value("paused", false)) return;
    auto& r = rt(p, slot);
    if (r.timer > 0) {
        r.timer = std::min(r.timer, requestInterval());
        --r.timer;
        return;
    }
    r.timer = requestInterval();
    auto address = panel.value("address", std::string{});
    if (address.find_first_not_of(' ') == std::string::npos) return;
    if (restocker) {
        tryRestock(p, slot, panel);
        return;
    }
    // the inputs, grouped by network and item (Create's ItemStackConnections)
    struct Need {
        ItemStack item;
        int       total = 0;
    };
    std::map<std::string, std::map<std::string, Need>> byNetwork;
    for (auto const& c : inputs) {
        auto from = posFrom(c.value("pos", Json()));
        if (!from) return;
        if (!blockAt(*from)) return;
        auto source = readPanel(*from, c.value("slot", 0));
        if (!active(source)) return;
        auto item = stackFromJson(source.value("filter", Json()));
        auto& need = byNetwork[source.value("freq", std::string{})][itemKey(item)];
        need.item  = item;
        need.total += c.value("amount", 1);
    }
    bool failed = false;
    std::vector<std::pair<std::string, std::vector<BigStack>>> orders;
    for (auto const& [network, needs] : byNetwork) {
        auto const& stock = summaryOf(network, true);
        std::vector<BigStack> order;
        for (auto const& [key, need] : needs) {
            if (need.total == 0 || items::isEmpty(need.item) || stock.countOf(need.item) < need.total) {
                failed = true;
                continue;
            }
            order.push_back({need.item, need.total});
        }
        orders.emplace_back(network, std::move(order));
    }
    if (failed) return;
    // "Use Mechanical Crafting": the inputs travel as one package per craft in the recipe's grid
    // (Create's PackageOrderWithCrafts.singleRecipe)
    Json crafts = Json::array();
    if (panel.contains("craft") && !panelRecipe(p, panel)) {
        // the inputs changed (disconnected, other items): the arrangement no longer applies
        panel.erase("craft");
        writePanel(p, slot, panel);
        log().info("stock: gauge panel at {} {} {} slot {} no longer uses mechanical crafting (inputs changed)", p.x, p.y, p.z, slot);
    }
    if (auto craft = panel.find("craft"); craft != panel.end() && craft->is_array() && craft->size() == 9) {
        Json pattern = Json::array();
        for (auto const& cell : *craft) pattern.push_back({{"item", cell}, {"count", cell.is_null() ? 0 : 1}});
        crafts.push_back({{"pattern", pattern}, {"count", 1}});
    }
    // Create checks every packager before sending anything; a busy one cancels the request
    for (auto const& [network, order] : orders)
        if (!requestOrder(network, RequestType::Restock, order, address, {}, crafts)) return;
    // keep the output promise
    if (auto* net = networkInfo(panel.value("freq", std::string{}))) {
        auto filter = stackFromJson(panel.value("filter", Json()));
        net->promises.add(itemKey(filter), std::max(1, panel.value("output", 1)));
    }
    advancements::awardOwner(p, "factory_gauge"); // Create's FactoryPanelBehaviour#tryRestock
    log().info("stock: factory gauge at {} {} {} panel {} requested its ingredients to '{}'", p.x, p.y, p.z, slot, address);
}

// ---- screens -------------------------------------------------------------------------------------

std::optional<std::pair<GlobalPos, int>> panelFrom(Player& player, Json const& ctx) {
    auto p    = posFrom(ctx.value("pos", Json()));
    int  slot = ctx.value("slot", -1);
    if (!p || slot < 0 || slot > 3 || !isGauge(*p) || !inReach(player, *p)) return std::nullopt;
    if (!active(readPanel(*p, slot))) return std::nullopt;
    return std::pair{*p, slot};
}

Json screenData(Player& player, GlobalPos const& p, int slot) {
    auto panel  = readPanel(p, slot);
    Json inputs = Json::array();
    for (auto const& c : panel.value("targetedBy", Json::array())) {
        auto from   = posFrom(c.value("pos", Json()));
        auto source = from ? readPanel(*from, c.value("slot", 0)) : Json::object();
        inputs.push_back({{"pos", c.value("pos", Json())}, {"slot", c.value("slot", 0)}, {"amount", c.value("amount", 1)}, {"item", source.value("filter", Json())}});
    }
    return {
        {"pos",             posJson(p)                                         },
        {"slot",            slot                                               },
        {"item",            panel.value("filter", Json())                       },
        {"count",           panel.value("count", 0)                             },
        {"upTo",            panel.value("upTo", true)                           },
        {"address",         panel.value("address", std::string{})               },
        {"output",          panel.value("output", 1)                            },
        {"promiseInterval", panel.value("promiseInterval", -1)                  },
        {"inputs",          inputs                                             },
        {"restocker",       isRestocker(p)                                     },
        {"promised",        promisedOf(p, slot, panel, false)                  },
        {"inStorage",       levelInStorage(p, panel)                           },
        {"label",           labelOf(player, p, panel)                          },
        {"craftable",       panelRecipe(p, panel).has_value()                  },
        {"crafting",        panel.contains("craft") && panelRecipe(p, panel).has_value()},
    };
}


/// Create's FactoryPanelConfigurationPacket.
void configure(Player& player, GlobalPos const& p, int slot, Json const& a) {
    auto panel = readPanel(p, slot);
    if (a.contains("crafting") && a["crafting"].is_boolean()) {
        auto recipe = a["crafting"].get<bool>() ? panelRecipe(p, panel) : std::nullopt;
        if (recipe) {
            // the arrangement and, as Create's screen does, the amounts the recipe uses
            auto inputs  = panel.value("targetedBy", Json::array());
            Json cells   = Json::array();
            std::vector<int> uses(inputs.size(), 0);
            for (int i : recipe->pattern) {
                if (i < 0 || static_cast<std::size_t>(i) >= inputs.size()) {
                    cells.push_back(nullptr);
                    continue;
                }
                auto from = posFrom(inputs[static_cast<std::size_t>(i)].value("pos", Json()));
                cells.push_back(from ? readPanel(*from, inputs[static_cast<std::size_t>(i)].value("slot", 0)).value("filter", Json()) : Json());
                ++uses[static_cast<std::size_t>(i)];
            }
            for (std::size_t i = 0; i < inputs.size(); ++i) inputs[i]["amount"] = std::max(1, uses[i]);
            panel["targetedBy"] = inputs;
            panel["craft"]      = cells;
            panel["output"]     = recipe->resultCount;
            log().info("stock: gauge panel at {} {} {} slot {} uses mechanical crafting ({})", p.x, p.y, p.z, slot, recipe->recipeId);
        } else {
            panel.erase("craft");
        }
    }
    if (a.contains("address") && a["address"].is_string()) panel["address"] = a["address"].get<std::string>().substr(0, 64);
    // with mechanical crafting the recipe sets the amounts (Create locks them)
    bool crafting = panel.contains("craft");
    if (a.contains("output") && a["output"].is_number_integer() && !crafting) panel["output"] = std::clamp(a["output"].get<int>(), 1, 64);
    if (a.contains("promiseInterval") && a["promiseInterval"].is_number_integer()) panel["promiseInterval"] = std::clamp(a["promiseInterval"].get<int>(), -1, 31);
    if (a.contains("amounts") && a["amounts"].is_array() && !crafting) {
        auto& inputs  = panel["targetedBy"];
        auto  amounts = a["amounts"];
        if (inputs.is_array())
            for (std::size_t i = 0; i < inputs.size() && i < amounts.size(); ++i)
                if (amounts[i].is_number_integer()) inputs[i]["amount"] = std::clamp(amounts[i].get<int>(), 1, 64);
    }
    writePanel(p, slot, panel);
    if (a.value("clearPromises", false)) {
        promisedOf(p, slot, panel, true);
        rt(p, slot).timer = requestInterval() / 2;
    }
    if (auto remove = posFrom(a.value("disconnect", Json()))) {
        int  rslot  = a.value("disconnectSlot", 0);
        auto inputs = panel.value("targetedBy", Json::array());
        Json kept   = Json::array();
        for (auto const& c : inputs)
            if (!(posFrom(c.value("pos", Json())) == remove && c.value("slot", 0) == rslot)) kept.push_back(c);
        panel["targetedBy"] = kept;
        panel.erase("craft"); // Create resets the crafting arrangement with the inputs
        writePanel(p, slot, panel);
        auto source    = readPanel(*remove, rslot);
        Json targeting = Json::array();
        for (auto const& t : source.value("targeting", Json::array()))
            if (!(posFrom(t.value("pos", Json())) == p && t.value("slot", 0) == slot)) targeting.push_back(t);
        source["targeting"] = targeting;
        if (active(source)) writePanel(*remove, rslot, source);
        log().info("stock: {} disconnected a gauge input at {} {} {}", player.getRealName(), p.x, p.y, p.z);
    }
    if (a.value("reset", false)) {
        // Create's disable() + enable(): every setting back to default, the network kept
        removeGaugePanel(p, slot, nullptr);
        placeGaugePanel(p, slot, panel.value("freq", std::string{}), nullptr);
        log().info("stock: {} reset the gauge panel at {} {} {}", player.getRealName(), p.x, p.y, p.z);
        return;
    }
    log().info("stock: {} configured the gauge panel at {} {} {} slot {}", player.getRealName(), p.x, p.y, p.z, slot);
}

void action(Player& player, Json const& ctx, Json const& a) {
    auto pp = panelFrom(player, ctx);
    if (!pp) return;
    auto [p, slot] = *pp;
    if (!mayInteract(readPanel(p, slot).value("freq", std::string{}), player)) return;
    auto op = a.value("op", std::string{});
    if (op == "set") configure(player, p, slot, a);
    else if (op == "connect") {
        screens::close(player, kScreen);
        startConnection(player, p, slot);
    } else if (op == "address_form") {
        auto                 panel     = readPanel(p, slot);
        bool                 restocker = isRestocker(p);
        ll::form::CustomForm form(trFor(player, restocker ? "create.gui.factory_panel.restocker_address" : "create.gui.factory_panel.recipe_address", restocker ? "Send items to..." : "Send inputs to..."));
        form.appendInput(
            "address",
            restocker ? trFor(player, "create.gui.factory_panel.restocker_address_tip", "Enter the address that will") + " " + trFor(player, "create.gui.factory_panel.restocker_address_tip_1", "cause packages to arrive here.")
                      : trFor(player, "create.gui.factory_panel.recipe_address_tip", "Enter the address where") + " " + trFor(player, "create.gui.factory_panel.recipe_address_tip_1", "this recipe is crafted."),
            "",
            panel.value("address", std::string{})
        );
        form.sendTo(player, [p, slot](Player& q, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
            if (!r || !isGauge(p)) return;
            if (auto it = r->find("address"); it != r->end())
                if (auto const* s = std::get_if<std::string>(&it->second)) configure(q, p, slot, {{"address", *s}});
            screens::open(q, kScreen, {{"pos", posJson(p)}, {"slot", slot}});
        });
    }
}

void form(Player& player, Json const& ctx) {
    auto pp = panelFrom(player, ctx);
    if (!pp) return;
    auto [p, slot] = *pp;
    auto data      = screenData(player, p, slot);
    bool restocker = data.value("restocker", false);
    ll::form::CustomForm f(trFor(player, restocker ? "create.gui.factory_panel.title_as_restocker" : "create.gui.factory_panel.title_as_recipe", restocker ? "Restocker Settings" : "Recipe Settings"));
    f.appendLabel(data.value("label", std::string{}));
    auto inputs = data["inputs"];
    if (!restocker) {
        if (inputs.empty())
            f.appendLabel(trFor(player, "create.gui.factory_panel.unconfigured_input", "Recipe ingredients") + ": " + trFor(player, "create.gui.factory_panel.unconfigured_input_tip", "Ingredients can be added by") + " "
                          + trFor(player, "create.gui.factory_panel.unconfigured_input_tip_1", "connecting other factory gauges"));
        for (std::size_t i = 0; i < inputs.size(); ++i)
            f.appendSlider("in" + std::to_string(i), itemName(inputs[i].value("item", Json())), 1, 64, 1, inputs[i].value("amount", 1));
        f.appendSlider("output", trFor(player, "create.gui.factory_panel.expected_output", "Expecting {}", itemName(data.value("item", Json()))), 1, 64, 1, data.value("output", 1));
        if (data.value("craftable", false))
            f.appendToggle("crafting", trFor(player, "create.gui.factory_panel.activate_crafting", "Use Mechanical Crafting"), data.value("crafting", false));
    }
    f.appendInput(
        "address",
        trFor(player, restocker ? "create.gui.factory_panel.restocker_address" : "create.gui.factory_panel.recipe_address", restocker ? "Send items to..." : "Send inputs to..."),
        "",
        data.value("address", std::string{})
    );
    std::vector<std::string> expiry{trFor(player, "create.gui.factory_panel.promises_do_not_expire", "Promises do not expire"), "30s"};
    for (int m = 1; m <= 31; ++m) expiry.push_back(std::to_string(m) + "m");
    f.appendStepSlider("expiry", trFor(player, "create.gui.factory_panel.promises_expire_title", "Promises expire after"), expiry, static_cast<std::size_t>(data.value("promiseInterval", -1) + 1));
    f.appendToggle("clear", trFor(player, "create.gui.factory_panel.left_click_reset", "Left-Click to reset") + " (" + trFor(player, "create.gui.factory_panel.promised_items", "Promised items") + ")", false);
    std::vector<std::string> extra{trFor(player, "create.gui.factory_panel.save_and_close", "Save and close")};
    if (!restocker) extra.push_back(trFor(player, "create.gui.factory_panel.connect_input", "Add new connection"));
    extra.push_back(trFor(player, "create.gui.factory_panel.reset", "Reset all settings"));
    f.appendDropdown("then", trFor(player, "lv.stock.form.then", "Then"), extra, 0);
    std::size_t n = inputs.size();
    f.sendTo(player, [p, slot, n, restocker](Player& q, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r || !isGauge(p) || !active(readPanel(p, slot)) || !mayInteract(readPanel(p, slot).value("freq", std::string{}), q)) return;
        auto num = [&](std::string const& k, int fallback) {
            auto it = r->find(k);
            if (it == r->end()) return fallback;
            if (auto const* d = std::get_if<double>(&it->second)) return static_cast<int>(std::lround(*d));
            if (auto const* u = std::get_if<uint64>(&it->second)) return static_cast<int>(*u);
            return fallback;
        };
        Json a{{"op", "set"}};
        Json amounts = Json::array();
        for (std::size_t i = 0; i < n; ++i) amounts.push_back(num("in" + std::to_string(i), 1));
        a["amounts"] = amounts;
        if (!restocker) a["output"] = num("output", 1);
        if (r->contains("crafting")) a["crafting"] = num("crafting", 0) != 0;
        a["promiseInterval"] = num("expiry", 0) - 1;
        a["clearPromises"]   = num("clear", 0) != 0;
        if (auto it = r->find("address"); it != r->end())
            if (auto const* s = std::get_if<std::string>(&it->second)) a["address"] = *s;
        int then = num("then", 0);
        if (then == (restocker ? 1 : 2)) a["reset"] = true;
        configure(q, p, slot, a);
        if (!restocker && then == 1) startConnection(q, p, slot);
    });
}

// ---- the item picker (Create's FactoryPanelSetItemScreen) ---------------------------------------

std::optional<std::pair<GlobalPos, int>> emptyPanelFrom(Player& player, Json const& ctx) {
    auto p    = posFrom(ctx.value("pos", Json()));
    int  slot = ctx.value("slot", -1);
    if (!p || slot < 0 || slot > 3 || !isGauge(*p) || !inReach(player, *p)) return std::nullopt;
    auto panel = readPanel(*p, slot);
    if (!active(panel) || !items::isEmpty(stackFromJson(panel.value("filter", Json())))) return std::nullopt;
    return std::pair{*p, slot};
}

bool setPanelItem(Player& player, GlobalPos const& p, int slot, ItemStack const& stack) {
    // Create's FactoryPanelBehaviour.setFilter: filter items can't be monitored
    if (items::isEmpty(stack) || common::filter::isFilterItem(stack)) return false;
    auto panel      = readPanel(p, slot);
    panel["filter"] = stackToJson(items::withCount(stack, 1));
    writePanel(p, slot, panel);
    log().info("stock: {} set the gauge panel at {} {} {} slot {} to {}", player.getRealName(), p.x, p.y, p.z, slot, items::idOf(stack));
    return true;
}
} // namespace

// ---- public ------------------------------------------------------------------------------------

void resetGaugeRuntime() {
    runtime().clear();
    connecting().clear();
}

int targetedSlotAt(GlobalPos const& p, double lx, double ly, double lz) {
    return static_cast<int>(cs::targetedSlot(faceOf(p), facingOf(p), {lx, ly, lz}));
}

bool placeGaugePanel(GlobalPos const& p, int slot, std::string const& network, Player* player) {
    if (network.empty() || slot < 0 || slot > 3) return false;
    if (active(readPanel(p, slot))) return false;
    Json panel{
        {"active", true}, {"freq", network}, {"filter", nullptr}, {"count", 0}, {"upTo", true}, {"address", ""}, {"output", 1},
        {"promiseInterval", -1}, {"targetedBy", Json::array()}, {"targeting", Json::array()},
    };
    writePanel(p, slot, panel);
    runtime().erase({p, slot});
    registry().gauges.insert(p);
    saveRegistry();
    updatePanelsState(p);
    log().info("stock: gauge panel placed at {} {} {} slot {} by {}", p.x, p.y, p.z, slot, player ? player->getRealName() : std::string("-"));
    return true;
}

namespace {
/// Removes a panel's connections from the panels it was connected to.
void disconnectAll(GlobalPos const& p, int slot, Json const& panel) {
    for (auto const& c : panel.value("targetedBy", Json::array())) {
        auto from = posFrom(c.value("pos", Json()));
        if (!from || !blockAt(*from)) continue;
        int  fs  = c.value("slot", 0);
        auto src = readPanel(*from, fs);
        if (!active(src)) continue;
        Json kept = Json::array();
        for (auto const& t : src.value("targeting", Json::array()))
            if (!(posFrom(t.value("pos", Json())) == p && t.value("slot", 0) == slot)) kept.push_back(t);
        src["targeting"] = kept;
        writePanel(*from, fs, src);
    }
    for (auto const& t : panel.value("targeting", Json::array())) {
        auto to = posFrom(t.value("pos", Json()));
        if (!to || !blockAt(*to)) continue;
        int  ts  = t.value("slot", 0);
        auto dst = readPanel(*to, ts);
        if (!active(dst)) continue;
        Json kept = Json::array();
        for (auto const& c : dst.value("targetedBy", Json::array()))
            if (!(posFrom(c.value("pos", Json())) == p && c.value("slot", 0) == slot)) kept.push_back(c);
        dst["targetedBy"] = kept;
        writePanel(*to, ts, dst);
    }
}
} // namespace

void removeGaugePanel(GlobalPos const& p, int slot, Player* player) {
    auto panel = readPanel(p, slot);
    if (!active(panel)) return;
    disconnectAll(p, slot, panel);
    writePanel(p, slot, Json::object());
    runtime().erase({p, slot});
    int left = 0;
    for (int s = 0; s < 4; ++s) left += active(readPanel(p, s)) ? 1 : 0;
    if (player && !player->isCreative()) {
        if (auto g = items::make(kFactoryGauge, 1)) giveOrDrop(*player, *g);
    }
    if (left == 0 && player) {
        if (auto* region = regionOf(p.dim)) game::clearBlock(*region, game::blockPos(p));
    } else if (left > 0) {
        updatePanelsState(p);
    }
    log().info("stock: gauge panel removed at {} {} {} slot {}", p.x, p.y, p.z, slot);
}

void gaugeRemoved(GlobalPos const& p, Json const& doc) {
    auto panels = doc.value("panels", Json::object());
    for (auto const& [k, v] : panels.items()) {
        if (!v.is_object() || !v.value("active", false)) continue;
        int slot = std::atoi(k.c_str());
        disconnectAll(p, slot, v);
        runtime().erase({p, slot});
    }
    log().info("stock: factory gauge removed at {} {} {}", p.x, p.y, p.z);
}

void startConnection(Player& player, GlobalPos const& p, int slot) {
    connecting()[uuidOf(player)] = {p, slot};
    statusKey(player, "create.factory_panel.click_second_panel", "Click a second gauge to connect...");
}

bool connectPanels(Player* player, GlobalPos const& target, int targetSlot, GlobalPos const& input, int inputSlot) {
    auto t = readPanel(target, targetSlot);
    auto i = readPanel(input, inputSlot);
    if (!active(t) || !active(i)) {
        if (player) statusKey(*player, "create.factory_panel.connection_aborted", "Input connection aborted", "§c");
        return false;
    }
    bool already = false;
    for (auto const& c : t.value("targetedBy", Json::array()))
        if (posFrom(c.value("pos", Json())) == input && c.value("slot", 0) == inputSlot) already = true;
    cs::PanelRef tr{target.x, target.y, target.z, faceOf(target), facingOf(target), isRestocker(target), !items::isEmpty(stackFromJson(t.value("filter", Json())))};
    cs::PanelRef ir{input.x, input.y, input.z, faceOf(input), facingOf(input), isRestocker(input), !items::isEmpty(stackFromJson(i.value("filter", Json())))};
    auto         issue = cs::connectionIssue(tr, ir, already, static_cast<int>(t.value("targetedBy", Json::array()).size()));
    if (!issue && target.dim != input.dim) issue = "create.factory_panel.too_far_apart";
    if (!issue && tr.restocker) issue = "create.factory_panel.input_in_restock_mode";
    if (issue) {
        if (player) {
            status(*player, "§c" + trFor(*player, *issue, *issue));
            playSound(target, "levitate.stock.deny", 1.0f, 1.0f);
        }
        log().info("stock: gauge connection refused: {}", *issue);
        return false;
    }
    t["targetedBy"].push_back({{"pos", posJson(input)}, {"slot", inputSlot}, {"amount", 1}, {"arrow", 0}});
    writePanel(target, targetSlot, t);
    i = readPanel(input, inputSlot);
    if (!i["targeting"].is_array()) i["targeting"] = Json::array();
    i["targeting"].push_back({{"pos", posJson(target)}, {"slot", targetSlot}});
    writePanel(input, inputSlot, i);
    if (player)
        status(*player, "§a" + trFor(*player, "create.factory_panel.panels_connected", "Now using {} to create {}", itemName(i.value("filter", Json())), itemName(t.value("filter", Json()))));
    playSound(target, "levitate.stock.connect", 0.5f, 0.5f);
    log().info("stock: gauge at {} {} {} slot {} now takes input from {} {} {} slot {}", target.x, target.y, target.z, targetSlot, input.x, input.y, input.z, inputSlot);
    return true;
}

void useGauge(Player& player, GlobalPos const& p, int slot, bool sneaking) {
    auto panel   = readPanel(p, slot);
    auto network = panel.value("freq", std::string{});
    if (!mayInteract(network, player)) {
        statusKey(player, "create.logistically_linked.protected", "Logistics Network is Protected", "§c");
        return;
    }
    auto const& held = player.getSelectedItem();
    auto        hid  = items::isEmpty(held) ? std::string{} : items::idOf(held);
    // the wrench cycles the arrow path of the panel's connections
    auto targeting = panel.value("targeting", Json::array());
    if (hid == kWrench && !targeting.empty()) {
        int shared = -1;
        for (auto const& tj : targeting) {
            auto to = posFrom(tj.value("pos", Json()));
            if (!to) continue;
            int  ts  = tj.value("slot", 0);
            auto dst = readPanel(*to, ts);
            for (auto& c : dst["targetedBy"]) {
                if (!(posFrom(c.value("pos", Json())) == p && c.value("slot", 0) == slot)) continue;
                if (shared == -1) shared = (c.value("arrow", 0) + 1) % 4;
                c["arrow"] = shared;
            }
            writePanel(*to, ts, dst);
        }
        if (shared >= 0) {
            std::string boxes;
            for (int i = 0; i < 4; ++i) boxes += i == shared ? "■" : "□";
            status(player, trFor(player, "create.factory_panel.cycled_arrow_path", "Cycled arrow pathing mode {}", boxes));
            return;
        }
    }
    // a pending connection from another panel
    if (auto it = connecting().find(uuidOf(player)); it != connecting().end()) {
        auto [from, fromSlot] = it->second;
        connecting().erase(it);
        if (from == p && fromSlot == slot) {
            status(player, "");
            return;
        }
        (void)connectPanels(&player, from, fromSlot, p, slot);
        return;
    }
    auto filter = stackFromJson(panel.value("filter", Json()));
    if (items::isEmpty(filter)) {
        if (hid.empty()) {
            screens::open(player, kItemScreen, {{"pos", posJson(p)}, {"slot", slot}});
            return;
        }
        if (!setPanelItem(player, p, slot, held)) statusKey(player, "create.logistics.filter.invalid_item", "Cannot use this item here.", "§c");
        return;
    }
    if (isLinkedItem(hid)) {
        ItemStack h = held;
        tuneItem(h, network);
        player.setSelectedItem(h);
        player.sendInventory(false);
        statusKey(player, "create.logistically_linked.tuned", "Tuned to this network");
        return;
    }
    std::string box = "panel" + std::to_string(slot);
    if (sneaking && hid.empty()) {
        // vanilla players' "click and hold": sneak-click opens the target amount board
        common::valuebox::openEditor(player, p, box);
        return;
    }
    // Create's FactoryPanelBehaviour: holding right-click opens the target amount board, a
    // short click the panel's screen (client mod players; the others get the screen at once)
    if (hid.empty()) {
        auto ctx = Json{{"pos", posJson(p)}, {"slot", slot}};
        if (common::valuebox::awaitHold(
                player,
                p,
                [ctx](Player& q) { screens::open(q, kScreen, ctx); },
                [p, box](Player& q) { common::valuebox::openEditor(q, p, box); }
            ))
            return;
    }
    screens::open(player, kScreen, {{"pos", posJson(p)}, {"slot", slot}});
}

// ---- client mod visuals (Create's FactoryPanelRenderer) ------------------------------------------

namespace {

/// The panel's normal (away from its support) as a Bedrock face id.
int panelFace(GlobalPos const& p) {
    auto s = cs::supportOffset(faceOf(p), facingOf(p));
    return s[1] > 0 ? 0 : s[1] < 0 ? 1 : s[2] > 0 ? 2 : s[2] < 0 ? 3 : s[0] > 0 ? 4 : 5;
}

/// The monitored item of each panel, in front of the panel (display items: no client code).
void panelItems(GlobalPos const& p, Block const&, std::vector<std::pair<std::int64_t, itemrender::DisplayItem>>& out) {
    auto s = cs::supportOffset(faceOf(p), facingOf(p));
    for (int slot = 0; slot < 4; ++slot) {
        auto panel = readPanel(p, slot);
        if (!active(panel)) continue;
        auto filter = stackFromJson(panel.value("filter", Json()));
        if (items::isEmpty(filter)) continue;
        auto                    c = cs::panelCentre(faceOf(p), facingOf(p), static_cast<cs::PanelSlot>(slot));
        itemrender::DisplayItem d;
        d.stack    = filter;
        d.at       = itemrender::facingFace(panelFace(p));
        d.at.x     = p.x + c.x - s[0] * .02;
        d.at.y     = p.y + c.y - s[1] * .02;
        d.at.z     = p.z + c.z - s[2] * .02;
        d.at.scale = .3f;
        out.emplace_back(slot, std::move(d));
    }
}

/// What the client mod draws on a gauge besides the items: per panel its count label and the
/// arrows from the panels it takes input from (Create's FactoryPanelRenderer paths).
Json gaugeVisual(GlobalPos const& p) {
    Json panels = Json::array();
    for (int slot = 0; slot < 4; ++slot) {
        auto panel = readPanel(p, slot);
        if (!active(panel)) continue;
        // the path colour: Create's satisfied / promised / missing states, grey without a target
        int  count  = panel.value("count", 0);
        auto colour = count == 0 ? 0x888898 : panel.value("satisfied", false) ? 0x9EF173 : panel.value("promisedSatisfied", false) ? 0xF2C14E : 0xE84D4D;
        Json from   = Json::array();
        for (auto const& c : panel.value("targetedBy", Json::array())) {
            auto at = posFrom(c.value("pos", Json()));
            if (!at || at->dim != p.dim) continue;
            from.push_back({{"pos", Json::array({at->x, at->y, at->z})}, {"slot", c.value("slot", 0)}, {"arrow", c.value("arrow", 0)}});
        }
        panels.push_back({{"slot", slot}, {"count", countLabel(p, slot, panel)}, {"colour", colour}, {"from", from}});
    }
    return {{"pos", Json::array({p.x, p.y, p.z})}, {"face", static_cast<int>(faceOf(p))}, {"facing", facingOf(p)}, {"panels", panels}};
}

struct VisualSync {
    std::map<GlobalPos, std::string>  sent;      ///< last payload per gauge
    std::map<std::int64_t, int>       companion; ///< companions -> dimension of their last full sync (-1: none)
};
VisualSync& visuals() {
    static VisualSync v;
    return v;
}

void sendVisuals(Player& player, int dim, Json const& gauges, bool reset) {
    if (gauges.empty() && !reset) return;
    net::send(player, "stock.gauge", {{"dim", dim}, {"reset", reset}, {"gauges", gauges}});
}

/// Every 10 ticks: changed gauges go to the companions of their dimension; a companion that
/// joined or changed dimension gets every gauge of it.
void syncVisuals() {
    auto& v     = visuals();
    auto  level = ll::service::getLevel();
    if (!level) return;
    bool anyCompanion = false;
    level->forEachPlayer([&](Player& pl) {
        anyCompanion = anyCompanion || net::isCompanion(pl);
        return !anyCompanion;
    });
    if (!anyCompanion) {
        v.sent.clear(); // a companion joining gets everything anyway
        return;
    }
    std::map<int, Json> changed;
    for (auto const& p : registry().gauges) {
        if (!blockAt(p) || !isGauge(p)) continue;
        auto payload = gaugeVisual(p);
        auto text    = payload.dump();
        auto it      = v.sent.find(p);
        if (it != v.sent.end() && it->second == text) continue;
        v.sent[p] = text;
        if (!changed.contains(p.dim)) changed[p.dim] = Json::array();
        changed[p.dim].push_back(payload);
    }
    // gauges that went: an empty panel list makes the client drop them
    for (auto it = v.sent.begin(); it != v.sent.end();) {
        if (registry().gauges.contains(it->first) && blockAt(it->first) && isGauge(it->first)) {
            ++it;
            continue;
        }
        changed[it->first.dim].push_back({{"pos", Json::array({it->first.x, it->first.y, it->first.z})}, {"panels", Json::array()}});
        it = v.sent.erase(it);
    }
    level->forEachPlayer([&](Player& pl) {
        if (!net::isCompanion(pl)) return true;
        int   dim = static_cast<int>(pl.getDimensionId());
        auto& d   = v.companion[pl.getOrCreateUniqueID().rawID];
        if (d != dim) {
            d        = dim;
            Json all = Json::array();
            for (auto const& p : registry().gauges)
                if (p.dim == dim && blockAt(p) && isGauge(p)) all.push_back(gaugeVisual(p));
            sendVisuals(pl, dim, all, true);
        } else if (auto it = changed.find(dim); it != changed.end()) {
            sendVisuals(pl, dim, it->second, false);
        }
        return true;
    });
}

} // namespace

void tickGauges() {
    if (currentTick() % 10 == 0) syncVisuals();
    std::vector<GlobalPos> gone;
    for (auto const& p : registry().gauges) {
        if (!blockAt(p)) continue;
        if (!isGauge(p)) {
            gone.push_back(p);
            continue;
        }
        for (int slot = 0; slot < 4; ++slot) {
            auto panel = readPanel(p, slot);
            if (!active(panel)) continue;
            bool changed = false;
            tickStorageMonitor(p, slot, panel, changed);
            Json before = panel.value("targetedBy", Json::array());
            tickRequests(p, slot, panel);
            auto& r = rt(p, slot);
            if (r.restockDirty || (currentTick() % 200 == 0 && !r.restock.empty())) {
                panel["restockPromises"] = r.restock.toJson();
                r.restockDirty           = false;
                changed                  = true;
            }
            if (changed) writePanel(p, slot, panel);
        }
    }
    for (auto const& p : gone) registry().gauges.erase(p);
    if (!gone.empty()) saveRegistry();
}

void restockerArrivals(GlobalPos const& packager, std::vector<cs::Entry> const& arrivals) {
    // Create's submitNewArrivals: restocker gauges on the packager get their promises fulfilled
    static int const dx[6]{0, 0, 0, 0, -1, 1}, dy[6]{-1, 1, 0, 0, 0, 0}, dz[6]{0, 0, -1, 1, 0, 0};
    for (int d = 0; d < 6; ++d) {
        auto g = offset(packager, dx[d], dy[d], dz[d]);
        if (!isGauge(g) || supportOf(g) != packager) continue;
        for (int slot = 0; slot < 4; ++slot) {
            if (!active(readPanel(g, slot))) continue;
            auto& r = rt(g, slot);
            for (auto const& a : arrivals) r.restock.itemEnteredSystem(a.key, a.count);
        }
    }
}

Json gaugeInfo(GlobalPos const& p, int slot) {
    auto panel = readPanel(p, slot);
    if (!active(panel)) return {{"active", false}};
    Json out           = panel;
    out["inStorage"]   = levelInStorage(p, panel);
    out["promised"]    = promisedOf(p, slot, panel, false);
    out["restocker"]   = isRestocker(p);
    return out;
}

std::optional<PanelState> panelAt(GlobalPos const& gauge, int slot) {
    if (!isGauge(gauge)) return std::nullopt;
    auto panel = readPanel(gauge, slot);
    if (!active(panel)) return std::nullopt;
    PanelState s;
    s.network           = panel.value("freq", std::string{});
    s.item              = stackFromJson(panel.value("filter", Json()));
    s.inStorage         = levelInStorage(gauge, panel);
    s.promised          = promisedOf(gauge, slot, panel, false);
    s.target            = panel.value("count", 0);
    s.upTo              = panel.value("upTo", true);
    s.satisfied         = panel.value("satisfied", false);
    s.promisedSatisfied = panel.value("promisedSatisfied", false);
    s.waiting           = panel.value("waiting", false);
    s.paused            = panel.value("paused", false);
    s.address           = panel.value("address", std::string{});
    return s;
}

void setPanelPaused(GlobalPos const& gauge, int slot, bool paused) {
    auto panel = readPanel(gauge, slot);
    if (!active(panel) || panel.value("paused", false) == paused) return;
    panel["paused"] = paused;
    writePanel(gauge, slot, panel);
    rt(gauge, slot).timer = 1;
}

void registerGaugeScreens() {
    static bool itemsRegistered = false;
    if (!itemsRegistered) {
        itemsRegistered = true;
        itemrender::registerProvider(kFactoryGauge, panelItems);
        net::onCompanionJoined([](Player& p) { visuals().companion[p.getOrCreateUniqueID().rawID] = -1; });
    }
    screens::registerType({
        .id   = kScreen,
        .data = [](Player& pl, Json const& ctx) -> std::optional<Json> {
            auto pp = panelFrom(pl, ctx);
            if (!pp) return std::nullopt;
            return screenData(pl, pp->first, pp->second);
        },
        .action = action,
        .form   = form,
    });
    screens::registerType({
        .id   = kItemScreen,
        .data = [](Player& pl, Json const& ctx) -> std::optional<Json> {
            auto pp = emptyPanelFrom(pl, ctx);
            if (!pp) return std::nullopt;
            return Json{{"pos", posJson(pp->first)}, {"slot", pp->second}};
        },
        .action =
            [](Player& pl, Json const& ctx, Json const& a) {
                auto pp = emptyPanelFrom(pl, ctx);
                if (!pp || a.value("op", std::string{}) != "set") return;
                if (setPanelItem(pl, pp->first, pp->second, stackFromJson(a.value("item", Json())))) screens::close(pl, kItemScreen);
            },
        .form =
            [](Player& pl, Json const& ctx) {
                auto pp = emptyPanelFrom(pl, ctx);
                if (!pp) return;
                std::vector<ItemStack>   choices;
                std::vector<std::string> names;
                auto&                    inv = pl.getInventory();
                for (int i = 0; i < inv.getContainerSize(); ++i) {
                    auto const& s = inv.getItem(i);
                    if (items::isEmpty(s) || common::filter::isFilterItem(s)) continue;
                    bool seen = std::any_of(choices.begin(), choices.end(), [&](ItemStack const& c) { return items::sameItemSameData(c, s); });
                    if (seen) continue;
                    choices.push_back(items::withCount(s, 1));
                    names.push_back(items::displayName(s));
                }
                ll::form::CustomForm f(trFor(pl, "create.gui.factory_panel.place_item_to_monitor", "Place Item to Monitor"));
                if (names.empty()) {
                    f.appendLabel(trFor(pl, "create.gui.factory_panel.empty_panel", "Empty panel"));
                    f.sendTo(pl);
                    return;
                }
                f.appendDropdown("item", trFor(pl, "create.gui.factory_panel.place_item_to_monitor", "Place Item to Monitor"), names, 0);
                auto at = *pp;
                f.sendTo(pl, [at, choices](Player& q, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
                    if (!r) return;
                    auto it = r->find("item");
                    if (it == r->end()) return;
                    if (auto const* v = std::get_if<uint64>(&it->second); v && *v < choices.size() && isGauge(at.first) && active(readPanel(at.first, at.second)))
                        setPanelItem(q, at.first, at.second, choices[*v]);
                });
            },
    });
    // the target amount board of each panel (opened by the gauge itself, so no box on the faces)
    for (int slot = 0; slot < 4; ++slot) {
        common::valuebox::registerBox(
            kFactoryGauge,
            {
                .id            = "panel" + std::to_string(slot),
                .label         = "create.factory_panel.target_amount",
                .labelFallback = "Target Amount in Storage",
                .board =
                    [](Block const&) {
                        core::common::BoardSpec b;
                        b.rows              = {tr("create.schedule.condition.threshold.items", "Items"), tr("create.schedule.condition.threshold.stacks", "Stacks")};
                        b.maxValue          = 100;
                        b.milestoneInterval = 10;
                        return b;
                    },
                .centre = [](Block const&, int) -> std::optional<core::common::Vec> { return std::nullopt; },
                .get =
                    [slot](GlobalPos const& p) {
                        auto panel = readPanel(p, slot);
                        return core::common::ValueSettings{panel.value("upTo", true) ? 0 : 1, panel.value("count", 0)};
                    },
                .set =
                    [slot](GlobalPos const& p, core::common::ValueSettings s, Player& editor) {
                        auto panel = readPanel(p, slot);
                        if (!active(panel) || !mayInteract(panel.value("freq", std::string{}), editor)) return;
                        panel["count"] = std::max(0, s.value);
                        panel["upTo"]  = s.row == 0;
                        writePanel(p, slot, panel);
                        rt(p, slot).timer = requestInterval() / 2;
                        log().info("stock: {} set the target of the gauge panel at {} {} {} slot {} to {}{}", editor.getRealName(), p.x, p.y, p.z, slot, s.value, s.row == 0 ? "" : " stacks");
                    },
                .format =
                    [](Block const&, core::common::ValueSettings s) {
                        if (s.value == 0) return tr("create.gui.factory_panel.inactive", " Inactive ");
                        return std::to_string(std::max(0, s.value)) + (s.row == 0 ? "" : "▤");
                    },
            }
        );
    }
    // Create's FactoryGaugeDisplaySource ("gauge_status"): a display link on the gauge lists its
    // panels as a value list (level in storage, then ✔ satisfied / ↑ promised / ▪ missing + item)
    display::registerSource(
        kFactoryGauge,
        display::Source{
            .id           = "gauge_status",
            .name         = "create.display_source.gauge_status",
            .nameFallback = "Factory gauge status",
            .text =
                [](display::Context& ctx, display::TargetStats const& stats) {
                    display::Lines out;
                    bool shortened = ctx.config.value("ValueDisplay", 0) == 0;
                    for (int slot = 0; slot < 4 && static_cast<int>(out.size()) < std::max(1, stats.maxRows); ++slot) {
                        auto panel = readPanel(ctx.source, slot);
                        if (!active(panel)) continue;
                        auto        filter = stackFromJson(panel.value("filter", Json()));
                        int         demand = items::isEmpty(filter) ? 0 : cs::demandOf(panel.value("count", 0), panel.value("upTo", true), maxStackOf(filter));
                        std::string mark   = " ";
                        if (demand != 0) mark = panel.value("satisfied", false) ? "✔" : promisedOf(ctx.source, slot, panel, false) ? "↑" : "▪";
                        out.push_back(display::formatValue(levelInStorage(ctx.source, panel), shortened) + " " + mark + " " + (items::isEmpty(filter) ? std::string{} : items::displayName(filter)));
                    }
                    return out;
                },
            .options = {display::Option{
                "ValueDisplay", "create.display_source.value_list.display", "Value Display",
                {"create.display_source.value_list.shortened", "create.display_source.value_list.full_number"}, {"Shortened", "Full Number"}, 0}},
            .multiLine = true,
        }
    );
    // the client mod's hover label for gauges and stock tickers
    net::on("stock.hover", [](Player& player, Json const& msg) {
        auto p = posFrom(msg.value("pos", Json()));
        if (!p || !inReach(player, *p, 16)) return;
        Json lines = Json::array();
        auto id    = idAt(*p);
        if (id == kFactoryGauge) {
            auto hit  = msg.value("hit", Json::array());
            int  slot = hit.is_array() && hit.size() == 3 ? targetedSlotAt(*p, hit[0].get<double>(), hit[1].get<double>(), hit[2].get<double>()) : 0;
            auto panel = readPanel(*p, slot);
            if (active(panel)) {
                lines.push_back(labelOf(player, *p, panel));
                auto c = countLabel(*p, slot, panel);
                if (!c.empty()) lines.push_back(c);
                auto filter = stackFromJson(panel.value("filter", Json()));
                lines.push_back("§7" + (items::isEmpty(filter) ? trFor(player, "create.logistics.filter.click_to_set", "Click with item to set") : trFor(player, "create.factory_panel.click_to_configure", "Click to configure")));
                if (!items::isEmpty(filter)) lines.push_back("§7" + trFor(player, "create.factory_panel.hold_to_set_amount", "Click and hold for target amount"));
            }
        } else if (id == kStockTicker) {
            auto pays = payments(*p);
            if (!pays.empty() && mayAdministrate(networkAt(*p), player)) {
                lines.push_back(trFor(player, "create.stock_ticker.contains_payments", "Contains payments:"));
                for (auto const& s : pays)
                    if (!items::isEmpty(s)) lines.push_back("§a" + items::displayName(s) + " x" + std::to_string(items::countOf(s)));
                lines.push_back("§7" + trFor(player, "create.stock_ticker.click_to_retrieve", "Click to retrieve"));
            }
        }
        net::send(player, "stock.hover", {{"pos", posJson(*p)}, {"lines", lines}});
    });
}

// ---- `/create stock gauges` (Levitate debug: a connected pair of gauges to look at) --------------

namespace {
void buildGaugeDemo(Player& player, CommandOutput& output) {
    auto* region = &player.getDimensionBlockSource();
    auto  feet   = player.getFeetBlockPos();
    GlobalPos a{static_cast<int>(player.getDimensionId()), feet.x + 2, feet.y, feet.z};
    GlobalPos b{a.dim, a.x + 1, a.y, a.z};
    std::string network = "levitate-gauge-demo";
    for (auto const& p : {a, b}) {
        for (int s = 0; s < 4; ++s) removeGaugePanel(p, s, nullptr);
        auto const* block = game::blockWith(kFactoryGauge, {{"levitate:face", 0}, {"levitate:facing", 2}});
        if (!block || !game::setBlock(*region, game::blockPos(p), *block)) return output.error("Could not place the gauges.");
    }
    // two inputs on the first gauge, the recipe panel on the second
    placeGaugePanel(a, 0, network, &player);
    placeGaugePanel(a, 3, network, &player);
    placeGaugePanel(b, 2, network, &player);
    auto make = [](char const* id) { return items::make(id).value_or(items::empty()); };
    setPanelItem(player, a, 0, make("minecraft:iron_ingot"));
    setPanelItem(player, a, 3, make("minecraft:gold_ingot"));
    setPanelItem(player, b, 2, make("minecraft:clock"));
    (void)connectPanels(nullptr, b, 2, a, 0);
    (void)connectPanels(nullptr, b, 2, a, 3);
    auto panel     = readPanel(b, 2);
    panel["count"] = 8;
    writePanel(b, 2, panel);
    log().info("stock: gauge demo built at {} {} {} for {}", a.x, a.y, a.z, player.getRealName());
    output.success("Built two connected factory gauges.");
}
} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload().text("stock").text("gauges").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.stock.need_op", "You need operator permission."));
        auto* player = commands::playerOf(origin);
        if (!player) return output.error(tr("lv.stock.need_player", "Run this as a player."));
        buildGaugeDemo(*player, output);
    });
});

} // namespace lv::feature::stock
