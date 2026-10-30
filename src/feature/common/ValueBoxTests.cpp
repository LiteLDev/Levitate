// Selftests of the value box click and hold (U42, dx 4200..4209).

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {
using lv::selftest::Context;
using JsonT     = nlohmann::json;
namespace common = lv::feature::common;
namespace ia     = lv::feature::common::interactions;
namespace net    = lv::feature::net;
using lv::core::GlobalPos;

GlobalPos up(GlobalPos p, int dy) { return {p.dim, p.x, p.y + dy, p.z}; }

/// A right-click on face north of `pos` at the face centre through the shared dispatch.
ia::Result click(Player& p, GlobalPos const& pos, ItemStack const& held, bool first = true) {
    auto&       region  = p.getDimensionBlockSource();
    auto const& block   = region.getBlock(lv::game::blockPos(pos));
    std::string blockId = block.getTypeName();
    std::string itemId  = common::items::isEmpty(held) ? "" : common::items::idOf(held);
    ia::UseOnContext c{p, held, itemId, block, blockId, pos, 2, {0.5, 0.5, 0.0}, false, first};
    return ia::dispatchUseOn(c);
}

/// The token of the last `valuebox.hold` sent to the test companion (0 if none).
std::uint64_t holdToken(Player& p) {
    std::uint64_t token = 0;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "valuebox.hold") token = m.payload.value("token", std::uint64_t{0});
    return token;
}
} // namespace

LV_REGISTER_SELFTEST("common.value_box_hold", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace vb = common::valuebox;
    auto pos = up(ctx.testPos(4200), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:smooth_stone"), "place the test block");
    int shortClicks = 0;
    vb::registerBox("minecraft:smooth_stone", {
        .id            = "lv_hold",
        .labelFallback = "Hold Test",
        .board         = [](Block const&) { return lv::core::common::plainBoard(16, 4, ""); },
        .centre        = [](Block const&, int face) -> std::optional<lv::core::common::Vec> {
            if (face != 2) return std::nullopt;
            return lv::core::common::faceLocalPoint(face, 8, 8);
        },
        .get           = [](GlobalPos const&) { return lv::core::common::ValueSettings{0, 3}; },
        .set           = [](GlobalPos const&, lv::core::common::ValueSettings, Player&) {},
        .onShortClick  = [&shortClicks](GlobalPos const&, Player&) { ++shortClicks; },
    });
    auto& p = ctx.player();
    p.teleport(Vec3{pos.x + 0.5f, pos.y + 0.0f, pos.z - 1.5f}, p.getDimensionId());
    p.setSelectedItem(common::items::empty());
    co_await ctx.ticks(2);
    net::setTestCompanion(p, true);
    net::takeTestMessages(p);
    auto posJ = JsonT::array({pos.dim, pos.x, pos.y, pos.z});

    // short click: released before the board opens
    ctx.check(click(p, pos, p.getSelectedItem()) == ia::Result::Consume, "the click on the box is taken");
    auto token = holdToken(p);
    ctx.check(token != 0, "companions are asked to watch the held button");
    ctx.check(vb::holdPending(p, pos), "the hold waits for the client");
    ctx.check(click(p, pos, p.getSelectedItem(), false) == ia::Result::Consume && holdToken(p) == 0, "repeats of the held button do nothing");
    net::injectFromClient(p, "valuebox.release", {{"token", token}, {"state", "short"}});
    ctx.check(shortClicks == 1 && !vb::holdPending(p, pos), "a short click runs onShortClick");
    co_await ctx.ticks(6);

    // hold: the board opens
    click(p, pos, p.getSelectedItem());
    token = holdToken(p);
    net::injectFromClient(p, "valuebox.release", {{"token", token + 1}, {"state", "hold"}});
    ctx.check(vb::holdPending(p, pos), "a stale token is ignored");
    net::injectFromClient(p, "valuebox.release", {{"token", token}, {"state", "hold"}});
    co_await ctx.ticks(1);
    bool opened = false;
    for (auto const& m : net::takeTestMessages(p)) opened |= m.channel == "valuebox.open" && m.payload.value("box", "") == "lv_hold";
    ctx.check(opened, "holding opens the board");
    ctx.check(shortClicks == 1, "holding is not a short click");
    co_await ctx.ticks(6);

    // cancelled (looked away)
    click(p, pos, p.getSelectedItem());
    token = holdToken(p);
    net::injectFromClient(p, "valuebox.release", {{"token", token}, {"state", "cancel"}});
    ctx.check(shortClicks == 1 && net::takeTestMessages(p).empty() && !vb::holdPending(p, pos), "a cancelled hold does nothing");
    co_await ctx.ticks(6);

    // vanilla players: the click opens the form at once
    net::setTestCompanion(p, false);
    click(p, pos, p.getSelectedItem());
    ctx.check(!vb::holdPending(p, pos), "vanilla players get no hold");

    // filter slot with onHold
    int held = 0;
    common::filterslot::registerSlot("minecraft:smooth_stone", {
        .id     = "lv_hold_slot",
        .centre = [](Block const&, int face) -> std::optional<lv::core::common::Vec> {
            if (face != 2) return std::nullopt;
            return lv::core::common::faceLocalPoint(face, 8, 8);
        },
        .onHold = [&held](Player&, GlobalPos const&) { ++held; },
    });
    net::setTestCompanion(p, true);
    co_await ctx.ticks(6);
    click(p, pos, common::items::empty());
    token = holdToken(p);
    ctx.check(token != 0, "a slot with onHold waits for the hold");
    net::injectFromClient(p, "valuebox.release", {{"token", token}, {"state", "hold"}});
    ctx.check(held == 1, "holding the slot runs onHold");
    net::setTestCompanion(p, false);
    common::filterslot::unregisterSlot("minecraft:smooth_stone", "lv_hold_slot");
    vb::unregisterBox("minecraft:smooth_stone", "lv_hold");
    common::blockdata::erase(pos);
});
