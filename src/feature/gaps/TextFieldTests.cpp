// U37 (gaps) selftests: the server side of the native text fields (SPEC §3.6.25, §3.6.26): the
// actions the client mod's text fields send, with the same limits as the forms.

#include <string>

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "feature/packages/Packages.h"
#include "feature/packages/PackagesInternal.h"
#include "feature/stock/StockInternal.h"
#include "selftest/SelfTest.h"

namespace {
using lv::selftest::Context;
using lv::core::GlobalPos;
using JsonT        = nlohmann::json;
namespace items    = lv::feature::common::items;
namespace net      = lv::feature::net;
namespace packages = lv::feature::packages;
namespace stock    = lv::feature::stock;

/// Sends a screen action as the client mod's text field does; returns the screen data the server
/// answers with (null if it sent none).
JsonT act(Player& p, std::string const& type, JsonT const& context, JsonT const& action) {
    net::takeTestMessages(p);
    net::injectFromClient(p, "screen.action", {{"type", type}, {"context", context}, {"action", action}});
    JsonT out;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "screen.update" && m.payload.value("type", std::string{}) == type) out = m.payload.value("data", JsonT());
    return out;
}

} // namespace

LV_REGISTER_SELFTEST("gaps.textfield_package_filter", [](Context& ctx) {
    auto& p      = ctx.player();
    auto  filter = items::make(packages::kPackageFilterId);
    ctx.require(filter.has_value(), "package filter item");
    int slot = p.getSelectedItemSlot();
    ctx.require(ctx.give(p, *filter, slot) == slot, "package filter in hand");
    net::setTestCompanion(p, true);
    net::takeTestMessages(p);
    packages::openPackageFilterScreen(p);
    auto msgs = net::takeTestMessages(p);
    bool opened = false;
    for (auto const& m : msgs) opened = opened || (m.channel == "screen.open" && m.payload.value("type", std::string{}) == "packages.package_filter");
    ctx.check(opened, "a companion gets the native address screen");
    constexpr char kType[] = "packages.package_filter";
    JsonT          context{{"slot", slot}};
    auto           data = act(p, kType, context, {{"op", "address"}, {"text", "Mill*"}});
    ctx.check(packages::addressOf(p.getSelectedItem()) == "Mill*", "the typed address is written to the filter");
    ctx.check(data.value("address", std::string{}) == "Mill*", "the screen gets the new address back");
    act(p, kType, context, {{"op", "address"}, {"text", std::string(80, 'a')}});
    ctx.check(packages::addressOf(p.getSelectedItem()).size() == 25, "addresses are cut at 25 characters (Create's AddressEditBox, U56)");
    act(p, kType, context, {{"op", "address"}, {"text", "  "}});
    ctx.check(packages::addressOf(p.getSelectedItem()).empty(), "a blank address clears the filter");
    act(p, kType, {{"slot", slot + 1}}, {{"op", "address"}, {"text", "Elsewhere"}});
    ctx.check(packages::addressOf(p.getSelectedItem()).empty(), "another slot's context is refused");
    net::setTestCompanion(p, false);
    p.setSelectedItem(ItemStack::EMPTY_ITEM());
});

LV_REGISTER_SELFTEST("gaps.textfield_stock", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto tickerPos = ctx.testPos(3750);
    tickerPos.y += 1;
    ctx.require(co_await ctx.ensureLoaded(tickerPos), "test area loaded");
    ctx.require(ctx.setBlock(tickerPos, stock::kStockTicker, {{"levitate:facing", 0}}), "place a stock ticker");
    auto tickerItem = items::make(stock::kStockTicker);
    ctx.require(tickerItem.has_value(), "stock ticker item");
    stock::tuneItem(*tickerItem, stock::newNetworkId());
    auto& p = ctx.player();
    stock::onTickerPlaced(tickerPos, &p, *tickerItem);
    net::setTestCompanion(p, true);
    JsonT context{{"pos", stock::posJson(tickerPos)}};
    // the request screen's search box filters as the player types
    auto data = act(p, "stock.request", context, {{"op", "search"}, {"text", "lo"}});
    ctx.check(data.value("search", std::string{}) == "lo", "the search text comes back with the stock");
    act(p, "stock.request", context, {{"op", "search"}, {"text", ""}});
    // its address box keeps the ticker's last used address
    data = act(p, "stock.request", context, {{"op", "address"}, {"text", "Mill"}});
    ctx.check(stock::dataOf(tickerPos) && stock::dataOf(tickerPos)->value("prevAddress", std::string{}) == "Mill", "the typed address is remembered");
    ctx.check(data.value("address", std::string{}) == "Mill", "the screen shows the address");
    act(p, "stock.request", context, {{"op", "address"}, {"text", std::string(70, 'x')}});
    ctx.check(stock::dataOf(tickerPos)->value("prevAddress", std::string{}).size() == 25, "addresses are cut at 25 characters (Create's AddressEditBox, U56)");
    // category names are edited in place
    stock::editData(tickerPos)["categories"] = JsonT::array({{{"filter", nullptr}, {"name", "Logs"}}});
    data = act(p, "stock.categories", context, {{"op", "rename"}, {"index", 0}, {"name", "Wood"}});
    ctx.check(stock::dataOf(tickerPos)->value("categories", JsonT::array())[0].value("name", std::string{}) == "Wood", "the typed category name is stored");
    net::setTestCompanion(p, false);
    ctx.cleanupBlock(tickerPos);
});
