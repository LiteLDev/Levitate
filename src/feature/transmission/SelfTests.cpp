// Selftests of the transmission unit (U6) on a real world. testPos dx 600..699, dz -2..2.
// Rotation comes from U2's test generator (levitate:kinetics_test_source, 16 RPM, 8 SU/RPM).

#include <algorithm>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/level/GameType.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Items.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::transmission {

namespace {

using selftest::Context;
namespace items = feature::common::items;

constexpr char kSource[]   = "levitate:kinetics_test_source";
constexpr char kConsumer[] = "levitate:kinetics_test_consumer";
constexpr int  X = 0, Y = 1, Z = 2;

struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy;
        return p;
    }
    bool put(GlobalPos const& p, std::string const& id, std::vector<std::pair<std::string, int>> states = {}) {
        return ctx.setBlock(p, id, states);
    }
};

BlockSource& world() { return *regionOf(0); }
std::string   speed(GlobalPos const& p) { return std::to_string(kinetics::speedAt(p)); }
int           st(GlobalPos const& p, char const* s) { return stateAt(world(), p, s, -1); }

} // namespace

LV_REGISTER_SELFTEST("transmission.registry", [](Context& ctx) {
    for (auto const& id : allBlocks()) {
        ctx.check(kinetics::specOf(id) != nullptr, id + " is a kinetic block");
        ctx.check(game::blockWith(id) != nullptr, id + " is in the behavior pack");
        if (isCuckooClock(id)) ctx.check(kinetics::stressImpactOf(id) == 1, id + " uses 1 SU per RPM");
        else ctx.check(kinetics::stressImpactOf(id) == 0, id + " has no stress impact");
    }
    ctx.check(items::make(kVerticalGearbox).has_value(), "the vertical gearbox item exists");
    ctx.check(items::make(kShaft).has_value(), "blocks have items");
});

LV_REGISTER_SELFTEST("transmission.shafts_and_cogs", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 600};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.put(a.at(0), kSource, {{kAxis, X}});
    a.put(a.at(1), kShaft, {{kAxis, X}});
    a.put(a.at(2), kAndesiteShaft, {{kAxis, X}});
    a.put(a.at(3), kCogwheel, {{kAxis, X}});
    a.put(a.at(3, 1), kCogwheel, {{kAxis, X}});            // meshes sideways
    a.put(a.at(3, 2, 1), kLargeCogwheel, {{kAxis, X}});    // diagonal to the upper cog in its plane: x -1/2
    a.put(a.at(4), kBrassLargeCog, {{kAxis, X}, {kBottomShaft, 1}}); // on the cog's shaft
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(a.at(2)) == 16, "encased shaft passes 16 RPM, got " + speed(a.at(2)));
    ctx.check(kinetics::speedAt(a.at(3)) == 16, "cog on the shaft: 16, got " + speed(a.at(3)));
    ctx.check(kinetics::speedAt(a.at(3, 1)) == -16, "meshing cog: -16, got " + speed(a.at(3, 1)));
    ctx.check(kinetics::speedAt(a.at(3, 2, 1)) == 8, "large cog at a small cog: 8, got " + speed(a.at(3, 2, 1)));
    ctx.check(kinetics::speedAt(a.at(4)) == 16, "encased large cog with its bottom shaft: 16, got " + speed(a.at(4)));
});

LV_REGISTER_SELFTEST("transmission.gearbox", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 610};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.put(a.at(0), kSource, {{kAxis, X}});
    a.put(a.at(1), kGearbox, {{kAxis, Y}});
    a.put(a.at(2), kShaft, {{kAxis, X}});        // opposite the source
    a.put(a.at(1, 0, 1), kShaft, {{kAxis, Z}});  // south
    a.put(a.at(1, 0, -1), kShaft, {{kAxis, Z}}); // north
    a.put(a.at(1, 1), kShaft, {{kAxis, Y}});     // above: no shaft end on the gearbox's axis
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(a.at(2)) == -16, "the far side turns the other way, got " + speed(a.at(2)));
    // the source is west of the gearbox (a negative face): positive faces across turn the same way
    ctx.check(kinetics::speedAt(a.at(1, 0, 1)) == 16, "south output: 16, got " + speed(a.at(1, 0, 1)));
    ctx.check(kinetics::speedAt(a.at(1, 0, -1)) == -16, "north output: -16, got " + speed(a.at(1, 0, -1)));
    ctx.check(kinetics::speedAt(a.at(1, 1)) == 0, "no output along the gearbox axis, got " + speed(a.at(1, 1)));
});

LV_REGISTER_SELFTEST("transmission.clutch_gearshift", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 620};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    for (int row : {0, 2}) {
        a.put(a.at(0, 0, row - 1), kSource, {{kAxis, X}});
        a.put(a.at(1, 0, row - 1), row == 0 ? kClutch : kGearshift, {{kAxis, X}});
        a.put(a.at(2, 0, row - 1), kShaft, {{kAxis, X}});
    }
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(a.at(2, 0, -1)) == 16, "unpowered clutch passes rotation");
    ctx.check(kinetics::speedAt(a.at(2, 0, 1)) == 16, "unpowered gearshift passes rotation");
    a.put(a.at(1, 1, -1), "minecraft:redstone_block");
    a.put(a.at(1, 1, 1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    ctx.check(st(a.at(1, 0, -1), kPowered) == 1, "the clutch notices the redstone block");
    ctx.check(kinetics::speedAt(a.at(2, 0, -1)) == 0, "powered clutch disconnects, got " + speed(a.at(2, 0, -1)));
    ctx.check(kinetics::speedAt(a.at(2, 0, 1)) == -16, "powered gearshift reverses, got " + speed(a.at(2, 0, 1)));
    game::clearBlock(world(), game::blockPos(a.at(1, 1, -1)));
    co_await ctx.ticks(4);
    ctx.check(kinetics::speedAt(a.at(2, 0, -1)) == 16, "unpowered again it reconnects, got " + speed(a.at(2, 0, -1)));
});

LV_REGISTER_SELFTEST("transmission.chain_drives", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 630};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    // three chain drives (axis y) side by side along x, driven from below the first one
    a.put(a.at(0, 1), kChainDrive, {{kAxis, Y}});
    a.put(a.at(1, 1), kChainDrive, {{kAxis, Y}});
    a.put(a.at(2, 1), kChainGearshift, {{kAxis, Y}});
    co_await ctx.ticks(3);
    co_await ctx.ticks(2);
    ctx.check(st(a.at(0, 1), kPart) == 1 && st(a.at(1, 1), kPart) == 2 && st(a.at(2, 1), kPart) == 3, "parts start / middle / end, got "
        + std::to_string(st(a.at(0, 1), kPart)) + std::to_string(st(a.at(1, 1), kPart)) + std::to_string(st(a.at(2, 1), kPart)));
    a.put(a.at(0), kSource, {{kAxis, Y}});
    a.put(a.at(2, 2), kShaft, {{kAxis, Y}});
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(a.at(1, 1)) == 16, "chain drives turn the same way, got " + speed(a.at(1, 1)));
    ctx.check(kinetics::speedAt(a.at(2, 2)) == 16, "through the chain to the shaft, got " + speed(a.at(2, 2)));
    a.put(a.at(3, 1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    ctx.check(st(a.at(2, 1), kSignal) == 15, "the adjustable gearshift reads signal 15, got " + std::to_string(st(a.at(2, 1), kSignal)));
    ctx.check(kinetics::speedAt(a.at(2, 2)) == 8, "a powered gearshift at the end of the chain halves, got " + speed(a.at(2, 2)));
    game::clearBlock(world(), game::blockPos(a.at(1, 1)));
    co_await ctx.ticks(3);
    ctx.check(st(a.at(0, 1), kPart) == 0, "the first drive is single again");
    ctx.check(kinetics::speedAt(a.at(2, 2)) == 0, "the chain is broken, got " + speed(a.at(2, 2)));
});

LV_REGISTER_SELFTEST("transmission.speed_controller", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 640};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.put(a.at(0), kSource, {{kAxis, X}});
    a.put(a.at(1), kSpeedController, {{kAxis, X}});
    a.put(a.at(1, 1), kLargeCogwheel, {{kAxis, Z}});
    a.put(a.at(2, 2), kCogwheel, {{kAxis, Z}}); // meshes with the large cog diagonally
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(a.at(1, 1)) == kDefaultTargetSpeed, "the cog turns at the default target, got " + speed(a.at(1, 1)));
    ctx.check(kinetics::speedAt(a.at(2, 2)) == -2 * kDefaultTargetSpeed, "small cog doubles, got " + speed(a.at(2, 2)));
    setTargetSpeed(a.at(1), -48);
    co_await ctx.ticks(3);
    ctx.check(targetSpeed(a.at(1)) == -48, "target stored");
    ctx.check(kinetics::speedAt(a.at(1, 1)) == -48, "new target -48, got " + speed(a.at(1, 1)));
    co_await ctx.ticks(2);
    ctx.check(st(a.at(1), kBracket) == 1, "the bracket shows under the large cog");
});

LV_REGISTER_SELFTEST("transmission.sequencer", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 650};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.put(a.at(0), kSource, {{kAxis, X}});
    a.put(a.at(1), kSequencedGearshift, {{kAxis, X}});
    a.put(a.at(2), kShaft, {{kAxis, X}});
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(a.at(2)) == 0, "idle: the output is disconnected, got " + speed(a.at(2)));
    ctx.check(sequencerInstructions(a.at(1)) == ct::defaultInstructions(), "default: turn 90 degrees");
    // turn 90 degrees, double speed backwards, then delay 10 ticks, then end
    setSequencerInstructions(a.at(1), {{ct::Instr::TurnAngle, -2, 90}, {ct::Instr::Delay, 1, 10}, {ct::Instr::End, 1, 1}});
    a.put(a.at(1, 1), "minecraft:redstone_block");
    co_await ctx.ticks(3);
    ctx.check(st(a.at(1), kSeqState) == 1, "running the first instruction");
    ctx.check(kinetics::speedAt(a.at(2)) == -32, "double speed reversed, got " + speed(a.at(2)));
    ctx.check(comparatorSignal(a.at(1)) == 1, "comparator output = instruction 1");
    // 32 RPM = 9.6 degrees per tick: ceil(90 / 9.6) + 2 = 12 ticks
    co_await ctx.ticks(13);
    ctx.check(st(a.at(1), kSeqState) == 2, "the delay runs, got state " + std::to_string(st(a.at(1), kSeqState)));
    ctx.check(kinetics::speedAt(a.at(2)) == 0, "no rotation during a delay, got " + speed(a.at(2)));
    co_await ctx.ticks(12);
    ctx.check(sequencerCurrent(a.at(1)) == -1, "the sequence ended");
    ctx.check(st(a.at(1), kSeqState) == 2, "still powered: the state stays");
    game::clearBlock(world(), game::blockPos(a.at(1, 1)));
    co_await ctx.ticks(3);
    ctx.check(st(a.at(1), kSeqState) == 0, "unpowered: back to 0");
});

LV_REGISTER_SELFTEST("transmission.gauges", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 660};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.put(a.at(0), kSource, {{kAxis, X}});
    a.put(a.at(1), kSpeedometer, {{kFacing, 2}, {kAlongFirst, 1}}); // facing north, axis x
    a.put(a.at(2), kStressometer, {{kFacing, 3}, {kAlongFirst, 1}}); // facing south, axis x
    a.put(a.at(3), kConsumer, {{kAxis, X}});                          // 4 SU/RPM of 8
    // a comparator north of the stressometer reading it: Bedrock's cardinal direction names the
    // input side (index 0 = south; south, west, north, east)
    a.put(a.at(2, 0, -1), "minecraft:unpowered_comparator", {{"minecraft:cardinal_direction", 0}});
    co_await ctx.ticks(4);
    ctx.check(kinetics::speedAt(a.at(3)) == 16, "gauges pass the rotation, got " + speed(a.at(3)));
    float speedDial = dialTarget(a.at(1));
    ctx.check(std::abs(speedDial - ct::speedDialTarget(16, 30, 100, 256)) < 1e-4, "speedometer dial for 16 RPM, got " + std::to_string(speedDial));
    ctx.check(std::abs(dialTarget(a.at(2)) - .5f) < 1e-4, "stressometer: 64 of 128 SU, got " + std::to_string(dialTarget(a.at(2))));
    ctx.check(comparatorSignal(a.at(2)) == 7, "comparator signal 7");
    ctx.check(st(a.at(2), kDial) == ct::dialStateOf(.5f), "the needle state follows");
    co_await ctx.ticks(4);
    auto comparator = idAt(world(), a.at(2, 0, -1));
    ctx.info("comparator behind the stressometer: " + comparator);
    ctx.check(comparator == "minecraft:powered_comparator", "a comparator reads the stressometer");
    a.put(a.at(4), kConsumer, {{kAxis, X}});
    a.put(a.at(5), kConsumer, {{kAxis, X}});
    co_await ctx.ticks(3);
    ctx.check(dialTarget(a.at(2)) == 1.125f, "overstressed: the dial goes past the end");
    ctx.check(dialTarget(a.at(1)) == 0, "overstressed: the speedometer drops to 0");
    // U42: GaugeBlock.animateTick puffs coloured dust from the stressometer's dial for players near it
    auto& player = ctx.player();
    auto  g      = a.at(2);
    player.teleport(Vec3{g.x + .5f, g.y + 1.f, g.z + 3.5f}, player.getDimensionId());
    auto puffs = gaugeDustSent();
    co_await ctx.ticks(200);
    ctx.check(gaugeDustSent() > puffs, "gauge dust particles are sent to players near a turning gauge");
});

LV_REGISTER_SELFTEST("transmission.placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 670};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto& sim = static_cast<SimulatedPlayer&>(ctx.player());
    sim.setPlayerGameType(GameType::Creative);
    auto use = [&](char const* item, GlobalPos const& on, ScriptModuleMinecraft::ScriptFacing face, Vec3 hit) {
        auto stack = items::make(item);
        if (!stack) return false;
        ctx.give(sim, *stack, 0);
        // stand in front of the clicked face (the server only accepts faces the player can see)
        auto o = ck::offsetOf(static_cast<Face>(static_cast<int>(face)));
        float dx = o.x ? o.x * 2.5f : 0.f, dz = o.z ? o.z * 2.5f : -2.5f;
        sim.teleport(Vec3{on.x + .5f + dx, on.y + 1.f, on.z + .5f + dz}, DimensionType(on.dim));
        sim.simulateLookAt(Vec3{on.x + .5f, on.y + .5f, on.z + .5f}, sim::LookDuration::Instant);
        return sim.simulateUseItemInSlotOnBlock(0, game::blockPos(on), face, hit);
    };
    using F = ScriptModuleMinecraft::ScriptFacing;
    // a shaft placed against the side of a shaft: placement assist extends the line
    a.put(a.at(0, 1), kShaft, {{kAxis, X}});
    use(kShaft, a.at(0, 1), F::Up, Vec3{.9f, 1.f, .5f});
    co_await ctx.ticks(2);
    ctx.check(idAt(world(), a.at(1, 1)) == kShaft && st(a.at(1, 1), kAxis) == X, "shaft placed at the end of the line (east)");
    co_await ctx.ticks(6); // one assisted use per 5 ticks
    // a cog against a cog's rim: the next cog meshes beside it
    a.put(a.at(4, 1), kCogwheel, {{kAxis, Y}});
    {
        // the unit's use handler directly (the SimulatedPlayer refuses clicks on side faces)
        auto cog = items::make(kCogwheel);
        ctx.require(cog.has_value(), "cogwheel item");
        ctx.check(cogItemUseFirst(sim, *cog, a.at(4, 1), 1., .5, .5), "the cog helper takes the click");
    }
    co_await ctx.ticks(2);
    ctx.check(idAt(world(), a.at(5, 1)) == kCogwheel && st(a.at(5, 1), kAxis) == Y, "cog placed beside the cog with its axis");
    ctx.cleanupBlock(a.at(5, 1));
    co_await ctx.ticks(6); // one assisted use per 5 ticks
    // plain placement next to a shaft end: the preferred axis
    a.put(a.at(8, 1), kShaft, {{kAxis, Z}});
    a.put(a.at(8, 0, 1), "minecraft:stone");
    use(kGearshift, a.at(8, 0, 1), F::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(2);
    ctx.check(idAt(world(), a.at(8, 1, 1)) == kGearshift && st(a.at(8, 1, 1), kAxis) == Z, "gearshift takes the axis of the shaft it touches");
    ctx.cleanupBlock(a.at(8, 1, 1));
    co_await ctx.ticks(6); // one assisted use per 5 ticks
    // the vertical gearbox item places a gearbox with a horizontal axis
    a.put(a.at(11, 0), "minecraft:stone");
    {
        // VerticalGearboxItem's axis: across the shaft beside it (a shaft along x east of the spot)
        a.put(a.at(12, 1), kShaft, {{kAxis, X}});
        PlaceContext pc{a.at(11, 0), Face::Up, a.at(11, 1)};
        auto         states = stateForPlacement(world(), kVerticalGearbox, pc, sim);
        int          axis   = -1;
        if (states)
            for (auto const& [k, v] : *states)
                if (k == std::string_view(kAxis)) axis = v;
        ctx.check(axis == Z, "vertical gearbox: axis across the neighbouring shaft, got " + std::to_string(axis));
    }
    ctx.cleanupBlock(a.at(1, 1));
    co_await ctx.ticks(6); // one assisted use per 5 ticks
    // encasing (needs U10's casing item)
    if (items::make(kAndesiteCasing)) {
        a.put(a.at(14, 1), kShaft, {{kAxis, X}});
        use(kAndesiteCasing, a.at(14, 1), F::Up, Vec3{.5f, 1.f, .5f});
        co_await ctx.ticks(2);
        ctx.check(idAt(world(), a.at(14, 1)) == kAndesiteShaft, "a casing encases the shaft");
    } else {
        ctx.info("andesite casing not registered yet: encasing not tested");
    }
    sim.setPlayerGameType(GameType::Survival);
});

LV_REGISTER_SELFTEST("transmission.sequencer_screen", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 680};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.put(a.at(0), kSequencedGearshift, {{kAxis, X}});
    auto& player = ctx.player();
    net::setTestCompanion(player, true);
    ctx.check(openSequencerScreen(player, a.at(0)), "the screen opens");
    auto msgs = net::takeTestMessages(player);
    bool open = std::any_of(msgs.begin(), msgs.end(), [](auto const& m) {
        return m.channel == "screen.open" && m.payload.value("type", std::string{}) == "transmission.sequenced_gearshift";
    });
    ctx.check(open, "companions get the native screen");
    auto pos = a.at(0);
    net::injectFromClient(player, "screen.action", {
        {"type",    "transmission.sequenced_gearshift"},
        {"context", {{"pos", {pos.dim, pos.x, pos.y, pos.z}}}},
        {"action",  {{"op", "set"}, {"instructions", ct::instructionsToJson({{ct::Instr::Await, 1, 1}, {ct::Instr::End, 1, 1}})}}},
    });
    co_await ctx.ticks(1);
    auto list = sequencerInstructions(pos);
    ctx.check(list.size() == 2 && list[0].type == ct::Instr::Await, "the screen action configures the instructions");
    net::setTestCompanion(player, false);
});


LV_REGISTER_SELFTEST("transmission.wrench_encased", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace wrench = goggles::wrench;
    Area a{ctx, 4220};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto& player = ctx.player();
    auto  click  = [&](GlobalPos const& p, int face, bool sneak) {
        auto const* block = &world().getBlock(game::blockPos(p));
        auto const* h     = wrench::handlerOf(block->getTypeName());
        if (!h) return false;
        wrench::Context c{player, world(), p, *block, face};
        auto const&     fn = sneak ? h->onSneakWrenched : h->onWrenched;
        return fn && fn(c) == wrench::Result::Success;
    };
    a.put(a.at(0, 1), kAndesiteCog, {{kAxis, Y}, {kTopShaft, 0}, {kBottomShaft, 0}});
    ctx.check(click(a.at(0, 1), 1, false), "the wrench on the top face is handled");
    ctx.check(st(a.at(0, 1), kTopShaft) == 1 && st(a.at(0, 1), kBottomShaft) == 0, "the top shaft end is toggled on");
    click(a.at(0, 1), 0, false);
    ctx.check(st(a.at(0, 1), kBottomShaft) == 1, "the bottom shaft end is toggled on");
    click(a.at(0, 1), 1, false);
    ctx.check(st(a.at(0, 1), kTopShaft) == 0 && st(a.at(0, 1), kBottomShaft) == 1, "and off again");
    ctx.check(click(a.at(0, 1), 1, true), "sneak-wrench is handled");
    ctx.check(idAt(world(), a.at(0, 1)) == kCogwheel && st(a.at(0, 1), kAxis) == Y, "the casing comes off: a cogwheel on the same axis");
    a.put(a.at(2, 1), kBrassShaft, {{kAxis, X}});
    click(a.at(2, 1), 1, true);
    ctx.check(idAt(world(), a.at(2, 1)) == kShaft && st(a.at(2, 1), kAxis) == X, "an encased shaft turns back into a shaft");
    a.put(a.at(4, 1), kBrassLargeCog, {{kAxis, Z}, {kTopShaft, 0}, {kBottomShaft, 0}});
    click(a.at(4, 1), 1, true);
    ctx.check(idAt(world(), a.at(4, 1)) == kLargeCogwheel, "an encased large cogwheel turns back into a large cogwheel");
    for (int dx : {0, 2, 4}) ctx.cleanupBlock(a.at(dx, 1));
});

} // namespace lv::feature::transmission
