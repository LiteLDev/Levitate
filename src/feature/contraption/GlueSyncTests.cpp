// Selftests of the glue outline sync to the client mod (U38, testPos dx 3880..3889).

#include <algorithm>

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "feature/contraption/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::contraption {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;

/// The last `contraption.glue` message among the test messages of `player` (null if none).
nlohmann::json lastGlue(Player const& player) {
    nlohmann::json out;
    for (auto const& m : net::takeTestMessages(player))
        if (m.channel == "contraption.glue") out = m.payload;
    return out;
}

} // namespace

LV_REGISTER_SELFTEST("contraption.glue_sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3880);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = ctx.player().getDimensionBlockSource();
    for (int x = 0; x < 3; ++x) {
        GP q{p.dim, p.x + x, p.y, p.z};
        ctx.cleanupBlock(q);
        auto const* stone = game::blockWith("minecraft:stone");
        ctx.require(stone && game::setBlock(region, game::blockPos(q), *stone), "stone placed");
    }
    cc::Pos a{p.x, p.y, p.z}, b{p.x + 2, p.y, p.z};
    clearGlue(p.dim, a, 4);
    auto& player = ctx.player();
    CompoundTag tag;
    tag["Name"]   = std::string(kSuperGlue);
    tag["Count"]  = static_cast<uchar>(1);
    tag["Damage"] = static_cast<short>(0);
    ctx.give(player, ItemStack::fromTag(tag), 0);
    player.setSelectedSlot(0);
    net::setTestCompanion(player, true);
    (void)net::takeTestMessages(player);

    detail::glueClick(player, a, false);
    auto first = lastGlue(player);
    ctx.check(first.is_object() && first["s"] == nlohmann::json::array({a.x, a.y, a.z}), "the first click is sent as the pending selection");
    ctx.check(first.value("m", 0) > 0, "the max glue range is sent");

    detail::glueClick(player, b, false);
    auto glued = lastGlue(player);
    bool has   = false;
    if (glued.is_object())
        for (auto const& r : glued["r"])
            if (r == nlohmann::json::array({a.x, a.y, a.z, b.x, b.y, b.z})) has = true;
    ctx.check(has, "the new region is sent");
    ctx.check(glued.is_object() && glued["s"].is_null(), "the selection is cleared after gluing");

    detail::glueOutlines(player);
    ctx.check(lastGlue(player).is_null(), "an unchanged state is not sent again");

    ctx.check(detail::glueAttack(player, cc::Pos{p.x + 1, p.y, p.z}), "hitting removes the region");
    auto removed = lastGlue(player);
    ctx.check(removed.is_object() && std::none_of(removed["r"].begin(), removed["r"].end(), [&](nlohmann::json const& r) {
                  return r == nlohmann::json::array({a.x, a.y, a.z, b.x, b.y, b.z});
              }),
              "the removal is sent");
    net::setTestCompanion(player, false);
    clearGlue(p.dim, a, 4);
});

} // namespace lv::feature::contraption
