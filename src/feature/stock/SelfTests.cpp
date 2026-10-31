// Selftests of the stock unit (U26, testPos dx 2600..2699; U56: dx 5600..5684). Packagers belong
// to U25: the tests plug in a test packager provider that treats barrels as packagers on their own
// inventory and records the packaging requests instead of boxing them.

#include <array>
#include <map>

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"
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
#include "feature/display/Clipboard.h"
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
    // a display link on the gauge: Create's "Factory gauge status" source lists the panels
    // connected to the display link (U56: not the gauge's own panels)
    auto& region = p.getDimensionBlockSource();
    auto  link   = at(gauge, 0, 0, 1);
    bool  shown = false, emptyBefore = false;
    for (auto const* s : lv::feature::display::sourcesAt(region, gauge)) {
        if (s->id != "gauge_status") continue;
        JsonT                         cfg = JsonT::object();
        lv::feature::display::Context dc{region, link, gauge, gauge, cfg};
        emptyBefore = s->text(dc, {4, 20, "display_board"}).empty();
        stock::editData(link)["linkedGauges"] = JsonT::array({{{"pos", stock::posJson(gauge)}, {"slot", 0}}});
        auto lines                            = s->text(dc, {4, 20, "display_board"});
        shown = lines.size() == 1 && lines[0].starts_with("20 ✔");
    }
    ctx.check(emptyBefore, "without connected panels the gauge_status source lists nothing");
    ctx.check(shown, "the gauge_status display source lists the connected panel");
    lv::feature::common::blockdata::erase(link);
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

// ---- U56 (testPos dx 5600..5684; row 56 lies outside the kept test area, so z spawn - 60) ---------

namespace {
namespace ia = lv::feature::common::interactions;
constexpr int kU56Dz = -295;

GlobalPos u56(Context& ctx, int dx) { return at(ctx.testPos(dx, kU56Dz), 0, 1, 0); }

ia::Result clickBlock(Player& p, GlobalPos const& pos, int face, std::array<double, 3> hit, bool sneaking, ItemStack const& held) {
    auto&       region  = p.getDimensionBlockSource();
    auto const& block   = region.getBlock(lv::game::blockPos(pos));
    std::string blockId = block.getTypeName();
    std::string itemId  = items::isEmpty(held) ? "" : items::idOf(held);
    ia::UseOnContext c{p, held, itemId, block, blockId, pos, face, {hit[0], hit[1], hit[2]}, sneaking, true};
    return ia::dispatchUseOn(c);
}

/// A floor gauge at `g` with an active panel in `slot` on `network` (optionally monitoring `item`).
bool floorGauge(Context& ctx, GlobalPos const& g, std::string const& network, int slot = 0, char const* itemId = nullptr) {
    if (!ctx.setBlock(g, stock::kFactoryGauge, {{"levitate:face", 0}, {"levitate:facing", 2}})) return false;
    if (!stock::placeGaugePanel(g, slot, network, nullptr)) return false;
    if (itemId) {
        auto panel      = stock::readGaugePanel(g, slot);
        panel["filter"] = stock::stackToJson(item(ctx, itemId));
        stock::writeGaugePanel(g, slot, panel);
    }
    return true;
}

std::size_t linksOf(GlobalPos const& g, int slot) { return stock::readGaugePanel(g, slot).value("targetedByLinks", JsonT::array()).size(); }

void gaugeAction(Player& p, GlobalPos const& g, int slot, JsonT const& action) {
    net::setTestCompanion(p, true);
    net::injectFromClient(p, "screen.action", {{"type", "stock.gauge"}, {"context", {{"pos", stock::posJson(g)}, {"slot", slot}}}, {"action", action}});
    net::setTestCompanion(p, false);
    (void)net::takeTestMessages(p);
}
} // namespace

LV_REGISTER_SELFTEST("stock.gauge_link_redstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's FactoryPanelSupportBehaviour on redstone links: a transmitter sends while a connected
    // panel with a target is satisfied; a receiver with a signal pauses its panels
    auto base = u56(ctx, 5600);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    std::string network = "u56-links";
    auto        ga = base, gb = at(base, 4, 0, 0);
    ctx.require(floorGauge(ctx, ga, network) && floorGauge(ctx, gb, network, 0, "minecraft:coal"), "two gauges");
    auto pa     = stock::readGaugePanel(ga, 0);
    pa["count"] = 5; // no item: always satisfied (Create)
    stock::writeGaugePanel(ga, 0, pa);
    auto tx = at(base, 1, 0, 0), rx = at(base, 3, 0, 0);
    // links need a block to hang on (they pop off otherwise)
    for (int dx = 0; dx <= 17; ++dx) ctx.setBlock(at(base, dx, -1, 0), "minecraft:stone");
    ctx.setBlock(at(base, 2, -1, 2), "minecraft:stone");
    ctx.setBlock(at(base, 2, 0, 2), "minecraft:stone"); // under the higher one (replaced below)
    ctx.require(ctx.setBlock(tx, stock::kRedstoneLinkId, {{"levitate:facing", 1}}), "transmitting link");
    ctx.require(ctx.setBlock(rx, stock::kRedstoneLinkId, {{"levitate:facing", 1}, {"levitate:receiver", 1}}), "receiving link");
    auto sideways = at(base, 2, 0, 3), higher = at(base, 2, 1, 2), far = at(base, 17, 0, 0);
    ctx.setBlock(at(base, 2, 0, 4), "minecraft:stone"); // the sideways link hangs on its south side
    ctx.setBlock(sideways, stock::kRedstoneLinkId, {{"levitate:facing", 2}});
    ctx.setBlock(higher, stock::kRedstoneLinkId, {{"levitate:facing", 1}});
    ctx.setBlock(far, stock::kRedstoneLinkId, {{"levitate:facing", 1}});
    auto& p = ctx.player();
    ctx.check(!stock::connectPanelToLink(&p, ga, 0, sideways), "a link facing another way is refused (same orientation)");
    ctx.check(!stock::connectPanelToLink(&p, ga, 0, higher), "a link on another plane is refused (same surface)");
    ctx.check(!stock::connectPanelToLink(&p, ga, 0, far), "a link 17 blocks away is refused");
    ctx.check(stock::connectPanelToLink(&p, ga, 0, tx), "panel A connects to the transmitter");
    ctx.check(stock::connectPanelToLink(&p, gb, 0, rx), "panel B connects to the receiver");
    ctx.check(linksOf(ga, 0) == 1 && stock::linkedPanelsOf(tx).size() == 1, "both sides know the connection");
    co_await ctx.ticks(12);
    ctx.check(stock::stateOr(tx, "levitate:powered", 0) == 1, "the transmitter sends while its panel is satisfied with a target");
    ctx.check(stock::stateOr(rx, "levitate:power", 0) == 15, "the receiver gets the signal, got " + std::to_string(stock::stateOr(rx, "levitate:power", -1)));
    ctx.check(stock::readGaugePanel(gb, 0).value("paused", false), "the powered receiver pauses panel B (Redstone Paused)");
    ctx.check(stock::panelAt(gb, 0) && stock::panelAt(gb, 0)->paused, "panelAt reports the pause");
    pa          = stock::readGaugePanel(ga, 0);
    pa["count"] = 0;
    stock::writeGaugePanel(ga, 0, pa);
    co_await ctx.ticks(12);
    ctx.check(stock::stateOr(tx, "levitate:powered", 1) == 0, "without a target the transmitter stops");
    ctx.check(!stock::readGaugePanel(gb, 0).value("paused", true), "panel B resumes");
    // the wrench also cycles the arrows of link connections
    p.setSelectedItem(item(ctx, "levitate:wrench"));
    stock::useGauge(p, gb, 0, false);
    p.setSelectedItem(items::empty());
    ctx.check(stock::readGaugePanel(gb, 0)["targetedByLinks"][0].value("arrow", 0) == 1, "the wrench cycles the link's arrow");
    // Create's redstone reset button
    gaugeAction(p, gb, 0, {{"op", "set"}, {"redstoneReset", true}});
    ctx.check(linksOf(gb, 0) == 0 && stock::linkedPanelsOf(rx).empty(), "the reset button drops the link connections");
    // a removed link leaves its panels
    lv::game::clearBlock(p.getDimensionBlockSource(), lv::game::blockPos(tx));
    co_await ctx.ticks(1);
    ctx.check(linksOf(ga, 0) == 0, "breaking the link disconnects it");
});

LV_REGISTER_SELFTEST("stock.gauge_pending", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's FactoryPanelConnectionHandler: a pending connection ends with the next click
    auto base = u56(ctx, 5620);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    std::string network = "u56-pending";
    auto        ga = base, gb = at(base, 2, 0, 0), stone = at(base, 0, 0, 2);
    ctx.require(floorGauge(ctx, ga, network, 0, "minecraft:clock") && floorGauge(ctx, gb, network, 0, "minecraft:gold_ingot"), "two gauges");
    ctx.require(ctx.setBlock(stone, "minecraft:stone"), "a stone block");
    auto& p = ctx.player();
    p.setSelectedItem(items::empty());
    stock::startConnection(p, ga, 0);
    ctx.check(stock::pendingConnectionOf(p).has_value(), "the connection is pending");
    ctx.check(clickBlock(p, stone, 1, {.5, 1, .5}, false, items::empty()) == ia::Result::Consume, "a click on another block is taken");
    ctx.check(!stock::pendingConnectionOf(p), "clicking another block cancels the connection");
    co_await ctx.ticks(6);
    stock::startConnection(p, ga, 0);
    ctx.check(clickBlock(p, gb, 1, {.5, 1, .5}, true, items::empty()) == ia::Result::Consume, "a sneak-click on a gauge is taken");
    ctx.check(!stock::pendingConnectionOf(p), "sneak-clicking cancels the connection");
    co_await ctx.ticks(6);
    stock::startConnection(p, ga, 0);
    stock::useGauge(p, gb, 0, false); // Create's panelClicked
    ctx.check(!stock::pendingConnectionOf(p), "the second panel ends the connection");
    ctx.check(stock::readGaugePanel(ga, 0).value("targetedBy", JsonT::array()).size() == 1, "and connects it as an input");
    stock::startConnection(p, ga, 0);
    auto& sp = static_cast<SimulatedPlayer&>(p);
    sp.teleport(Vec3{base.x + 20.5f, static_cast<float>(base.y), base.z + .5f}, DimensionType(base.dim));
    co_await ctx.ticks(3);
    ctx.check(!stock::pendingConnectionOf(p), "walking 16 blocks away drops the connection");
    sp.teleport(Vec3{base.x + .5f, static_cast<float>(base.y), base.z - 1.5f}, DimensionType(base.dim));
    co_await ctx.ticks(2);
    stock::startConnection(p, ga, 0);
    stock::removeGaugePanel(ga, 0, nullptr);
    co_await ctx.ticks(2);
    ctx.check(!stock::pendingConnectionOf(p), "a removed panel drops the connection");
});

LV_REGISTER_SELFTEST("stock.gauge_relocate", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's "Move this gauge" (FactoryPanelBehaviour.moveTo)
    auto base = u56(ctx, 5640);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    std::string network = "u56-move";
    auto        ga = base, gc = at(base, 0, 0, 2), target = at(base, 3, 0, 0);
    for (auto const& g : {ga, gc, target}) ctx.require(ctx.setBlock(at(g, 0, -1, 0), "minecraft:stone"), "support");
    ctx.require(floorGauge(ctx, ga, network, 0, "minecraft:clock") && floorGauge(ctx, gc, network, 0, "minecraft:gold_ingot"), "two gauges");
    ctx.cleanupBlock(target);
    auto& p = ctx.player();
    p.setSelectedItem(items::empty());
    ctx.require(stock::connectPanels(&p, ga, 0, gc, 0), "the gold panel feeds the clock panel");
    stock::startRelocation(p, ga, 0);
    ctx.check(clickBlock(p, at(target, 0, -1, 0), 1, {.25, 1, .25}, false, items::empty()) == ia::Result::Consume, "the click picks the new spot");
    ctx.check(stock::idAt(target) == stock::kFactoryGauge, "a gauge stands at the new spot");
    int moved = -1;
    for (int s = 0; s < 4; ++s)
        if (stock::readGaugePanel(target, s).value("active", false)) moved = s;
    ctx.check(moved >= 0 && items::idOf(stock::stackFromJson(stock::readGaugePanel(target, moved).value("filter", JsonT()))) == "minecraft:clock",
              "the panel moved with its item");
    ctx.check(stock::idAt(ga) != stock::kFactoryGauge, "the old gauge block went with its only panel");
    auto targeting = stock::readGaugePanel(gc, 0).value("targeting", JsonT::array());
    ctx.check(targeting.size() == 1 && stock::posFrom(targeting[0].value("pos", JsonT())) == target, "its input now points at the new spot");
    if (moved >= 0) stock::removeGaugePanel(target, moved, nullptr);
    co_await ctx.ticks(1);
});

LV_REGISTER_SELFTEST("stock.gauge_wrench_remove", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's FactoryPanelBlock.onSneakWrenched: the targeted panel comes off
    auto base = u56(ctx, 5655);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    std::string network = "u56-wrench";
    ctx.require(floorGauge(ctx, base, network, 0) && stock::placeGaugePanel(base, 1, network, nullptr), "a gauge with two panels");
    auto& p      = ctx.player();
    auto  wrench = item(ctx, "levitate:wrench");
    p.setSelectedItem(wrench);
    auto hitOf = [&](int slot) {
        auto c = stock::cs::panelCentre(stock::faceOf(base), stock::facingOf(base), static_cast<stock::cs::PanelSlot>(slot));
        return std::array<double, 3>{c.x, c.y, c.z};
    };
    clickBlock(p, base, 1, hitOf(0), true, wrench);
    ctx.check(!stock::readGaugePanel(base, 0).value("active", false) && stock::readGaugePanel(base, 1).value("active", false), "the targeted panel came off");
    ctx.check(stock::idAt(base) == stock::kFactoryGauge, "the block stays with the other panel");
    co_await ctx.ticks(6);
    clickBlock(p, base, 1, hitOf(1), true, wrench);
    ctx.check(stock::idAt(base) != stock::kFactoryGauge, "the last panel takes the block with it");
    p.setSelectedItem(items::empty());
});

LV_REGISTER_SELFTEST("stock.gauge_reset_promises", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's reset sets forceClearPromises: the promises of the next monitored item are dropped
    auto base = u56(ctx, 5660);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    std::string network = "u56-reset";
    ctx.require(floorGauge(ctx, base, network, 0, "minecraft:coal"), "a gauge");
    auto& p = ctx.player();
    stock::ensureNetwork(network).promises.add(stock::itemKey(item(ctx, "minecraft:coal")), 7);
    gaugeAction(p, base, 0, {{"op", "set"}, {"reset", true}});
    ctx.check(stock::readGaugePanel(base, 0).value("forceClearPromises", false), "the reset marks the promises for clearing");
    auto panel      = stock::readGaugePanel(base, 0);
    panel["filter"] = stock::stackToJson(item(ctx, "minecraft:coal"));
    stock::writeGaugePanel(base, 0, panel);
    co_await ctx.ticks(2);
    ctx.check(stock::gaugeInfo(base, 0).value("promised", 1) == 0, "the promises are gone");
    ctx.check(!stock::readGaugePanel(base, 0).value("forceClearPromises", false), "the mark is used up");
});

LV_REGISTER_SELFTEST("stock.table_cloth_items", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's TableClothBlockEntity.use on a plain cloth: up to 4 items, an empty hand takes one back
    auto base = u56(ctx, 5665);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(ctx.setBlock(base, "levitate:white_table_cloth"), "a table cloth");
    auto& p = ctx.player();
    for (int i = 0; i < 5; ++i) {
        p.setSelectedItem(item(ctx, "minecraft:apple", 3));
        clickBlock(p, base, 1, {.5, 1, .5}, false, p.getSelectedItem());
        co_await ctx.ticks(6);
    }
    ctx.check(lv::feature::itemrender::itemsOf(base).size() == 4, "four items lie on the cloth (display items), got " + std::to_string(lv::feature::itemrender::itemsOf(base).size()));
    ctx.check(items::countOf(p.getSelectedItem()) == 3, "a full cloth takes nothing more");
    p.setSelectedItem(items::empty());
    clickBlock(p, base, 1, {.5, 1, .5}, false, items::empty());
    ctx.check(items::idOf(p.getSelectedItem()) == "minecraft:apple", "an empty hand takes the last item back");
    co_await ctx.ticks(6);
    p.setSelectedItem(items::empty());
    lv::game::clearBlock(p.getDimensionBlockSource(), lv::game::blockPos(base));
    co_await ctx.ticks(2);
    int dropped = 0;
    for (auto* a : p.getDimensionBlockSource().getEntities(AABB{Vec3{base.x - 2.f, base.y - 2.f, base.z - 2.f}, Vec3{base.x + 3.f, base.y + 3.f, base.z + 3.f}}, 0.f)) {
        if (!a || a->getEntityTypeId() != ActorType::ItemEntity) continue;
        auto& it = static_cast<ItemActor&>(*a);
        if (items::idOf(it.item()) == "minecraft:apple") {
            dropped += items::countOf(it.item());
            a->remove();
        }
    }
    ctx.check(dropped == 3, "breaking the cloth drops the items on it, got " + std::to_string(dropped));
});

LV_REGISTER_SELFTEST("stock.request_screen", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's StockKeeperRequestScreen: a clipboard checklist becomes the order (schematic list
    // mode); category headers collapse; Shift moves a category to the end
    auto base = u56(ctx, 5670);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "");
    fill(ctx, base, "minecraft:coal", 10);
    auto ticker = at(base, 2, 0, 0);
    ctx.require(ctx.setBlock(ticker, stock::kStockTicker), "a stock ticker");
    auto& p          = ctx.player();
    auto  tickerItem = item(ctx, stock::kStockTicker);
    stock::tuneItem(tickerItem, network);
    stock::onTickerPlaced(ticker, &p, tickerItem);
    namespace cb = lv::feature::display::clipboard;
    cb::Entry coal{"Coal", false, items::toJson(item(ctx, "minecraft:coal")), 15};
    cb::Entry iron{"Iron", false, items::toJson(item(ctx, "minecraft:iron_ingot")), 3};
    auto      list = cb::makeWritten({{coal, iron}}, "Checklist");
    ctx.require(list.has_value(), "a checklist clipboard");
    p.setSelectedItem(*list);
    net::setTestCompanion(p, true);
    (void)net::takeTestMessages(p);
    lv::feature::common::screens::open(p, "stock.request", {{"pos", stock::posJson(ticker)}});
    JsonT data;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "screen.open") data = m.payload.value("data", JsonT());
    auto checklist = data.value("checklist", JsonT::array());
    ctx.check(checklist.size() == 1 && checklist[0].value("count", 0) == 10, "the checklist's coal is ordered as far as it is in stock; iron is not in stock");
    net::injectFromClient(p, "screen.action", {{"type", "stock.request"}, {"context", {{"pos", stock::posJson(ticker)}}}, {"action", {{"op", "order"}, {"items", checklist}, {"address", "Builder"}}}});
    ctx.check(pk.total("minecraft:coal") == 10 && !pk.sent.empty() && pk.sent.back().address == "Builder", "the checklist order is sent");
    p.setSelectedItem(items::empty());
    // categories: three dividers, the first moved to the end with Shift, then one collapsed
    auto cat = [&](JsonT const& action) {
        net::injectFromClient(p, "screen.action", {{"type", "stock.categories"}, {"context", {{"pos", stock::posJson(ticker)}}}, {"action", action}});
    };
    for (char const* name : {"a", "b", "c"}) {
        cat({{"op", "add"}, {"slot", -1}});
        auto n = stock::dataOf(ticker)->value("categories", JsonT::array()).size();
        cat({{"op", "rename"}, {"index", static_cast<int>(n) - 1}, {"name", name}});
    }
    cat({{"op", "move"}, {"index", 0}, {"delta", 1}, {"toEnd", true}});
    auto cats = stock::dataOf(ticker)->value("categories", JsonT::array());
    ctx.check(cats.size() == 3 && cats[2].value("name", std::string{}) == "a", "Shift moves the category to the bottom");
    net::injectFromClient(p, "screen.action", {{"type", "stock.request"}, {"context", {{"pos", stock::posJson(ticker)}}}, {"action", {{"op", "hide"}, {"index", 1}}}});
    auto hidden = stock::dataOf(ticker)->value("hidden", JsonT::object()).value(stock::uuidOf(p), JsonT::array());
    ctx.check(hidden.size() == 1 && hidden[0] == 1, "a click on a category header collapses it");
    net::setTestCompanion(p, false);
    (void)net::takeTestMessages(p);
});
