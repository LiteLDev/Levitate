// Selftests of the display item contract (U33) and of the units that publish display items.
// testPos dx 3300..3399.

#include <atomic>
#include <cmath>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"

#include "core/itemrender/DisplayItem.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::itemrender {

namespace {

using selftest::Context;
namespace items = common::items;
namespace ci    = core::itemrender;
using Json      = nlohmann::json;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

ItemStack stackOf(Context& ctx, char const* id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), std::string("item ") + id);
    return *s;
}

/// The "itemrender.batch" messages recorded for a test companion since the last call.
std::vector<Json> batches(Player& p) {
    std::vector<Json> out;
    for (auto& e : net::takeTestMessages(p))
        if (e.channel == "itemrender.batch") out.push_back(std::move(e.payload));
    return out;
}

/// Entries of `list` ("set" / "move" / "del") about (owner, slot) over all batches.
int entries(std::vector<Json> const& all, char const* list, GlobalPos const& owner, std::int64_t slot) {
    int n = 0;
    for (auto const& b : all) {
        for (auto const& j : b.value(list, Json::array())) {
            if (std::string_view(list) == "del") {
                auto k = ci::decodeKey(j);
                if (k && k->first == std::array<int, 3>{owner.x, owner.y, owner.z} && k->second == slot) ++n;
            } else if (auto e = ci::decode(j); e && e->owner == std::array<int, 3>{owner.x, owner.y, owner.z} && e->slot == slot) {
                ++n;
            }
        }
    }
    return n;
}

/// A companion test player standing next to `pos`; unmarked when the test ends.
struct Companion {
    Player& player;
    explicit Companion(Context& ctx) : player(ctx.player()) {
        net::setTestCompanion(player, true);
        (void)net::takeTestMessages(player);
    }
    ~Companion() { net::setTestCompanion(player, false); }
};

std::atomic<bool> testProviderOn{false};

} // namespace

LV_REGISTER_SELFTEST("itemrender.sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = at(ctx.testPos(3300), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    Companion c(ctx);
    DisplayItem d;
    d.stack = stackOf(ctx, "minecraft:diamond", 3);
    d.at    = {o.x + .5, o.y + .5, o.z + .5};
    d.style = Style::Lying;
    set(o, 7, d);
    ctx.check(count(&o) == 1 && find(o, 7) != nullptr, "the item is registered");
    co_await ctx.ticks(2);
    auto b = batches(c.player);
    ctx.check(entries(b, "set", o, 7) == 1, "a companion nearby gets the item once");
    // the same item again: nothing to send
    set(o, 7, d);
    co_await ctx.ticks(2);
    ctx.check(entries(batches(c.player), "set", o, 7) == 0, "an unchanged item is not sent again");
    // moving it sends a move without the stack
    d.at.x += 1;
    d.lerpTicks = 1;
    set(o, 7, d);
    co_await ctx.ticks(2);
    b = batches(c.player);
    ctx.check(entries(b, "move", o, 7) == 1 && entries(b, "set", o, 7) == 0, "a moved item is sent as a move");
    for (auto const& m : b)
        for (auto const& e : m.value("move", Json::array())) ctx.check(!e.contains("i"), "moves carry no stack");
    // a new stack is a set
    d.stack = stackOf(ctx, "minecraft:emerald");
    set(o, 7, d);
    co_await ctx.ticks(2);
    ctx.check(entries(batches(c.player), "set", o, 7) == 1, "a changed stack is sent again");
    remove(o, 7);
    co_await ctx.ticks(2);
    ctx.check(entries(batches(c.player), "del", o, 7) == 1, "a removed item is deleted on the client");
    ctx.check(count(&o) == 0, "nothing left");
    // far away items are not sent
    auto far = at(o, 0, 0, 300);
    d.at     = {far.x + .5, far.y + .5, far.z + .5};
    set(far, 1, d);
    co_await ctx.ticks(12);
    ctx.check(entries(batches(c.player), "set", far, 1) == 0, "items beyond 64 blocks are not sent");
    clear(far);
    co_return;
});

LV_REGISTER_SELFTEST("itemrender.provider", [](Context& ctx) -> ll::coro::CoroTask<> {
    static bool registered = false;
    if (!registered) {
        registered = true;
        registerProvider([](std::string_view id) { return testProviderOn && id == "minecraft:lapis_block"; },
                         [](GlobalPos const& pos, Block const&, std::vector<std::pair<std::int64_t, DisplayItem>>& out) {
                             DisplayItem d;
                             if (auto s = items::make("minecraft:lapis_lazuli")) d.stack = *s;
                             d.at = {pos.x + .5, pos.y + 1.0, pos.z + .5};
                             out.emplace_back(3, d);
                         });
    }
    testProviderOn = true;
    struct Off {
        ~Off() { testProviderOn = false; }
    } off;
    auto o = at(ctx.testPos(3310), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(ctx.setBlock(o, "minecraft:lapis_block"), "block placed");
    ctx.check(hasProvider("minecraft:lapis_block"), "the block id has a provider");
    Companion c(ctx);
    // the client mod saw the block and asks for its items
    net::injectFromClient(c.player, "itemrender.query", {{"dim", o.dim}, {"pos", Json::array({Json::array({o.x, o.y, o.z})})}});
    ctx.check(find(o, 3) != nullptr, "the provider built the item");
    co_await ctx.ticks(2);
    ctx.check(entries(batches(c.player), "set", o, 3) == 1, "and the companion got it");
    // a block change drops provided items by itself
    ctx.setBlock(o, "minecraft:stone");
    co_await ctx.ticks(45);
    ctx.check(count(&o) == 0, "provided items go with their block");
    ctx.check(entries(batches(c.player), "del", o, 3) == 1, "and are deleted on the client");
    clear(o);
    co_return;
});

LV_REGISTER_SELFTEST("itemrender.facing", [](Context& ctx) {
    auto check = [&](int face, float yaw, float pitch) {
        auto t = facingFace(face);
        ctx.check(std::abs(t.yaw - yaw) < 1e-3f && std::abs(t.pitch - pitch) < 1e-3f, "face " + std::to_string(face));
    };
    check(0, 0, 90);
    check(1, 0, -90);
    check(2, 180, 0);
    check(3, 0, 0);
    check(4, -90, 0);
    check(5, 90, 0);
});

// ---- the units that publish display items --------------------------------------------------------

LV_REGISTER_SELFTEST("itemrender.depot", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = at(ctx.testPos(3320), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(ctx.setBlock(o, "levitate:depot"), "depot placed");
    co_await ctx.ticks(2);
    auto& region = ctx.player().getDimensionBlockSource();
    auto  rest   = common::items::insert(region, game::blockPos(o), 1, stackOf(ctx, "minecraft:iron_ingot", 5), false);
    ctx.require(items::isEmpty(rest), "the depot takes the stack");
    co_await ctx.ticks(3);
    auto list = itemsOf(o);
    ctx.check(list.size() == 1, "the held stack is a display item for the client mod");
    if (!list.empty()) {
        auto const* d = &list.front().second;
        ctx.check(d->style == Style::Lying && d->pile && std::abs(d->at.scale - .5f) < 1e-3f, "lying, half size, piled (Create's depot)");
        ctx.check(d->at.y > o.y + .5 && d->at.y < o.y + 1.1, "on top of the depot");
        ctx.check(d->hideActor != 0, "it hides the vanilla item entity from the client mod");
        ctx.check(items::countOf(d->stack) == 5, "the whole stack");
    }
    (void)common::items::extract(region, game::blockPos(o), 1, [](ItemStack const&) { return true; }, 64, false);
    co_await ctx.ticks(3);
    ctx.check(count(&o) == 0, "taken out: gone");
});

LV_REGISTER_SELFTEST("itemrender.chute", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = at(ctx.testPos(3330), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    auto chute = at(o, 0, 1, 0);
    ctx.require(ctx.setBlock(o, "minecraft:chest"), "chest below");
    ctx.require(ctx.setBlock(chute, "levitate:chute"), "chute");
    co_await ctx.ticks(2);
    auto& region = ctx.player().getDimensionBlockSource();
    auto  rest   = common::items::insert(region, game::blockPos(chute), -1, stackOf(ctx, "minecraft:stone", 5), false);
    ctx.require(items::isEmpty(rest), "the chute takes the stack");
    co_await ctx.ticks(1);
    auto const* d = find(chute, 0);
    ctx.check(d != nullptr, "the moving stack is a display item");
    if (d) ctx.check(d->style == Style::Lying && d->lerpTicks == 1, "lying, interpolated tick to tick");
    co_await ctx.ticks(20);
    ctx.check(count(&chute) == 0, "gone once it left the chute");
});

LV_REGISTER_SELFTEST("itemrender.filter_slot", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = at(ctx.testPos(3340), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(ctx.setBlock(o, "levitate:smart_chute"), "smart chute");
    co_await ctx.ticks(1);
    ctx.check(hasProvider("levitate:smart_chute"), "blocks with filter slots have a provider");
    common::filterslot::set(o, stackOf(ctx, "minecraft:diamond"));
    ctx.check(count(&o) >= 1, "the filter item is drawn on the slot's faces");
    for (auto const& [slot, d] : itemsOf(o)) {
        ctx.check(d.style == Style::Facing && std::abs(d.at.scale - .25f) < 1e-3f, "facing out, a quarter block");
        double dx = d.at.x - (o.x + .5), dy = d.at.y - (o.y + .5), dz = d.at.z - (o.z + .5);
        ctx.check(std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) > .45, "on the block's surface");
    }
    common::filterslot::set(o, items::empty());
    ctx.check(count(&o) == 0, "a cleared filter is not drawn");
});

LV_REGISTER_SELFTEST("itemrender.funnel_filter", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = at(ctx.testPos(3345), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(ctx.setBlock(o, "levitate:brass_funnel", {{"levitate:facing", 1}}), "brass funnel");
    co_await ctx.ticks(1);
    common::filterslot::set(o, stackOf(ctx, "minecraft:iron_ingot"));
    auto list = itemsOf(o);
    ctx.check(list.size() == 4, "the filter item on the four sides of an upright funnel, got " + std::to_string(list.size()));
    for (auto const& [slot, d] : list) {
        double dx = d.at.x - o.x, dz = d.at.z - o.z;
        ctx.check(dx < 0 || dx > 1 || dz < 0 || dz > 1, "outside the funnel's base (not hidden in its model)");
    }
    common::filterslot::set(o, items::empty());
});

LV_REGISTER_SELFTEST("itemrender.placard", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = at(ctx.testPos(3350), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(ctx.setBlock(o, "levitate:placard", {{"levitate:facing", 3}}), "placard facing south");
    co_await ctx.ticks(1);
    common::blockdata::edit(o)["building"]["item"] = items::toJson(stackOf(ctx, "minecraft:diamond"));
    refresh(o);
    auto const* d = find(o, 0);
    ctx.require(d != nullptr, "the placard's item is a display item");
    ctx.check(d->style == Style::Facing && std::abs(d->at.yaw) < 1e-3f && std::abs(d->at.pitch) < 1e-3f, "facing south");
    ctx.check(std::abs(d->at.z - (o.z + 3 / 16.0 + .01)) < 1e-3, "on the plate (against the north side)");
    common::blockdata::edit(o).erase("building");
    refresh(o);
    ctx.check(count(&o) == 0, "an empty placard shows nothing");
});

LV_REGISTER_SELFTEST("itemrender.deployer", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = at(ctx.testPos(3360), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(ctx.setBlock(o, "levitate:deployer", {{"levitate:facing", 3}}), "deployer");
    co_await ctx.ticks(2);
    Companion c(ctx);
    auto& region = ctx.player().getDimensionBlockSource();
    auto  rest   = common::items::insert(region, game::blockPos(o), 1, stackOf(ctx, "minecraft:iron_sword"), false);
    ctx.require(items::isEmpty(rest), "the deployer takes the sword");
    co_await ctx.ticks(3);
    bool sent = false;
    for (auto& e : net::takeTestMessages(c.player)) {
        if (e.channel != "assembly.deployer") continue;
        auto h = e.payload.value("held", Json());
        if (h.is_object() && items::idOf(items::fromJson(h)) == "minecraft:iron_sword") sent = true;
    }
    ctx.check(sent, "the client mod is told the held item (drawn in the hand)");
    (void)common::items::extract(region, game::blockPos(o), 1, [](ItemStack const&) { return true; }, 64, false);
    co_return;
});

} // namespace lv::feature::itemrender
