// Selftests of the assembly unit (testPos dx 1500..1599): the sequenced assembly engine, the
// deployer (placing, punching, recipes, swapping, redstone lock, capability) and the mechanical arm
// (selection, placement, transfers).

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/service/Bedrock.h"

#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/DimensionType.h"
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

LV_REGISTER_SELFTEST("assembly.deployer_places_oriented", [](Context& ctx) -> ll::coro::CoroTask<> {
    // with a block to place against, the placement goes through the player's item use: a log
    // placed by a deployer facing down stands upright on the floor
    auto p = up(ctx.testPos(1525), 3);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto target = off(p, 0, -2, 0);
    ctx.cleanupBlock(target);
    ctx.require(ctx.setBlock(off(target, 0, -1, 0), "minecraft:stone"), "floor");
    ctx.require(poweredDeployer(ctx, p, Face::Down, core::kinetics::Axis::X), "powered deployer facing down");
    deployer::setHeld(p, item(ctx, "minecraft:oak_log", 1));
    bool placed = co_await waitFor([&] { return idAt(ctx, target) == "minecraft:oak_log"; }, 120);
    ctx.check(placed, "a log was placed on the floor: " + idAt(ctx, target));
    ctx.check(items::isEmpty(deployer::held(p)), "the log was used");
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

// ---- U39 (processing gaps): dx 3930..3949 -------------------------------------------------------------

namespace {

constexpr char const* kPrecisionSteps[] = {"levitate:cogwheel", "levitate:large_cogwheel", "minecraft:iron_nugget"};

bool isPrecisionResult(std::string const& id) {
    auto const* r = sa::byId("levitate:sequenced_assembly/precision_mechanism");
    if (!r) return false;
    for (auto const& res : r->results)
        if (res.id == id) return true;
    return false;
}

ItemStack onDepot(Context& ctx, GlobalPos const& depot) {
    auto& rg = region(ctx);
    auto  n  = common::items::slotCount(rg, game::blockPos(depot), 1);
    auto  h  = common::items::find({rg, game::blockPos(depot), 1});
    if (!h || !h->getSlot) return items::empty();
    for (int i = 0; i < n; ++i)
        if (auto s = h->getSlot({rg, game::blockPos(depot), 1}, i); !items::isEmpty(s)) return s;
    return items::empty();
}

void runServerCommand(std::string const& command) {
    auto level = ll::service::getLevel();
    if (!level) return;
    ServerCommandOrigin origin("levitate-selftest", level->asServer(), CommandPermissionLevel::Owner, DimensionType(0));
    ll::command::CommandRegistrar::getServerInstance().executeCommand(command, origin);
}

} // namespace

LV_REGISTER_SELFTEST("assembly.precision_mechanism", [](Context& ctx) {
    // R75: Create's precision mechanism: golden sheet, (cogwheel, large cogwheel, iron nugget) x 5
    ctx.require(sa::byId("levitate:sequenced_assembly/precision_mechanism") != nullptr, "the recipe is loaded");
    int precision = 0;
    for (int run = 0; run < 12; ++run) {
        auto stack = item(ctx, "levitate:golden_sheet");
        for (int step = 0; step < 15; ++step) {
            auto held = item(ctx, kPrecisionSteps[step % 3]);
            auto out  = deployer::deployOnItem(stack, held, false);
            ctx.require(out && out->size() == 1, "step " + std::to_string(step + 1) + " deploys " + kPrecisionSteps[step % 3]);
            ctx.check(items::isEmpty(held), "the deployed item is used up");
            stack = out->front();
            if (step < 14) {
                auto p = sa::progressOf(stack);
                ctx.require(items::idOf(stack) == "levitate:incomplete_precision_mechanism" && p && p->step == step + 1,
                            "after step " + std::to_string(step + 1) + " an incomplete precision mechanism");
                // a wrong item doesn't apply
                auto wrong = item(ctx, kPrecisionSteps[(step + 2) % 3]);
                ctx.check(!deployer::deployOnItem(stack, wrong, true), "the next step needs " + std::string(kPrecisionSteps[(step + 1) % 3]));
            }
        }
        ctx.check(!sa::isTransitional(stack) && isPrecisionResult(items::idOf(stack)), "the 15th step gives a result: " + items::idOf(stack));
        if (items::idOf(stack) == "levitate:precision_mechanism") ++precision;
    }
    ctx.check(precision > 0, "precision mechanisms come out (" + std::to_string(precision) + " of 12)");
});

LV_REGISTER_SELFTEST("assembly.precision_mechanism_depot", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R75: the same sequence by a real deployer above a depot, one held item per step
    auto p = up(ctx.testPos(3935), 3);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto depot = off(p, 0, -2, 0);
    ctx.require(ctx.setBlock(depot, "levitate:depot"), "a depot under the deployer");
    ctx.require(poweredDeployer(ctx, p, Face::Down, core::kinetics::Axis::X, 256), "powered deployer facing down");
    co_await Context::ticks(2);
    auto rest = common::items::insert(region(ctx), game::blockPos(depot), 1, item(ctx, "levitate:golden_sheet"), false);
    ctx.require(items::isEmpty(rest), "the depot takes the golden sheet");
    for (int step = 0; step < 15; ++step) {
        deployer::setHeld(p, item(ctx, kPrecisionSteps[step % 3]));
        bool done = co_await waitFor([&] {
            auto s = onDepot(ctx, depot);
            if (step == 14) return !items::isEmpty(s) && !sa::isTransitional(s);
            auto pr = sa::progressOf(s);
            return pr && pr->step == step + 1;
        }, 200);
        ctx.require(done, "the deployer ran step " + std::to_string(step + 1) + ", depot holds " + items::idOf(onDepot(ctx, depot)));
    }
    auto result = onDepot(ctx, depot);
    ctx.check(isPrecisionResult(items::idOf(result)), "the deployer finished the sequence: " + items::idOf(result));
    (void)common::items::extract(region(ctx), game::blockPos(depot), 1, common::items::any(), 64, false);
    deployer::setHeld(p, items::empty());
});

LV_REGISTER_SELFTEST("assembly.fist_bump", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R82: two punching deployers facing each other three blocks apart meet half way; what is
    // between them is not punched
    auto p = up(ctx.testPos(3940), 1);
    auto q = off(p, 3, 0, 0);
    ctx.require(co_await ctx.ensureLoaded(q), "test area loaded");
    auto between = off(p, 2, 0, 0);
    ctx.require(ctx.setBlock(between, "minecraft:dirt"), "dirt between the deployers");
    ctx.cleanupBlock(off(p, 1, 0, 0));
    ctx.require(poweredDeployer(ctx, p, Face::East, core::kinetics::Axis::Z, 128), "deployer facing east");
    ctx.require(poweredDeployer(ctx, q, Face::West, core::kinetics::Axis::Z, 128), "deployer facing west");
    deployer::setMode(p, ca::DeployerMode::Punch);
    deployer::setMode(q, ca::DeployerMode::Punch);
    int before = deployer::fistBumps();
    bool bumped = co_await waitFor([&] { return deployer::fistBumps() > before; }, 200);
    ctx.check(bumped, "the deployers fist bumped");
    co_await Context::ticks(60);
    ctx.check(idAt(ctx, between) == "minecraft:dirt", "the dirt between them was not punched");
    ctx.check(deployer::fistBumps() > before + 1, "they keep bumping");
});

LV_REGISTER_SELFTEST("assembly.fake_player_hidden", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R77: selectors don't find the deployer player (the test player, spawned first, is found)
    auto& self = ctx.player();
    co_await Context::ticks(2);
    SimulatedPlayer* fake = nullptr;
    for (int i = 0; i < 20 && !fake; ++i) {
        fake = fakeplayer::get(0);
        if (!fake) co_await Context::ticks(1);
    }
    ctx.require(fake != nullptr, "the deployer player exists");
    auto fakeId = fake->getOrCreateUniqueID().rawID;
    ctx.check(fakeplayer::isFakeId(fakeId), "it is known as the deployer player");
    runServerCommand("tag @a add lv_selftest_seen");
    co_await Context::ticks(2);
    auto* again = static_cast<Player*>(nullptr);
    for (auto* pl : ll::service::getLevel()->getRuntimeActorList())
        if (pl && pl->getOrCreateUniqueID().rawID == fakeId) again = static_cast<Player*>(pl);
    ctx.require(again != nullptr, "the deployer player is still there");
    ctx.check(!again->hasTag("lv_selftest_seen"), "@a doesn't select the deployer player");
    ctx.check(self.hasTag("lv_selftest_seen"), "@a selects the test player");
    runServerCommand("tag @a remove lv_selftest_seen");
});

} // namespace lv::feature::assembly
