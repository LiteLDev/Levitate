// Generators (U7) selftests: `/create selftest "generators.*"`. Test area dx 700..799.

#include <string>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/generators/Generators.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::generators {

namespace {

using selftest::Context;
namespace k = core::kinetics;

constexpr char kShaft[]  = "levitate:kinetics_test_shaft";
constexpr char kSource[] = "levitate:kinetics_test_source";

struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy;
        return p;
    }
};

std::string id(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? b->getTypeName() : std::string("<unloaded>");
}
int facing(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? stateOf(*b, kFacing, -1) : -1;
}
std::string num(float v) { return std::to_string(static_cast<int>(v)); }

bool shaftLine(Context& ctx, Area const& a, int from, int to, int dy = 0) {
    bool ok = true;
    for (int i = from; i <= to; ++i) ok &= ctx.setBlock(a.at(i, dy), kShaft, {{"levitate:axis", 0}});
    return ok;
}

ItemStack item(std::string const& name) {
    auto s = common::items::make(name, 1);
    return s ? *s : ItemStack{};
}

bool useOn(SimulatedPlayer& sp, ItemStack& stack, GlobalPos const& pos, ScriptModuleMinecraft::ScriptFacing face) {
    return sp.simulateUseItemOnBlock(stack, game::blockPos(pos), face, Vec3{0.5f, 0.5f, 0.5f});
}

void stand(SimulatedPlayer& sp, GlobalPos const& p) { sp.teleport(Vec3{p.x + .5f, static_cast<float>(p.y), p.z + .5f}, DimensionType(p.dim)); }

/// A rising bubble column at x = `dx`, y = `y0..y1`: water sources walled in by stone (so they
/// don't spread into the wheel's other cells), soul sand below; waits until the game turned the
/// water into a bubble column.
ll::coro::CoroTask<bool> bubbleColumn(Context& ctx, Area const& a, int dx, int y0, int y1) {
    for (int y = y0; y <= y1; ++y) {
        ctx.setBlock(a.at(dx - 1, y), "minecraft:stone");
        ctx.setBlock(a.at(dx, y, 1), "minecraft:stone");
        ctx.setBlock(a.at(dx, y, -1), "minecraft:stone");
        ctx.setBlock(a.at(dx, y), "minecraft:water");
    }
    ctx.setBlock(a.at(dx, y1 + 1), "minecraft:stone");
    ctx.setBlock(a.at(dx, y0 - 1), "minecraft:soul_sand");
    for (int i = 0; i < 40; ++i) {
        co_await ctx.ticks(2);
        bool all = true;
        for (int y = y0; y <= y1; ++y) all &= id(a.at(dx, y)) == "minecraft:bubble_column";
        if (all) co_return true;
    }
    ctx.info("column blocks: " + id(a.at(dx, y0)) + " .. " + id(a.at(dx, y1)));
    co_return false;
}

} // namespace

LV_REGISTER_SELFTEST("generators.registry", [](Context& ctx) {
    std::vector<std::string> ids{kHandCrank, kCopperValve, kWaterWheel, kLargeWheel, kMotor};
    for (int c = 0; c < 16; ++c) ids.push_back(valveId(c));
    for (auto const& i : ids) {
        ctx.check(kinetics::specOf(i) != nullptr, i + " is a kinetic block");
        ctx.check(game::blockWith(i) != nullptr, i + " is in the behavior pack");
    }
    ctx.check(game::blockWith(kStructure) != nullptr, "the structure block is in the behavior pack");
    ctx.check(kinetics::specOf(kStructure) == nullptr, "structure blocks are not kinetic");
    ctx.check(kinetics::stressCapacityOf(kHandCrank) == 8, "hand crank capacity 8");
    ctx.check(kinetics::stressCapacityOf(valveId(14)) == 8, "red valve handle capacity 8");
    ctx.check(kinetics::stressCapacityOf(kWaterWheel) == 32, "water wheel capacity 32");
    ctx.check(kinetics::stressCapacityOf(kLargeWheel) == 128, "large water wheel capacity 128");
    ctx.check(kinetics::stressCapacityOf(kMotor) == 16384, "creative motor capacity 16384");
    ctx.check(valveColour(valveId(3)) == 3 && valveColour(kCopperValve) == -1 && valveColour(kHandCrank) == -2, "valve colours");
    ctx.check(kindOf(std::string_view(kStructure)) == Kind::Structure && kindOf(std::string_view("minecraft:stone")) == Kind::None, "kinds");
});

LV_REGISTER_SELFTEST("generators.crank", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 700};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    // crank facing west: its shaft points east into the shaft line
    ctx.require(ctx.setBlock(a.at(0), kHandCrank, {{kFacing, static_cast<int>(Face::West)}}), "place the crank");
    ctx.require(shaftLine(ctx, a, 1, 3), "place the shafts");
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(a.at(2)) == 0, "the crank rests");
    ctx.require(useCrank(ctx.player(), a.at(0), false, true), "turn the crank");
    ctx.check(crankInUse(a.at(0)) == cg::kCrankUseTicks, "in use for 10 ticks");
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(a.at(0)) == -32, "a west-facing crank turns at -32 RPM, got " + num(kinetics::speedAt(a.at(0))));
    ctx.check(kinetics::speedAt(a.at(3)) == -32, "the shafts follow, got " + num(kinetics::speedAt(a.at(3))));
    auto net = kinetics::networkAt(a.at(1));
    ctx.check(net && net->capacity == 8 * 32, "capacity 8 SU/RPM at 32 RPM");
    // sneaking turns it the other way at once
    useCrank(ctx.player(), a.at(0), true, true);
    co_await ctx.ticks(1);
    ctx.check(kinetics::speedAt(a.at(3)) == 32, "backwards: +32 RPM, got " + num(kinetics::speedAt(a.at(3))));
    co_await ctx.ticks(cg::kCrankUseTicks + 2);
    ctx.check(kinetics::speedAt(a.at(3)) == 0, "released: it stops after 10 ticks, got " + num(kinetics::speedAt(a.at(3))));
    ctx.check(crankInUse(a.at(0)) == 0, "no longer in use");
});

LV_REGISTER_SELFTEST("generators.crank_click", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 708};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    stand(sp, a.at(0, 0, 2));
    ctx.require(ctx.setBlock(a.at(0), kHandCrank, {{kFacing, static_cast<int>(Face::Up)}}), "place the crank");
    co_await ctx.ticks(2);
    auto stick = item("minecraft:stick");
    useOn(sp, stick, a.at(0), ScriptModuleMinecraft::ScriptFacing::Up);
    co_await ctx.ticks(1);
    ctx.check(crankInUse(a.at(0)) > 0, "a right-click with a stick turns the crank");
    ctx.check(kinetics::speedAt(a.at(0)) == 32, "an up-facing crank turns at +32 RPM, got " + num(kinetics::speedAt(a.at(0))));
    ctx.check(id(a.at(0, 1)) != "minecraft:stick", "nothing was placed");
    co_await ctx.ticks(cg::kCrankUseTicks + 2);
});

LV_REGISTER_SELFTEST("generators.placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 714};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    stand(sp, a.at(0, 0, 3));
    co_await ctx.ticks(2);
    // a crank placed on top of a vertical shaft points its shaft down into it (facing up)
    ctx.require(ctx.setBlock(a.at(0), kShaft, {{"levitate:axis", 1}}), "vertical shaft");
    ctx.cleanupBlock(a.at(0, 1));
    auto crank = item(kHandCrank);
    useOn(sp, crank, a.at(0), ScriptModuleMinecraft::ScriptFacing::Up);
    co_await ctx.ticks(2);
    ctx.check(id(a.at(0, 1)) == kHandCrank, "the crank was placed, got " + id(a.at(0, 1)));
    ctx.check(facing(a.at(0, 1)) == static_cast<int>(Face::Up), "facing up towards the shaft, got " + std::to_string(facing(a.at(0, 1))));
    // a valve handle on the east side of a stone block faces east (the clicked face)
    ctx.require(ctx.setBlock(a.at(3), "minecraft:stone"), "stone");
    ctx.cleanupBlock(a.at(4));
    stand(sp, a.at(3, 0, 2));
    co_await ctx.ticks(10);
    auto valve = item(kCopperValve);
    useOn(sp, valve, a.at(3), ScriptModuleMinecraft::ScriptFacing::East);
    co_await ctx.ticks(2);
    ctx.check(id(a.at(4)) == kCopperValve, "the valve handle was placed, got " + id(a.at(4)));
    ctx.check(facing(a.at(4)) == static_cast<int>(Face::East), "facing the clicked face, got " + std::to_string(facing(a.at(4))));
    // a creative motor next to a horizontal shaft turns its shaft towards it
    ctx.require(ctx.setBlock(a.at(7, 1), kShaft, {{"levitate:axis", 0}}), "horizontal shaft");
    ctx.require(ctx.setBlock(a.at(6), "minecraft:stone"), "stone below");
    ctx.cleanupBlock(a.at(6, 1));
    stand(sp, a.at(6, 1, 2));
    co_await ctx.ticks(10);
    auto motor = item(kMotor);
    useOn(sp, motor, a.at(6), ScriptModuleMinecraft::ScriptFacing::Up);
    co_await ctx.ticks(2);
    ctx.check(id(a.at(6, 1)) == kMotor, "the motor was placed, got " + id(a.at(6, 1)));
    ctx.check(facing(a.at(6, 1)) == static_cast<int>(Face::East), "the motor faces the shaft, got " + std::to_string(facing(a.at(6, 1))));
    ctx.check(kinetics::speedAt(a.at(7, 1)) == 16, "the motor drives the shaft at 16 RPM, got " + num(kinetics::speedAt(a.at(7, 1))));
});

LV_REGISTER_SELFTEST("generators.valve", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 724};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(ctx.setBlock(a.at(0), kCopperValve, {{kFacing, static_cast<int>(Face::West)}}), "place the valve");
    ctx.require(shaftLine(ctx, a, 1, 3), "place the shafts");
    co_await ctx.ticks(2);
    ctx.require(useValve(ctx.player(), a.at(0), "", false), "use the valve");
    // 45 degrees: ceil(45 / 9.6) + 2 = 7 ticks at 32 RPM
    ctx.check(crankInUse(a.at(0)) == 7, "45 degrees take 7 ticks, got " + std::to_string(crankInUse(a.at(0))));
    co_await ctx.ticks(1);
    ctx.check(kinetics::speedAt(a.at(3)) == -32, "turning at -32 RPM, got " + num(kinetics::speedAt(a.at(3))));
    int left = crankInUse(a.at(0));
    useValve(ctx.player(), a.at(0), "", true);
    ctx.check(left > 0 && crankInUse(a.at(0)) == left, "a second click while turning does nothing");
    co_await ctx.ticks(8);
    ctx.check(kinetics::speedAt(a.at(3)) == 0, "stopped after its angle, got " + num(kinetics::speedAt(a.at(3))));
    // a clockwise angle of 90 degrees: the other way, 12 ticks
    setValveAngle(a.at(0), -90);
    co_await ctx.ticks(cg::kValveCooldown + 1);
    useValve(ctx.player(), a.at(0), "", false);
    ctx.check(crankInUse(a.at(0)) == 12, "90 degrees take 12 ticks, got " + std::to_string(crankInUse(a.at(0))));
    co_await ctx.ticks(1);
    ctx.check(kinetics::speedAt(a.at(3)) == 32, "clockwise: +32 RPM, got " + num(kinetics::speedAt(a.at(3))));
    co_await ctx.ticks(14);
    // a turning network: the valve does not start
    ctx.require(ctx.setBlock(a.at(4), kSource, {{"levitate:axis", 0}}), "a turning source");
    co_await ctx.ticks(cg::kValveCooldown + 2);
    useValve(ctx.player(), a.at(0), "", false);
    ctx.check(crankInUse(a.at(0)) == 0, "no turn while the network already turns");
    ctx.cleanupBlock(a.at(4));
    // dyeing
    ctx.require(useValve(ctx.player(), a.at(0), "minecraft:red_dye", false), "dye it red");
    co_await ctx.ticks(1);
    ctx.check(id(a.at(0)) == valveId(14), "red valve handle, got " + id(a.at(0)));
    ctx.check(facing(a.at(0)) == static_cast<int>(Face::West), "it keeps its facing");
    ctx.check(kinetics::specOf(id(a.at(0))) != nullptr, "the red handle is kinetic");
});

LV_REGISTER_SELFTEST("generators.value_boxes", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 732};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(ctx.setBlock(a.at(0), kCopperValve, {{kFacing, static_cast<int>(Face::Up)}}), "valve");
    ctx.require(ctx.setBlock(a.at(2), kMotor, {{kFacing, static_cast<int>(Face::East)}}), "motor");
    ctx.require(shaftLine(ctx, a, 3, 4), "shafts");
    co_await ctx.ticks(2);
    auto const* valve = blockAt(a.at(0));
    ctx.require(valve != nullptr, "valve loaded");
    ctx.check(common::valuebox::boxAt(*valve, 1, {0.5, 0.25, 0.5}) != nullptr, "the valve's box is on its front");
    ctx.check(common::valuebox::boxAt(*valve, 2, {0.5, 0.25, 0.0}) == nullptr, "no box on its side");
    detail::suppressValveBox = true;
    ctx.check(common::valuebox::boxAt(*valve, 1, {0.5, 0.25, 0.5}) == nullptr, "a plain click turns the valve instead");
    detail::suppressValveBox = false;
    ctx.check(valveAngle(a.at(0)) == 45, "default angle 45");
    ctx.check(common::valuebox::apply(ctx.player(), a.at(0), "angle", {0, 135}), "set the angle");
    ctx.check(valveAngle(a.at(0)) == -135, "clockwise 135 degrees, got " + std::to_string(valveAngle(a.at(0))));
    auto const* motor = blockAt(a.at(2));
    ctx.require(motor != nullptr, "motor loaded");
    ctx.check(common::valuebox::boxAt(*motor, 1, {0.5, 1.0, 0.5}) != nullptr, "the motor's box on top");
    ctx.check(common::valuebox::boxAt(*motor, 5, {1.0, 0.5, 0.5}) == nullptr, "no box at the shaft");
    ctx.check(motorSpeed(a.at(2)) == 16, "default 16 RPM");
    ctx.check(kinetics::speedAt(a.at(4)) == 16, "the motor turns the shafts at 16 RPM, got " + num(kinetics::speedAt(a.at(4))));
    ctx.check(common::valuebox::apply(ctx.player(), a.at(2), "speed", {0, 200}), "set the speed");
    co_await ctx.ticks(1);
    ctx.check(motorSpeed(a.at(2)) == -200, "-200 RPM stored, got " + std::to_string(motorSpeed(a.at(2))));
    ctx.check(kinetics::speedAt(a.at(4)) == -200, "the shafts follow, got " + num(kinetics::speedAt(a.at(4))));
    ctx.check(common::valuebox::apply(ctx.player(), a.at(2), "speed", {1, 999}), "set beyond the board");
    co_await ctx.ticks(1);
    ctx.check(motorSpeed(a.at(2)) == 256, "clamped to 256, got " + std::to_string(motorSpeed(a.at(2))));
});

LV_REGISTER_SELFTEST("generators.motor_facing", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 740};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(ctx.setBlock(a.at(3), kMotor, {{kFacing, static_cast<int>(Face::West)}}), "motor facing west");
    ctx.require(shaftLine(ctx, a, 0, 2), "shafts west of it");
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(a.at(0)) == -16, "a west-facing motor turns at -16 RPM, got " + num(kinetics::speedAt(a.at(0))));
    // turning the motor around (e.g. a wrench) re-applies its speed
    ctx.require(setStates(a.at(3), {{kFacing, static_cast<int>(Face::East)}}), "face east");
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(a.at(0)) == 0, "facing away it no longer drives the shafts");
    ctx.check(kinetics::speedAt(a.at(3)) == 16, "facing east: +16 RPM, got " + num(kinetics::speedAt(a.at(3))));
});

LV_REGISTER_SELFTEST("generators.water_wheel_bubbles", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 746};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto wheel = a.at(0, 3);
    ctx.require(ctx.setBlock(wheel, kWaterWheel, {{kFacing, static_cast<int>(Face::South)}}), "wheel along z");
    co_await ctx.ticks(2);
    ctx.check(tracksWheel(wheel), "the wheel is tracked");
    ctx.check(kinetics::speedAt(wheel) == 0, "no flow: no speed");
    // a rising bubble column on its west side turns it clockwise seen from the south
    ctx.require(co_await bubbleColumn(ctx, a, -1, 3, 3), "a bubble column forms");
    auto* region = regionOf(wheel.dim);
    ctx.require(region != nullptr, "region");
    auto f = flowAt(*region, a.at(-1, 3));
    ctx.check(f.y > 0, "the bubble column flows up");
    evaluateWheel(wheel);
    ctx.check(wheelFlowScore(wheel) == -1, "flow score -1, got " + std::to_string(wheelFlowScore(wheel)));
    ctx.check(kinetics::speedAt(wheel) == -8, "-8 RPM, got " + num(kinetics::speedAt(wheel)));
    ctx.require(ctx.setBlock(a.at(-1, 2), "minecraft:stone"), "replace the soul sand");
    ctx.require(ctx.setBlock(a.at(-1, 3), "minecraft:stone"), "remove the column");
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(wheel) == 0, "the wheel stops when the flow is gone, got " + num(kinetics::speedAt(wheel)));
});

LV_REGISTER_SELFTEST("generators.water_wheel_flow", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 760};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto wheel = a.at(0, 3);
    ctx.require(ctx.setBlock(wheel, kWaterWheel, {{kFacing, static_cast<int>(Face::South)}}), "wheel along z");
    co_await ctx.ticks(2);
    // water running east over the wheel: a source west of its top, thinning out to the east
    // (the liquid's depths give the flow, read the same tick)
    for (int dx = -1; dx <= 1; ++dx) {
        ctx.setBlock(a.at(dx, 3), dx == 0 ? kWaterWheel : "minecraft:stone", dx == 0 ? std::vector<std::pair<std::string, int>>{{kFacing, 3}} : std::vector<std::pair<std::string, int>>{});
        ctx.setBlock(a.at(dx, 4, 1), "minecraft:stone");
        ctx.setBlock(a.at(dx, 4, -1), "minecraft:stone");
    }
    ctx.require(ctx.setBlock(a.at(-1, 4), "minecraft:flowing_water", {{"liquid_depth", 0}}), "a source");
    ctx.require(ctx.setBlock(a.at(0, 4), "minecraft:flowing_water", {{"liquid_depth", 1}}), "flowing water over the wheel");
    ctx.require(ctx.setBlock(a.at(1, 4), "minecraft:flowing_water", {{"liquid_depth", 2}}), "flowing water east of it");
    auto* region = regionOf(wheel.dim);
    ctx.require(region != nullptr, "region");
    auto f = flowAt(*region, a.at(0, 4));
    ctx.info("flow over the wheel " + std::to_string(f.x) + "," + std::to_string(f.y) + "," + std::to_string(f.z));
    ctx.check(f.x > 0, "the water over the wheel flows east");
    evaluateWheel(wheel);
    ctx.check(wheelFlowScore(wheel) == -1, "flow score -1, got " + std::to_string(wheelFlowScore(wheel)));
    ctx.check(kinetics::speedAt(wheel) == -8, "water running east over the top: -8 RPM, got " + num(kinetics::speedAt(wheel)));
    for (int dx = -1; dx <= 1; ++dx) ctx.setBlock(a.at(dx, 4), "minecraft:air");
});

LV_REGISTER_SELFTEST("generators.large_wheel", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 780};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto wheel = a.at(0, 4);
    auto cells = cg::structureCells(Axis::Z);
    for (auto const& c : cells) ctx.cleanupBlock(k::shifted(wheel, c.offset));
    ctx.require(ctx.setBlock(wheel, kLargeWheel, {{kAxisState, 2}}), "large wheel along z");
    co_await ctx.ticks(2);
    int built = 0;
    for (auto const& c : cells) {
        auto const* b = blockAt(k::shifted(wheel, c.offset));
        if (b && kindOf(*b) == Kind::Structure && stateOf(*b, kSegment) == c.segment && axisStateOf(*b) == Axis::Z) ++built;
    }
    ctx.check(built == 8, "8 structure blocks around it, got " + std::to_string(built));
    // bubble columns along its west edge (3 of the 12 checked cells)
    ctx.require(co_await bubbleColumn(ctx, a, -2, 3, 5), "a bubble column forms");
    evaluateWheel(wheel);
    ctx.check(wheelFlowScore(wheel) == -3, "flow score -3, got " + std::to_string(wheelFlowScore(wheel)));
    ctx.check(kinetics::speedAt(wheel) == -4, "a large wheel turns at -4 RPM, got " + num(kinetics::speedAt(wheel)));
    // planks on a structure block change the whole wheel's wood
    ctx.check(applyMaterial(k::shifted(wheel, cells[0].offset), "minecraft:birch_planks"), "birch planks");
    ctx.check(!applyMaterial(wheel, "minecraft:birch_planks"), "the same wood again does nothing");
    ctx.check(!applyMaterial(wheel, "minecraft:stone"), "stone is no wood");
    co_await ctx.ticks(2);
    auto const* m = blockAt(wheel);
    ctx.check(m && stateOf(*m, kWood) == 2, "the wheel is birch");
    auto const* s = blockAt(k::shifted(wheel, cells[3].offset));
    ctx.check(s && stateOf(*s, kWood) == 2, "its structure is birch too");
    // breaking a structure block (creative: no drop) breaks the wheel and the rest of its structure
    auto broken = k::shifted(wheel, cells[5].offset);
    noteCreativeBreak(broken);
    auto* region = regionOf(wheel.dim);
    ctx.require(region && game::clearBlock(*region, game::blockPos(broken)), "break a structure block");
    co_await ctx.ticks(3);
    ctx.check(id(wheel) == "minecraft:air", "the wheel broke, got " + id(wheel));
    int left = 0;
    for (auto const& c : cells) {
        auto const* b = blockAt(k::shifted(wheel, c.offset));
        if (b && kindOf(*b) == Kind::Structure) ++left;
    }
    ctx.check(left == 0, "its structure is gone, " + std::to_string(left) + " left");
    ctx.check(!tracksWheel(wheel), "the wheel is no longer tracked");
});

LV_REGISTER_SELFTEST("generators.large_wheel_place", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 790};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = 0; dy <= 4; ++dy)
            for (int dz = -2; dz <= 2; ++dz) ctx.cleanupBlock(a.at(dx, dy, dz));
    ctx.require(ctx.setBlock(a.at(0, 1), "minecraft:stone"), "a block to click on");
    stand(sp, a.at(0, 0, 4));
    sp.simulateLookAt(Vec3{a.at(0).x + .5f, a.at(0).y + 1.5f, a.at(0).z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(3);
    // blocked: a block right next to where the wheel would go in every plane
    ctx.require(ctx.setBlock(a.at(1, 2, 0), "minecraft:stone"), "an obstacle");
    ctx.require(ctx.setBlock(a.at(0, 2, 1), "minecraft:stone"), "an obstacle");
    auto stack = item(kLargeWheel);
    net::setTestCompanion(sp, true);
    net::takeTestMessages(sp);
    useOn(sp, stack, a.at(0, 1), ScriptModuleMinecraft::ScriptFacing::Up);
    co_await ctx.ticks(2);
    ctx.check(id(a.at(0, 2)) != kLargeWheel && id(a.at(0, 3)) != kLargeWheel, "no room: nothing placed");
    bool outlined = false;
    for (auto const& m : net::takeTestMessages(sp))
        if (m.channel == "generators.room") {
            double extent = 0; // 3 x 3 x 1 blocks
            for (int i = 0; i < 3; ++i) extent += m.payload["hi"][i].get<double>() - m.payload["lo"][i].get<double>();
            outlined = extent == 7.0;
        }
    ctx.check(outlined, "the client mod is sent the missing room's box");
    net::setTestCompanion(sp, false);
    ctx.require(ctx.setBlock(a.at(1, 2, 0), "minecraft:air"), "clear");
    ctx.require(ctx.setBlock(a.at(0, 2, 1), "minecraft:air"), "clear");
    co_await ctx.ticks(10);
    useOn(sp, stack, a.at(0, 1), ScriptModuleMinecraft::ScriptFacing::Up);
    co_await ctx.ticks(3);
    GlobalPos at = id(a.at(0, 2)) == kLargeWheel ? a.at(0, 2) : a.at(0, 3);
    ctx.check(id(at) == kLargeWheel, "the large wheel was placed");
    if (auto const* b = blockAt(at)) {
        int structures = 0;
        for (auto const& c : cg::structureCells(axisStateOf(*b))) {
            auto const* s = blockAt(k::shifted(at, c.offset));
            if (s && kindOf(*s) == Kind::Structure) ++structures;
        }
        ctx.check(structures == 8, "with its 8 structure blocks, got " + std::to_string(structures));
        ctx.info(std::string("placed with axis ") + k::axisName(axisStateOf(*b)));
    }
});

} // namespace lv::feature::generators