// Selftests of the advancement triggers wired in the units (U49, testPos dx 4900..4929): the
// awards fire from the units' real processing (press / spout counters, millstone, display link
// and board, trains, stock ticker, table cloth shop, factory gauge, linked controller).

#include <string>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "core/tracks/Shape.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/display/Internal.h"
#include "feature/equipment/Equipment.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/redstone/RedstoneLink.h"
#include "feature/stock/Stock.h"
#include "feature/stock/StockInternal.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
using JsonT     = nlohmann::json;
namespace adv   = lv::feature::advancements;
namespace items = lv::feature::common::items;
namespace caps  = lv::feature::common;
namespace dsp   = lv::feature::display;
namespace eq    = lv::feature::equipment;
namespace rl    = lv::feature::redstone::link;
namespace stock = lv::feature::stock;
namespace tn    = lv::feature::trains;
namespace tr    = lv::feature::tracks;
namespace ct    = lv::core::trains;

GlobalPos at(GlobalPos p, int dx, int dy = 0, int dz = 0) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

void revokeAll(Player& p, std::initializer_list<char const*> ids) {
    for (auto const* id : ids) adv::revoke(p, id);
}

BlockSource& region(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

// ---- block owners and per-block counters ------------------------------------------------------

LV_REGISTER_SELFTEST("advancements.trigger_counters", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4920), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    revokeAll(p, {"mechanical_press", "track_crafting_factory", "spout", "foods", "self_deploying", "packager", "frogport", "factory_gauge"});
    // the blocks U49 added to Create's registerAwardables keep their placer as owner
    int x = 0;
    for (auto const* id : {"levitate:packager", "levitate:repackager", "levitate:package_frogport", "levitate:factory_gauge"}) {
        auto pos = at(base, x++, 0, 0);
        ctx.require(ctx.setBlock(pos, id), std::string("place ") + id);
        adv::setOwner(pos, p);
        ctx.check(!adv::ownerOf(pos).empty(), std::string(id) + " remembers its owner");
    }
    // Create's MechanicalPressBlockEntity#onItemPressed: tracks count towards 1000
    auto press = at(base, 0, 0, 2);
    ctx.require(ctx.setBlock(press, "levitate:mechanical_press"), "place a press");
    adv::setOwner(press, p);
    auto uuid = adv::ownerOf(press);
    ctx.require(!uuid.empty(), "the press has an owner");
    adv::onItemPressed(press, "levitate:track", 999);
    ctx.check(adv::has(p, "mechanical_press"), "pressing awards mechanical_press");
    ctx.check(!adv::has(p, "track_crafting_factory"), "999 tracks are not enough");
    adv::onItemPressed(press, "minecraft:iron_ingot", 1);
    ctx.check(!adv::has(p, "track_crafting_factory"), "other results don't count");
    adv::onItemPressed(press, "levitate:track", 1);
    ctx.check(adv::has(p, "track_crafting_factory"), "the 1000th track awards track_crafting_factory");
    // Create's SpoutBlockEntity: the three foods made by one spout
    auto spout = at(base, 2, 0, 2);
    ctx.require(ctx.setBlock(spout, "levitate:spout"), "place a spout");
    adv::setOwner(spout, p);
    adv::onSpoutFilled(spout, "levitate:honeyed_apple");
    ctx.check(adv::has(p, "spout") && !adv::has(p, "foods"), "filling awards spout");
    adv::onSpoutFilled(spout, "levitate:sweet_roll");
    ctx.check(!adv::has(p, "foods"), "two of the three foods");
    adv::onSpoutFilled(spout, "levitate:chocolate_glazed_berries");
    ctx.check(adv::has(p, "foods"), "all three foods award foods");
    // Create's DeployerMovementBehaviour#checkForTrackPlacementAdvancement
    JsonT doc{{"advancements", {{"owner", uuid}}}};
    adv::onMovingDeployerUsed("mechanical_bearing", doc, "levitate:track", true);
    ctx.check(!adv::has(p, "self_deploying"), "not on a minecart contraption or train");
    adv::onMovingDeployerUsed("train", doc, "minecraft:dirt", true);
    ctx.check(!adv::has(p, "self_deploying"), "not a track");
    adv::onMovingDeployerUsed("train", doc, "levitate:track", true);
    ctx.check(adv::has(p, "self_deploying"), "a deployer on a train placing track awards self_deploying");
    revokeAll(p, {"mechanical_press", "track_crafting_factory", "spout", "foods", "self_deploying"});
});

// ---- a real machine: the millstone ------------------------------------------------------------

LV_REGISTER_SELFTEST("advancements.trigger_millstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4905), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    adv::revoke(p, "millstone");
    auto mill = at(base, 0, 1);
    ctx.require(ctx.setBlock(base, "levitate:kinetics_test_source", {{"levitate:axis", 1}}), "place the source");
    ctx.require(ctx.setBlock(mill, "levitate:millstone"), "place the millstone");
    adv::setOwner(mill, p);
    co_await ctx.ticks(2);
    lv::kinetics::setGeneratedSpeed(base, 256);
    co_await ctx.ticks(2);
    auto rest = caps::items::insert(region(ctx), BlockPos{mill.x, mill.y, mill.z}, 1, item(ctx, "minecraft:bone"), false);
    ctx.require(items::isEmpty(rest), "a bone goes in");
    for (int i = 0; i < 60 && !adv::has(p, "millstone"); ++i) co_await ctx.ticks(1);
    ctx.check(adv::has(p, "millstone"), "milling awards millstone to the owner");
    lv::kinetics::setGeneratedSpeed(base, 0);
    adv::revoke(p, "millstone");
});

// ---- linked controller (U23 link query) -------------------------------------------------------

LV_REGISTER_SELFTEST("advancements.trigger_linked_controller", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4910), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    adv::revoke(p, "linked_controller");
    auto controller = item(ctx, "levitate:linked_controller");
    auto pair       = [](JsonT a, JsonT b) { return JsonT::array({a, b}); };
    JsonT none      = nullptr;
    JsonT freq      = JsonT::array({pair(items::toJson(item(ctx, "minecraft:redstone")), items::toJson(item(ctx, "minecraft:emerald"))), pair(none, none),
                                    pair(none, none), pair(none, none), pair(none, none), pair(none, none)});
    items::setData(controller, eq::kControllerData, JsonT{{"freq", freq}});
    p.getInventory().setItem(p.getSelectedItemSlot(), controller);
    p.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    auto key = rl::keyOf(item(ctx, "minecraft:redstone"), item(ctx, "minecraft:emerald"));
    ctx.check(!rl::hasListeningLinks(base.dim, key), "nobody listens yet");
    eq::controllerButtons(p, {true, false, false, false, false, false}, std::nullopt);
    eq::controllerButtons(p, {false, false, false, false, false, false}, std::nullopt);
    ctx.check(!adv::has(p, "linked_controller"), "a signal nobody receives awards nothing");
    auto listener = rl::add({.pos = at(base, 3), .key = key, .listening = true});
    ctx.check(rl::hasListeningLinks(base.dim, key), "a receiver listens on the frequency");
    ctx.check(!rl::hasListeningLinks(base.dim == 0 ? 1 : 0, key), "not in another dimension");
    eq::controllerButtons(p, {true, false, false, false, false, false}, std::nullopt);
    ctx.check(adv::has(p, "linked_controller"), "pressing a button a link listens to awards linked_controller");
    eq::controllerButtons(p, {false, false, false, false, false, false}, std::nullopt);
    rl::remove(listener);
    p.getInventory().setItem(p.getSelectedItemSlot(), items::empty());
    adv::revoke(p, "linked_controller");
});

// ---- stock: ticker, table cloth shop, factory gauge -------------------------------------------

/// Test packagers: barrels whose requests are only recorded.
struct TestPackagers {
    std::optional<stock::PackagerProvider> previous;
    int                                    sent = 0;
    TestPackagers() {
        previous = stock::currentPackagerProvider();
        stock::PackagerProvider pr;
        pr.isPackager     = [](BlockSource& r, BlockPos const& pos) { return lv::game::blockIdAt(r, pos) == "minecraft:barrel"; };
        pr.availableItems = [](BlockSource& r, BlockPos const& pos) -> std::optional<std::vector<stock::BigStack>> {
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
        pr.inventoryId    = [](BlockSource& r, BlockPos const& pos) { return lv::game::globalPos(r, pos).key(); };
        pr.queuedPackages = [](BlockSource&, BlockPos const&) { return 0; };
        pr.send           = [this](BlockSource&, BlockPos const&, std::vector<stock::PackagingRequest> const& reqs) { sent += int(reqs.size()); };
        stock::setPackagerProvider(pr);
    }
    ~TestPackagers() { stock::setPackagerProvider(previous); }
};

/// A barrel (packager) with a stock link on top, on a new network.
std::string linkedBarrel(Context& ctx, GlobalPos const& barrel, char const* fillId, int count) {
    ctx.require(ctx.setBlock(barrel, "minecraft:barrel"), "place a barrel");
    auto rest = caps::items::insert(region(ctx), lv::game::blockPos(barrel), -1, item(ctx, fillId, count), false);
    ctx.require(items::isEmpty(rest), "the barrel takes the items");
    auto link = at(barrel, 0, 1, 0);
    ctx.require(ctx.setBlock(link, stock::kStockLink, {{"levitate:face", 0}, {"levitate:facing", 2}}), "place a stock link");
    stock::onLinkPlaced(link, &ctx.player(), item(ctx, stock::kStockLink));
    return stock::networkAt(link);
}

LV_REGISTER_SELFTEST("advancements.trigger_stock", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4912), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    revokeAll(p, {"stock_ticker", "table_cloth_shop", "factory_gauge"});
    TestPackagers pk;
    auto          network = linkedBarrel(ctx, base, "minecraft:iron_ingot", 64);
    // Create's PackageOrderRequestPacket: an order from the stock ticker
    auto tickerPos = at(base, 0, 0, 3);
    ctx.require(ctx.setBlock(tickerPos, stock::kStockTicker, {{"levitate:facing", 0}}), "place a stock ticker");
    ItemStack tickerItem = item(ctx, stock::kStockTicker);
    stock::tuneItem(tickerItem, network);
    stock::onTickerPlaced(tickerPos, &p, tickerItem);
    stock::placeOrder(p, tickerPos, {}, "Nowhere", false);
    ctx.check(!adv::has(p, "stock_ticker"), "an empty order awards nothing");
    stock::placeOrder(p, tickerPos, {{item(ctx, "minecraft:iron_ingot"), 4}}, "Mill", false);
    ctx.check(adv::has(p, "stock_ticker"), "ordering awards stock_ticker");
    // Create's TableClothBlock#setPlacedBy with an encoded order
    if (lv::game::blockWith("levitate:white_table_cloth")) {
        auto cloth = at(base, 3, 0, 3);
        ctx.require(ctx.setBlock(cloth, "levitate:white_table_cloth"), "place a table cloth");
        ItemStack clothItem = item(ctx, "levitate:white_table_cloth");
        items::setData(clothItem, stock::kRequestData,
                       {{"order", stock::orderToJson({{item(ctx, "minecraft:iron_ingot"), 2}})}, {"address", "Buyer"}, {"ticker", stock::posJson(tickerPos)}});
        stock::onClothPlaced(cloth, p, clothItem);
        ctx.check(adv::has(p, "table_cloth_shop"), "placing a programmed table cloth awards table_cloth_shop");
    }
    // Create's FactoryPanelBehaviour#tickRequests: a gauge with connected inputs requests them
    auto wall = at(base, 0, 0, 4);
    ctx.setBlock(wall, "minecraft:stone");
    ctx.setBlock(at(wall, 1, 0, 0), "minecraft:stone");
    auto out = at(wall, 0, 0, -1), in = at(wall, 1, 0, -1);
    for (auto g : {out, in}) {
        ctx.require(ctx.setBlock(g, stock::kFactoryGauge, {{"levitate:face", 1}, {"levitate:facing", 2}}), "place a gauge on the wall");
        ctx.require(stock::placeGaugePanel(g, 0, network, nullptr), "panel");
    }
    adv::setOwner(out, p);
    p.setSelectedItem(item(ctx, "minecraft:iron_block"));
    stock::useGauge(p, out, 0, false);
    p.setSelectedItem(item(ctx, "minecraft:iron_ingot"));
    stock::useGauge(p, in, 0, false);
    p.setSelectedItem(items::empty());
    ctx.require(stock::connectPanels(&p, out, 0, in, 0), "the panels connect");
    lv::feature::common::valuebox::apply(p, out, "panel0", {0, 2});
    auto& panel                      = stock::editData(out)["panels"]["0"];
    panel["address"]                 = "Press";
    panel["targetedBy"][0]["amount"] = 9;
    for (int i = 0; i < 120 && !adv::has(p, "factory_gauge"); ++i) co_await ctx.ticks(1);
    ctx.check(pk.sent > 0, "the gauge requested its inputs");
    ctx.check(adv::has(p, "factory_gauge"), "a gauge requesting its inputs awards factory_gauge to its owner");
    stock::removeGaugePanel(in, 0, nullptr);
    stock::removeGaugePanel(out, 0, nullptr);
    revokeAll(p, {"stock_ticker", "table_cloth_shop", "factory_gauge"});
});

// ---- trains: assembly, conductor, display link and board --------------------------------------

std::vector<std::pair<std::string, int>> xo() {
    int s = int(lv::core::tracks::Shape::XO);
    return {{std::string(tr::kShapeState), s % 16}, {std::string(tr::kShapeHiState), s / 16}, {std::string(tr::kTurnState), 0}};
}

void clearBox(Context& ctx, GlobalPos a, GlobalPos b) {
    auto&       r   = region(ctx);
    auto const* air = lv::game::blockWith("minecraft:air");
    if (!air) return;
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
            for (int z = std::min(a.z, b.z); z <= std::max(a.z, b.z); ++z) {
                BlockPos bp{x, y, z};
                if (!r.getBlock(bp).isAir()) lv::game::setBlock(r, bp, *air);
            }
}

LV_REGISTER_SELFTEST("advancements.trigger_trains", [](Context& ctx) -> ll::coro::CoroTask<> {
    // a 28-block east-west line with a station at x + 20 (its assembly area runs west of it)
    auto base = ctx.testPos(4900, 2);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(at(base, 27)), "test area loaded");
    auto& p = ctx.player();
    revokeAll(p, {"train", "long_train", "conductor", "display_link", "display_board_0"});
    clearBox(ctx, at(base, -1, 0, -2), at(base, 28, 4, 2));
    co_await ctx.ticks(2);
    for (int x = 0; x < 28; ++x) ctx.setBlock(at(base, x), std::string(tr::kTrackId), xo());
    co_await ctx.ticks(3);
    auto track   = at(base, 20);
    auto station = at(base, 20, 0, 1);
    ctx.setBlock(station, std::string(tn::kStation));
    co_await ctx.ticks(2);
    std::string err;
    ctx.require(tn::createStation(station, track, 1, 0, &err), "station created " + err);
    auto* s = tn::stationAt(station);
    ctx.require(s != nullptr, "the station exists");
    s->name = "U49 Trigger Station";
    tn::refreshStationPoints();
    adv::setOwner(station, p);
    ctx.require(!adv::ownerOf(station).empty(), "the station keeps its owner");
    // Create's StationBlockEntity#assemble: train (and long_train from 6 carriages)
    tn::setAssembling(*s, true, nullptr);
    ctx.check(tn::useCasingOnTrack(nullptr, at(track, -1), true).starts_with("+"), "a bogey at the station marker");
    ctx.setBlock(at(track, -1, 2), std::string(tn::kControls), {{"levitate:facing", 4}});
    auto id = tn::assemble(*s, nullptr);
    ctx.require(id.has_value(), "the train assembles");
    ctx.check(adv::has(p, "train"), "assembling awards train to the station's owner");
    ctx.check(!adv::has(p, "long_train"), "one carriage is no long train");
    // Create's ScheduleItem#handScheduleTo: conductor to the player
    auto* t = tn::findTrain(*id);
    ctx.require(t != nullptr, "the train exists");
    ct::Schedule sch;
    auto         e              = ct::Entry::makeDefault();
    e.instruction.data["Text"]  = s->name;
    sch.entries.push_back(e);
    p.getInventory().setItem(p.getSelectedItemSlot(), tn::makeScheduleItem(sch));
    ctx.check(tn::handScheduleTo(p, *t, p.getSelectedItemSlot()), "the schedule is handed to the train");
    ctx.check(adv::has(p, "conductor"), "handing a schedule awards conductor");
    // a display link on the station targeting a display board: display_link and display_board_0
    auto link  = at(station, 0, 1);
    auto board = at(station, 3, 1);
    ctx.require(ctx.setBlock(board, dsp::kBoardId, {{"levitate:facing", 0}}), "place a display board");
    dsp::board::updateColumns(region(ctx), board);
    ctx.require(ctx.setBlock(link, dsp::kLinkId), "place a display link");
    if (auto const* up = dsp::withStates(region(ctx).getBlock(lv::game::blockPos(link)), {{"minecraft:block_face", std::string("up")}}))
        lv::game::setBlock(region(ctx), lv::game::blockPos(link), *up);
    dsp::link::LinkData d;
    d.targetOffset = {board.x - link.x, board.y - link.y, board.z - link.z};
    dsp::link::write(link, d);
    dsp::link::track(link);
    adv::setOwner(link, p);
    dsp::link::selectSource(link, "station_summary");
    ctx.check(dsp::link::update(link), "the link sends the station summary");
    ctx.check(adv::has(p, "display_link"), "a link sending data awards display_link");
    ctx.check(adv::has(p, "display_board_0"), "a train listed on a board awards display_board_0");
    // teardown
    tn::removeTrain(*id);
    tn::removeStation(station);
    co_await ctx.ticks(2);
    clearBox(ctx, at(base, -1, 0, -2), at(base, 28, 4, 2));
    p.getInventory().setItem(p.getSelectedItemSlot(), items::empty());
    revokeAll(p, {"train", "long_train", "conductor", "display_link", "display_board_0"});
});

} // namespace
