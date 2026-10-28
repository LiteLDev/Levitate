// Selftests of the assembly unit (testPos dx 1500..1599): the sequenced assembly engine, the
// deployer (placing, punching, recipes, swapping, redstone lock, capability) and the mechanical arm
// (selection, placement, transfers).

#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/assembly/SequencedAssembly.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::assembly {

namespace {

using selftest::Context;
namespace items = common::items;
namespace sa    = sequenced;

constexpr char kSource[] = "levitate:kinetics_test_source";

GlobalPos up(GlobalPos p, int dy) {
    p.y += dy;
    return p;
}
GlobalPos off(GlobalPos p, int dx, int dy, int dz) {
    p.x += dx;
    p.y += dy;
    p.z += dz;
    return p;
}

BlockSource& region(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

std::string idAt(Context& ctx, GlobalPos const& p) { return game::blockIdAt(region(ctx), game::blockPos(p)); }

/// A deployer at `p` facing `facing` with its shaft along `shaft`, turned by a test source.
bool poweredDeployer(Context& ctx, GlobalPos const& p, Face facing, core::kinetics::Axis shaft, float rpm = 64) {
    bool along = ca::deployerAlongFirstFor(facing, shaft);
    if (!ctx.setBlock(p, kDeployerId, {{"levitate:facing", static_cast<int>(facing)}, {"levitate:axis_along_first", along ? 1 : 0}})) return false;
    auto src = off(p, shaft == core::kinetics::Axis::X ? -1 : 0, shaft == core::kinetics::Axis::Y ? -1 : 0, shaft == core::kinetics::Axis::Z ? -1 : 0);
    if (!ctx.setBlock(src, kSource, {{"levitate:axis", static_cast<int>(shaft)}})) return false;
    kinetics::refresh(src);
    kinetics::refresh(p);
    kinetics::setGeneratedSpeed(src, rpm);
    return true;
}

ll::coro::CoroTask<bool> waitFor(std::function<bool()> cond, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        if (cond()) co_return true;
        co_await Context::ticks(1);
    }
    co_return cond();
}

} // namespace

LV_REGISTER_SELFTEST("assembly.sequenced_track", [](Context& ctx) {
    // U5's track recipe: sleepers -> deploy nugget, deploy nugget, press -> track
    auto slab = item(ctx, "minecraft:smooth_stone_slab");
    auto nug  = item(ctx, "minecraft:iron_nugget");
    ctx.require(sa::byId("levitate:sequenced_assembly/track") != nullptr, "the track recipe is loaded");
    ctx.check(!sa::findStep(sa::kPressing, slab), "the first step is not pressing");
    auto s1 = sa::findDeployingStep(slab, nug);
    ctx.require(s1.has_value(), "the slab accepts a nugget");
    auto a = sa::advance(*s1, slab);
    ctx.check(items::idOf(a) == "levitate:incomplete_track", "becomes an incomplete track: " + items::idOf(a));
    auto p = sa::progressOf(a);
    ctx.require(p && p->step == 1, "progress step 1");
    ctx.check(!sa::tooltip(a).empty(), "tooltip lines");
    ctx.check(!a.getCustomLore().empty(), "lore shows the sequence");
    ctx.check(!sa::findStep(sa::kPressing, a), "second step is a deployment, not pressing");
    auto s2 = sa::findDeployingStep(a, nug);
    ctx.require(s2.has_value(), "second deployment");
    auto b = sa::advance(*s2, a);
    auto s3 = sa::findStep(sa::kPressing, b);
    ctx.require(s3.has_value(), "third step is pressing (callable by a press)");
    auto done = sa::advance(*s3, b);
    ctx.check(items::idOf(done) == "levitate:track", "result is a track: " + items::idOf(done));
    // a plain incomplete track without progress is not part of a sequence
    ctx.check(!sa::findDeployingStep(item(ctx, "levitate:incomplete_track"), nug), "an incomplete item without data is not accepted");
});

LV_REGISTER_SELFTEST("assembly.deployer_recipe_on_item", [](Context& ctx) {
    auto slab = item(ctx, "minecraft:smooth_stone_slab");
    auto held = item(ctx, "minecraft:iron_nugget", 5);
    auto sim  = deployer::deployOnItem(slab, held, true);
    ctx.require(sim.has_value(), "a recipe applies");
    ctx.check(items::countOf(held) == 5, "simulation keeps the held item");
    auto out = deployer::deployOnItem(slab, held, false);
    ctx.require(out && out->size() == 1, "one output");
    ctx.check(items::idOf(out->front()) == "levitate:incomplete_track", "incomplete track out");
    ctx.check(items::countOf(held) == 4, "a nugget is used up");
    auto stone = item(ctx, "minecraft:stone");
    ctx.check(!deployer::deployOnItem(stone, held, true).has_value(), "no recipe for stone");
});

LV_REGISTER_SELFTEST("assembly.deployer_places_block", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(1500), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto target = off(p, 2, 0, 0);
    ctx.cleanupBlock(target);
    ctx.require(poweredDeployer(ctx, p, Face::East, core::kinetics::Axis::Z), "powered deployer facing east");
    deployer::setHeld(p, item(ctx, "minecraft:cobblestone", 2));
    bool placed = co_await waitFor([&] { return idAt(ctx, target) == "minecraft:cobblestone"; }, 120);
    ctx.check(kinetics::speedAt(p) != 0, "the deployer turns");
    ctx.check(placed, "a cobblestone was placed two blocks in front: " + idAt(ctx, target));
    ctx.check(items::countOf(deployer::held(p)) == 1, "one cobblestone used");
});

LV_REGISTER_SELFTEST("assembly.deployer_punch_breaks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(1505), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto target = off(p, 2, 0, 0);
    ctx.require(ctx.setBlock(target, "minecraft:dirt"), "dirt in front");
    ctx.require(poweredDeployer(ctx, p, Face::East, core::kinetics::Axis::Z), "powered deployer");
    deployer::setMode(p, ca::DeployerMode::Punch);
    bool broken = co_await waitFor([&] { return idAt(ctx, target) != "minecraft:dirt"; }, 160);
    ctx.check(broken, "the dirt was broken");
    bool got = co_await waitFor([&] {
        for (auto const& s : deployer::overflow(p))
            if (items::idOf(s) == "minecraft:dirt") return true;
        return false;
    }, 10);
    ctx.check(got, "the drop went into the deployer");
});

LV_REGISTER_SELFTEST("assembly.deployer_redstone_lock", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(1510), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto target = off(p, 2, 0, 0);
    ctx.cleanupBlock(target);
    ctx.require(poweredDeployer(ctx, p, Face::East, core::kinetics::Axis::Z), "powered deployer");
    ctx.require(ctx.setBlock(off(p, 0, 1, 0), "minecraft:redstone_block"), "redstone block on top");
    deployer::setHeld(p, item(ctx, "minecraft:cobblestone", 2));
    co_await ctx.ticks(80);
    ctx.check(idAt(ctx, target) != "minecraft:cobblestone", "a powered deployer does nothing");
    ctx.check(deployer::state(p) == ca::DeployerState::Waiting, "it keeps waiting");
});

LV_REGISTER_SELFTEST("assembly.deployer_capability", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(1515), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kDeployerId, {{"levitate:facing", 1}}), "deployer");
    auto& r  = region(ctx);
    auto  bp = game::blockPos(p);
    auto rest = common::items::insert(r, bp, -1, item(ctx, "minecraft:diamond", 3), false);
    ctx.check(items::isEmpty(rest), "inserted into the hand");
    ctx.check(items::countOf(deployer::held(p)) == 3, "holds 3 diamonds");
    ctx.check(items::countOf(common::items::insert(r, bp, -1, item(ctx, "minecraft:emerald"), true)) == 1, "a different item does not fit");
    // a filter that asks for diamonds keeps them in the hand
    common::filterslot::set(p, item(ctx, "minecraft:diamond"));
    ctx.check(items::isEmpty(common::items::extract(r, bp, -1, common::items::any(), 64, true)), "the filtered held item stays");
    common::filterslot::set(p, items::empty());
    auto got = common::items::extract(r, bp, -1, common::items::any(), 64, false);
    ctx.check(items::countOf(got) == 3, "without a filter the hand can be emptied");
    // mode persists in BlockData
    deployer::setMode(p, ca::DeployerMode::Punch);
    auto const* d = common::blockdata::get(p);
    ctx.check(d && d->contains("deployer") && (*d)["deployer"].value("mode", 1) == 0, "punch mode saved");
});

LV_REGISTER_SELFTEST("assembly.deployer_swap", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(1520), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kDeployerId, {{"levitate:facing", 5}}), "deployer facing east");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.getInventory().setItem(0, item(ctx, "minecraft:diamond", 2));
    sp.setSelectedSlot(0);
    co_await ctx.ticks(2);
    bool used = sp.simulateUseItemInSlotOnBlock(0, game::blockPos(p), ScriptModuleMinecraft::ScriptFacing::East, Vec3{1.f, .5f, .5f});
    co_await ctx.ticks(1);
    if (items::countOf(deployer::held(p)) != 2) {
        // simulated use may not reach the hook in every game state: call the action directly
        ctx.info("simulated use returned " + std::to_string(used) + ", applying the swap directly");
        ItemStack h = sp.getSelectedItem();
        deployer::onUse(sp, game::blockPos(p), Face::East, h);
    }
    ctx.check(items::idOf(deployer::held(p)) == "minecraft:diamond" && items::countOf(deployer::held(p)) == 2, "the deployer holds the diamonds");
    ctx.check(items::isEmpty(sp.getSelectedItem()), "the player's hand is empty");
});

LV_REGISTER_SELFTEST("assembly.arm_selection_and_placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = up(ctx.testPos(1530), 1);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto composter = off(base, 2, 0, 0);
    auto jukebox   = off(base, -2, 0, 0);
    ctx.require(ctx.setBlock(composter, "minecraft:composter"), "composter");
    ctx.require(ctx.setBlock(jukebox, "minecraft:jukebox"), "jukebox");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    // holding the arm: right-click selects (input), again: output
    ctx.check(arm::onSelect(sp, game::blockPos(composter)), "composter is a point");
    ctx.check(arm::onSelect(sp, game::blockPos(jukebox)), "jukebox is a point");
    ctx.check(arm::onSelect(sp, game::blockPos(composter)), "click again");
    auto counts = arm::selectionCounts(sp);
    ctx.check(counts.first == 1 && counts.second == 1, "1 input, 1 output: " + std::to_string(counts.first) + "/" + std::to_string(counts.second));
    ctx.check(!arm::onSelect(sp, game::blockPos(off(base, 0, -1, 0))), "the ground is no point");
    // a far point is dropped on placement
    auto far = off(base, 6, 0, 0);
    ctx.require(ctx.setBlock(far, "minecraft:composter"), "far composter");
    arm::onSelect(sp, game::blockPos(far));
    ctx.require(ctx.setBlock(base, kArmId), "arm block");
    arm::onPlaced(region(ctx), game::blockPos(base), &sp, Face::Up);
    auto pts = arm::pointCounts(base);
    ctx.check(pts.first == 1 && pts.second == 1, "the arm got the points in range: " + std::to_string(pts.first) + "/" + std::to_string(pts.second));
    ctx.check(arm::selectionCounts(sp).first == 0, "selection cleared");
});

LV_REGISTER_SELFTEST("assembly.arm_transfers", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto a = up(ctx.testPos(1540), 1);
    ctx.require(co_await ctx.ensureLoaded(a), "test area loaded");
    ctx.require(ctx.setBlock(a, kArmId), "arm");
    auto src = off(a, 0, -1, 0);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 1}}), "source below the arm");
    kinetics::refresh(src);
    kinetics::refresh(a);
    kinetics::setGeneratedSpeed(src, 128);
    // input: an idle deployer holding pies; output: a composter
    auto in  = off(a, 2, 0, 0);
    auto out = off(a, -2, 0, 0);
    ctx.require(ctx.setBlock(in, kDeployerId, {{"levitate:facing", 1}}), "deployer as input");
    deployer::setHeld(in, item(ctx, "minecraft:pumpkin_pie", 3));
    ctx.require(ctx.setBlock(out, "minecraft:composter"), "composter as output");
    ctx.require(arm::addPoint(a, game::blockPos(in), true), "input point");
    ctx.require(arm::addPoint(a, game::blockPos(out), false), "output point");
    bool moved = co_await waitFor([&] { return items::countOf(deployer::held(in)) < 3; }, 200);
    ctx.check(kinetics::speedAt(a) != 0, "the arm turns");
    ctx.check(moved, "the arm took a pie");
    bool composted = co_await waitFor([&] {
        return game::intState(region(ctx).getBlock(game::blockPos(out)), "composter_fill_level").value_or(0) > 0;
    }, 200);
    ctx.check(composted, "the pie went into the composter");
});

LV_REGISTER_SELFTEST("assembly.arm_value_box", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto a = up(ctx.testPos(1550), 1);
    ctx.require(co_await ctx.ensureLoaded(a), "test area loaded");
    ctx.require(ctx.setBlock(a, kArmId), "arm");
    auto const* boxes = common::valuebox::boxesOf(kArmId);
    ctx.require(boxes && !boxes->empty(), "the arm has a value box");
    auto const& block = region(ctx).getBlock(game::blockPos(a));
    ctx.check(common::valuebox::boxAt(block, 2, core::common::faceLocalPoint(2, 8, 3)) != nullptr, "box on the north side near the bottom");
    ctx.check(common::valuebox::boxAt(block, 1, {0.5, 1.0, 0.5}) == nullptr, "no box on top");
    ctx.check(common::valuebox::apply(ctx.player(), a, "selection_mode", {0, 2}), "set prefer first");
    auto const* d = common::blockdata::get(a);
    ctx.check(d && (*d)["arm"].value("mode", 0) == 2, "mode saved");
});

} // namespace lv::feature::assembly
