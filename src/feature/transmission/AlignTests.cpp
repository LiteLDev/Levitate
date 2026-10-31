// Selftests of the U52 alignment of the transmission unit with Create 6.0.10: the speedometer
// while overstressed, the wrench on gauges / chain drives / the speed controller / encased cogs,
// and the shaft placement helper with cogs and the extendo grip. testPos dx 5200..5259 (U52).

#include <algorithm>

#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/goggles/Tooltip.h"
#include "feature/common/Items.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::transmission {

namespace {

using selftest::Context;
namespace items  = feature::common::items;
namespace wrench = goggles::wrench;

constexpr char kSource[]   = "levitate:kinetics_test_source";
constexpr char kConsumer[] = "levitate:kinetics_test_consumer";
constexpr int  X = 0, Y = 1, Z = 2;

GlobalPos at(Context& ctx, int dx, int dy = 0, int dz = 0) {
    auto p = ctx.testPos(dx, dz);
    p.y += dy;
    return p;
}

BlockSource& world() { return *regionOf(0); }
int          st(GlobalPos const& p, char const* s) { return stateAt(world(), p, s, -1); }

/// A wrench click (not sneaking) on `face` of the block at `p`, through the block's handler.
bool wrenchClick(Context& ctx, GlobalPos const& p, int face) {
    auto const& block = world().getBlock(game::blockPos(p));
    auto const* h     = wrench::handlerOf(block.getTypeName());
    if (!h) return false;
    wrench::Context c{ctx.player(), world(), p, block, face};
    auto            r = h->onWrenched ? h->onWrenched(c) : wrench::defaultOnWrenched(c);
    return r == wrench::Result::Success;
}

} // namespace

// K1: SpeedGaugeBlockEntity formats its theoretical speed (struck through while overstressed)
LV_REGISTER_SELFTEST("transmission.speedometer_overstressed", [](Context& ctx) -> ll::coro::CoroTask<> {
    ctx.require(co_await ctx.ensureLoaded(at(ctx, 5200)), "test area loaded");
    ctx.setBlock(at(ctx, 5200), kSource, {{kAxis, X}});
    ctx.setBlock(at(ctx, 5201), kSpeedometer, {{kFacing, 2}, {kAlongFirst, 1}}); // facing north, axis x
    for (int i = 2; i <= 4; ++i) ctx.setBlock(at(ctx, 5200 + i), kConsumer, {{kAxis, X}});    // 192 of 128 SU
    co_await ctx.ticks(3);
    ctx.require(kinetics::isOverstressed(at(ctx, 5201)), "the network is overstressed");
    auto& p = ctx.player();
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, items::make("levitate:goggles").value_or(ItemStack{}));
    auto        lines = goggles::overlayFor(p, at(ctx, 5201)).lines;
    std::string all;
    for (auto const& l : lines) all += "[" + core::goggles::stripFormatting(l) + "]";
    ctx.check(all.find("(16 RPM)") != std::string::npos, "the speedometer shows the network's 16 RPM while overstressed: " + all);
    ctx.check(all.find("(0 RPM)") == std::string::npos, "not 0 RPM: " + all);
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
});

// K2: the wrench toggles a gauge's shaft axis on its dial axis and on top / bottom turns
LV_REGISTER_SELFTEST("transmission.wrench_gauge", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g = at(ctx, 5206, 1);
    ctx.require(co_await ctx.ensureLoaded(g), "test area loaded");
    ctx.setBlock(g, kSpeedometer, {{kFacing, 2}, {kAlongFirst, 0}}); // facing north, shaft along y
    ctx.check(wrenchClick(ctx, g, 2), "the wrench on the dial is handled");
    ctx.check(st(g, kFacing) == 2 && st(g, kAlongFirst) == 1, "a click on the dial toggles the shaft axis to x");
    wrenchClick(ctx, g, 1);
    ctx.check(st(g, kFacing) == 5, "a click on top turns the dial east, got " + std::to_string(st(g, kFacing)));
    ctx.check(st(g, kAlongFirst) == 0, "and toggles the flag (the shaft stays horizontal: along z)");
    ctx.check(ct::gaugeRotationAxis(Face::East, false) == Axis::Z, "facing east, not along first: shaft along z");
    wrenchClick(ctx, g, 3); // south face (z axis): the dial turns about z
    ctx.check(st(g, kFacing) == 0 && st(g, kAlongFirst) == 0, "a click on the side turns the dial down and keeps the flag");
});

// K3: a connected chain drive turns about its connection axis and works out its part again
LV_REGISTER_SELFTEST("transmission.wrench_chain_drive", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto lo = at(ctx, 5210, 1), hi = at(ctx, 5210, 2), single = at(ctx, 5213, 1);
    ctx.require(co_await ctx.ensureLoaded(lo), "test area loaded");
    ctx.setBlock(lo, kChainDrive, {{kAxis, X}});
    ctx.setBlock(hi, kChainDrive, {{kAxis, X}});
    co_await ctx.ticks(2);
    ctx.require(st(lo, kPart) == static_cast<int>(ct::ChainPart::Start) && st(hi, kPart) == static_cast<int>(ct::ChainPart::End),
                "the two drives connect along y, parts " + std::to_string(st(lo, kPart)) + " / " + std::to_string(st(hi, kPart)));
    ctx.check(wrenchClick(ctx, lo, 2), "the wrench on a chain drive is handled");
    co_await ctx.ticks(2);
    ctx.check(st(lo, kAxis) == Z, "clicked on its north face it still turns about the chain (y): x -> z, got " + std::to_string(st(lo, kAxis)));
    ctx.check(st(lo, kPart) == static_cast<int>(ct::ChainPart::Start), "and stays connected upwards, part " + std::to_string(st(lo, kPart)));
    ctx.check(st(hi, kPart) == static_cast<int>(ct::ChainPart::End), "the upper drive keeps its end, part " + std::to_string(st(hi, kPart)));
    ctx.setBlock(single, kChainDrive, {{kAxis, X}});
    co_await ctx.ticks(1);
    wrenchClick(ctx, single, 2); // unconnected: the clicked face's axis (z) turns x to y
    ctx.check(st(single, kAxis) == Y && st(single, kPart) == 0, "an unconnected drive turns about the clicked face");
});

// K4: the speed controller (HorizontalAxisKineticBlock) only turns on its top and bottom
LV_REGISTER_SELFTEST("transmission.wrench_speed_controller", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto c = at(ctx, 5216, 1);
    ctx.require(co_await ctx.ensureLoaded(c), "test area loaded");
    ctx.setBlock(c, kSpeedController, {{kAxis, X}});
    wrenchClick(ctx, c, 2);
    ctx.check(st(c, kAxis) == X, "a side click leaves it along x, got " + std::to_string(st(c, kAxis)));
    wrenchClick(ctx, c, 5);
    ctx.check(st(c, kAxis) == X, "never vertical");
    wrenchClick(ctx, c, 1);
    ctx.check(st(c, kAxis) == Z, "a click on top turns it to z, got " + std::to_string(st(c, kAxis)));
});

// K5: EncasedCogwheelBlock.swapShaftsForRotation on a wrench turn
LV_REGISTER_SELFTEST("transmission.wrench_encased_cog_turn", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto c = at(ctx, 5220, 1);
    ctx.require(co_await ctx.ensureLoaded(c), "test area loaded");
    ctx.setBlock(c, kAndesiteCog, {{kAxis, X}, {kTopShaft, 1}, {kBottomShaft, 0}}); // shaft end east
    wrenchClick(ctx, c, 1); // about y: east -> south (still the positive end)
    ctx.check(st(c, kAxis) == Z && st(c, kTopShaft) == 1 && st(c, kBottomShaft) == 0, "x -> z: the end goes south (top)");
    wrenchClick(ctx, c, 1); // south -> west: the end is now the negative one
    ctx.check(st(c, kAxis) == X && st(c, kTopShaft) == 0 && st(c, kBottomShaft) == 1, "z -> x: the end goes west (bottom)");
    wrenchClick(ctx, c, 2); // about z: west -> up
    ctx.check(st(c, kAxis) == Y && st(c, kTopShaft) == 1 && st(c, kBottomShaft) == 0, "x -> y: the end goes up (top)");
});

// K17: ShaftBlock.PlacementHelper counts cogs, takes cog items, +4 with the extendo grip
LV_REGISTER_SELFTEST("transmission.shaft_helper", [](Context& ctx) -> ll::coro::CoroTask<> {
    ctx.require(co_await ctx.ensureLoaded(at(ctx, 5224, 1)), "test area loaded");
    auto& player = ctx.player();
    ctx.setBlock(at(ctx, 5223, 1), "minecraft:stone");
    ctx.setBlock(at(ctx, 5224, 1), kShaft, {{kAxis, X}});
    ctx.setBlock(at(ctx, 5225, 1), kCogwheel, {{kAxis, X}});
    ctx.setBlock(at(ctx, 5226, 1), kLargeCogwheel, {{kAxis, X}});
    ctx.cleanupBlock(at(ctx, 5227, 1));
    auto cog = items::make(kCogwheel, 4);
    ctx.require(cog.has_value(), "cogwheel item");
    ctx.check(useOn(player, *cog, at(ctx, 5224, 1), Face::Up, .9, 1., .5), "a cogwheel used on a shaft takes the helper");
    ctx.check(idAt(world(), at(ctx, 5227, 1)) == kCogwheel && st(at(ctx, 5227, 1), kAxis) == X,
              "the cog goes past the cogs of the line, got " + idAt(world(), at(ctx, 5227, 1)));
    // 12 members east of the clicked shaft: out of range, the west side is stone
    for (int i = 5228; i <= 5236; ++i) ctx.setBlock(at(ctx, i, 1), kShaft, {{kAxis, X}});
    ctx.cleanupBlock(at(ctx, 5237, 1));
    auto shaft = items::make(kShaft, 4);
    ctx.require(shaft.has_value(), "shaft item");
    ctx.check(!useOn(player, *shaft, at(ctx, 5224, 1), Face::Up, .9, 1., .5), "12 poles: out of the assist range");
    player.setOffhandSlot(items::make(kExtendoGrip).value_or(ItemStack{}));
    bool placed = useOn(player, *shaft, at(ctx, 5224, 1), Face::Up, .9, 1., .5);
    player.setOffhandSlot(ItemStack{});
    if (!items::make(kExtendoGrip)) {
        ctx.info("extendo grip not registered: the +4 range not tested");
        co_return;
    }
    ctx.check(placed && idAt(world(), at(ctx, 5237, 1)) == kShaft, "with the extendo grip in the other hand the range is 16");
});

} // namespace lv::feature::transmission
