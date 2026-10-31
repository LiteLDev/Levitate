// Selftests of the belt / depot / ejector rules aligned with Create in U55 (testPos dx 5520..5549).

#include <algorithm>

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/service/Bedrock.h"

#include "mc/server/ServerLevel.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::belts {

namespace {

using selftest::Context;
namespace citems = common::items;

constexpr char const* kSource = "levitate:kinetics_test_source";
constexpr int         AZ      = 2;

struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy;
        return p;
    }
};

ItemStack stackOf(Context& ctx, std::string const& id, int count = 1) {
    auto s = citems::make(id, count);
    ctx.require(s.has_value(), id + " is registered");
    return *s;
}

/// A belt along +x from `a.at(0, 0, 1)` (shafts with axis z), driven by a test source north of it.
ll::coro::CoroTask<bool> makeBelt(Area& a, int len, float rpm) {
    GlobalPos s = a.at(0, 0, 1), e = a.at(len - 1, 0, 1);
    a.ctx.setBlock(s, kTestShaftId, {{kAxis, AZ}});
    a.ctx.setBlock(e, kTestShaftId, {{kAxis, AZ}});
    for (int i = 1; i < len - 1; ++i) a.ctx.cleanupBlock(a.at(i, 0, 1));
    a.ctx.setBlock(a.at(0, 0, 0), kSource, {{kAxis, AZ}});
    co_await a.ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), rpm);
    co_await a.ctx.ticks(1);
    co_return createBelt(s, e);
}

int inventoryCount(Player& p, std::string const& id) {
    auto& inv = p.getInventory();
    int   n   = 0;
    for (int i = 0; i < inv.getContainerSize(); ++i)
        if (citems::idOf(inv.getItem(i)) == id) n += citems::countOf(inv.getItem(i));
    return n;
}

void clearInventory(Player& p) {
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, ItemStack());
}

int countOn(GlobalPos const& pos) {
    int n = 0;
    for (auto const& t : itemsAt(pos)) n += citems::countOf(t.stack);
    return n;
}

void clearDepotState(GlobalPos const& pos) {
    auto& s = state();
    if (auto it = s.depots.find(pos); it != s.depots.end()) {
        for (auto& [id, sd] : it->second->stacks) removeDisplay(sd);
        for (auto& ab : it->second->airborne) removeDisplay(ab.data);
        it->second->stacks.clear();
        it->second->held.reset();
        it->second->incoming.clear();
        it->second->airborne.clear();
        it->second->outputs = {};
    }
}

int itemEntities(BlockSource& region, GlobalPos const& p, double r, std::string const& id, bool remove) {
    AABB box{Vec3{float(p.x - r), float(p.y - r), float(p.z - r)}, Vec3{float(p.x + 1 + r), float(p.y + 1 + r), float(p.z + 1 + r)}};
    int  n = 0;
    for (auto* a : region.getEntities(box, 0.f, ActorType::ItemEntity)) {
        if (!a || a->getEntityTypeId() != ActorType::ItemEntity || isDisplayActor(*a)) continue;
        auto& item = static_cast<ItemActor&>(*a);
        if (citems::idOf(item.mItem) != id) continue;
        n += citems::countOf(item.mItem);
        if (remove) a->remove();
    }
    return n;
}

void runServerCommand(std::string const& command) {
    auto level = ll::service::getLevel();
    if (!level) return;
    ServerCommandOrigin origin("levitate-selftest", level->asServer(), CommandPermissionLevel::Owner, DimensionType(0));
    ll::command::CommandRegistrar::getServerInstance().executeCommand(command, origin);
}

GlobalPos off(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

} // namespace

LV_REGISTER_SELFTEST("belts.tunnel_needs_belt", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's BeltTunnelBlock.canSurvive: a horizontal, encased belt below; BeltTunnelItem cases it
    GlobalPos base = ctx.testPos(5550, 2);
    base.y += 1;
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = *regionOf(base.dim);
    ctx.setBlock(off(base, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(base, "levitate:andesite_tunnel");
    co_await ctx.ticks(4);
    ctx.check(idAt(base) == "minecraft:air", "a tunnel without a belt below breaks");
    ctx.check(itemEntities(r, base, 2.0, "levitate:andesite_tunnel", true) == 1, "and drops itself");
    // on a real belt it stays and encases the segment
    auto s = off(base, -1, -1, 0), e = off(base, 1, -1, 0);
    ctx.setBlock(s, "levitate:kinetics_test_shaft", {{"levitate:axis", 2}});
    ctx.setBlock(e, "levitate:kinetics_test_shaft", {{"levitate:axis", 2}});
    ctx.setBlock(off(base, 0, -1, 0), "minecraft:air");
    co_await ctx.ticks(2);
    ctx.require(createBelt(s, e), "belt built");
    co_await ctx.ticks(2);
    ctx.setBlock(base, "levitate:brass_tunnel");
    co_await ctx.ticks(4);
    ctx.check(idAt(base) == "levitate:brass_tunnel", "a tunnel on a belt stays");
    auto info = beltAt(off(base, 0, -1, 0));
    ctx.check(info && info->casing == 2, "the segment took the brass casing");
    ctx.setBlock(base, "minecraft:air");
});

LV_REGISTER_SELFTEST("belts.split_takes_connector", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's BeltSlicer: a survival split takes the shafts and one belt connector
    Area a{ctx, 5520};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 6, -32), "belt built");
    auto& p = ctx.spawnPlayer("lv_u55_split");
    p.setPlayerGameType(GameType::Survival);
    co_await ctx.ticks(1);
    clearInventory(p);
    ctx.give(p, stackOf(ctx, kTestShaftId, 2));
    p.setSelectedItem(ItemStack());
    onBeltWrench(p, a.at(2, 0, 1), 1, {.8, .9, .5});
    ctx.check(segmentInfo(a.at(0, 0, 1))->length == 6, "no split without a belt connector");
    ctx.check(inventoryCount(p, kTestShaftId) == 2, "nothing was taken");
    ctx.give(p, stackOf(ctx, kBeltConnectorId, 1));
    ctx.check(onBeltWrench(p, a.at(2, 0, 1), 1, {.8, .9, .5}), "the wrench splits");
    auto l = segmentInfo(a.at(0, 0, 1)), r = segmentInfo(a.at(5, 0, 1));
    ctx.check(l && r && l->length == 3 && r->length == 3, "split into 3 + 3");
    ctx.check(inventoryCount(p, kBeltConnectorId) == 0 && inventoryCount(p, kTestShaftId) == 0, "the split took 2 shafts and a connector");
});

LV_REGISTER_SELFTEST("belts.connector_keeps_selection", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's BeltConnectorItem.useOn: a failed connection keeps the first shaft selected
    Area a{ctx, 5527};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.setBlock(a.at(0, 0, 1), kTestShaftId, {{kAxis, AZ}});
    ctx.setBlock(a.at(3, 1, 1), kTestShaftId, {{kAxis, AZ}}); // not straight, not 45 degrees
    ctx.setBlock(a.at(3, 0, 1), kTestShaftId, {{kAxis, AZ}});
    co_await ctx.ticks(2);
    auto& p = ctx.player();
    p.setSelectedItem(stackOf(ctx, kBeltConnectorId, 4));
    onConnectorUse(p, a.at(0, 0, 1), 1, {.5, .5, .5});
    onConnectorUse(p, a.at(3, 1, 1), 1, {.5, .5, .5});
    ctx.check(state().connector.contains(uuidOf(p)), "the first shaft stays selected after a failed connection");
    ctx.check(onConnectorUse(p, a.at(3, 0, 1), 1, {.5, .5, .5}) && segmentInfo(a.at(0, 0, 1)).has_value(), "the next valid shaft connects");
    ctx.check(!state().connector.contains(uuidOf(p)), "a belt made clears the selection");
    p.setSelectedItem(ItemStack());
});

LV_REGISTER_SELFTEST("belts.depot_rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 5530};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    GlobalPos d = a.at(0, 0, 1), e = a.at(2, 0, 1);
    ctx.setBlock(d, kDepotId);
    ctx.setBlock(e, kEjectorId, {{kFacing, 3}});
    co_await ctx.ticks(2);
    auto& p      = ctx.player();
    auto& region = *regionOf(d.dim);
    // SharedDepotBlockMethods.onUse: only the top face
    p.setSelectedItem(stackOf(ctx, "minecraft:bread", 3));
    ctx.check(!onDepotUse(p, d, 2, {.5, .5, 0}), "a click on the side is not the depot's");
    ctx.check(countOn(d) == 0 && citems::countOf(p.getSelectedItem()) == 3, "nothing was put on it");
    p.setSelectedItem(ItemStack());
    // DepotBehaviour.getRemainingSpace: an ejector set to 32 takes at most 16 ender pearls
    auto* ed = depotFor(e);
    ctx.require(ed != nullptr, "ejector runtime");
    ed->stackSize = 32;
    auto rest = common::items::insert(region, game::blockPos(e), 1, stackOf(ctx, "minecraft:ender_pearl", 16), false);
    ctx.check(citems::isEmpty(rest), "16 ender pearls fit");
    rest = common::items::insert(region, game::blockPos(e), 1, stackOf(ctx, "minecraft:ender_pearl", 4), false);
    ctx.check(citems::countOf(rest) == 4, "no more than the pearl's max stack size");
    clearDepotState(e);
    // depot comparator: 16 pearls are a full depot (DepotBehaviour.maxStackSize = the held stack's)
    (void)common::items::insert(region, game::blockPos(d), 1, stackOf(ctx, "minecraft:ender_pearl", 16), false);
    co_await ctx.ticks(2);
    auto sig = common::redstone::analogOutput(region, game::blockPos(d));
    ctx.check(sig && *sig == 15, "16 ender pearls give a comparator signal of 15: " + std::to_string(sig.value_or(-1)));
    clearDepotState(d);
});

LV_REGISTER_SELFTEST("belts.ejector_outputs", [](Context& ctx) -> ll::coro::CoroTask<> {
    // EjectorBlockEntity.launchItems: the output slots fly too and count for the trigger
    Area a{ctx, 5535};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    GlobalPos e = a.at(0, 0, 1), target = a.at(3, 0, 1);
    ctx.setBlock(e, kEjectorId, {{kFacing, 3}});
    ctx.setBlock(target, kDepotId);
    ctx.setBlock(a.at(0, 0, 0), kSource, {{kAxis, AZ}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), 128);
    co_await ctx.ticks(2);
    auto* d = depotFor(e);
    ctx.require(d != nullptr, "ejector runtime");
    d->targetH   = 3;
    d->targetV   = 0;
    d->outputs[0] = stackOf(ctx, "minecraft:flint", 5);
    bool arrived  = false;
    for (int i = 0; i < 100 && !arrived; ++i) {
        co_await ctx.ticks(1);
        arrived = countOn(target) == 5;
    }
    ctx.check(arrived, "the output slot was thrown onto the target");
    ctx.check(citems::isEmpty(depotFor(e)->outputs[0]), "the outputs are empty");
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), 0);
    clearDepotState(e);
    clearDepotState(target);
});

LV_REGISTER_SELFTEST("belts.ejector_hits_walls", [](Context& ctx) -> ll::coro::CoroTask<> {
    // EjectorBlockEntity.scanTrajectoryForObstacles: a wall in the arc ends the flight early
    Area a{ctx, 5540};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    GlobalPos e = a.at(0, 0, 1);
    ctx.setBlock(e, kEjectorId, {{kFacing, 3}});
    for (int y = 0; y < 6; ++y) ctx.setBlock(a.at(3, y, 1), "minecraft:stone");
    ctx.setBlock(a.at(0, 0, 0), kSource, {{kAxis, AZ}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), 128);
    co_await ctx.ticks(2);
    auto* d = depotFor(e);
    ctx.require(d != nullptr, "ejector runtime");
    d->targetH = 6;
    d->targetV = 0;
    auto& region = *regionOf(e.dim);
    (void)common::items::insert(region, game::blockPos(e), 1, stackOf(ctx, "minecraft:clay_ball", 2), false);
    co_await ctx.ticks(60);
    int before = itemEntities(region, a.at(1, 0, 1), 1.5, "minecraft:clay_ball", true);
    int beyond = itemEntities(region, a.at(6, 0, 1), 1.5, "minecraft:clay_ball", true);
    ctx.check(before == 2 && beyond == 0, "the stack dropped in front of the wall: " + std::to_string(before) + " / " + std::to_string(beyond));
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), 0);
    for (int y = 0; y < 6; ++y) ctx.setBlock(a.at(3, y, 1), "minecraft:air");
    clearDepotState(e);
});

LV_REGISTER_SELFTEST("belts.end_needs_full_face", [](Context& ctx) -> ll::coro::CoroTask<> {
    // BeltInventory.resolveEnding: only a full face stops the items; they fall off past a slab
    Area a{ctx, 5545};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 3, -64), "belt built");
    auto& region = *regionOf(a.at(0).dim);
    auto  info   = beltAt(a.at(0, 0, 1));
    ctx.require(info.has_value(), "belt info");
    int  dir     = info->movementFace == 5 ? 1 : -1; // east: past the last segment
    auto after   = dir > 0 ? a.at(3, 0, 1) : a.at(-1, 0, 1);
    ctx.setBlock(after, "minecraft:smooth_stone_slab");
    co_await ctx.ticks(1);
    (void)common::items::insert(region, game::blockPos(a.at(1, 0, 1)), 1, stackOf(ctx, "minecraft:brick", 3), false);
    co_await ctx.ticks(60);
    ctx.check(countOn(a.at(dir > 0 ? 2 : 0, 0, 1)) == 0, "the items fell off past the slab");
    itemEntities(region, after, 2.0, "minecraft:brick", true);
    ctx.setBlock(after, "minecraft:stone");
    co_await ctx.ticks(1);
    (void)common::items::insert(region, game::blockPos(a.at(1, 0, 1)), 1, stackOf(ctx, "minecraft:brick", 3), false);
    co_await ctx.ticks(60);
    ctx.check(countOn(a.at(dir > 0 ? 2 : 0, 0, 1)) == 3, "a full block stops them");
    ctx.setBlock(after, "minecraft:air");
});

LV_REGISTER_SELFTEST("belts.setblock_destroy_drops", [](Context& ctx) -> ll::coro::CoroTask<> {
    // `/setblock ... destroy` breaks a belt segment like Create's destroyBlock(pos, true): its loot drops
    Area a{ctx, 5548};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 3, 0), "belt built");
    auto& region = *regionOf(a.at(0).dim);
    auto  mid    = a.at(1, 0, 1);
    itemEntities(region, mid, 2.0, kBeltConnectorId, true);
    runServerCommand("setblock " + std::to_string(mid.x) + " " + std::to_string(mid.y) + " " + std::to_string(mid.z) + " air destroy");
    co_await ctx.ticks(3);
    ctx.check(itemEntities(region, mid, 2.0, kBeltConnectorId, true) == 1, "the belt connector dropped");
});

} // namespace lv::feature::belts
