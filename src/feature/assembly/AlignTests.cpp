// Selftests of the U54 alignment of the assembly unit (testPos dx 5400..5429): arm interaction
// points (packager, funnels, saws), Create's vanilla copper deploying recipes, the deployer's goggle
// lines, the transitional item progress for the client mod's bar and the vanilla-client items.

#include <algorithm>
#include <cmath>

#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/assembly/SequencedAssembly.h"
#include "feature/common/Items.h"
#include "feature/common/VanillaItems.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::assembly {

namespace {

using selftest::Context;
namespace items = common::items;
namespace sa    = sequenced;

constexpr char kSource[] = "levitate:kinetics_test_source";

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

// Create's AllArmInteractionPointTypes: packagers and repackagers are points; extracting funnels,
// funnels pushing onto their belt and saws that don't turn are not.
LV_REGISTER_SELFTEST("assembly.arm_points_create", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = off(ctx.testPos(5400), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& region = ctx.player().getDimensionBlockSource();
    auto  pk     = base;
    ctx.require(ctx.setBlock(pk, "levitate:packager"), "packager");
    ctx.check(arm::isPoint(region, game::blockPos(pk)), "a packager is an arm point");
    auto rp = off(base, 2, 0, 0);
    ctx.require(ctx.setBlock(rp, "levitate:repackager"), "repackager");
    ctx.check(arm::isPoint(region, game::blockPos(rp)), "a repackager is an arm point");
    auto fn = off(base, 4, 0, 0);
    ctx.require(ctx.setBlock(fn, "levitate:andesite_funnel", {{"levitate:facing", 1}}), "funnel");
    ctx.check(arm::isPoint(region, game::blockPos(fn)), "an inserting funnel is a point");
    ctx.require(ctx.setBlock(fn, "levitate:andesite_funnel", {{"levitate:facing", 1}, {"levitate:extracting", 1}}), "extracting funnel");
    ctx.check(!arm::isPoint(region, game::blockPos(fn)), "an extracting funnel is no point");
    ctx.require(ctx.setBlock(fn, "levitate:brass_funnel", {{"levitate:facing", 2}, {"levitate:shape", 3}}), "pushing belt funnel");
    ctx.check(!arm::isPoint(region, game::blockPos(fn)), "a belt funnel pushing onto its belt is no point");
    auto saw = off(base, 6, 0, 0);
    ctx.require(ctx.setBlock(saw, "levitate:mechanical_saw", {{"levitate:facing", 1}, {"levitate:axis_along_first", 0}}), "saw facing up");
    co_await ctx.ticks(2);
    ctx.check(!arm::isPoint(region, game::blockPos(saw)), "a saw that does not turn is no point");
    auto src = off(saw, 0, 0, -1);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 2}}), "source behind the saw");
    kinetics::refresh(src);
    kinetics::setGeneratedSpeed(src, 32);
    co_await ctx.ticks(3);
    ctx.require(kinetics::speedAt(saw) != 0, "the saw turns");
    ctx.check(arm::isPoint(region, game::blockPos(saw)), "a turning saw facing up is a point");
    kinetics::setGeneratedSpeed(src, 0);
});

// Create's generated deploying recipes for vanilla copper (DeployingRecipeGen): honeycomb blocks
// wax, axes scrape wax and oxidation; honeycomb items have no deploying recipe.
LV_REGISTER_SELFTEST("assembly.copper_wax", [](Context& ctx) {
    auto deploy = [&](char const* on, char const* with) -> std::string {
        auto held = item(ctx, with);
        auto out  = deployer::deployOnItem(item(ctx, on), held, false); // a simulation returns no outputs
        return out && out->size() == 1 ? items::idOf(out->front()) : std::string("-");
    };
    ctx.check(deploy("minecraft:copper_block", "minecraft:honeycomb_block") == "minecraft:waxed_copper", "copper block waxed");
    ctx.check(deploy("minecraft:exposed_cut_copper_slab", "minecraft:honeycomb_block") == "minecraft:waxed_exposed_cut_copper_slab", "slab waxed");
    ctx.check(deploy("minecraft:waxed_weathered_copper_door", "minecraft:iron_axe") == "minecraft:weathered_copper_door", "door unwaxed");
    ctx.check(deploy("minecraft:oxidized_copper_bulb", "minecraft:stone_axe") == "minecraft:weathered_copper_bulb", "bulb deoxidised");
    ctx.check(deploy("minecraft:exposed_copper", "minecraft:diamond_axe") == "minecraft:copper_block", "exposed copper scraped");
    ctx.check(deploy("minecraft:copper_block", "minecraft:iron_axe") == "-", "fresh copper can't be scraped");
    ctx.check(deploy("levitate:copper_shingles", "minecraft:honeycomb_block") == "levitate:waxed_copper_shingles", "shingles waxed");
    ctx.check(deploy("levitate:copper_shingles", "minecraft:honeycomb") == "-", "a honeycomb item is no deploying recipe (Create)");
    auto held = item(ctx, "minecraft:honeycomb_block", 2);
    auto out  = deployer::deployOnItem(item(ctx, "minecraft:copper_grate"), held, false);
    ctx.check(out && items::countOf(held) == 2, "keep_held_item: the honeycomb block stays");
});

// The progress of a transitional item in percent, mirrored into the aux value for the client mod.
LV_REGISTER_SELFTEST("assembly.transitional_progress", [](Context& ctx) {
    auto slab = item(ctx, "minecraft:smooth_stone_slab");
    auto nug  = item(ctx, "minecraft:iron_nugget");
    auto s1   = sa::findDeployingStep(slab, nug);
    ctx.require(s1.has_value(), "the track recipe starts on a slab");
    auto a = sa::advance(*s1, slab);
    auto p = sa::progressOf(a);
    ctx.require(p.has_value(), "transitional item");
    ctx.check(a.getAuxValue() == static_cast<short>(std::lround(p->progress * 100)), "aux = progress in percent, got " + std::to_string(a.getAuxValue()));
    ctx.check(a.getAuxValue() > 0 && a.getAuxValue() < 100, "between 0 and 100");
    auto s2 = sa::findDeployingStep(a, nug);
    ctx.require(s2.has_value(), "the transitional item with its aux is still accepted");
    auto b = sa::advance(*s2, a);
    ctx.check(b.getAuxValue() > a.getAuxValue(), "the progress grows");
});

// Create's DeployerBlockEntity.addToGoggleTooltip.
LV_REGISTER_SELFTEST("assembly.deployer_goggles", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = off(ctx.testPos(5410), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kDeployerId, {{"levitate:facing", 1}}), "deployer");
    co_await ctx.ticks(1);
    deployer::setHeld(p, item(ctx, "minecraft:iron_nugget", 3));
    deployer::setMode(p, ca::DeployerMode::Use);
    auto& pl = ctx.player();
    auto  ok = items::make("levitate:goggles");
    ctx.require(ok.has_value(), "goggles");
    pl.setArmor(SharedTypes::Legacy::ArmorSlot::Head, *ok);
    auto lines = goggles::overlayFor(pl, p).lines;
    auto text  = joined(lines);
    ctx.check(lines.size() >= 3 && lines[0].find("Deployer Information") != std::string::npos, "header first: " + text);
    ctx.check(text.find("Mode: Use") != std::string::npos, "use mode line: " + text);
    ctx.check(text.find(" x3") != std::string::npos, "held item line: " + text);
    ctx.check(text.find("Contents:") == std::string::npos, "no generic contents line: " + text);
    deployer::setMode(p, ca::DeployerMode::Punch);
    ctx.check(joined(goggles::overlayFor(pl, p).lines).find("Mode: Attack") != std::string::npos, "attack mode line");
    pl.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
    deployer::setHeld(p, items::empty());
});

// Vanilla clients see the deployer's held item as a client-only item entity in front of it.
LV_REGISTER_SELFTEST("assembly.vanilla_items", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = off(ctx.testPos(5420), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kDeployerId, {{"levitate:facing", 5}}), "deployer facing east");
    co_await ctx.ticks(1);
    deployer::setHeld(p, item(ctx, "minecraft:cobblestone", 4));
    auto find = [&]() -> std::optional<common::vanillaitems::Shown> {
        for (auto const& s : common::vanillaitems::collect())
            if (s.owner == p) return s;
        return std::nullopt;
    };
    auto s = find();
    ctx.require(s.has_value(), "the held item is listed for vanilla clients");
    ctx.check(items::idOf(s->stack) == "minecraft:cobblestone" && items::countOf(s->stack) == 1, "one cobblestone shown");
    ctx.check(s->pos.x > p.x + 1.f && s->pos.x < p.x + 1.3f, "in front of the east face: " + std::to_string(s->pos.x - p.x));
    deployer::setHeld(p, items::empty());
    ctx.check(!find().has_value(), "nothing shown once the hand is empty");
});

} // namespace lv::feature::assembly
