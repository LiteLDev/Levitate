// Selftest of the minecart coupling sync (U38, dx 3890; testPos shifted back into the run's
// ticking area with dz): companions get the coupled cart pairs to draw (Create's CouplingRenderer)
// when a coupling is made and when it goes.

#include "feature/contraption/U38Test.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"

#include "feature/bearings/Bearings.h"
#include "feature/net/Net.h"
#include "selftest/SelfTest.h"

namespace lv::feature::bearings {

using ::lv::feature::contraption::kU38Dz;

namespace {

using lv::selftest::Context;
namespace net = ::lv::feature::net;

GP at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

/// Whether the last "bearings.couplings" message lists the pair (nullopt: no message).
std::optional<bool> lists(std::vector<core::common::Envelope> const& msgs, std::int64_t a, std::int64_t b) {
    std::optional<bool> out;
    for (auto const& m : msgs) {
        if (m.channel != "bearings.couplings") continue;
        bool found = false;
        for (auto const& e : m.payload.value("couplings", Json::array()))
            if (e.size() == 2 && ((e[0] == a && e[1] == b) || (e[0] == b && e[1] == a))) found = true;
        out = found;
    }
    return out;
}

} // namespace

LV_REGISTER_SELFTEST("bearings.coupling_sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3890, kU38Dz);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = ctx.player().getDimensionBlockSource();
    for (int dx = 0; dx <= 4; ++dx) {
        ctx.setBlock(at(p, dx, -1, 0), "minecraft:stone");
        ctx.setBlock(at(p, dx, 0, 0), "minecraft:rail", {{"rail_direction", 1}});
    }
    auto* a = spawnCart(r, {p.x + .5, p.y + cartRailY(), p.z + .5});
    auto* b = spawnCart(r, {p.x + 3.5, p.y + cartRailY(), p.z + .5});
    ctx.require(a && b, "two minecarts");
    auto ua = uidOf(*a), ub = uidOf(*b);
    auto& player = ctx.player();
    net::setTestCompanion(player, true);
    co_await ctx.ticks(2);
    (void)net::takeTestMessages(player);

    ctx.check(coupleCarts(nullptr, ua, ub).empty(), "coupled"); // (a contraption coupling: no item drops when it goes)
    co_await ctx.ticks(2);
    ctx.check(lists(net::takeTestMessages(player), ua, ub) == std::optional<bool>(true), "companions get the new coupling");
    co_await ctx.ticks(2);
    ctx.check(!lists(net::takeTestMessages(player), ua, ub).has_value(), "an unchanged coupling isn't sent again");

    net::joinTestCompanion(player);
    ctx.check(lists(net::takeTestMessages(player), ua, ub) == std::optional<bool>(true), "a joining companion gets every coupling");

    // a broken cart drops its coupling after Carts.cpp's kMissingTicks (40)
    if (auto* ca = actorById(ua)) ca->remove();
    co_await ctx.ticks(45);
    ctx.check(lists(net::takeTestMessages(player), ua, ub) == std::optional<bool>(false), "companions are told the coupling went");
    if (auto* cb = actorById(ub)) cb->remove();
    net::setTestCompanion(player, false);
});

} // namespace lv::feature::bearings
