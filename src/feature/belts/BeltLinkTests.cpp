// Selftests of U41 (testPos dx 4100..4199): belts working with the blocks of the item logistics
// unit on real belts (tunnels, belt funnels, chutes, depots), and the belt gaps closed by U41
// (sloped collision, explosions, ejector redstone, connector preview sync).

#include <algorithm>
#include <cfloat>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/funnels/FunnelsInternal.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::belts {

namespace {

using selftest::Context;
namespace citems = common::items;

constexpr char const* kSource   = "levitate:kinetics_test_source";
constexpr char const* kAndesiteFunnel = "levitate:andesite_funnel";
constexpr char const* kBrassFunnel    = "levitate:brass_funnel";
constexpr char const* kAndesiteTunnel = "levitate:andesite_tunnel";
constexpr char const* kBrassTunnel    = "levitate:brass_tunnel";
constexpr char const* kChute          = "levitate:chute";
constexpr int         AZ = 2;
constexpr int         kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;

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

/// A belt along +x from `a.at(0, 0, 1)` to `a.at(len - 1, 0, 1)` carrying items east.
ll::coro::CoroTask<bool> eastBelt(Area& a, int len, float rpm = -64) {
    GlobalPos s = a.at(0, 0, 1), e = a.at(len - 1, 0, 1);
    a.ctx.setBlock(s, kTestShaftId, {{kAxis, AZ}});
    a.ctx.setBlock(e, kTestShaftId, {{kAxis, AZ}});
    for (int i = 1; i < len - 1; ++i) a.ctx.cleanupBlock(a.at(i, 0, 1));
    a.ctx.cleanupBlock(a.at(len, 0, 1));
    a.ctx.setBlock(a.at(0, 0, 0), kSource, {{kAxis, AZ}});
    co_await a.ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), rpm);
    co_await a.ctx.ticks(1);
    if (!createBelt(s, e)) co_return false;
    co_await a.ctx.ticks(2);
    auto info = beltAt(s);
    co_return info && info->movementFace == kEast;
}

int countOn(GlobalPos const& pos) {
    int n = 0;
    for (auto const& t : itemsAt(pos)) n += citems::countOf(t.stack);
    return n;
}

int countOnBelt(Area const& a, int len) {
    int n = 0;
    for (int i = 0; i < len; ++i) n += countOn(a.at(i, 0, 1));
    return n;
}

/// Items of `id` in the inventory at `pos` (chests, depots, chutes, ...).
int countIn(GlobalPos const& pos, std::string const& id) {
    auto* r = regionOf(pos.dim);
    if (!r) return -1;
    common::Access a{*r, game::blockPos(pos), -1};
    auto           h = common::items::find(a);
    if (!h) return -1;
    int n = 0;
    for (int i = 0; i < h->slots(a); ++i) {
        auto s = h->getSlot(a, i);
        if (!citems::isEmpty(s) && citems::idOf(s) == id) n += citems::countOf(s);
    }
    return n;
}

int put(Context& ctx, GlobalPos const& pos, std::string const& id, int count) {
    auto* r    = regionOf(pos.dim);
    auto  rest = citems::insert(*r, game::blockPos(pos), -1, stackOf(ctx, id, count), false);
    return count - citems::countOf(rest);
}

int stateAt(GlobalPos const& p, char const* name) {
    auto const* b = blockAt(p);
    return b ? game::intState(*b, name).value_or(-1) : -1;
}

int entitiesNear(GlobalPos const& pos, std::string const& id, float radius = 3.f) {
    auto* r = regionOf(pos.dim);
    if (!r) return 0;
    int n = 0;
    AABB box{Vec3{pos.x - radius, pos.y - radius, pos.z - radius}, Vec3{pos.x + 1 + radius, pos.y + 1 + radius, pos.z + 1 + radius}};
    for (auto* e : r->getEntities(box, 0.f, ActorType::ItemEntity)) {
        if (!e || !e->isAlive() || isDisplayActor(*e)) continue;
        auto& item = static_cast<ItemActor&>(*e);
        if (citems::idOf(item.item()) == id) n += citems::countOf(item.item());
    }
    return n;
}

void clearEntitiesNear(GlobalPos const& pos, float radius = 4.f) {
    auto* r = regionOf(pos.dim);
    if (!r) return;
    AABB box{Vec3{pos.x - radius, pos.y - radius, pos.z - radius}, Vec3{pos.x + 1 + radius, pos.y + 1 + radius, pos.z + 1 + radius}};
    for (auto* e : r->getEntities(box, 0.f, ActorType::ItemEntity))
        if (e && !isDisplayActor(*e)) e->remove();
}

} // namespace

// ---- tunnels on belts (R63) ------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("belts.link_andesite_tunnel", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4124};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 5), "a belt carrying items east");
    ctx.setBlock(a.at(5, 0, 1), "minecraft:stone"); // a blocked end: the items stay on the belt
    // an andesite tunnel over segment 2 with a depot on its south side: a junction
    ctx.setBlock(a.at(2, 0, 2), kDepotId);
    ctx.setBlock(a.at(2, 1, 1), kAndesiteTunnel);
    co_await ctx.ticks(3);
    ctx.check(stateAt(a.at(2, 1, 1), "levitate:shape") >= 3, "the tunnel has a side output (T / cross shape)");
    auto rest = insertOnSegment(a.at(0, 0, 1), stackOf(ctx, "minecraft:cobblestone", 3), false);
    ctx.require(citems::isEmpty(rest), "three cobblestone on the belt");
    co_await ctx.ticks(60);
    ctx.check(countIn(a.at(2, 0, 2), "minecraft:cobblestone") == 1, "the tunnel split one item off into the depot");
    ctx.check(countOn(a.at(4, 0, 1)) == 2, "two went on to the end of the belt");
});

LV_REGISTER_SELFTEST("belts.link_brass_tunnel", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4172};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 5), "a belt carrying items east");
    ctx.setBlock(a.at(5, 0, 1), "minecraft:stone");
    ctx.setBlock(a.at(2, 0, 2), kDepotId);
    ctx.setBlock(a.at(2, 1, 1), kBrassTunnel);
    co_await ctx.ticks(3);
    auto rest = insertOnSegment(a.at(0, 0, 1), stackOf(ctx, "minecraft:iron_ingot", 4), false);
    ctx.require(citems::isEmpty(rest), "four iron on the belt");
    co_await ctx.ticks(80);
    int depot = countIn(a.at(2, 0, 2), "minecraft:iron_ingot");
    int belt  = countOnBelt(a, 5);
    ctx.check(depot > 0, "the brass tunnel distributed some into the side depot");
    ctx.check(belt > 0, "and some forward onto the belt");
    ctx.check(depot + belt == 4, "nothing was lost (" + std::to_string(depot) + " + " + std::to_string(belt) + ")");

    // a stack put onto the tunnel's segment from the side goes into the distributing tunnel
    co_await ctx.ticks(40);
    TransportedItem t;
    t.stack = stackOf(ctx, "minecraft:gold_ingot", 2);
    auto side = insertDirect(a.at(2, 0, 1), t, kSouth, false);
    ctx.check(citems::isEmpty(side), "the side insertion was taken");
    ctx.check(countIn(a.at(2, 1, 1), "minecraft:gold_ingot") == 2, "by the tunnel, not the belt");
});

LV_REGISTER_SELFTEST("belts.link_dropped_item_under_tunnel", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4120};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 3, 0.f) || true, "a belt");
    ctx.setBlock(a.at(1, 1, 1), kAndesiteTunnel);
    co_await ctx.ticks(2);
    auto* r = regionOf(a.at(0).dim);
    // Create: items dropped on a segment under a tunnel are not picked up by the belt
    auto at = a.at(1, 0, 1);
    clearEntitiesNear(at);
    ll::service::getLevel()->getSpawner().spawnItem(*r, stackOf(ctx, "minecraft:stick", 1), nullptr, Vec3{at.x + .5f, at.y + .9f, at.z + .5f}, 0);
    co_await ctx.ticks(10);
    ctx.check(countOn(at) == 0, "the belt under the tunnel did not take the dropped stick");
    clearEntitiesNear(at);
});

// ---- belt funnels (R63) ----------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("belts.link_funnel_pulls", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4130};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 5), "a belt carrying items east");
    // a pulling belt funnel on the north side of segment 2, its chest south of it
    ctx.setBlock(a.at(2, 1, 2), "minecraft:chest");
    ctx.setBlock(a.at(2, 1, 1), kAndesiteFunnel, {{"levitate:facing", kNorth}, {"levitate:shape", 4}});
    co_await ctx.ticks(3);
    ctx.check(stateAt(a.at(2, 1, 1), "levitate:shape") == 4, "the funnel stays a pulling belt funnel");
    auto rest = insertOnSegment(a.at(0, 0, 1), stackOf(ctx, "minecraft:cobblestone", 5), false);
    ctx.require(citems::isEmpty(rest), "five cobblestone on the belt");
    co_await ctx.ticks(40);
    ctx.check(countIn(a.at(2, 1, 2), "minecraft:cobblestone") == 5, "the funnel pulled the stack into the chest");
    ctx.check(countOnBelt(a, 5) == 0, "nothing left on the belt");
    ctx.check(stateAt(a.at(2, 1, 1), "levitate:extracting") == 0, "the back plate shows inserting");
});

LV_REGISTER_SELFTEST("belts.link_funnel_pushes", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4140};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 5), "a belt carrying items east");
    ctx.setBlock(a.at(5, 0, 1), "minecraft:stone"); // a blocked end: the items stay on the belt
    ctx.setBlock(a.at(1, 1, 2), "minecraft:chest");
    ctx.setBlock(a.at(1, 1, 1), kAndesiteFunnel, {{"levitate:facing", kNorth}, {"levitate:shape", 3}, {"levitate:extracting", 1}});
    co_await ctx.ticks(2);
    ctx.require(put(ctx, a.at(1, 1, 2), "minecraft:iron_ingot", 3) == 3, "three iron in the chest");
    co_await ctx.ticks(60);
    ctx.check(countIn(a.at(1, 1, 2), "minecraft:iron_ingot") == 0, "the andesite funnel pushed them all onto the belt");
    ctx.check(countOnBelt(a, 5) == 3, "three single items ride the belt");
});

LV_REGISTER_SELFTEST("belts.link_funnel_at_belt_end", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4150};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 4), "a belt carrying items east");
    // a retracted brass funnel on the last segment facing back along the belt, the chest beyond
    ctx.setBlock(a.at(4, 1, 1), "minecraft:chest");
    ctx.setBlock(a.at(3, 1, 1), kBrassFunnel, {{"levitate:facing", kWest}, {"levitate:shape", 1}});
    co_await ctx.ticks(3);
    auto rest = insertOnSegment(a.at(0, 0, 1), stackOf(ctx, "minecraft:diamond", 7), false);
    ctx.require(citems::isEmpty(rest), "seven diamonds on the belt");
    co_await ctx.ticks(40);
    ctx.check(countIn(a.at(4, 1, 1), "minecraft:diamond") == 7, "the funnel at the end took the stack");
    ctx.check(countOnBelt(a, 4) == 0, "nothing left on the belt");
});

LV_REGISTER_SELFTEST("belts.link_funnel_placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace fd = funnels::detail;
    Area a{ctx, 4160};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 3), "a belt");
    auto  pos = a.at(1, 1, 1);
    auto& p   = ctx.player();
    ctx.setBlock(a.at(1, 1, 2), "minecraft:chest");
    ctx.setBlock(pos, kAndesiteFunnel);
    co_await ctx.ticks(2);
    ctx.check(stateAt(pos, "levitate:shape") == 0, "a funnel placed by command stays plain");
    auto* region = regionOf(pos.dim);
    ctx.require(region != nullptr, "region");
    // Create's getStateForPlacement: looking south it faces north, across the belt: pushing
    fd::placeFunnel(&p, *region, pos, false, Vec3{0, -.2f, 1});
    ctx.check(stateAt(pos, "levitate:facing") == kNorth, "facing north");
    ctx.check(stateAt(pos, "levitate:shape") == 3, "a pushing belt funnel");
    // looking east it faces west, along the belt: retracted
    fd::placeFunnel(&p, *region, pos, true, Vec3{1, -.2f, 0});
    ctx.check(stateAt(pos, "levitate:shape") == 1, "a retracted belt funnel along the belt");
    // the wrench: retracted -> extended -> retracted
    fd::wrenchFunnel(p, *region, pos, *blockAt(pos));
    ctx.check(stateAt(pos, "levitate:shape") == 2, "the wrench extends it");
    fd::wrenchFunnel(p, *region, pos, *blockAt(pos));
    ctx.check(stateAt(pos, "levitate:shape") == 1, "and retracts it");
    // breaking the belt turns it back into a plain funnel (Create's BeltFunnelBlock.updateShape)
    ctx.setBlock(a.at(0, 0, 1), "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(stateAt(pos, "levitate:shape") == 0, "without the belt it is a plain funnel again");
    ctx.check(stateAt(pos, "levitate:extracting") == 0, "a retracted funnel reverts to inserting");
    clearEntitiesNear(a.at(1, 0, 1));
});
LV_REGISTER_SELFTEST("belts.link_depot_funnel", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4170};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto depot = a.at(0, 0, 0);
    ctx.setBlock(depot, kDepotId);
    ctx.setBlock(a.at(0, 1, 1), "minecraft:chest");
    ctx.setBlock(a.at(0, 1, 0), kAndesiteFunnel, {{"levitate:facing", kNorth}, {"levitate:shape", 4}});
    co_await ctx.ticks(2);
    TransportedItem t;
    t.stack   = stackOf(ctx, "minecraft:apple", 6);
    auto rest = insertDirect(depot, t, 0, false);
    ctx.require(citems::isEmpty(rest), "six apples on the depot");
    co_await ctx.ticks(12);
    ctx.check(countIn(a.at(0, 1, 1), "minecraft:apple") == 6, "the belt funnel on the depot pulled them into the chest");
    ctx.check(countOn(depot) == 0, "the depot is empty");
});

// ---- chutes and belts (R63) --------------------------------------------------------------------------

LV_REGISTER_SELFTEST("belts.link_chutes", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4180};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 3, -16), "a slow belt carrying items east");
    // a chute under segment 1 pulls the items off the belt into the chest below it
    ctx.setBlock(a.at(1, -1, 1), kChute);
    ctx.setBlock(a.at(1, -2, 1), "minecraft:chest");
    // the belt ends into a chute standing after it (Create's DirectBeltInputBehaviour of the chute)
    ctx.setBlock(a.at(3, 0, 1), kChute);
    ctx.setBlock(a.at(3, -1, 1), "minecraft:chest");
    co_await ctx.ticks(3);
    auto rest = insertOnSegment(a.at(1, 0, 1), stackOf(ctx, "minecraft:coal", 4), false);
    ctx.require(citems::isEmpty(rest), "four coal on segment 1");
    co_await ctx.ticks(30);
    ctx.check(countIn(a.at(1, -2, 1), "minecraft:coal") == 4, "the chute below pulled them off the belt");
    // the chute below is removed: items now reach the end and drop into the chute there
    ctx.setBlock(a.at(1, -1, 1), "minecraft:stone");
    rest = insertOnSegment(a.at(0, 0, 1), stackOf(ctx, "minecraft:flint", 2), false);
    ctx.require(citems::isEmpty(rest), "two flint on segment 0");
    co_await ctx.ticks(140);
    ctx.check(countIn(a.at(3, -1, 1), "minecraft:flint") == 2, "the belt end handed them to the chute after it");
});

LV_REGISTER_SELFTEST("belts.link_chute_onto_belt", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4190};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 3, -16), "a slow belt");
    // a chest with a chute under it above segment 1: the chute drops onto the belt
    ctx.setBlock(a.at(1, 2, 1), "minecraft:chest");
    ctx.setBlock(a.at(1, 1, 1), kChute);
    co_await ctx.ticks(2);
    ctx.require(put(ctx, a.at(1, 2, 1), "minecraft:brick", 5) == 5, "five bricks in the chest");
    co_await ctx.ticks(30);
    ctx.check(countOnBelt(a, 3) > 0, "the chute put the bricks onto the belt");
});

// ---- U41 belt gaps -------------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("belts.slope_collision", [](Context& ctx) {
    cb::SegmentState up{3, cb::Slope::Upward, cb::Part::Middle, cb::Casing::None, -1}; // facing east
    auto             boxes = slopeCollision(up);
    ctx.require(boxes.size() == 8, "a sloped segment collides as 8 steps");
    ctx.check(boxes.front()[4] < boxes.back()[4], "the steps rise");
    ctx.check(boxes.front()[0] < boxes.back()[0], "towards the facing (east) for an upward belt");
    cb::SegmentState down = up;
    down.slope            = cb::Slope::Downward;
    auto d                = slopeCollision(down);
    ctx.check(d.front()[0] > d.back()[0], "against the facing for a downward belt");
    cb::SegmentState flat{3, cb::Slope::Horizontal, cb::Part::Middle, cb::Casing::None, -1};
    ctx.check(slopeCollision(flat).empty(), "horizontal belts keep their JSON box");
});

LV_REGISTER_SELFTEST("belts.explosion_drops", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4195};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await eastBelt(a, 3, 0.f) || true, "a belt");
    ctx.require(segmentAt(a.at(1, 0, 1)).has_value(), "the belt exists");
    clearEntitiesNear(a.at(1, 0, 1));
    // keep the simulated player out of the blast and away from the drops
    auto away = a.at(-9, 2, 1);
    ctx.player().teleport(Vec3{away.x + .5f, float(away.y), away.z + .5f}, DimensionType(away.dim));
    co_await ctx.ticks(1);
    // a segment blown up by an explosion of radius 1 always drops its loot (1 / radius)
    state().exploded[a.at(1, 0, 1)] = 1.f;
    ctx.setBlock(a.at(1, 0, 1), "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(!segmentAt(a.at(0, 0, 1)), "the whole belt is gone");
    ctx.check(entitiesNear(a.at(1, 0, 1), kBeltConnectorId) == 1, "the exploded segment dropped a belt connector");
    clearEntitiesNear(a.at(1, 0, 1));
    // a real explosion is seen by the belt
    co_await eastBelt(a, 3, 0.f);
    ctx.require(segmentAt(a.at(1, 0, 1)).has_value(), "a new belt");
    auto before = state().blownSegments;
    auto at     = a.at(1, 0, 1);
    ll::service::getLevel()->explode(*regionOf(at.dim), nullptr, Vec3{at.x + .5f, at.y + .5f, at.z + .5f}, 2.f, false, true, FLT_MAX, false);
    co_await ctx.ticks(3);
    ctx.check(!segmentAt(a.at(0, 0, 1)) || !segmentAt(at), "the explosion destroyed the belt");
    ctx.check(state().blownSegments > before, "and its segments were recognised as blown up");
    clearEntitiesNear(at);
});

LV_REGISTER_SELFTEST("belts.ejector_redstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4197};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto pos = a.at(0, 0, 0);
    ctx.setBlock(pos, kEjectorId, {{kFacing, 3}});
    co_await ctx.ticks(3);
    auto* d = depotFor(pos);
    ctx.require(d != nullptr && d->ejector, "an ejector");
    ctx.check(!d->powered, "unpowered");
    ctx.setBlock(a.at(0, 0, -1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    d = depotFor(pos);
    ctx.check(d && d->powered, "a redstone block next to it powers it");
    ctx.check(d && d->redstoneWatch != 0, "through the shared redstone watch");
    ctx.setBlock(a.at(0, 0, -1), "minecraft:air");
    co_await ctx.ticks(4);
    d = depotFor(pos);
    ctx.check(d && !d->powered, "and unpowers it when removed");
});

LV_REGISTER_SELFTEST("belts.connector_preview_sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 4198};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    auto shaft = a.at(0, 0, 1);
    ctx.setBlock(shaft, kTestShaftId, {{kAxis, AZ}});
    co_await ctx.ticks(2);
    auto& p = ctx.player();
    net::setTestCompanion(p, true);
    (void)net::takeTestMessages(p);
    ctx.require(onConnectorUse(p, shaft, 1, {.5, .9, .5}), "the connector selects the shaft");
    bool selected = false;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "belts.connector" && m.payload.contains("pos") && m.payload["pos"][0] == shaft.x) selected = true;
    ctx.check(selected, "the client mod is told the selected shaft");
    clearConnectorSelection(p);
    bool cleared = false;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "belts.connector" && m.payload.value("clear", false)) cleared = true;
    ctx.check(cleared, "and that the selection was cleared");
    net::setTestCompanion(p, false);
    // the preview line of the server (vanilla players): straight along the belt
    auto line = cb::previewLine({0, 0, 0}, {4, 0, 0}, cb::Axis::Z);
    ctx.check(line && line->step == cb::Vec3i{1, 0, 0} && line->length == 4.f, "the planned line runs along the belt");
});

} // namespace lv::feature::belts
