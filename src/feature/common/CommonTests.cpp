// Selftests of the U3 shared contracts (testPos dx 300..399).

#include <filesystem>
#include <fstream>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/common/Recipe.h"
#include "core/common/Transfer.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/common/Screens.h"
#include "feature/common/ValueBox.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Main.h"
#include "selftest/SelfTest.h"

namespace {
using lv::selftest::Context;
using JsonT = nlohmann::json;
namespace common    = lv::feature::common;
namespace blockdata = lv::feature::common::blockdata;
namespace items     = lv::feature::common::items;
namespace filter    = lv::feature::common::filter;
namespace net       = lv::feature::net;
using lv::core::GlobalPos;

ItemStack item(Context& ctx, char const* id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), std::string("item ") + id + " exists");
    return *s;
}

BlockSource& region(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

GlobalPos up(GlobalPos p, int dy) {
    p.y += dy;
    return p;
}
} // namespace

// ---- block data --------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("common.block_data", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = up(ctx.testPos(300), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:stone"), "place stone");
    co_await ctx.ticks(1);

    blockdata::edit(pos)["probe"] = 42;
    ctx.check(blockdata::get(pos) && blockdata::get(pos)->value("probe", 0) == 42, "edit() then get() reads back");
    int seen = 0;
    blockdata::forEachInChunk(pos.dim, lv::core::common::chunkOf(pos.x), lv::core::common::chunkOf(pos.z), [&](GlobalPos const& p, JsonT const& d) {
        if (p == pos && d.value("probe", 0) == 42) ++seen;
    });
    ctx.check(seen == 1, "forEachInChunk finds the document");

    blockdata::flush();
    auto key = lv::core::common::blockDataChunkKey(pos.dim, lv::core::common::chunkOf(pos.x), lv::core::common::chunkOf(pos.z));
    auto doc = lv::game::db::load(key);
    ctx.check(doc && doc->contains(lv::core::common::posKey(pos.x, pos.y, pos.z)), "flush() writes the chunk document to the world DB");

    // contraption hand-off: take() keeps the data out of the automatic erase
    auto taken = blockdata::take(pos);
    ctx.check(taken && taken->value("probe", 0) == 42 && !blockdata::has(pos), "take() removes and returns the document");
    blockdata::put(pos, *taken);
    ctx.check(blockdata::has(pos), "put() stores it again");

    // a block state change keeps it, a different block erases it (and runs erase listeners)
    ctx.setBlock(pos, "minecraft:stone");
    co_await ctx.ticks(1);
    ctx.check(blockdata::has(pos), "same block type keeps the document");
    ctx.setBlock(pos, "minecraft:dirt");
    co_await ctx.ticks(1);
    ctx.check(!blockdata::has(pos), "replacing the block erases the document");
    blockdata::flush();
    auto after = lv::game::db::load(key);
    ctx.check(!after || !after->contains(lv::core::common::posKey(pos.x, pos.y, pos.z)), "the erase is persisted");
});

// ---- capabilities ------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("common.items_chest", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = up(ctx.testPos(305), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:chest"), "place a chest");
    co_await ctx.ticks(1);
    auto& r  = region(ctx);
    auto  bp = lv::game::blockPos(pos);
    ctx.require(common::items::has(r, bp), "a chest has an item handler");
    ctx.check(common::items::slotCount(r, bp, 1) == 27, "27 slots");

    auto stone = item(ctx, "minecraft:stone", 40);
    auto sim   = common::items::insert(r, bp, 1, stone, true);
    ctx.check(items::isEmpty(sim), "simulated insert fits");
    ctx.check(items::isEmpty(common::items::extract(r, bp, 0, common::items::any(), 64, true)), "simulation changed nothing");
    auto rest = common::items::insert(r, bp, 1, stone, false);
    ctx.check(items::isEmpty(rest), "insert of 40 stone fits");
    common::items::insert(r, bp, 1, item(ctx, "minecraft:dirt", 5), false);

    auto onlyDirt = [](ItemStack const& s) { return items::idOf(s) == "minecraft:dirt"; };
    auto got      = common::items::extract(r, bp, 0, onlyDirt, 64, false);
    ctx.check(items::idOf(got) == "minecraft:dirt" && items::countOf(got) == 5, "extract with a predicate takes the dirt");
    auto stoneOut = common::items::extract(r, bp, 0, common::items::any(), 30, false);
    ctx.check(items::countOf(stoneOut) == 30, "extract takes the requested amount");
    auto restOut = common::items::extract(r, bp, 0, common::items::any(), 64, false);
    ctx.check(items::countOf(restOut) == 10, "the rest is still there");
});

LV_REGISTER_SELFTEST("common.items_furnace_sides", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = up(ctx.testPos(310), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:furnace"), "place a furnace");
    co_await ctx.ticks(1);
    auto& r  = region(ctx);
    auto  bp = lv::game::blockPos(pos);
    auto  h  = common::items::find({r, bp, 1});
    ctx.require(h.has_value(), "a furnace has an item handler");
    ctx.check(items::isEmpty(common::items::insert(r, bp, 1, item(ctx, "minecraft:raw_iron", 2), false)), "top accepts smeltables");
    ctx.check(items::idOf(h->getSlot({r, bp, -1}, 0)) == "minecraft:raw_iron", "top goes into the input slot");
    ctx.check(items::isEmpty(common::items::insert(r, bp, 4, item(ctx, "minecraft:coal", 3), false)), "side accepts fuel");
    ctx.check(items::idOf(h->getSlot({r, bp, -1}, 1)) == "minecraft:coal", "side goes into the fuel slot");
    ctx.check(items::isEmpty(common::items::extract(r, bp, 0, common::items::any(), 64, true)), "bottom only extracts results");
});

LV_REGISTER_SELFTEST("common.items_composter", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = up(ctx.testPos(315), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:composter"), "place a composter");
    co_await ctx.ticks(1);
    auto& r    = region(ctx);
    auto  bp   = lv::game::blockPos(pos);
    auto  rest = common::items::insert(r, bp, 1, item(ctx, "minecraft:pumpkin_pie", 3), false);
    ctx.check(items::countOf(rest) == 2, "a composter takes one compostable per insertion from the top");
    ctx.check(items::countOf(common::items::insert(r, bp, 2, item(ctx, "minecraft:pumpkin_pie"), true)) == 1, "not from the side");
    ctx.check(items::countOf(common::items::insert(r, bp, 1, item(ctx, "minecraft:stone"), true)) == 1, "not non-compostables");
    ctx.check(items::isEmpty(common::items::extract(r, bp, 0, common::items::any(), 1, true)), "no bone meal before it is ready");
});

LV_REGISTER_SELFTEST("common.fluids_cauldron", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace fluids = common::fluids;
    auto pos = up(ctx.testPos(320), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:cauldron"), "place a cauldron");
    co_await ctx.ticks(1);
    auto& r  = region(ctx);
    auto  bp = lv::game::blockPos(pos);
    ctx.check(fluids::fill(r, bp, 1, {"minecraft:water", 500}, false) == 0, "cauldrons take whole buckets only");
    ctx.check(fluids::fill(r, bp, 1, {"minecraft:water", 1000}, true) == 1000, "simulated fill of a bucket");
    ctx.check(fluids::tanks(r, bp).front().contents.empty(), "simulation left it empty");
    ctx.check(fluids::fill(r, bp, 1, {"minecraft:water", 1000}, false) == 1000, "fill a bucket of water");
    auto t = fluids::tanks(r, bp);
    ctx.check(!t.empty() && t.front().contents == fluids::FluidStack{"minecraft:water", 1000}, "the cauldron holds 1000 mB of water");
    ctx.check(fluids::drain(r, bp, 0, {"minecraft:lava", 1000}, true).empty(), "can't drain lava from water");
    ctx.check(fluids::drain(r, bp, 0, {"", 1000}, false) == fluids::FluidStack{"minecraft:water", 1000}, "drain the water");
    ctx.check(fluids::fill(r, bp, 1, {"minecraft:lava", 1000}, false) == 1000, "fill lava");
    co_await ctx.ticks(1);
    ctx.check(fluids::tanks(r, bp).front().contents == fluids::FluidStack{"minecraft:lava", 1000}, "the cauldron holds lava");
    ctx.check(fluids::drain(r, bp, 0, {"minecraft:lava", 1000}, false).amount == 1000, "drain the lava");
    ctx.check(fluids::fluidInItem(item(ctx, "minecraft:water_bucket")) == fluids::FluidStack{"minecraft:water", 1000}, "water bucket holds 1000 mB");
});

LV_REGISTER_SELFTEST("common.heat", [](Context& ctx) -> ll::coro::CoroTask<> {
    using lv::core::common::HeatLevel;
    auto pos = up(ctx.testPos(325), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto& r = region(ctx);
    ctx.setBlock(pos, "minecraft:magma");
    co_await ctx.ticks(1);
    ctx.check(common::heat::level(r, lv::game::blockPos(pos)) == HeatLevel::Smouldering, "magma is a passive heater");
    ctx.setBlock(pos, "minecraft:stone");
    co_await ctx.ticks(1);
    ctx.check(common::heat::level(r, lv::game::blockPos(pos)) == HeatLevel::None, "stone gives no heat");
    common::heat::registerBlock("minecraft:gold_block", [](BlockSource&, BlockPos const&, Block const&) { return HeatLevel::Seething; });
    ctx.setBlock(pos, "minecraft:gold_block");
    co_await ctx.ticks(1);
    ctx.check(common::heat::level(r, lv::game::blockPos(pos)) == HeatLevel::Seething, "registered providers win");
    common::heat::registerBlock("minecraft:gold_block", [](BlockSource&, BlockPos const&, Block const&) { return HeatLevel::None; });
});

// ---- filters -----------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("common.filter_list", [](Context& ctx) {
    auto f = item(ctx, filter::kListFilterId);
    auto stone = item(ctx, "minecraft:stone"), dirt = item(ctx, "minecraft:dirt");
    ctx.check(filter::matches(items::empty(), dirt), "an empty filter slot matches everything");
    ctx.check(filter::matches(stone, stone) && !filter::matches(stone, dirt), "a plain item matches the same item");
    ctx.check(!filter::matches(f, stone) && filter::matches(f, item(ctx, filter::kListFilterId)), "an empty list filter only matches filters");

    auto data = filter::readList(f);
    ctx.check(data.items.size() == 18, "a list filter has 18 slots");
    data.items[3] = stone;
    filter::writeList(f, data);
    ctx.check(filter::matches(f, stone) && !filter::matches(f, dirt), "allow-list");
    auto pred = filter::predicate(f);
    ctx.check(pred(stone) && !pred(dirt), "predicate() agrees");
    data.blacklist = true;
    filter::writeList(f, data);
    ctx.check(!filter::matches(f, stone) && filter::matches(f, dirt), "deny-list inverts");
    ctx.check(!f.getCustomLore().empty(), "the filter lore shows the summary");

    // nesting: a list filter inside a list filter
    auto outer = item(ctx, filter::kListFilterId);
    auto od    = filter::readList(outer);
    od.items[0] = f;
    filter::writeList(outer, od);
    ctx.check(filter::matches(outer, dirt) && !filter::matches(outer, stone), "nested filters are matched recursively");

    // respect data: a named stone only matches a named stone
    auto named = stone;
    items::setData(named, "probe", 1);
    auto rf = item(ctx, filter::kListFilterId);
    auto rd = filter::readList(rf);
    rd.items[0]    = named;
    rd.respectData = true;
    filter::writeList(rf, rd);
    ctx.check(filter::matches(rf, named) && !filter::matches(rf, stone), "respect data compares item data");
    rd.respectData = false;
    filter::writeList(rf, rd);
    ctx.check(filter::matches(rf, stone), "ignore data does not");

    // read back after a JSON round trip (BlockData / packets)
    auto back = items::fromJson(items::toJson(f));
    ctx.check(filter::readList(back).blacklist && filter::matches(back, dirt), "filters survive serialisation");
    ctx.check(filter::matchesFluid(item(ctx, "minecraft:water_bucket"), {"minecraft:water", 250}), "a bucket filters its fluid");
});

LV_REGISTER_SELFTEST("common.filter_attribute", [](Context& ctx) {
    using lv::core::common::AttributeMode;
    auto f     = item(ctx, filter::kAttributeFilterId);
    auto stone = item(ctx, "minecraft:stone"), stick = item(ctx, "minecraft:stick"), sword = item(ctx, "minecraft:iron_sword");
    auto attrs = filter::attributesOf(stone);
    bool placeable = std::any_of(attrs.begin(), attrs.end(), [](auto const& a) { return a.type == "placeable"; });
    ctx.check(placeable, "stone lists the placeable attribute");
    ctx.check(!filter::matches(f, stone), "an empty attribute filter only matches attribute filters");

    filter::AttributeFilter data;
    data.attributes.push_back({"placeable", "", false});
    filter::writeAttributes(f, data);
    ctx.check(filter::matches(f, stone) && !filter::matches(f, stick), "allow-list (any)");
    data.attributes.push_back({"not_stackable", "", false});
    data.mode = AttributeMode::WhitelistAll;
    filter::writeAttributes(f, data);
    ctx.check(!filter::matches(f, stone) && !filter::matches(f, sword), "allow-list (all) needs every attribute");
    data.mode = AttributeMode::Blacklist;
    filter::writeAttributes(f, data);
    ctx.check(!filter::matches(f, stone) && !filter::matches(f, sword) && filter::matches(f, stick), "deny-list");
    data.attributes = {{"placeable", "", true}};
    data.mode       = AttributeMode::WhitelistAny;
    filter::writeAttributes(f, data);
    ctx.check(filter::matches(f, stick) && !filter::matches(f, stone), "inverted attributes");
    ctx.check(filter::describe({"placeable", "", true}, "en_US") == "is not placeable", "Create's attribute wording");
    ctx.check(filter::describe({"added_by", "minecraft", false}, "en_US") == "was added by minecraft", "attribute values are formatted");
    auto addedBy = filter::attributesOf(stick);
    ctx.check(std::any_of(addedBy.begin(), addedBy.end(), [](auto const& a) { return a.type == "added_by" && a.value == "minecraft"; }), "added_by lists the namespace");
});

// ---- recipes -----------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("common.recipes", [](Context& ctx) {
    namespace fs = std::filesystem;
    namespace recipes = common::recipes;
    auto dir = lv::Mod::getInstance().getSelf().getModDir() / "recipes" / "lv_selftest";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream(dir / "iron_sheet.json") << R"({"type": "create:pressing", "ingredients": [{"tag": "c:ingots/iron"}],
        "results": [{"id": "minecraft:iron_nugget", "count": 2}, {"id": "minecraft:gold_nugget", "chance": 0.0}],
        "processing_time": 40, "heat_requirement": "heated"})";
    std::ofstream(dir / "broken.json") << "{ not json";
    recipes::reload();
    auto const& list = recipes::ofType("lv_selftest");
    fs::remove_all(dir, ec);
    ctx.require(list.size() == 1, "one valid recipe loaded (the broken one is skipped)");
    auto const& r = list.front();
    ctx.check(r.id == "levitate:lv_selftest/iron_sheet", "recipe id from the file name");
    ctx.check(r.heat == lv::core::common::HeatRequirement::Heated && r.processingTime == 40, "heat and time parsed");
    ctx.check(recipes::findFirst("lv_selftest", item(ctx, "minecraft:iron_ingot")) == &r, "tags resolve through tag files");
    ctx.check(!recipes::findFirst("lv_selftest", item(ctx, "minecraft:gold_ingot")), "other items don't match");
    auto out = recipes::rollResults(r);
    ctx.check(out.size() == 1 && items::countOf(out[0]) == 2, "chance 0 outputs are never produced");
    ctx.check(recipes::inTag("minecraft:wool", item(ctx, "minecraft:red_wool")), "minecraft:wool tag has every wool colour");
    recipes::reload();
});

// ---- protocol and screens ----------------------------------------------------------------------

LV_REGISTER_SELFTEST("common.screen_native", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    ctx.check(!net::isCompanion(p), "a simulated player is no companion");
    net::setTestCompanion(p, true);
    ctx.check(net::isCompanion(p) && net::companionProtocol(p) == lv::core::common::kProtocolVersion, "test companion");
    p.setSelectedItem(item(ctx, filter::kListFilterId));
    JsonT context{{"slot", p.getSelectedItemSlot()}};
    ctx.check(common::screens::open(p, "common.filter", context), "the filter screen opens");
    auto msgs = net::takeTestMessages(p);
    ctx.check(msgs.size() == 1 && msgs[0].channel == "screen.open" && msgs[0].payload["data"]["items"].size() == 18, "screen.open carries 18 slots");

    net::injectFromClient(p, "screen.action", {{"type", "common.filter"}, {"context", context}, {"action", {{"op", "set"}, {"index", 2}, {"item", items::toJson(item(ctx, "minecraft:stone"))}}}});
    net::injectFromClient(p, "screen.action", {{"type", "common.filter"}, {"context", context}, {"action", {{"op", "mode"}, {"blacklist", true}}}});
    auto held = p.getSelectedItem();
    auto data = filter::readList(held);
    ctx.check(items::idOf(data.items[2]) == "minecraft:stone" && data.blacklist, "native actions edit the held filter");
    msgs = net::takeTestMessages(p);
    ctx.check(msgs.size() == 2 && msgs[1].channel == "screen.update" && msgs[1].payload["data"]["blacklist"] == true, "each action is answered with fresh data");

    p.setSelectedItem(item(ctx, "minecraft:stick"));
    net::injectFromClient(p, "screen.action", {{"type", "common.filter"}, {"context", context}, {"action", {{"op", "reset"}}}});
    msgs = net::takeTestMessages(p);
    ctx.check(msgs.size() == 1 && msgs[0].channel == "screen.close", "the screen closes when the filter left the hand");

    // attribute filter: reference item, then add
    p.setSelectedItem(item(ctx, filter::kAttributeFilterId));
    ctx.check(common::screens::open(p, "common.attribute_filter", context), "the attribute filter screen opens");
    net::injectFromClient(p, "screen.action", {{"type", "common.attribute_filter"}, {"context", context}, {"action", {{"op", "reference"}, {"item", items::toJson(item(ctx, "minecraft:stone"))}}}});
    net::injectFromClient(p, "screen.action", {{"type", "common.attribute_filter"}, {"context", context}, {"action", {{"op", "add"}, {"type", "placeable"}, {"value", ""}, {"inverted", false}}}});
    net::injectFromClient(p, "screen.action", {{"type", "common.attribute_filter"}, {"context", context}, {"action", {{"op", "add"}, {"type", "enchanted"}, {"value", ""}, {"inverted", false}}}});
    auto attrs = filter::readAttributes(p.getSelectedItem());
    ctx.check(attrs.attributes.size() == 1 && attrs.attributes[0].type == "placeable", "only attributes of the reference item can be added");
    msgs = net::takeTestMessages(p);
    ctx.check(!msgs.empty() && !msgs.back().payload["data"]["options"].empty(), "the screen lists the reference item's attributes");
    net::setTestCompanion(p, false);
});

LV_REGISTER_SELFTEST("common.screen_form", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    p.setSelectedItem(item(ctx, filter::kListFilterId));
    ctx.check(!net::send(p, "screen.open", {}), "nothing is sent to vanilla players");
    ctx.check(common::screens::open(p, "common.filter", {{"slot", p.getSelectedItemSlot()}}), "vanilla players get the form");
    ctx.check(!common::screens::open(p, "common.filter", {{"slot", (p.getSelectedItemSlot() + 1) % 9}}), "no screen for a slot without a filter");
});

// ---- value boxes and filter slots --------------------------------------------------------------

LV_REGISTER_SELFTEST("common.value_box", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace vb = common::valuebox;
    using lv::core::common::ValueSettings;
    auto pos = up(ctx.testPos(330), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:smooth_stone"), "place the test block");
    co_await ctx.ticks(1);
    vb::registerBox("minecraft:smooth_stone", {
        .id            = "lv_test",
        .label         = "lv.test.value",
        .labelFallback = "Test Value",
        .board         = [](Block const&) { return lv::core::common::signedBoard(256, 32, "cw", "ccw"); },
        .centre        = [](Block const&, int face) -> std::optional<lv::core::common::Vec> {
            if (face != 2) return std::nullopt;
            return lv::core::common::faceLocalPoint(face, 8, 8);
        },
        .get           = [](GlobalPos const& p) {
            auto const* d = blockdata::get(p);
            return lv::core::common::signedToSettings(d ? d->value("lv_test", 16) : 16);
        },
        .set           = [](GlobalPos const& p, ValueSettings s, Player&) {
            blockdata::edit(p)["lv_test"] = lv::core::common::signedFromSettings(s, 256);
        },
        .format        = [](Block const&, ValueSettings s) { return std::to_string(lv::core::common::signedFromSettings(s, 256)) + " RPM"; },
    });
    auto const& block = region(ctx).getBlock(lv::game::blockPos(pos));
    ctx.check(vb::boxAt(block, 2, {0.5, 0.5, 0.0}) != nullptr, "a click in the box centre hits it");
    ctx.check(vb::boxAt(block, 2, {0.05, 0.95, 0.0}) == nullptr, "a click in the corner misses");
    ctx.check(vb::boxAt(block, 3, {0.5, 0.5, 1.0}) == nullptr, "no box on other faces");

    auto& p = ctx.player();
    p.teleport(Vec3{pos.x + 0.5f, pos.y + 0.0f, pos.z - 1.5f}, p.getDimensionId());
    co_await ctx.ticks(2);
    ctx.check(vb::apply(p, pos, "lv_test", {0, 64}), "apply() within reach");
    ctx.check(blockdata::get(pos) && blockdata::get(pos)->value("lv_test", 0) == -64, "row 0 is the negative direction");
    ctx.check(vb::apply(p, pos, "lv_test", {1, 999}) && blockdata::get(pos)->value("lv_test", 0) == 256, "values are clamped to the board");

    net::setTestCompanion(p, true);
    vb::openEditor(p, pos, "lv_test");
    auto msgs = net::takeTestMessages(p);
    ctx.check(msgs.size() == 1 && msgs[0].channel == "valuebox.open" && msgs[0].payload["rows"].size() == 2, "companions get the native board");
    ctx.check(msgs.size() == 1 && msgs[0].payload["labels"][1][256] == "256 RPM", "the board carries formatted labels");
    net::injectFromClient(p, "valuebox.set", {{"pos", JsonT::array({pos.dim, pos.x, pos.y, pos.z})}, {"box", "lv_test"}, {"row", 1}, {"value", 32}});
    ctx.check(blockdata::get(pos)->value("lv_test", 0) == 32, "valuebox.set from the client applies");
    net::setTestCompanion(p, false);
    vb::unregisterBox("minecraft:smooth_stone", "lv_test");
    ctx.check(vb::boxesOf("minecraft:smooth_stone") == nullptr, "unregister");
});

LV_REGISTER_SELFTEST("common.filter_slot", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace fs = common::filterslot;
    auto pos = up(ctx.testPos(335), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:smooth_stone"), "place the test block");
    co_await ctx.ticks(1);
    int changed = 0;
    fs::registerSlot("minecraft:smooth_stone", {
        .id        = "lv_test",
        .centre    = [](Block const&, int face) -> std::optional<lv::core::common::Vec> { return lv::core::common::faceLocalPoint(face, 8, 8); },
        .onChanged = [&changed](GlobalPos const&) { ++changed; },
    });
    auto& p = ctx.player();
    auto spec = *fs::slotAt(region(ctx).getBlock(lv::game::blockPos(pos)), 1, {0.5, 1.0, 0.5});
    ctx.check(fs::matches(pos, item(ctx, "minecraft:dirt"), "lv_test"), "an empty slot passes everything");
    p.setSelectedItem(item(ctx, "minecraft:stone", 5));
    fs::interact(p, pos, spec);
    ctx.check(items::idOf(fs::get(pos, "lv_test")) == "minecraft:stone", "right-click with an item sets the filter");
    ctx.check(items::countOf(p.getSelectedItem()) == 5, "plain items are not consumed");
    ctx.check(fs::matches(pos, item(ctx, "minecraft:stone"), "lv_test") && !fs::matches(pos, item(ctx, "minecraft:dirt"), "lv_test"), "matches()");

    auto f = item(ctx, filter::kListFilterId);
    p.setSelectedItem(f);
    fs::interact(p, pos, spec);
    bool survival = !p.isCreative();
    ctx.check(!survival || items::isEmpty(p.getSelectedItem()), "filter items are taken from the hand in survival");
    p.setSelectedItem(items::empty());
    fs::interact(p, pos, spec);
    ctx.check(items::isEmpty(fs::get(pos, "lv_test")), "an empty hand clears the slot");
    ctx.check(changed == 3, "onChanged runs for every change");
    fs::unregisterSlot("minecraft:smooth_stone", "lv_test");
    blockdata::erase(pos);
});

LV_REGISTER_SELFTEST("common.lang", [](Context& ctx) {
    for (auto const* locale : {"en_US", "zh_CN"}) {
        ctx.check(lv::translate("create.gui.filter.allow_list", "?", locale) != "?", std::string("filter strings in ") + locale);
        ctx.check(lv::translate("create.item_attributes.in_tag", "?", locale) != "?", std::string("attribute strings in ") + locale);
        ctx.check(lv::translate("lv.common.filter.title", "?", locale) != "?", std::string("Levitate strings in ") + locale);
    }
});
