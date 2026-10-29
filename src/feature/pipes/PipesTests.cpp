// Selftests of the pipes unit (U17, testPos dx 1700..1799), plus the test-only tank block
// `levitate:pipes_test_tank` (an 8-bucket fluid handler kept in BlockData) that stands in for the
// fluid tanks of U18 so pumps can be tested tank to tank.

#include <cmath>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/FluidRegistry.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pipes/OpenEnds.h"
#include "feature/pipes/Pipes.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::pipes {

namespace {

using selftest::Context;
namespace bd = common::blockdata;

constexpr char kTestTank[] = "levitate:pipes_test_tank";
constexpr int  kTankCap    = 8000;

// ---- the test tank -------------------------------------------------------------------------------

common::fluids::FluidStack tankContents(GlobalPos const& p) {
    auto const* d = bd::get(p);
    if (!d || !d->contains("pipes_test_tank")) return {};
    auto const& t = (*d)["pipes_test_tank"];
    return {t.value("fluid", std::string{}), t.value("amount", 0)};
}

void setTank(GlobalPos const& p, common::fluids::FluidStack const& s) {
    if (s.empty()) {
        if (bd::has(p)) bd::edit(p).erase("pipes_test_tank");
        return;
    }
    bd::edit(p)["pipes_test_tank"] = {{"fluid", s.fluid}, {"amount", s.amount}};
}

struct TestTankRegistrar {
    TestTankRegistrar() {
        common::fluids::Handler h;
        h.tanks = [](common::Access const& at) {
            return std::vector<common::fluids::Tank>{{tankContents(game::globalPos(at.region, at.pos)), kTankCap}};
        };
        h.fill = [](common::Access const& at, common::fluids::FluidStack const& s, bool simulate) {
            auto p   = game::globalPos(at.region, at.pos);
            auto cur = tankContents(p);
            if (s.empty() || (!cur.empty() && cur.fluid != s.fluid)) return 0;
            int n = std::min(s.amount, kTankCap - cur.amount);
            if (!simulate && n > 0) setTank(p, {s.fluid, cur.amount + n});
            return n;
        };
        h.drain = [](common::Access const& at, common::fluids::FluidStack const& req, bool simulate) -> common::fluids::FluidStack {
            auto p   = game::globalPos(at.region, at.pos);
            auto cur = tankContents(p);
            if (cur.empty() || (!req.fluid.empty() && req.fluid != cur.fluid)) return {};
            int n = std::min(req.amount, cur.amount);
            if (n <= 0) return {};
            if (!simulate) setTank(p, {cur.fluid, cur.amount - n});
            return {cur.fluid, n};
        };
        common::fluids::registerBlock(kTestTank, std::move(h));
    }
} const gTestTank;

// ---- helpers -------------------------------------------------------------------------------------

struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy + 1;
        return p;
    }
};

BlockSource& region() { return *regionOf(0); }

Block const& blockAt(GlobalPos const& p) { return region().getBlock(toBlockPos(p)); }

std::vector<std::pair<std::string, int>> faces(FaceMask m) {
    static constexpr char const* names[6]{"levitate:down", "levitate:up", "levitate:north", "levitate:south", "levitate:west", "levitate:east"};
    std::vector<std::pair<std::string, int>> out;
    for (int i = 0; i < 6; ++i) out.emplace_back(names[i], (m >> i) & 1);
    return out;
}

constexpr FaceMask kX = cp::axisMask(Axis::X);

bool tank(Context& ctx, GlobalPos const& p, common::fluids::FluidStack s) {
    bool ok = ctx.setBlock(p, kTestTank);
    setTank(p, s);
    return ok;
}

/// A pump at `p` facing `front` (east or west), driven through a cog on its south side by a test
/// generator at `rpm`. Returns the generator's position.
GlobalPos pumpAt(Context& ctx, GlobalPos const& p, Face front, float rpm) {
    ctx.setBlock(p, kPump, {{"levitate:facing", static_cast<int>(front)}});
    GlobalPos cog = p;
    cog.z += 1;
    GlobalPos src = cog;
    src.x -= 1;
    ctx.setBlock(cog, "levitate:kinetics_test_cog", {{"levitate:axis", 0}});
    ctx.setBlock(src, "levitate:kinetics_test_source", {{"levitate:axis", 0}});
    lv::kinetics::setGeneratedSpeed(src, rpm);
    return src;
}

ll::coro::CoroTask<bool> waitFor(std::function<bool()> cond, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        if (cond()) co_return true;
        co_await Context::ticks(1);
    }
    co_return cond();
}

} // namespace

// ---- registry / specs ------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("pipes.registry", [](Context& ctx) {
    namespace ft = common::fluidtypes;
    auto const* water = ft::find("minecraft:water");
    ctx.require(water && water->sourceBlock == "minecraft:water", "water is a registered fluid with a world block");
    ctx.check(ft::find("minecraft:lava") && ft::find("minecraft:lava")->hot, "lava is hot");
    ctx.check(ft::find("potion:swiftness") != nullptr, "potion fluids resolve");
    ctx.check(ft::potionName("potion:long_swiftness") == "long_swiftness", "potion name");
    ctx.check(ft::byWorldBlock("minecraft:flowing_water") == water, "flowing water is water");
    // U19's ids, by string (its potion form and its honey / chocolate source blocks)
    ctx.check(ft::isPotion("levitate:potion/splash/long_swiftness"), "U19 potion fluids are potions");
    ctx.check(ft::potionName("levitate:potion/regular/long_swiftness") == "long_swiftness", "U19 potion name");
    ctx.check(ft::byWorldBlock("levitate:honey") == ft::find("levitate:honey"), "honey's world block is U19's source");
    for (char const* id : {"minecraft:water", "minecraft:lava", "minecraft:milk", "levitate:tea", "potion:healing",
                           "levitate:potion/lingering/healing"})
        ctx.check(ft::effectFor(id) != nullptr, std::string("open pipe effect for ") + id);
    auto const* pump = lv::kinetics::specOf(kPump);
    ctx.require(pump != nullptr, "the pump is a kinetic block");
    lv::kinetics::BlockState s{kPump, {{"levitate:facing", 5}}};
    ctx.check(pump->axisOf(s) == Axis::X && pump->cogOf(s) == lv::kinetics::CogType::Small, "the pump turns as a small cog on its facing axis");
    ctx.check(lv::kinetics::stressImpactOf(kPump) == 4, "the pump's stress impact is 4 su/rpm");
    auto const* valve = lv::kinetics::specOf(kValve);
    ctx.require(valve != nullptr, "the valve is a kinetic block");
    lv::kinetics::BlockState v{kValve, {{"levitate:facing", 1}, {"levitate:along_first", 0}}};
    ctx.check(valve->shaftTowards(v, Face::North) && !valve->shaftTowards(v, Face::East), "valve shaft along z when its pipe runs along x");
});

// ---- connections -------------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("pipes.connect", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1700};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.setBlock(a.at(0), kFluidPipe);
    ctx.setBlock(a.at(1), kFluidPipe);
    co_await ctx.ticks(3);
    ctx.check(connections(blockAt(a.at(0))) == kX, "the first pipe turned along x");
    ctx.check(connections(blockAt(a.at(1))) == kX, "the second pipe runs along x");
    // a third pipe above the first makes it a corner / junction
    ctx.setBlock(a.at(0, 1), kFluidPipe);
    co_await ctx.ticks(3);
    auto m = connections(blockAt(a.at(0)));
    ctx.check(cp::has(m, Face::Up) && cp::has(m, Face::East), "the first pipe now connects up and east");
    ctx.check(cp::has(connections(blockAt(a.at(0, 1))), Face::Down), "the upper pipe connects down");
    // rims: towards connected pipes none, the open west end has one
    auto rimA = game::intState(blockAt(a.at(1)), "levitate:rim_a").value_or(-1);
    auto rimB = game::intState(blockAt(a.at(1)), "levitate:rim_b").value_or(-1);
    ctx.check(rimA == 0 && rimB == 4, "the second pipe has a rim on its open east end only");
    // a test tank connects
    tank(ctx, a.at(2, 0, 1), {});
    ctx.setBlock(a.at(2), kFluidPipe); // unoriented: connects to its neighbours like a placed pipe
    co_await ctx.ticks(3);
    ctx.check(cp::has(connections(blockAt(a.at(2))), Face::South), "pipes connect to fluid handlers");
    ctx.check(canConnectTo(region(), toBlockPos(a.at(1)), Face::East), "canConnectTo sees the next pipe");
});

LV_REGISTER_SELFTEST("pipes.placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1710};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto& player = ctx.player();
    auto* sp     = static_cast<SimulatedPlayer*>(&player);
    ctx.setBlock(a.at(0, -1), "minecraft:stone");
    ctx.setBlock(a.at(1, -1), "minecraft:stone");
    ctx.cleanupBlock(a.at(0));
    ctx.cleanupBlock(a.at(1));
    // stand three blocks south of the spot, looking at it
    auto stand = a.at(0, 0, 3);
    player.teleport(Vec3{stand.x + .5f, float(stand.y), stand.z + .5f}, player.getDimensionId());
    co_await ctx.ticks(2);
    auto item = common::items::make(kFluidPipe, 4);
    ctx.require(item.has_value(), "the fluid pipe item exists");
    ctx.require(ctx.give(player, *item, 0) == 0, "pipe in the hotbar");
    BlockPos target = toBlockPos(a.at(0, -1));
    sp->simulateLookAt(Vec3{target.x + .5f, target.y + 1.f, target.z + .5f}, sim::LookDuration::Instant);
    sp->simulateUseItemInSlotOnBlock(0, target, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(3);
    ctx.require(kindOf(blockAt(a.at(0))) == BlockKind::Pipe, "a player placed a fluid pipe");
    ctx.check(cp::count(connections(blockAt(a.at(0)))) == 2, "a lone placed pipe is straight");

    // a pump placed east of the pipe faces the pipe (Create's PumpBlock placement)
    auto pumpItem = common::items::make(kPump, 1);
    ctx.require(pumpItem.has_value(), "the pump item exists");
    ctx.require(ctx.give(player, *pumpItem, 0) == 0, "pump in the selected slot");
    co_await ctx.ticks(20); // the game refuses block placements in quick succession
    BlockPos floor = toBlockPos(a.at(1, -1));
    sp->simulateLookAt(Vec3{floor.x + .5f, floor.y + 1.f, floor.z + .5f}, sim::LookDuration::Instant);
    bool pumpUsed = sp->simulateUseItemInSlotOnBlock(0, floor, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(3);
    if (!pumpUsed) ctx.info("the simulated player could not use the pump item");
    ctx.require(kindOf(blockAt(a.at(1))) == BlockKind::Pump, "a player placed a pump");
    Face front    = pumpFront(blockAt(a.at(1)));
    Face expected = Face::West;    ctx.check(front == expected, "the pump faces the pipe beside it (" + std::string(core::kinetics::faceName(front)) + ")");
});

// ---- wrench, casing, brackets ---------------------------------------------------------------------

LV_REGISTER_SELFTEST("pipes.wrench", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1720};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto p = a.at(0);
    ctx.setBlock(p, kFluidPipe, faces(kX));
    co_await ctx.ticks(2);
    ctx.check(!wrench(nullptr, region(), toBlockPos(p), Face::East, 1, .5, .5), "wrenching a straight pipe's end does nothing");
    ctx.require(wrench(nullptr, region(), toBlockPos(p), Face::Up, .5, 1, .5), "wrench on the side of a straight pipe");
    ctx.require(kindOf(blockAt(p)) == BlockKind::Glass, "it became a glass pipe");
    ctx.check(game::intState(blockAt(p), "levitate:axis") == 0, "the glass pipe runs along x");
    co_await ctx.ticks(2);
    ctx.require(wrench(nullptr, region(), toBlockPos(p), Face::Up, .5, 1, .5), "wrench on the glass pipe");
    ctx.check(kindOf(blockAt(p)) == BlockKind::Pipe && connections(blockAt(p)) == kX, "back to a straight fluid pipe");
    // pump: the wrench reverses it
    auto q = a.at(2);
    ctx.setBlock(q, kPump, {{"levitate:facing", static_cast<int>(Face::East)}});
    ctx.require(wrench(nullptr, region(), toBlockPos(q), Face::Up, .5, 1, .5), "wrench on the pump");
    ctx.check(pumpFront(blockAt(q)) == Face::West, "the pump was reversed");
    // smart pipe: turns on the floor
    auto s = a.at(4);
    ctx.setBlock(s, kSmartPipe, {{"levitate:face", 0}, {"levitate:facing", 3}});
    ctx.require(wrench(nullptr, region(), toBlockPos(s), Face::Up, .5, 1, .5), "wrench on the smart pipe");
    ctx.check(cp::smartPipeAxis(smartState(blockAt(s))) == Axis::Z, "the smart pipe turned to z");
});

LV_REGISTER_SELFTEST("pipes.encased", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1730};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto p = a.at(0);
    ctx.setBlock(p, kFluidPipe, faces(kX));
    co_await ctx.ticks(2);
    ctx.require(encase(region(), toBlockPos(p)), "copper casing on a pipe");
    ctx.require(kindOf(blockAt(p)) == BlockKind::Encased, "the pipe is encased");
    ctx.check(connections(blockAt(p)) == kX, "it keeps its connections");
    // locked: a new neighbour doesn't change it
    ctx.setBlock(a.at(0, 1), kFluidPipe);
    co_await ctx.ticks(3);
    ctx.check(connections(blockAt(p)) == kX, "encased pipes ignore new neighbours");
    ctx.check(!cp::has(connections(blockAt(a.at(0, 1))), Face::Down), "the pipe above doesn't connect to a closed encased face");
    ctx.require(wrench(nullptr, region(), toBlockPos(p), Face::Up, .5, 1, .5), "wrench on the encased pipe");
    ctx.check(kindOf(blockAt(p)) == BlockKind::Pipe, "the casing came off");
    // a glass pipe encases along its axis
    auto g = a.at(3);
    ctx.setBlock(g, kGlassPipe, {{"levitate:axis", 2}});
    ctx.require(encase(region(), toBlockPos(g)), "copper casing on a glass pipe");
    ctx.check(connections(blockAt(g)) == cp::axisMask(Axis::Z), "encased along the glass pipe's axis");
});

LV_REGISTER_SELFTEST("pipes.bracket", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1740};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto p = a.at(0);
    ctx.setBlock(p, kFluidPipe, faces(kX));
    co_await ctx.ticks(2);
    ctx.require(attachBracket(region(), toBlockPos(p), kMetalBracket), "bracket attached");
    ctx.check(bracketOf(p) == kMetalBracket, "the bracket is remembered");
    ctx.check(game::intState(blockAt(p), "levitate:bracket") == 2, "the metal bracket shows");
    ctx.setBlock(a.at(0, 0, 1), kFluidPipe);
    co_await ctx.ticks(3);
    ctx.check(connections(blockAt(p)) == kX, "a bracketed pipe keeps its shape");
    ctx.check(!canConnectTo(region(), toBlockPos(a.at(0, 0, 1)), Face::North), "pipes don't connect sideways to a bracketed pipe");
    ctx.require(wrench(nullptr, region(), toBlockPos(p), Face::Up, .5, 1, .5), "wrench removes the bracket");
    ctx.check(bracketOf(p).empty() && kindOf(blockAt(p)) == BlockKind::Pipe, "bracket gone, still a fluid pipe");
    // corners can't hold a bracket
    ctx.setBlock(a.at(3), kFluidPipe, faces(cp::bit(Face::East) | cp::bit(Face::Up)));
    ctx.check(!attachBracket(region(), toBlockPos(a.at(3)), kWoodBracket), "no bracket on a corner");
});

// ---- flow ---------------------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("pipes.pump_tanks", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1750};
    ctx.require(co_await ctx.ensureLoaded(a.at(4)), "test area loaded");
    tank(ctx, a.at(0), {"minecraft:water", 8000});
    for (int x : {1, 2, 4, 5}) ctx.setBlock(a.at(x), kFluidPipe, faces(kX));
    pumpAt(ctx, a.at(3), Face::East, 64);
    tank(ctx, a.at(6), {});
    co_await ctx.ticks(2);
    ctx.require(std::abs(lv::kinetics::speedAt(a.at(3))) == 64, "the pump turns at 64 RPM");
    bool moved = co_await waitFor([&] { return tankContents(a.at(6)).amount > 0; }, 200);
    ctx.require(moved, "water reached the second tank");
    co_await ctx.ticks(5);
    int before = tankContents(a.at(6)).amount;
    co_await ctx.ticks(20);
    int rate = (tankContents(a.at(6)).amount - before) / 20;
    ctx.check(rate == 32, "64 RPM moves 32 mB per tick (got " + std::to_string(rate) + ")");
    ctx.check(tankContents(a.at(0)).amount + tankContents(a.at(6)).amount == 8000, "no fluid lost or made");
    ctx.check(fluidIn(a.at(5)).id == "minecraft:water", "the pipes carry water");
    auto const* n = model().node(a.at(5));
    ctx.check(n && n->at(Face::East) && n->at(Face::East)->pressureOut == 64, "the pushing side has 64 outward pressure");
    ctx.info("transfer " + std::to_string(rate) + " mB/t");
    // a glass pipe shows its fluid
    ctx.require(wrench(nullptr, region(), toBlockPos(a.at(5)), Face::Up, .5, 1, .5), "make a glass pipe");
    bool shows = co_await waitFor([&] { return game::intState(blockAt(a.at(5)), "levitate:fluid").value_or(0) == 1; }, 60);
    ctx.check(shows, "the glass pipe shows water");
    // stopping the pump stops the flow
    for (auto const& p : {a.at(3, 0, 1), a.at(2, 0, 1)}) ctx.setBlock(p, "minecraft:air");
    co_await ctx.ticks(40);
    int still = tankContents(a.at(6)).amount;
    co_await ctx.ticks(20);
    ctx.check(tankContents(a.at(6)).amount == still, "no transfer without rotation");
});

LV_REGISTER_SELFTEST("pipes.open_ends", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1760};
    ctx.require(co_await ctx.ensureLoaded(a.at(4)), "test area loaded");
    // a full lava cauldron is drained through an open end into a tank
    ctx.setBlock(a.at(0), "minecraft:lava_cauldron", {{"fill_level", 6}});
    ctx.setBlock(a.at(1), kFluidPipe, faces(kX));
    pumpAt(ctx, a.at(2), Face::East, 128);
    ctx.setBlock(a.at(3), kFluidPipe, faces(kX));
    tank(ctx, a.at(4), {});
    bool drained = co_await waitFor([&] { return tankContents(a.at(4)).amount == 1000; }, 200);
    ctx.check(drained, "the cauldron's bucket of lava reached the tank (" + std::to_string(tankContents(a.at(4)).amount) + ")");
    ctx.check(game::blockIdAt(region(), toBlockPos(a.at(0))) == "minecraft:cauldron", "the cauldron is empty");

    // a tank of water poured out of an open end into a closed pocket places a source
    Area b{ctx, 1770};
    ctx.require(co_await ctx.ensureLoaded(b.at(4)), "second area loaded");
    tank(ctx, b.at(0), {"minecraft:water", 4000});
    ctx.setBlock(b.at(1), kFluidPipe, faces(kX));
    pumpAt(ctx, b.at(2), Face::East, 128);
    ctx.setBlock(b.at(3), kFluidPipe, faces(kX));
    auto out = b.at(4);
    for (auto const& p : {b.at(4, -1), b.at(5), b.at(4, 0, -1), b.at(4, 0, 1)}) ctx.setBlock(p, "minecraft:stone");
    ctx.cleanupBlock(out);
    bool placed = co_await waitFor([&] {
        auto w = worldFluidAt(region(), toBlockPos(out));
        return w && w->source && w->id == "minecraft:water";
    }, 200);
    ctx.check(placed, "the open end placed a water source");
    ctx.check(tankContents(b.at(0)).amount == 3000, "one bucket left the tank (" + std::to_string(tankContents(b.at(0)).amount) + ")");
});

LV_REGISTER_SELFTEST("pipes.valve", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1780};
    ctx.require(co_await ctx.ensureLoaded(a.at(5)), "test area loaded");
    tank(ctx, a.at(0), {"minecraft:water", 8000});
    ctx.setBlock(a.at(1), kFluidPipe, faces(kX));
    pumpAt(ctx, a.at(2), Face::East, 64);
    // facing up, pipe along x, shaft along z
    auto v = a.at(3);
    ctx.setBlock(v, kValve, {{"levitate:facing", 1}, {"levitate:along_first", 0}});
    GlobalPos src = v;
    src.z += 1;
    ctx.setBlock(src, "levitate:kinetics_test_source", {{"levitate:axis", 2}});
    ctx.setBlock(a.at(4), kFluidPipe, faces(kX));
    tank(ctx, a.at(5), {});
    co_await ctx.ticks(3);
    float speed = lv::kinetics::speedAt(v);
    ctx.require(speed != 0, "the valve's shaft turns");
    if (speed < 0) lv::kinetics::setGeneratedSpeed(src, -lv::kinetics::speedAt(src));
    co_await ctx.ticks(2);
    ctx.require(lv::kinetics::speedAt(v) > 0, "the valve turns in the opening direction");
    bool opened = co_await waitFor([&] { return game::intState(blockAt(v), "levitate:enabled").value_or(0) == 1; }, 60);
    ctx.require(opened, "the valve opened");
    ctx.check(valvePointer(v) == 1.f, "pointer fully open");
    bool flows = co_await waitFor([&] { return tankContents(a.at(5)).amount > 0; }, 200);
    ctx.check(flows, "water flows through the open valve");
    lv::kinetics::setGeneratedSpeed(src, -lv::kinetics::speedAt(src));
    bool closed = co_await waitFor([&] { return game::intState(blockAt(v), "levitate:enabled").value_or(1) == 0; }, 60);
    ctx.require(closed, "reversed rotation closes the valve");
    co_await ctx.ticks(40);
    int still = tankContents(a.at(5)).amount;
    co_await ctx.ticks(20);
    ctx.check(tankContents(a.at(5)).amount == still, "no flow through a closed valve");
});

LV_REGISTER_SELFTEST("pipes.smart_filter", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1790};
    ctx.require(co_await ctx.ensureLoaded(a.at(5)), "test area loaded");
    tank(ctx, a.at(0), {"minecraft:water", 8000});
    auto s = a.at(1);
    ctx.setBlock(s, kSmartPipe, {{"levitate:face", 0}, {"levitate:facing", 3}});
    auto lava = common::items::make("minecraft:lava_bucket");
    ctx.require(lava.has_value(), "lava bucket");
    common::filterslot::set(s, *lava);
    scheduleChanged(s);
    pumpAt(ctx, a.at(2), Face::East, 64);
    ctx.setBlock(a.at(3), kFluidPipe, faces(kX));
    tank(ctx, a.at(4), {});
    co_await ctx.ticks(80);
    ctx.check(tankContents(a.at(4)).amount == 0, "a lava filter keeps water back");
    auto water = common::items::make("minecraft:water_bucket");
    ctx.require(water.has_value(), "water bucket");
    common::filterslot::set(s, *water);
    scheduleChanged(s);
    bool flows = co_await waitFor([&] { return tankContents(a.at(4)).amount > 0; }, 200);
    ctx.check(flows, "a water filter lets water through");
    auto centre = smartState(blockAt(s));
    ctx.check(centre.face == cp::AttachFace::Floor, "smart pipe on the floor");
    ctx.check(common::filterslot::hasSlots(kSmartPipe), "the smart pipe has a filter slot");
});

LV_REGISTER_SELFTEST("pipes.collision", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1750};
    GlobalPos base = a.at(0, 0, 6);
    auto      at   = [&](int dx, int dy = 0) {
        auto p = base;
        p.x += dx;
        p.y += dy;
        return p;
    };
    ctx.require(co_await ctx.ensureLoaded(at(3)), "test area loaded");
    tank(ctx, at(0), {"minecraft:water", 8000});
    ctx.setBlock(at(1), kFluidPipe, faces(kX));
    pumpAt(ctx, at(2), Face::East, 128);
    ctx.setBlock(at(3), kFluidPipe, faces(kX | cp::bit(Face::Down)));
    ctx.setBlock(at(3, -1), kFluidPipe, faces(cp::axisMask(Axis::Y)));
    tank(ctx, at(3, -2), {});
    // lava from the east: its pump's cog sits on the south side like the others
    auto lavaPump = at(4);
    ctx.setBlock(lavaPump, kPump, {{"levitate:facing", static_cast<int>(Face::West)}});
    GlobalPos cog = lavaPump;
    cog.z -= 1;
    GlobalPos src = cog;
    src.x += 1;
    ctx.setBlock(cog, "levitate:kinetics_test_cog", {{"levitate:axis", 0}});
    ctx.setBlock(src, "levitate:kinetics_test_source", {{"levitate:axis", 0}});
    lv::kinetics::setGeneratedSpeed(src, 128);
    ctx.setBlock(at(5), kFluidPipe, faces(kX));
    tank(ctx, at(6), {"minecraft:lava", 8000});
    ctx.cleanupBlock(at(3));
    bool met = co_await waitFor([&] { return game::blockIdAt(region(), toBlockPos(at(3))) == "minecraft:cobblestone"; }, 300);
    ctx.check(met, "water and lava meeting in a pipe leave cobblestone");
});

LV_REGISTER_SELFTEST("pipes.milk", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1760};
    GlobalPos base = a.at(0, 0, 6);
    auto      at   = [&](int dx) {
        auto p = base;
        p.x += dx;
        return p;
    };
    ctx.require(co_await ctx.ensureLoaded(at(4)), "test area loaded");
    tank(ctx, at(0), {"minecraft:milk", 8000});
    ctx.setBlock(at(1), kFluidPipe, faces(kX));
    pumpAt(ctx, at(2), Face::East, 64);
    ctx.setBlock(at(3), kFluidPipe, faces(kX));
    auto& mob = ctx.spawnPlayer("pipes_milk");
    auto  out = at(4);
    mob.teleport(Vec3{out.x + .5f, float(out.y), out.z + .5f}, mob.getDimensionId());
    mob.addEffect(MobEffectInstance(1, EffectDuration{2400})); // speed
    auto hasSpeed = [&] {
        for (auto const& e : mob.getAllEffects())
            if (e.mId == 1u && e.mDuration->mValue > 0) return true;
        return false;
    };
    ctx.require(hasSpeed(), "the player has an effect");
    bool cured = co_await waitFor([&] { return !hasSpeed(); }, 200);
    ctx.check(cured, "milk poured out of a pipe clears effects");
    ctx.check(tankContents(at(0)).amount < 8000, "milk left the tank");
    ctx.check(game::blockIdAt(region(), toBlockPos(out)) == "minecraft:air", "milk has no world block");
});

LV_REGISTER_SELFTEST("pipes.persistence", [](Context& ctx) {
    auto& m = model();
    auto  j = m.save(0);
    ctx.check(j.is_object() && j.contains("nodes") && j.contains("pumps"), "the model saves");
    cp::Model copy{m.host()};
    copy.load(j);
    ctx.check(copy.nodes().size() <= m.nodes().size(), "the saved model loads back");
});

} // namespace lv::feature::pipes

