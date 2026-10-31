// Selftests of the U54 alignment of the basin unit (testPos dx 5460..5479): the basin's goggle
// lines (Create's BasinBlockEntity.addToGoggleTooltip), none for blaze burners, hoppers feeding a
// basin and the basin items shown to vanilla clients.

#include <algorithm>
#include <string>

#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/VanillaItems.h"
#include "feature/goggles/GoggleInfo.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::basin {

namespace {

using selftest::Context;
namespace items = common::items;

GlobalPos off(GlobalPos p, int dx, int dy, int dz) {
    p.x += dx;
    p.y += dy;
    p.z += dz;
    return p;
}

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

std::string joined(std::vector<std::string> const& lines) {
    std::string out;
    for (auto const& l : lines) out += l + "|";
    return out;
}

} // namespace

LV_REGISTER_SELFTEST("basin.goggles", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = off(ctx.testPos(5460), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kBasin), "basin");
    auto burner = off(p, 2, 0, 0);
    ctx.require(ctx.setBlock(burner, kLitBurner, {{"levitate:facing", 2}}), "lit blaze burner");
    co_await ctx.ticks(1);
    auto& pl = ctx.player();
    auto  g  = items::make("levitate:goggles");
    ctx.require(g.has_value(), "goggles");
    pl.setArmor(SharedTypes::Legacy::ArmorSlot::Head, *g);
    ctx.check(goggles::overlayFor(pl, p).lines.empty(), "an empty basin shows nothing: " + joined(goggles::overlayFor(pl, p).lines));
    auto& region = pl.getDimensionBlockSource();
    (void)items::insert(region, game::blockPos(p), 1, item(ctx, "minecraft:iron_ingot", 3), false);
    (void)common::fluids::fill(region, game::blockPos(p), 1, {"minecraft:water", 250}, false);
    auto lines = goggles::overlayFor(pl, p).lines;
    auto text  = joined(lines);
    ctx.check(!lines.empty() && lines[0].find("Basin Contents:") != std::string::npos, "Basin Contents header: " + text);
    ctx.check(text.find(" x3") != std::string::npos, "the ingots with their count: " + text);
    ctx.check(text.find("250") != std::string::npos, "the water amount: " + text);
    ctx.check(std::find(lines.begin(), lines.end(), goggles::line("Contents:")) == lines.end(), "no generic contents header");
    ctx.check(goggles::overlayFor(pl, burner).lines.empty(), "a blaze burner shows no goggle lines (Create)");
    pl.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
    if (auto* b = basinops::get(region, p)) {
        *b = BasinState{};
        basinops::save(p, *b);
    }
});

// Vanilla hoppers use the basin's item capability (NeoForge), from above into its inputs.
LV_REGISTER_SELFTEST("basin.hopper", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = off(ctx.testPos(5465), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kBasin), "basin");
    auto hopper = off(p, 0, 1, 0);
    ctx.require(ctx.setBlock(hopper, "minecraft:hopper", {{"facing_direction", 0}}), "hopper above, pointing down");
    auto&      region = ctx.player().getDimensionBlockSource();
    Container* c      = region.tryGetContainer(game::blockPos(hopper));
    ctx.require(c != nullptr, "hopper container");
    auto ingots = item(ctx, "minecraft:copper_ingot", 2);
    c->addItem(ingots);
    co_await ctx.ticks(40);
    auto* b = basinops::get(region, p);
    ctx.require(b != nullptr, "basin state");
    int n = 0;
    for (auto const& s : b->in)
        if (items::idOf(s) == "minecraft:copper_ingot") n += items::countOf(s);
    ctx.check(n == 2, "the hopper put both ingots into the basin, got " + std::to_string(n));
    // vanilla clients see them as item entities on the basin's floor
    int shown = 0;
    for (auto const& s : common::vanillaitems::collect())
        if (s.owner == p && items::idOf(s.stack) == "minecraft:copper_ingot") {
            ++shown;
            ctx.check(s.pos.y > p.y && s.pos.y < p.y + .5f, "inside the basin");
        }
    ctx.check(shown == 1, "one entity per stack, got " + std::to_string(shown));
    *b = BasinState{};
    basinops::save(p, *b);
});

} // namespace lv::feature::basin
