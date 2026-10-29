// Selftests of the shared interaction and redstone dispatch (U36, testPos dx 3600..3699): the
// order of the use-on handlers (a unit's own wrench handling before U8's generic wrench) and
// comparators reading Create blocks through the shared analog registry (tank, vault, depot).

#include <string>

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::hooks {

namespace {

using selftest::Context;
using GP = core::GlobalPos;
namespace ia     = common::interactions;
namespace rs     = common::redstone;
namespace citems = common::items;

constexpr char kSource[]  = "levitate:kinetics_test_source";
constexpr char kShaft[]   = "levitate:shaft";
constexpr char kTank[]    = "levitate:fluid_tank";
constexpr char kVault[]   = "levitate:item_vault";
constexpr char kDepot[]   = "levitate:depot";
constexpr char kAxis[]    = "levitate:axis";

GP at(GP p, int dx, int dy = 0, int dz = 0) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

ItemStack stackOf(Context& ctx, std::string const& id, int count = 1) {
    auto s = citems::make(id, count);
    ctx.require(s.has_value(), id + " is registered");
    return *s;
}

/// A vanilla comparator at `pos` whose input (rear) is on the `towards` side (Bedrock's cardinal
/// direction points at the input).
bool placeComparator(Context& ctx, GP pos, std::string const& towards) {
    auto def = Block::tryGetFromRegistry(HashedString("minecraft:unpowered_comparator"));
    if (!def) return false;
    CompoundTag nbt = def->mSerializationId.get();
    nbt["states"].get<CompoundTag>().putString("minecraft:cardinal_direction", towards);
    auto cb = Block::tryGetFromRegistry(nbt);
    if (!cb) return false;
    ctx.cleanupBlock(pos);
    game::setBlock(regionOf(ctx), game::blockPos(pos), *cb);
    return true;
}

int wireSignal(Context& ctx, GP wire) {
    auto& r  = regionOf(ctx);
    auto  bp = game::blockPos(wire);
    if (!r.hasChunksAt(bp, 0, false)) return -1;
    auto const& b = r.getBlock(bp);
    if (b.getTypeName() != "minecraft:redstone_wire") return -1;
    return game::intState(b, "redstone_signal").value_or(-1);
}

/// A comparator east of `block` reading it (directly, or through `solid` blocks in between) and
/// a wire behind the comparator.
struct ComparatorRig {
    GP comparator, wire;
};
ComparatorRig rigEast(Context& ctx, GP block, int solid = 0) {
    for (int i = 1; i <= solid; ++i) ctx.setBlock(at(block, i), "minecraft:stone");
    ComparatorRig rig{at(block, 1 + solid), at(block, 2 + solid)};
    ctx.require(placeComparator(ctx, rig.comparator, "west"), "comparator placed");
    ctx.require(ctx.setBlock(rig.wire, "minecraft:redstone_wire"), "wire placed");
    return rig;
}

void floorUnder(Context& ctx, GP p, int len) {
    for (int i = -1; i <= len; ++i) ctx.setBlock(at(p, i, -1), "minecraft:stone");
}

} // namespace

// ---- dispatch order ------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("hooks.wrench_order", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3600), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& player = ctx.player();
    auto& region = regionOf(ctx);
    floorUnder(ctx, at(base, 0, 0, 1), 5);

    // a belt along +x between two test shafts, driven from the north
    GP s = at(base, 0, 0, 1), e = at(base, 3, 0, 1);
    ctx.setBlock(s, belts::kTestShaftId, {{kAxis, 2}});
    ctx.setBlock(e, belts::kTestShaftId, {{kAxis, 2}});
    for (int i = 1; i < 3; ++i) ctx.cleanupBlock(at(s, i));
    ctx.setBlock(base, kSource, {{kAxis, 2}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(base, 16);
    co_await ctx.ticks(1);
    ctx.require(belts::createBelt(s, e), "belt built");
    ctx.require(belts::segmentAt(at(s, 1)).has_value(), "belt segment");

    // a probe between the units' handlers (Normal) and U8's generic wrench (Fallback)
    int  probed = 0;
    auto probe  = ia::onWrench("", [&](ia::UseOnContext const&) {
        ++probed;
        return ia::Result::Pass;
    }, ia::priority::Low);

    auto        wrench = stackOf(ctx, ia::kWrenchId);
    std::string wrenchId = ia::kWrenchId;
    player.setSelectedItem(wrench);
    {
        GP           seg     = at(s, 1);
        Block const& block   = region.getBlock(game::blockPos(seg));
        std::string  blockId = block.getTypeName();
        ia::UseOnContext c{player, player.getSelectedItem(), wrenchId, block, blockId, seg, 1, {.5, 1.0, .5}, false, true};
        auto r = ia::dispatchUseOn(c);
        ctx.check(r == ia::Result::Consume, "the wrench click on a belt is taken");
        ctx.check(probed == 0, "by the belt's own handler, before the generic wrench");
    }
    co_await ctx.ticks(6); // past the click debounces

    // a shaft has no handler of its own: the click reaches the generic wrench, which turns it
    GP shaft = at(base, 0, 0, 4);
    ctx.require(ctx.setBlock(shaft, kShaft, {{kAxis, 1}}), "shaft placed");
    {
        Block const& block   = region.getBlock(game::blockPos(shaft));
        std::string  blockId = block.getTypeName();
        ia::UseOnContext c{player, player.getSelectedItem(), wrenchId, block, blockId, shaft, 5, {1.0, .5, .5}, false, true};
        auto r = ia::dispatchUseOn(c);
        ctx.check(probed == 1, "the click passes the units' handlers");
        ctx.check(r == ia::Result::Consume, "and the generic wrench takes it");
        ctx.check(game::intState(region.getBlock(game::blockPos(shaft)), kAxis).value_or(1) != 1, "the shaft was turned");
    }
    ia::remove(probe);
    player.setSelectedItem(ItemStack());
});

LV_REGISTER_SELFTEST("hooks.place_and_next_tick", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& player = ctx.player();
    auto  p      = at(ctx.testPos(3620), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    std::vector<std::string> seen;
    auto id = ia::onPlace({.block = "minecraft:stone"}, [&](ia::PlaceContext const& c) { seen.push_back(c.blockId + "@" + c.pos.key()); });
    auto const& stone   = *game::blockWith("minecraft:stone");
    std::string blockId = "minecraft:stone", item = "minecraft:stone";
    ia::PlaceContext pc{player, item, stone, blockId, p, at(p, 0, -1), 1, {.5, 1.0, .5}, false};
    ia::dispatchPlace(pc);
    std::string dirt = "minecraft:dirt";
    ia::PlaceContext other{player, dirt, stone, dirt, p, at(p, 0, -1), 1, {.5, 1.0, .5}, false};
    ia::dispatchPlace(other);
    ia::remove(id);
    ctx.check(seen.size() == 1 && seen[0] == "minecraft:stone@" + p.key(), "place handlers see the matching block only");
    bool ran = false;
    ia::nextTick([&ran] { ran = true; });
    ctx.check(!ran, "nextTick does not run inside the call");
    co_await ctx.ticks(2);
    ctx.check(ran, "nextTick runs on a later tick");
});

// ---- comparator output ---------------------------------------------------------------------------

LV_REGISTER_SELFTEST("hooks.comparator_tank", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3640), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = regionOf(ctx);
    floorUnder(ctx, base, 3);
    ctx.require(ctx.setBlock(base, kTank), "tank placed");
    co_await ctx.ticks(4);
    auto rig = rigEast(ctx, base);
    co_await ctx.ticks(6);
    ctx.check(rs::analogOutput(r, game::blockPos(base)).value_or(-1) == 0, "an empty tank outputs 0");
    ctx.check(wireSignal(ctx, rig.wire) == 0, "the wire stays off");
    int filled = common::fluids::fill(r, game::blockPos(base), -1, {"minecraft:water", 4000}, false);
    ctx.require(filled == 4000, "4 buckets in, got " + std::to_string(filled));
    co_await ctx.ticks(10);
    auto out = rs::analogOutput(r, game::blockPos(base)).value_or(-1);
    ctx.check(out == 8, "half full: 8 (fractionToRedstoneLevel), got " + std::to_string(out));
    ctx.check(wireSignal(ctx, rig.wire) == 8, "the comparator reads 8, wire " + std::to_string(wireSignal(ctx, rig.wire)));
    (void)common::fluids::drain(r, game::blockPos(base), -1, {"", 8000}, false);
    co_await ctx.ticks(10);
    ctx.check(wireSignal(ctx, rig.wire) == 0, "drained: the comparator goes off");
    for (auto const& p : {rig.comparator, rig.wire, base}) ctx.setBlock(p, "minecraft:air");
});

LV_REGISTER_SELFTEST("hooks.comparator_vault", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3660), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = regionOf(ctx);
    floorUnder(ctx, base, 3);
    ctx.require(ctx.setBlock(base, kVault, {{kAxis, 0}}), "vault placed");
    co_await ctx.ticks(2);
    auto rig = rigEast(ctx, base);
    for (int i = 0; i < 10; ++i) (void)citems::insert(r, game::blockPos(base), -1, stackOf(ctx, "minecraft:cobblestone", 64), false);
    co_await ctx.ticks(10);
    ctx.check(wireSignal(ctx, rig.wire) == 8, "a half-full vault reads 8, wire " + std::to_string(wireSignal(ctx, rig.wire)));
    // the comparator is released when the vault goes
    (void)common::blockdata::take(base);
    ctx.setBlock(base, "minecraft:air");
    co_await ctx.ticks(10);
    ctx.check(wireSignal(ctx, rig.wire) == 0, "without the vault the comparator goes off");
    for (auto const& p : {rig.comparator, rig.wire}) ctx.setBlock(p, "minecraft:air");
});

LV_REGISTER_SELFTEST("hooks.comparator_depot", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3680), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = regionOf(ctx);
    floorUnder(ctx, base, 4);
    ctx.require(ctx.setBlock(base, kDepot), "depot placed");
    co_await ctx.ticks(2);
    // Java: a comparator also reads through a solid block
    auto rig  = rigEast(ctx, base, 1);
    auto rest = citems::insert(r, game::blockPos(base), 1, stackOf(ctx, "minecraft:cobblestone", 32), false);
    ctx.require(citems::isEmpty(rest), "32 items on the depot");
    co_await ctx.ticks(10);
    auto out = rs::analogOutput(r, game::blockPos(base)).value_or(-1);
    ctx.check(out == 8, "32 of 64: 8, got " + std::to_string(out));
    ctx.check(wireSignal(ctx, rig.wire) == 8, "read through the stone, wire " + std::to_string(wireSignal(ctx, rig.wire)));
    (void)citems::extract(r, game::blockPos(base), 1, [](ItemStack const&) { return true; }, 64, false);
    co_await ctx.ticks(10);
    ctx.check(wireSignal(ctx, rig.wire) == 0, "an empty depot: off");
    for (auto const& p : {rig.comparator, rig.wire, at(base, 1), base}) ctx.setBlock(p, "minecraft:air");
});

} // namespace lv::feature::hooks
