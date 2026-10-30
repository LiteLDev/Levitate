// Selftests of the copycat additions of U48 (testPos dx 4870..4899): bars and trapdoors as copycat
// panel materials (Create's CopycatSpecialCases / prepareMaterial / cycleMaterial), the copycat
// placement helpers and the off-hand material on placement (Create's setPlacedBy).

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/building/Building.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
namespace b = lv::feature::building;
using lv::core::GlobalPos;

GlobalPos at(GlobalPos const& p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
int       st(GlobalPos const& p, char const* name) {
    auto const* blk = b::blockAt(p);
    return blk ? b::stateOf(*blk, name, -1) : -1;
}
std::string materialState(GlobalPos const& p, std::string const& key) {
    auto const* m = b::copycatMaterialBlock(p);
    if (!m) return {};
    auto const& nbt = *m->mSerializationId;
    if (!nbt.contains("states") || !nbt["states"].contains(key)) return {};
    auto s = nbt["states"][key].toSnbt(SnbtFormat::Minimize, 0);
    std::erase(s, '"');
    return s;
}
void stand(Player& sp, GlobalPos const& p) { sp.teleport(Vec3{p.x + .5f, float(p.y), p.z + .5f}, DimensionType(p.dim)); }

} // namespace

LV_REGISTER_SELFTEST("building.copycat_bars_trapdoors", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4870);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = ctx.player();
    stand(sp, at(p, 0, 0, 4));
    GlobalPos floor = at(p, 0, 1, 0), wall = at(p, 2, 1, 0), ceiling = at(p, 4, 1, 0), step = at(p, 6, 1, 0), bars = at(p, 8, 1, 0);
    ctx.setBlock(floor, "levitate:copycat_panel", {{"levitate:facing", b::Up}});
    ctx.setBlock(wall, "levitate:copycat_panel", {{"levitate:facing", b::North}});
    ctx.setBlock(ceiling, "levitate:copycat_panel", {{"levitate:facing", b::Down}});
    ctx.setBlock(step, "levitate:copycat_step", {{"levitate:facing", b::North}});
    ctx.setBlock(bars, "levitate:copycat_panel", {{"levitate:facing", b::South}});
    co_await ctx.ticks(2);

    // bars: the panel takes iron bars and Create's bars; the step doesn't
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:iron_bars", 1), bars, b::South), "a panel takes iron bars");
    ctx.check(b::copycatMaterial(bars) == std::optional<std::string>("minecraft:iron_bars"), "the material is the bars");
    ctx.check(!b::useCopycat(sp, b::makeItem("minecraft:iron_bars", 1), step, b::North), "a step doesn't take bars");
    ctx.check(!b::useCopycat(sp, b::makeItem("minecraft:glass_pane", 1), at(p, 0, 1, 0), b::Up), "glass panes are no bars");
    if (auto const* m = lv::game::blockWith("minecraft:iron_bars")) ctx.check(b::isBarsMaterial(*m), "iron bars are bars");
    if (auto const* m = lv::game::blockWith("levitate:brass_bars")) ctx.check(b::isBarsMaterial(*m), "Create's bars are bars");
    if (auto const* m = lv::game::blockWith("minecraft:glass_pane")) ctx.check(!b::isBarsMaterial(*m), "a glass pane is no bars");

    // trapdoors (Create's prepareMaterial): flat in a floor panel facing the player's way, upside down
    // in a ceiling panel, open against a wall panel in the clicked half
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:trapdoor", 1), floor, b::Up, 0.2f), "a floor panel takes a trapdoor");
    ctx.check(materialState(floor, "open_bit") == "0b" || materialState(floor, "open_bit") == "0", "closed in a floor panel (" + materialState(floor, "open_bit") + ")");
    ctx.check(materialState(floor, "upside_down_bit").starts_with("0"), "bottom half in a floor panel");
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:trapdoor", 1), ceiling, b::Down, 0.8f), "a ceiling panel takes a trapdoor");
    ctx.check(materialState(ceiling, "upside_down_bit").starts_with("1"), "top half in a ceiling panel");
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:trapdoor", 1), wall, b::North, 0.8f), "a wall panel takes a trapdoor");
    ctx.check(materialState(wall, "open_bit").starts_with("1"), "open against a wall panel");
    ctx.check(materialState(wall, "upside_down_bit").starts_with("1"), "the clicked (upper) half");
    ctx.check(materialState(wall, "direction") == "3", "facing the panel's way (north: direction 3, got " + materialState(wall, "direction") + ")");
    ctx.check(!b::useCopycat(sp, b::makeItem("minecraft:trapdoor", 1), step, b::North), "a step doesn't take trapdoors");
    // Create's cycleMaterial: an open trapdoor flips its half, a closed one turns clockwise
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:trapdoor", 1), wall, b::North, 0.8f), "the same trapdoor again turns it");
    ctx.check(materialState(wall, "upside_down_bit").starts_with("0"), "the open trapdoor flips to the lower half");
    std::string before = materialState(floor, "direction");
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:trapdoor", 1), floor, b::Up), "the floor trapdoor turns");
    ctx.check(materialState(floor, "direction") != before, "a closed trapdoor turns clockwise (" + before + " -> " + materialState(floor, "direction") + ")");
    // our own trapdoors are trapdoors too
    if (auto const* m = lv::game::blockWith("levitate:train_trapdoor")) ctx.check(b::isTrapdoorMaterial(*m), "the train trapdoor is a trapdoor");
});

LV_REGISTER_SELFTEST("building.copycat_helpers", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4885);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = ctx.player();
    stand(sp, at(p, 0, 0, 5));
    auto* region = b::regionOf(p.dim);
    ctx.require(region != nullptr, "region");
    GlobalPos panel = at(p, 0, 1, 0), step = at(p, 0, 1, 3);
    ctx.setBlock(panel, "levitate:copycat_panel", {{"levitate:facing", b::Up}});
    ctx.setBlock(step, "levitate:copycat_step", {{"levitate:facing", b::North}, {"levitate:top", 1}});
    ctx.cleanupBlock(at(panel, 1, 0, 0));
    ctx.cleanupBlock(at(panel, -1, 0, 0));
    for (int i = -3; i <= 3; ++i) ctx.cleanupBlock(at(step, i, 0, 0));
    co_await ctx.ticks(2);

    // the panel: the free side nearest the click, across the facing (never above / below)
    auto off = b::copycatHelperOffset(*region, panel, *b::blockAt(panel), "levitate:copycat_panel", 0.9, 0.1, 0.5, 12);
    ctx.require(off.has_value(), "a panel helper spot");
    ctx.check(off->pos == at(panel, 1, 0, 0), "east of the panel (clicked near its east edge)");
    off = b::copycatHelperOffset(*region, panel, *b::blockAt(panel), "levitate:copycat_panel", 0.5, 0.95, 0.5, 12);
    ctx.check(off && off->pos.y == panel.y, "a click on the top never goes up");
    ctx.check(!b::copycatHelperOffset(*region, panel, *b::blockAt(panel), "levitate:copycat_step", 0.9, 0.1, 0.5, 12), "a step item does nothing on a panel");

    // through the use dispatch: a panel used on a panel places the next one, facing up
    ItemStack item = b::makeItem("levitate:copycat_panel", 4);
    ctx.give(sp, item, 0);
    ctx.check(b::onUse(sp, item, panel, b::Up, Vec3{0.9f, 0.95f, 0.5f}, true), "the helper takes the click");
    co_await ctx.ticks(1);
    ctx.check(b::idAt(at(panel, 1, 0, 0)) == "levitate:copycat_panel" && st(at(panel, 1, 0, 0), "levitate:facing") == b::Up, "a panel facing up east of it");

    // the step: along the line across its facing, past the steps already there, same half
    ctx.setBlock(at(step, 1, 0, 0), "levitate:copycat_step", {{"levitate:facing", b::North}, {"levitate:top", 1}});
    co_await ctx.ticks(1);
    off = b::copycatHelperOffset(*region, step, *b::blockAt(step), "levitate:copycat_step", 0.9, 0.5, 0.5, 12);
    ctx.require(off.has_value(), "a step helper spot");
    ctx.check(off->pos == at(step, 2, 0, 0), "past the neighbouring step");
    ItemStack steps = b::makeItem("levitate:copycat_step", 4);
    ctx.give(sp, steps, 0);
    ctx.check(b::onUse(sp, steps, step, b::Up, Vec3{0.1f, 0.5f, 0.5f}, true), "the step helper takes the click");
    co_await ctx.ticks(1);
    ctx.check(b::idAt(at(step, -1, 0, 0)) == "levitate:copycat_step", "a step west of it (clicked near the west edge)");
    ctx.check(st(at(step, -1, 0, 0), "levitate:facing") == b::North && st(at(step, -1, 0, 0), "levitate:top") == 1, "same facing and half");

    // Create's setPlacedBy: a block in the off hand becomes the material of the placed copycat
    sp.setOffhandSlot(b::makeItem("minecraft:stone", 2));
    ctx.check(b::onUse(sp, item, panel, b::Up, Vec3{0.1f, 0.5f, 0.5f}, true), "another panel by the helper");
    co_await ctx.ticks(1);
    ctx.check(b::copycatMaterial(at(panel, -1, 0, 0)) == std::optional<std::string>("minecraft:stone"), "it takes the off-hand stone");
    sp.setOffhandSlot(ItemStack{});
    ctx.give(sp, ItemStack{}, 0);
});

// the same through a player's use (the interaction dispatch), as a client's click arrives
LV_REGISTER_SELFTEST("building.copycat_special_use", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4895);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    GlobalPos bars = at(p, 0, 1, 0), trap = at(p, 2, 1, 0);
    ctx.setBlock(bars, "levitate:copycat_panel", {{"levitate:facing", b::North}});
    ctx.setBlock(trap, "levitate:copycat_panel", {{"levitate:facing", b::North}});
    co_await ctx.ticks(2);
    for (auto [pos, item, expect] : {std::tuple{bars, "minecraft:iron_bars", "minecraft:iron_bars"}, std::tuple{trap, "minecraft:trapdoor", "minecraft:trapdoor"}}) {
        stand(sp, at(pos, 0, -1, -3));
        sp.simulateLookAt(Vec3{pos.x + .5f, pos.y + .4f, pos.z + .9f}, sim::LookDuration::Instant);
        ctx.give(sp, b::makeItem(item, 2), 0);
        co_await ctx.ticks(2);
        for (int i = 0; i < 4 && !b::copycatMaterial(pos); ++i) {
            sp.simulateUseItemInSlotOnBlock(0, BlockPos{pos.x, pos.y, pos.z}, ScriptModuleMinecraft::ScriptFacing::North, Vec3{.5f, .4f, .8125f});
            co_await ctx.ticks(6);
        }
        ctx.check(b::copycatMaterial(pos) == std::optional<std::string>(expect), std::string("a player's use applies ") + item + " (" + b::copycatMaterial(pos).value_or("none") + ")");
        ctx.check(b::idAt(at(pos, 0, 0, -1)) != item, std::string("and places no ") + item);
    }
    ctx.cleanupBlock(at(bars, 0, 0, -1));
    ctx.cleanupBlock(at(trap, 0, 0, -1));
    ctx.give(sp, ItemStack{}, 0);
});
