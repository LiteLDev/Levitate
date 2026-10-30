// Selftests of the stock unit (U26, testPos dx 2600..2699). Packagers belong to U25: the tests
// plug in a test packager provider that treats barrels as packagers on their own inventory and
// records the packaging requests instead of boxing them.

#include <map>

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/common/ValueBox.h"
#include "feature/display/Display.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/net/Net.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {
using lv::selftest::Context;
using lv::core::GlobalPos;
using JsonT = nlohmann::json;
namespace stock = lv::feature::stock;
namespace items = lv::feature::common::items;
namespace caps  = lv::feature::common;
namespace net   = lv::feature::net;

constexpr char kBarrel[] = "minecraft:barrel";

struct Sent {
    GlobalPos   packager;
    std::string item;
    int         count = 0;
    std::string address;
    bool        context = false;
};

/// Test packagers: barrels. Requests take the items out of the barrel at once.
struct TestPackagers {
    std::optional<stock::PackagerProvider> previous;
    std::vector<Sent>                      sent;
    int                                    queued = 0;

    TestPackagers() {
        previous = stock::currentPackagerProvider();
        stock::PackagerProvider p;
        p.isPackager     = [](BlockSource& r, BlockPos const& pos) { return lv::game::blockIdAt(r, pos) == kBarrel; };
        p.availableItems = [](BlockSource& r, BlockPos const& pos) -> std::optional<std::vector<stock::BigStack>> {
            std::vector<stock::BigStack> out;
            int                          n = caps::items::slotCount(r, pos, -1);
            auto                         h = caps::items::find({r, pos, -1});
            if (!h) return std::nullopt;
            for (int i = 0; i < n; ++i) {
                auto s = h->getSlot({r, pos, -1}, i);
                if (!items::isEmpty(s)) out.push_back({items::withCount(s, 1), items::countOf(s)});
            }
            return out;
        };
        p.inventoryId    = [](BlockSource& r, BlockPos const& pos) { return lv::game::globalPos(r, pos).key(); };
        p.queuedPackages = [this](BlockSource&, BlockPos const&) { return queued; };
        p.send           = [this](BlockSource& r, BlockPos const& pos, std::vector<stock::PackagingRequest> const& reqs) {
            for (auto const& q : reqs) {
                auto key   = stock::itemKey(q.item);
                int  taken = 0;
                while (taken < q.count) {
                    auto got = caps::items::extract(r, pos, -1, [&](ItemStack const& s) { return stock::itemKey(s) == key; }, q.count - taken, false);
                    if (items::isEmpty(got)) break;
                    taken += items::countOf(got);
                }
                sent.push_back({lv::game::globalPos(r, pos), items::idOf(q.item), taken, q.address, !q.orderContext.is_null()});
            }
        };
        stock::setPackagerProvider(p);
    }
    ~TestPackagers() { stock::setPackagerProvider(previous); }
    int total(std::string const& id) const {
        int n = 0;
        for (auto const& s : sent)
            if (s.item == id) n += s.count;
        return n;
    }
};

ItemStack item(Context& ctx, char const* id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), std::string("item ") + id + " exists");
    return *s;
}

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

void fill(Context& ctx, GlobalPos const& barrel, char const* id, int count) {
    auto& region = ctx.player().getDimensionBlockSource();
    while (count > 0) {
        int  n    = std::min(count, 64);
        auto rest = caps::items::insert(region, lv::game::blockPos(barrel), -1, item(ctx, id, n), false);
        ctx.require(items::isEmpty(rest), "the barrel takes the items");
        count -= n;
    }
}

/// A barrel (packager) with a stock link on top, on `network` ("" = a new network).
std::string linkedBarrel(Context& ctx, GlobalPos const& barrel, std::string const& network) {
    ctx.require(ctx.setBlock(barrel, kBarrel), "place a barrel");
    auto link = at(barrel, 0, 1, 0);
    ctx.require(ctx.setBlock(link, stock::kStockLink, {{"levitate:face", 0}, {"levitate:facing", 2}}), "place a stock link");
    ItemStack linkItem = item(ctx, stock::kStockLink);
    if (!network.empty()) stock::tuneItem(linkItem, network);
    stock::onLinkPlaced(link, &ctx.player(), linkItem);
    return stock::networkAt(link);
}
} // namespace

LV_REGISTER_SELFTEST("stock.link_network", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2600), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    ctx.require(!network.empty(), "an untuned link starts a new network");
    ctx.check(stock::linkOfPackager(base) == at(base, 0, 1, 0), "the packager knows its link");
    fill(ctx, base, "minecraft:iron_ingot", 100);
    fill(ctx, base, "minecraft:gold_ingot", 3);
    auto const& s = stock::summaryOf(network, true);
    ctx.check(s.countOf(item(ctx, "minecraft:iron_ingot")) == 100, "the network counts the packager's inventory");
    ctx.check(s.summary.contributingLinks == 1, "one link contributes");
    auto second  = at(base, 3, 0, 0);
    auto network2 = linkedBarrel(ctx, second, network);
    ctx.check(network2 == network, "a tuned link joins the network");
    fill(ctx, second, "minecraft:iron_ingot", 20);
    ctx.check(stock::summaryOf(network, true).countOf(item(ctx, "minecraft:iron_ingot")) == 120, "two links add up");
    ctx.check(stock::summaryOf(network, true).byCount().front().count == 120, "byCount sorts by count");
    ctx.check(stock::isLockable(network) && stock::unloadedLinkCount(network) == 0, "the network exists, every link is loaded");
    co_return;
});

LV_REGISTER_SELFTEST("stock.order_split", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2605), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    linkedBarrel(ctx, at(base, 2, 0, 0), network);
    fill(ctx, base, "minecraft:iron_ingot", 30);
    fill(ctx, at(base, 2, 0, 0), "minecraft:iron_ingot", 30);
    fill(ctx, at(base, 2, 0, 0), "minecraft:gold_ingot", 5);
    bool ok = stock::requestOrder(network, stock::RequestType::Player, {{item(ctx, "minecraft:iron_ingot"), 50}, {item(ctx, "minecraft:gold_ingot"), 5}}, "Home");
    ctx.check(ok, "the order is sent");
    ctx.check(pk.total("minecraft:iron_ingot") == 50 && pk.total("minecraft:gold_ingot") == 5, "both packagers pack their share");
    ctx.check(!pk.sent.empty() && pk.sent[0].address == "Home" && pk.sent[0].context, "requests carry the address, the first one the order context");
    ctx.check(stock::summaryOf(network, true).countOf(item(ctx, "minecraft:iron_ingot")) == 10, "the stock drops by the order");
    pk.queued = 50;
    ctx.check(!stock::requestOrder(network, stock::RequestType::Player, {{item(ctx, "minecraft:iron_ingot"), 1}}, "Home"), "a busy packager refuses (50 queued)");
    pk.queued = 20;
    ctx.check(!stock::requestOrder(network, stock::RequestType::Redstone, {{item(ctx, "minecraft:iron_ingot"), 1}}, "Home"), "redstone requests stop at 20 queued");
    ctx.check(stock::requestOrder(network, stock::RequestType::Player, {{item(ctx, "minecraft:iron_ingot"), 1}}, "Home"), "player requests still go at 20 queued");
    co_return;
});

LV_REGISTER_SELFTEST("stock.link_power", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2610), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    fill(ctx, base, "minecraft:stone", 8);
    ctx.check(stock::summaryOf(network, true).countOf(item(ctx, "minecraft:stone")) == 8, "unpowered: counted");
    ctx.setBlock(at(base, 1, 1, 0), "minecraft:redstone_block");
    co_await ctx.ticks(10);
    ctx.check(stock::stateOr(at(base, 0, 1, 0), "levitate:powered", 0) == 1, "the link shows its power");
    ctx.check(stock::summaryOf(network, true).countOf(item(ctx, "minecraft:stone")) == 0, "a link powered with 15 leaves the network");
    ctx.setBlock(at(base, 1, 1, 0), "minecraft:air");
    co_await ctx.ticks(10);
    ctx.check(stock::summaryOf(network, true).countOf(item(ctx, "minecraft:stone")) == 8, "unpowered again: back");
});

LV_REGISTER_SELFTEST("stock.tuning", [](Context& ctx) {
    ItemStack ticker = item(ctx, stock::kStockTicker);
    ctx.check(stock::networkOfItem(ticker).empty(), "a fresh item is untuned");
    stock::tuneItem(ticker, "abc");
    ctx.check(stock::networkOfItem(ticker) == "abc", "tuned");
    ctx.check(!ticker.getCustomLore().empty(), "the lore says the frequency is configured");
    stock::clearTuning(ticker);
    ctx.check(stock::networkOfItem(ticker).empty() && ticker.getCustomLore().empty(), "cleared");
    auto a = stock::newNetworkId(), b = stock::newNetworkId();
    ctx.check(a.size() == 36 && a != b, "network ids are random UUIDs");
    auto order = stock::orderFromJson(stock::orderToJson({{item(ctx, "minecraft:dirt"), 300}}));
    ctx.check(order.size() == 1 && order[0].count == 300, "orders round-trip through JSON");
});

LV_REGISTER_SELFTEST("stock.ticker_request", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2620), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    fill(ctx, base, "minecraft:oak_log", 40);
    auto tickerPos = at(base, 0, 0, 3);
    ctx.require(ctx.setBlock(tickerPos, stock::kStockTicker, {{"levitate:facing", 0}}), "place a stock ticker");
    ItemStack tickerItem = item(ctx, stock::kStockTicker);
    stock::tuneItem(tickerItem, network);
    stock::onTickerPlaced(tickerPos, &ctx.player(), tickerItem);
    ctx.check(stock::networkAt(tickerPos) == network, "the ticker is on the network");
    ctx.check(!stock::keeperPresent(tickerPos), "no keeper yet");
    auto& p = ctx.player();
    net::setTestCompanion(p, true);
    stock::useKeeper(p, tickerPos);
    auto msgs = net::takeTestMessages(p);
    ctx.require(!msgs.empty() && msgs[0].channel == "screen.open" && msgs[0].payload["type"] == "stock.request", "the request screen opens");
    auto groups = msgs[0].payload["data"]["groups"];
    ctx.check(!groups.empty() && groups.back()["items"].size() == 1 && groups.back()["items"][0]["count"] == 40, "the screen lists the network's stock");
    JsonT context{{"pos", stock::posJson(tickerPos)}};
    net::injectFromClient(p, "screen.action", {{"type", "stock.request"}, {"context", context}, {"action", {{"op", "order"}, {"address", "Mill"}, {"items", stock::orderToJson({{item(ctx, "minecraft:oak_log"), 16}})}}}});
    ctx.check(pk.total("minecraft:oak_log") == 16 && pk.sent.back().address == "Mill", "the native screen orders");
    ctx.check(stock::dataOf(tickerPos) && stock::dataOf(tickerPos)->value("prevAddress", std::string{}) == "Mill", "the address is remembered");
    // programming a held redstone requester instead of ordering
    p.setSelectedItem(item(ctx, stock::kRedstoneRequester));
    ctx.check(stock::placeOrder(p, tickerPos, {{item(ctx, "minecraft:oak_log"), 4}}, "Saw", true), "the order is programmed");
    auto held = p.getSelectedItem();
    ctx.check(stock::networkOfItem(held) == network && items::getData(held, stock::kRequestData).is_object(), "the requester item carries the order and the network");
    ctx.check(pk.total("minecraft:oak_log") == 16, "programming sends nothing");
    // categories
    stock::editData(tickerPos)["categories"] = JsonT::array({{{"filter", nullptr}, {"name", "Logs"}}});
    net::injectFromClient(p, "screen.action", {{"type", "stock.categories"}, {"context", context}, {"action", {{"op", "rename"}, {"index", 0}, {"name", "Wood"}}}});
    ctx.check(stock::dataOf(tickerPos)->value("categories", JsonT::array())[0].value("name", std::string{}) == "Wood", "categories are renamed");
    net::setTestCompanion(p, false);
});

LV_REGISTER_SELFTEST("stock.lock", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2630), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    auto&         owner   = ctx.player();
    auto&         other   = ctx.spawnPlayer("stock_other");
    ctx.check(stock::mayAdministrate(network, owner) && !stock::mayAdministrate(network, other), "the placer owns the network");
    ctx.check(stock::mayInteract(network, other), "an open network lets anyone in");
    stock::setLocked(network, true);
    ctx.check(stock::isLocked(network) && !stock::mayInteract(network, other) && stock::mayInteract(network, owner), "a locked network only lets the owner in");
    stock::setLocked(network, false);
    stock::saveNetworks();
    stock::markNetworksDirty();
    stock::saveNetworks();
    stock::loadNetworks();
    ctx.check(stock::networkInfo(network) && stock::networkInfo(network)->links.size() == 1, "networks survive a save and load");
});

LV_REGISTER_SELFTEST("stock.requester", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2640), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    fill(ctx, base, "minecraft:cobblestone", 10);
    auto req = at(base, 0, 0, 3);
    ctx.require(ctx.setBlock(req, stock::kRedstoneRequester), "place a redstone requester");
    ItemStack reqItem = item(ctx, stock::kRedstoneRequester);
    stock::tuneItem(reqItem, network);
    items::setData(reqItem, stock::kRequestData, {{"order", stock::orderToJson({{item(ctx, "minecraft:cobblestone"), 4}, {item(ctx, "minecraft:dirt"), 1}})}, {"address", "Wall"}, {"ticker", stock::posJson(base)}});
    stock::onRequesterPlaced(req, &ctx.player(), reqItem);
    ctx.check(stock::networkAt(req) == network, "the requester is on the network");
    // not all in stock and partial orders off: refused
    ctx.setBlock(at(req, 1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(10);
    ctx.check(pk.sent.empty(), "a full order that is not in stock is refused");
    ctx.setBlock(at(req, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(10);
    stock::editData(req)["partial"] = true;
    ctx.setBlock(at(req, 1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(10);
    ctx.check(pk.total("minecraft:cobblestone") == 4, "partial orders send what is there on a rising edge");
    ctx.check(stock::dataOf(req)->value("success", false), "the request succeeded");
    co_await ctx.ticks(20);
    ctx.check(pk.total("minecraft:cobblestone") == 4, "a constant signal requests once");
    ctx.setBlock(at(req, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(10);
});

LV_REGISTER_SELFTEST("stock.gauge_restock", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2650), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    fill(ctx, base, "minecraft:coal", 64);
    // a restocker: a gauge on another packager (barrel) of the same network
    auto target = at(base, 3, 0, 0);
    ctx.require(ctx.setBlock(target, kBarrel), "place the restocked barrel");
    linkedBarrel(ctx, at(base, 6, 0, 0), network); // unrelated second link
    auto gauge = at(target, 0, 1, 0);
    ctx.require(ctx.setBlock(gauge, stock::kFactoryGauge, {{"levitate:face", 0}, {"levitate:facing", 2}}), "place a factory gauge");
    ctx.require(stock::placeGaugePanel(gauge, 0, network, &ctx.player()), "place a panel");
    auto& p = ctx.player();
    p.setSelectedItem(item(ctx, "minecraft:coal"));
    stock::useGauge(p, gauge, 0, false);
    ctx.check(items::idOf(items::fromJson(stock::gaugeInfo(gauge, 0).value("filter", JsonT()))) == "minecraft:coal", "clicking with an item sets the monitored item");
    ctx.check(stock::gaugeInfo(gauge, 0).value("restocker", false), "a gauge on a packager restocks");
    ctx.check(lv::feature::itemrender::itemsOf(gauge).size() == 1, "the client mod draws the monitored item on the panel (U42)");
    ctx.check(lv::feature::common::valuebox::apply(p, gauge, "panel0", {0, 20}), "the target amount is set on the board");
    net::setTestCompanion(p, true);
    {
        // client mod: click and hold (U42): a short click opens the panel screen, holding the board
        p.setSelectedItem(items::empty());
        auto lastToken = [&] {
            std::uint64_t t = 0;
            for (auto const& m : net::takeTestMessages(p)) {
                if (m.channel == "valuebox.hold") t = m.payload.value("token", std::uint64_t{0});
                if (m.channel == "screen.open" || m.channel == "valuebox.open") t = ~std::uint64_t{0};
            }
            return t;
        };
        (void)net::takeTestMessages(p);
        stock::useGauge(p, gauge, 0, false);
        auto token = lastToken();
        ctx.check(token != 0 && token != ~std::uint64_t{0}, "an empty-hand click waits for the hold");
        net::injectFromClient(p, "valuebox.release", {{"token", token}, {"state", "short"}});
        ctx.check(lastToken() == ~std::uint64_t{0}, "a short click opens the panel screen");
        stock::useGauge(p, gauge, 0, false);
        token = lastToken();
        net::injectFromClient(p, "valuebox.release", {{"token", token}, {"state", "hold"}});
        ctx.check(lastToken() == ~std::uint64_t{0}, "holding opens the target amount board");
    }
    net::injectFromClient(p, "screen.action", {{"type", "stock.gauge"}, {"context", {{"pos", stock::posJson(gauge)}, {"slot", 0}}}, {"action", {{"op", "set"}, {"address", "Furnace"}}}});
    net::setTestCompanion(p, false);
    (void)net::takeTestMessages(p);
    co_await ctx.ticks(60); // a new target amount waits half the request interval
    ctx.check(pk.total("minecraft:coal") == 20 && pk.sent.back().address == "Furnace", "the restocker orders the missing 20 coal");
    ctx.check(stock::gaugeInfo(gauge, 0).value("promised", 0) == 20, "the order is promised");
    co_await ctx.ticks(5);
    ctx.check(pk.total("minecraft:coal") == 20, "promised items are not ordered again");
    fill(ctx, target, "minecraft:coal", 20); // the package arrived
    co_await ctx.ticks(30);
    auto info = stock::gaugeInfo(gauge, 0);
    ctx.check(info.value("satisfied", false) && info.value("promised", 1) == 0, "arrivals fulfil the promise and satisfy the gauge");
    // a display link on the gauge: Create's "Factory gauge status" source
    auto& region = p.getDimensionBlockSource();
    bool  shown  = false;
    for (auto const* s : lv::feature::display::sourcesAt(region, gauge)) {
        if (s->id != "gauge_status") continue;
        JsonT                         cfg = JsonT::object();
        lv::feature::display::Context dc{region, gauge, gauge, gauge, cfg};
        auto                          lines = s->text(dc, {4, 20, "display_board"});
        shown = lines.size() == 1 && lines[0].starts_with("20 ✔");
    }
    ctx.check(shown, "the gauge_status display source lists the panel");
});

LV_REGISTER_SELFTEST("stock.gauge_recipe", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(2665), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    fill(ctx, base, "minecraft:iron_ingot", 64);
    // two gauges on the same wall of stone: iron (input) and iron blocks (output)
    auto wall = at(base, 0, 0, 4);
    ctx.setBlock(wall, "minecraft:stone");
    ctx.setBlock(at(wall, 1, 0, 0), "minecraft:stone");
    auto out = at(wall, 0, 0, -1), in = at(wall, 1, 0, -1);
    for (auto g : {out, in}) {
        ctx.require(ctx.setBlock(g, stock::kFactoryGauge, {{"levitate:face", 1}, {"levitate:facing", 2}}), "place a gauge on the wall");
        ctx.require(stock::placeGaugePanel(g, 0, network, nullptr), "panel");
    }
    auto& p = ctx.player();
    p.setSelectedItem(item(ctx, "minecraft:iron_block"));
    stock::useGauge(p, out, 0, false);
    p.setSelectedItem(item(ctx, "minecraft:iron_ingot"));
    stock::useGauge(p, in, 0, false);
    p.setSelectedItem(items::empty());
    ctx.check(stock::connectPanels(&p, out, 0, in, 0), "the panels connect");
    ctx.check(!stock::connectPanels(&p, out, 0, in, 0), "a second connection is refused");
    auto stray = at(base, 0, 0, -3);
    ctx.setBlock(stray, stock::kFactoryGauge, {{"levitate:face", 0}, {"levitate:facing", 2}});
    stock::placeGaugePanel(stray, 0, network, nullptr);
    p.setSelectedItem(item(ctx, "minecraft:dirt"));
    stock::useGauge(p, stray, 0, false);
    p.setSelectedItem(items::empty());
    ctx.check(!stock::connectPanels(&p, out, 0, stray, 0), "panels of different orientation don't connect");
    lv::feature::common::valuebox::apply(p, out, "panel0", {0, 2});
    auto doc   = stock::editData(out);
    auto& panel = stock::editData(out)["panels"]["0"];
    panel["address"] = "Press";
    panel["targetedBy"][0]["amount"] = 9;
    (void)doc;
    co_await ctx.ticks(60); // a new target amount waits half the request interval
    ctx.check(pk.total("minecraft:iron_ingot") == 9 && pk.sent.back().address == "Press", "missing output: the inputs are sent to the recipe address");
    ctx.check(stock::gaugeInfo(out, 0).value("promised", 0) == 1, "the output is promised");
    co_await ctx.ticks(5);
    ctx.check(pk.total("minecraft:iron_ingot") == 9, "while the promise holds nothing more is sent");
    stock::removeGaugePanel(in, 0, nullptr);
    ctx.check(stock::gaugeInfo(out, 0).value("targetedBy", JsonT::array()).empty(), "removing an input disconnects it");
});

LV_REGISTER_SELFTEST("stock.shop", [](Context& ctx) -> ll::coro::CoroTask<> {
    if (!lv::game::blockWith("levitate:white_table_cloth")) {
        ctx.info("levitate:white_table_cloth is not registered (U10): shop placement skipped");
        co_return;
    }
    auto base = at(ctx.testPos(2680), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    fill(ctx, base, "minecraft:bread", 10);
    auto tickerPos = at(base, 0, 0, 3);
    ctx.setBlock(tickerPos, stock::kStockTicker, {{"levitate:facing", 0}});
    ItemStack tickerItem = item(ctx, stock::kStockTicker);
    stock::tuneItem(tickerItem, network);
    stock::onTickerPlaced(tickerPos, &ctx.player(), tickerItem);
    auto cloth = at(base, 3, 0, 3);
    ctx.require(ctx.setBlock(cloth, "levitate:white_table_cloth"), "place a table cloth");
    ItemStack clothItem = item(ctx, "levitate:white_table_cloth");
    items::setData(clothItem, stock::kRequestData, {{"order", stock::orderToJson({{item(ctx, "minecraft:bread"), 2}})}, {"address", "Buyer"}, {"ticker", stock::posJson(tickerPos)}});
    auto& p = ctx.player();
    stock::onClothPlaced(cloth, p, clothItem);
    ctx.check(stock::isShop(cloth), "a programmed cloth is a shop");
    p.setSelectedItem(items::empty());
    stock::useShop(p, cloth, 0.1, 0.1, 0.1);
    ctx.check(items::isEmpty(p.getSelectedItem()), "no price: no shopping list");
    stock::editData(cloth)["shop"]["price"] = items::toJson(item(ctx, "minecraft:emerald"));
    ctx.check(stock::stockLevelForTrade(cloth, nullptr) == 5, "10 bread allow 5 trades of 2");
    stock::useShop(p, cloth, 0.1, 0.1, 0.1);
    ctx.check(items::isEmpty(p.getSelectedItem()), "no keeper at the ticker: no shopping list");
    ctx.setBlock(at(tickerPos, 1, 0, 0), "levitate:blaze_burner", {{"levitate:blaze", 1}});
    {
        ItemStack list;
        if (stock::keeperPresent(tickerPos)) {
            stock::useShop(p, cloth, 0.1, 0.1, 0.1);
            stock::useShop(p, cloth, 0.1, 0.1, 0.1);
            list = p.getSelectedItem();
            ctx.check(items::idOf(list) == stock::kShoppingList, "the shop hands out a shopping list");
        } else {
            // no blaze burner (U14) to seat as keeper: write the list the shop would hand out
            ctx.info("levitate:blaze_burner is not registered (U14): the shopping list is written directly");
            list = item(ctx, stock::kShoppingList);
            items::setData(list, stock::kListData, {{"purchases", JsonT::array({{{"pos", stock::posJson(cloth)}, {"n", 2}}})}, {"owner", stock::uuidOf(p)}, {"network", network}, {"address", "Buyer"}});
            p.setSelectedItem(list);
        }
        auto baked = stock::bakeList(items::getData(list, stock::kListData), std::nullopt);
        ctx.check(baked.order.countOf(item(ctx, "minecraft:bread")) == 4 && baked.payment.countOf(item(ctx, "minecraft:emerald")) == 2, "two purchases: 4 bread for 2 emeralds");
        stock::checkout(p, tickerPos, list);
        ctx.check(pk.total("minecraft:bread") == 0, "without the price nothing is bought");
        ctx.give(p, item(ctx, "minecraft:emerald", 2));
        stock::checkout(p, tickerPos, list);
        ctx.check(pk.total("minecraft:bread") == 4 && pk.sent.back().address == "Buyer", "paying sends the order");
        ctx.check(!stock::payments(tickerPos).empty(), "the payment is in the ticker");
        ctx.check(items::isEmpty(p.getSelectedItem()), "the list is used up");
        stock::useTicker(p, tickerPos);
        ctx.check(stock::payments(tickerPos).empty(), "the owner collects the payments at the ticker");
    }
});

LV_REGISTER_SELFTEST("stock.packager", [](Context& ctx) -> ll::coro::CoroTask<> {
    // U25's packager: a chest below a packager facing up, a stock link on top of the packager
    if (!lv::game::blockWith("levitate:packager")) {
        ctx.info("levitate:packager is not registered (U25): skipped");
        co_return;
    }
    auto chest = at(ctx.testPos(2690), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(chest), "test area loaded");
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "place a chest");
    auto packager = at(chest, 0, 1, 0);
    ctx.require(ctx.setBlock(packager, "levitate:packager", {{"levitate:facing", 1}}), "place a packager facing up");
    auto link = at(packager, 0, 1, 0);
    ctx.require(ctx.setBlock(link, stock::kStockLink, {{"levitate:face", 0}, {"levitate:facing", 2}}), "place a stock link on it");
    stock::onLinkPlaced(link, &ctx.player(), item(ctx, stock::kStockLink));
    auto network = stock::networkAt(link);
    fill(ctx, chest, "minecraft:iron_ingot", 20);
    ctx.check(stock::summaryOf(network, true).countOf(item(ctx, "minecraft:iron_ingot")) == 20, "the packager's inventory is the network's stock");
    ctx.check(stock::requestOrder(network, stock::RequestType::Player, {{item(ctx, "minecraft:iron_ingot"), 12}}, "Depot"), "the order goes to U25's packager");
    co_await ctx.ticks(5);
    ctx.check(stock::summaryOf(network, true).countOf(item(ctx, "minecraft:iron_ingot")) == 8, "the packager boxed the ordered items");
    co_await ctx.ticks(30);
    ctx.check(stock::stateOr(packager, "levitate:linked", 0) == 1, "the packager shows that it is linked");
});

LV_REGISTER_SELFTEST("stock.table_cloth_helper", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace ia = lv::feature::common::interactions;
    auto base = at(ctx.testPos(4230), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz) ctx.cleanupBlock(at(base, dx, 0, dz));
    ctx.require(ctx.setBlock(base, "levitate:white_table_cloth"), "place a table cloth");
    auto& p = ctx.player();
    p.setSelectedItem(item(ctx, "levitate:red_table_cloth", 4));
    auto click = [&](double hx, double hz) {
        auto&       region  = p.getDimensionBlockSource();
        auto const& block   = region.getBlock(lv::game::blockPos(base));
        std::string blockId = block.getTypeName();
        auto const& held    = p.getSelectedItem();
        std::string itemId  = items::idOf(held);
        ia::UseOnContext c{p, held, itemId, block, blockId, base, 1, {hx, 1.0, hz}, false, true};
        return ia::dispatchUseOn(c);
    };
    ctx.check(click(0.95, 0.5) == ia::Result::Consume, "a table cloth in hand on a table cloth is taken");
    ctx.check(lv::game::blockIdAt(p.getDimensionBlockSource(), lv::game::blockPos(at(base, 1, 0, 0))) == "levitate:red_table_cloth",
              "the cloth extends towards the clicked edge (east)");
    co_await ctx.ticks(6);
    click(0.5, 0.05);
    ctx.check(lv::game::blockIdAt(p.getDimensionBlockSource(), lv::game::blockPos(at(base, 0, 0, -1))) == "levitate:red_table_cloth",
              "and north for a click near the north edge");
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz) ctx.cleanupBlock(at(base, dx, 0, dz));
});
LV_REGISTER_SELFTEST("stock.gauge_crafting", [](Context& ctx) {
    auto planks = item(ctx, "minecraft:oak_planks");
    auto sticks = stock::craftingArrangement(item(ctx, "minecraft:stick"), {planks});
    ctx.check(sticks.has_value(), "sticks are crafted from planks");
    if (sticks) {
        int cells = 0;
        for (int i : sticks->pattern) cells += i == 0;
        ctx.check(cells == 2 && sticks->resultCount == 4, "two planks in the pattern, 4 sticks out, got " + std::to_string(cells) + " / " + std::to_string(sticks->resultCount));
        ctx.check(sticks->pattern[0] == 0 && sticks->pattern[3] == 0, "the planks stand in one column of the 3x3 grid");
    }
    ctx.check(!stock::craftingArrangement(item(ctx, "minecraft:stick"), {planks, item(ctx, "minecraft:dirt")}), "every input must be used");
    ctx.check(!stock::craftingArrangement(item(ctx, "minecraft:diamond"), {planks}), "no recipe: no arrangement");
});