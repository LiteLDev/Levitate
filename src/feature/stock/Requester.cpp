// Stock unit: the redstone requester (Create's RedstoneRequesterBlock / BlockEntity / Screen):
// sends its programmed order when it receives a redstone pulse.

#include <algorithm>

#include "ll/api/form/CustomForm.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/stock/StockInternal.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::stock {

namespace items   = common::items;
namespace screens = common::screens;

namespace {
constexpr char kScreen[] = "stock.requester";

std::optional<GlobalPos> requesterFrom(Player& player, Json const& ctx) {
    auto p = posFrom(ctx.value("pos", Json()));
    if (!p || idAt(*p) != kRedstoneRequester || !inReach(player, *p)) return std::nullopt;
    return p;
}

Json screenData(GlobalPos const& p) {
    auto const* d = dataOf(p);
    Json        order = d ? d->value("order", Json::array()) : Json::array();
    return {
        {"pos",     posJson(p)                                                },
        {"order",   order                                                     },
        {"address", d ? d->value("address", std::string{}) : std::string{}    },
        {"partial", d ? d->value("partial", false) : false                    },
    };
}

/// Create's RedstoneRequesterConfigurationPacket: address, partial flag and amounts (1..256).
void configure(GlobalPos const& p, std::optional<std::string> const& address, std::optional<bool> partial, Json const& amounts) {
    auto& d = editData(p);
    if (address) d["address"] = address->substr(0, 64);
    if (partial) d["partial"] = *partial;
    if (amounts.is_array()) {
        auto& order = d["order"];
        if (order.is_array()) {
            for (std::size_t i = 0; i < order.size() && i < amounts.size(); ++i)
                if (amounts[i].is_number_integer()) order[i]["count"] = std::clamp(amounts[i].get<int>(), 1, 256);
        }
    }
}

void action(Player& player, Json const& ctx, Json const& a) {
    auto p = requesterFrom(player, ctx);
    if (!p || !mayInteract(networkAt(*p), player)) return;
    auto op = a.value("op", std::string{});
    if (op == "set") {
        std::optional<std::string> address;
        std::optional<bool>        partial;
        if (a.contains("address") && a["address"].is_string()) address = a["address"].get<std::string>();
        if (a.contains("partial") && a["partial"].is_boolean()) partial = a["partial"].get<bool>();
        configure(*p, address, partial, a.value("amounts", Json()));
        log().info("stock: {} configured the redstone requester at {} {} {}", player.getRealName(), p->x, p->y, p->z);
    } else if (op == "address_form") {
        ll::form::CustomForm form(trFor(player, "create.gui.redstone_requester.requester_address", "Send order to..."));
        auto const*          d = dataOf(*p);
        form.appendInput("address", trFor(player, "create.gui.redstone_requester.requester_address_tip", "Enter the address this") + " " + trFor(player, "create.gui.redstone_requester.requester_address_tip_1", "order should be send to."), "", d ? d->value("address", std::string{}) : std::string{});
        GlobalPos at = *p;
        form.sendTo(player, [at](Player& q, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
            if (!r || idAt(at) != kRedstoneRequester) return;
            if (auto it = r->find("address"); it != r->end())
                if (auto const* s = std::get_if<std::string>(&it->second)) configure(at, *s, std::nullopt, Json());
            screens::open(q, kScreen, {{"pos", posJson(at)}});
        });
    }
}

void form(Player& player, Json const& ctx) {
    auto p = requesterFrom(player, ctx);
    if (!p) return;
    auto                 data  = screenData(*p);
    auto                 order = orderFromJson(data["order"]);
    ll::form::CustomForm f(items::displayName(*items::make(kRedstoneRequester, 1)));
    if (order.empty()) f.appendLabel(trFor(player, "lv.stock.requester.empty", "No order programmed. Program the requester at a stock ticker."));
    for (std::size_t i = 0; i < order.size(); ++i) {
        f.appendSlider("n" + std::to_string(i), items::displayName(order[i].stack), 1, 256, 1, order[i].count);
    }
    f.appendInput(
        "address",
        trFor(player, "create.gui.redstone_requester.requester_address", "Send order to..."),
        trFor(player, "create.gui.redstone_requester.requester_address_tip", "Enter the address this"),
        data.value("address", std::string{})
    );
    f.appendDropdown(
        "partial",
        trFor(player, "lv.stock.requester.partial", "Partial orders"),
        {trFor(player, "create.gui.redstone_requester.dont_allow_partial", "Must send all items"), trFor(player, "create.gui.redstone_requester.allow_partial", "Allow partial orders")},
        data.value("partial", false) ? 1 : 0
    );
    GlobalPos at = *p;
    std::size_t n = order.size();
    f.sendTo(player, [at, n](Player& q, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r || idAt(at) != kRedstoneRequester || !mayInteract(networkAt(at), q)) return;
        Json amounts = Json::array();
        for (std::size_t i = 0; i < n; ++i) {
            int v = 1;
            if (auto it = r->find("n" + std::to_string(i)); it != r->end()) {
                if (auto const* d = std::get_if<double>(&it->second)) v = static_cast<int>(*d);
                else if (auto const* u = std::get_if<uint64>(&it->second)) v = static_cast<int>(*u);
            }
            amounts.push_back(v);
        }
        std::optional<std::string> address;
        std::optional<bool>        partial;
        if (auto it = r->find("address"); it != r->end())
            if (auto const* s = std::get_if<std::string>(&it->second)) address = *s;
        if (auto it = r->find("partial"); it != r->end())
            if (auto const* v = std::get_if<uint64>(&it->second)) partial = *v == 1;
        configure(at, address, partial, amounts);
        log().info("stock: {} configured the redstone requester at {} {} {} (form)", q.getRealName(), at.x, at.y, at.z);
    });
}
} // namespace

void useRequester(Player& player, GlobalPos const& p) {
    if (!mayInteract(networkAt(p), player)) {
        statusKey(player, "create.logistically_linked.protected", "Logistics Network is Protected", "§c");
        return;
    }
    screens::open(player, kScreen, {{"pos", posJson(p)}});
}

void pollRequester(GlobalPos const& p) {
    if (!blockAt(p) || idAt(p) != kRedstoneRequester) return;
    bool        signal = receivedPower(p) > 0;
    auto const* d      = dataOf(p);
    bool        before = d && d->value("powered", false);
    if (signal == before) return;
    // Create's onRedstonePowerChanged: a rising edge requests
    editData(p)["success"] = false;
    if (signal) triggerRequest(p);
    editData(p)["powered"] = signal;
    setStates(p, {{kStatePowered, signal ? 1 : 0}});
}

bool triggerRequest(GlobalPos const& p) {
    auto const* d = dataOf(p);
    if (!d) return false;
    auto order = orderFromJson(d->value("order", Json::array()));
    if (order.empty()) return false;
    auto network = d->value("freq", std::string{});
    bool partial = d->value("partial", false);
    auto address = d->value("address", std::string{});
    // the order as a summary (same items merged)
    std::vector<BigStack> merged;
    for (auto const& e : order) {
        auto it = std::find_if(merged.begin(), merged.end(), [&](BigStack const& m) { return itemKey(m.stack) == itemKey(e.stack); });
        if (it == merged.end()) merged.push_back(e);
        else it->count += e.count;
    }
    auto const& stock        = summaryOf(network, true);
    bool        anySucceeded = false;
    for (auto const& e : merged) {
        if (stock.countOf(e.stack) >= e.count) {
            anySucceeded = true;
            continue;
        }
        if (!partial) {
            playSound(p, "levitate.stock.deny", 0.5f, 1.0f);
            log().info("stock: redstone requester at {} {} {} refused: not in stock", p.x, p.y, p.z);
            return false;
        }
    }
    requestOrder(network, RequestType::Redstone, order, address);
    if (anySucceeded) {
        playSound(p, "levitate.stock.confirm", 0.5f, 1.5f);
        linkEffect(p);
    } else {
        playSound(p, "levitate.stock.deny", 0.5f, 1.0f);
    }
    editData(p)["success"] = true;
    log().info("stock: redstone requester at {} {} {} sent its order to '{}'", p.x, p.y, p.z, address);
    return true;
}

void registerRequesterScreens() {
    screens::registerType({
        .id   = kScreen,
        .data = [](Player& pl, Json const& ctx) -> std::optional<Json> {
            auto p = requesterFrom(pl, ctx);
            if (!p) return std::nullopt;
            return screenData(*p);
        },
        .action = action,
        .form   = form,
    });
}

} // namespace lv::feature::stock
