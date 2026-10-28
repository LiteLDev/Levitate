// Selftests of the tracks unit (U5, testPos dx 500..599).

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "client/tracks/TrackRenderer.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
namespace tr = lv::feature::tracks;
namespace ct = lv::core::tracks;

ct::BlockPos3 cpos(lv::core::GlobalPos const& p) { return {p.x, p.y, p.z}; }
lv::core::GlobalPos gpos(ct::BlockPos3 const& p) { return {0, p.x, p.y, p.z}; }

std::optional<ct::Shape> shapeAtPos(ct::BlockPos3 const& p) {
    auto* region = tr::regionOf(0);
    return region ? tr::shapeAt(*region, tr::toGame(p)) : std::nullopt;
}

bool isFakeAt(ct::BlockPos3 const& p) {
    auto* region = tr::regionOf(0);
    return region && tr::isFakeTrack(region->getBlock(tr::toGame(p)));
}

/// Clears everything in the box (curves without drops, blocks to air) so tests leave nothing.
void clearArea(ct::BlockPos3 const& min, ct::BlockPos3 const& max) {
    auto& s      = tr::state();
    auto* region = tr::regionOf(0);
    auto const* air = lv::game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (auto const& [dim, c] : s.curves.all()) {
        if (dim != 0) continue;
        auto in = [&](ct::BlockPos3 const& p) {
            return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
        };
        if (in(c->block(0)) || in(c->block(1))) tr::destroyCurve(0, *c, false, nullptr);
    }
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) {
                BlockPos bp{x, y, z};
                if (region->getBlock(bp).isAir()) continue;
                s.noDrops.insert({0, {x, y, z}});
                lv::game::setBlock(*region, bp, *air);
            }
}

std::vector<std::pair<std::string, int>> states(ct::Shape shape, bool turn = false) {
    int s = int(shape);
    return {{std::string(tr::kShapeState), s % 16}, {std::string(tr::kShapeHiState), s / 16}, {std::string(tr::kTurnState), turn ? 1 : 0}};
}

ItemStack tracks(int n) {
    ItemStack s;
    s.reinit(tr::kTrackId, n, 0);
    return s;
}

int countTracks(Player& p) {
    int  n   = 0;
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& it = inv.getItem(i);
        if (!it.isNull() && it.getTypeName() == tr::kTrackId) n += it.mCount;
    }
    return n;
}

/// Turns the player towards `at` (applied on the next tick).
void lookAt(SimulatedPlayer& sp, ct::Vec3d const& at) {
    sp.simulateLookAt(Vec3{float(at.x), float(at.y), float(at.z)}, sim::LookDuration::Continuous);
}

/// Right-clicks the top face of `pos` with the item in slot 0.
bool useOn(SimulatedPlayer& sp, ct::BlockPos3 const& pos) {
    return sp.simulateUseItemInSlotOnBlock(0, tr::toGame(pos), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.0f, 0.5f});
}

void stand(SimulatedPlayer& sp, ct::BlockPos3 const& at) {
    sp.teleport(Vec3{at.x + .5f, at.y + .0f, at.z + .5f}, DimensionType(0));
}

} // namespace

LV_REGISTER_SELFTEST("tracks.block_states", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = ctx.testPos(500);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(lv::game::blockWith(tr::kTrackId) != nullptr, "levitate:track is registered");
    ctx.require(lv::game::blockWith(tr::kFakeTrackId) != nullptr, "levitate:fake_track is registered");
    for (int shape : {int(ct::Shape::ZO), int(ct::Shape::AE), int(ct::Shape::CR_NDZ)}) {
        ctx.require(ctx.setBlock(pos, std::string(tr::kTrackId), states(ct::Shape(shape), true)), "set a track");
        co_await ctx.ticks(1);
        auto read = shapeAtPos(cpos(pos));
        ctx.check(read && int(*read) == shape, "shape " + std::to_string(shape) + " reads back");
    }
    clearArea(cpos(pos), cpos(pos));
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("tracks.placement_shape", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(505);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.give(sp, tracks(16), 0);
    ct::BlockPos3 b   = cpos(base);
    ct::BlockPos3 min = b.offset(-1, -1, -2), max = b.offset(8, 2, 2);
    stand(sp, b.offset(-1, 0, 0));
    co_await ctx.ticks(2);
    // looking east along the ground: an east-west track
    lookAt(sp, b.offset(8, 0, 0).center());
    co_await ctx.ticks(2);
    bool used = useOn(sp, b.below());
    co_await ctx.ticks(3);
    auto first = shapeAtPos(b);
    ctx.check(used, "the track item can be used");
    ctx.check(first && *first == ct::Shape::XO, "track placed looking east is XO");
    // a solid block to the east with air above: ascending east
    ctx.require(ctx.setBlock(gpos(b.offset(4, 0, 0)), "minecraft:stone"), "stone");
    stand(sp, b.offset(1, 0, 1));
    lookAt(sp, b.offset(8, 0, 1).center());
    co_await ctx.ticks(10);
    bool used2 = useOn(sp, b.offset(3, -1, 0));
    co_await ctx.ticks(3);
    if (!used2) ctx.info("second use returned false");
    auto asc = shapeAtPos(b.offset(3, 0, 0));
    ctx.check(asc && *asc == ct::Shape::AE, "track next to a block ascends towards it");
    clearArea(min, max);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("tracks.straight_graph", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(515);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ct::BlockPos3 b = cpos(base);
    for (int i = 0; i < 8; ++i) ctx.setBlock(gpos(b.offset(i, 0, 0)), std::string(tr::kTrackId), states(ct::Shape::XO));
    co_await ctx.ticks(2);
    auto at = tr::locate(0, Vec3{b.x + 3.5f, float(b.y), b.z + .5f}, 1);
    ctx.require(at.has_value(), "the line is on a graph");
    auto* g = tr::railways().graph(at->graph);
    ctx.require(g != nullptr, "graph exists");
    auto const* e = g->edge(at->node1, at->node2);
    ctx.require(e != nullptr, "edge exists");
    ctx.check(std::abs(g->edgeLength(*e) - 8) < 1e-6, "edge length is 8 blocks");

    // break the middle: two graphs
    clearArea(b.offset(4, 0, 0), b.offset(4, 0, 0));
    co_await ctx.ticks(2);
    auto left  = tr::locate(0, Vec3{b.x + 1.5f, float(b.y), b.z + .5f}, 1);
    auto right = tr::locate(0, Vec3{b.x + 6.5f, float(b.y), b.z + .5f}, 1);
    ctx.check(left && right && left->graph != right->graph, "breaking the middle splits the graph");
    clearArea(b.offset(-1, 0, 0), b.offset(9, 0, 0));
    co_await ctx.ticks(2);
    ctx.check(!tr::locate(0, Vec3{b.x + 1.5f, float(b.y), b.z + .5f}, 1), "no graph left after clearing");
});

LV_REGISTER_SELFTEST("tracks.connect_straight", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(530);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.give(sp, tracks(32), 0);
    ct::BlockPos3 a = cpos(base);
    ctx.setBlock(gpos(a), std::string(tr::kTrackId), states(ct::Shape::XO));
    stand(sp, a.offset(-2, 0, 0));
    co_await ctx.ticks(2);
    int before = countTracks(sp);
    lookAt(sp, a.offset(6, 0, 0).center());
    co_await ctx.ticks(2);
    useOn(sp, a); // select, looking east
    co_await ctx.ticks(6);
    ctx.check(tr::state().selections.contains(sp.getUuid().asString()), "first click selects the track");
    stand(sp, a.offset(6, 0, 0));
    co_await ctx.ticks(2);
    lookAt(sp, a.offset(14, 0, 0).center());
    co_await ctx.ticks(2);
    useOn(sp, a.offset(10, -1, 0)); // ground 10 blocks east
    co_await ctx.ticks(3);
    int placed = 0;
    for (int i = 0; i <= 10; ++i) placed += shapeAtPos(a.offset(i, 0, 0)) == ct::Shape::XO ? 1 : 0;
    ctx.check(placed == 11, "a straight run of 11 tracks (" + std::to_string(placed) + ")");
    if (!sp.isCreative()) ctx.check(before - countTracks(sp) == 10, "10 tracks taken from the inventory");
    ctx.check(!tr::state().selections.contains(sp.getUuid().asString()), "the selection is cleared");
    clearArea(a.offset(-1, 0, -1), a.offset(11, 0, 1));
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("tracks.connect_curve", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(545, 3);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.give(sp, tracks(64), 0);
    ct::BlockPos3 a = cpos(base);           // heading east
    ct::BlockPos3 b = a.offset(9, 0, -9);   // heading north-south
    ctx.setBlock(gpos(a), std::string(tr::kTrackId), states(ct::Shape::XO));
    ctx.setBlock(gpos(b), std::string(tr::kTrackId), states(ct::Shape::ZO));
    stand(sp, a.offset(-2, 0, 0));
    co_await ctx.ticks(2);
    int before = countTracks(sp);
    lookAt(sp, a.offset(6, 0, 0).center());
    co_await ctx.ticks(2);
    useOn(sp, a);
    co_await ctx.ticks(6);
    stand(sp, b.offset(0, 0, 3));
    co_await ctx.ticks(2);
    lookAt(sp, b.center());
    co_await ctx.ticks(2);
    useOn(sp, b);
    co_await ctx.ticks(3);

    auto c = tr::curves().between(0, a.offset(1, 0, 0), b.offset(0, 0, 1));
    ctx.require(c != nullptr, "a curve joins the two tracks");
    ctx.check(std::abs(c->radius() - 7.5) < 1e-6, "the turn has a radius of 7.5");
    ctx.check(shapeAtPos(a.offset(1, 0, 0)) == ct::Shape::XO, "straight extension at the first end");
    ctx.check(shapeAtPos(b.offset(0, 0, 1)) == ct::Shape::ZO, "straight extension at the second end");
    int fakes = 0;
    for (auto const& p : c->occupiedBlocks()) fakes += isFakeAt(p) ? 1 : 0;
    ctx.check(fakes > 5, "fake tracks along the curve (" + std::to_string(fakes) + ")");
    if (!sp.isCreative()) {
        int expected = 2 + c->trackItemCost();
        ctx.check(before - countTracks(sp) == expected, "tracks taken: " + std::to_string(before - countTracks(sp)) + " of " + std::to_string(expected));
    }
    auto mid = tr::locate(0, Vec3{float(c->position(0.5).x), float(c->position(0.5).y), float(c->position(0.5).z)}, 0.5);
    ctx.require(mid.has_value(), "the curve is on the graph");
    auto* g = tr::railways().graph(mid->graph);
    auto const* e = g ? g->edge(mid->node1, mid->node2) : nullptr;
    ctx.check(e && e->isTurn(), "the curve is a turn edge");
#ifdef LV_CLIENT
    co_await ctx.ticks(2);
    ctx.check(lv::client::tracks::knownCurves() > 0, "the client received the curve");
#endif
    ctx.info("curve length " + std::to_string(c->length()));

    // breaking an end removes the curve and its fake tracks
    clearArea(b.offset(0, 0, 1), b.offset(0, 0, 1));
    co_await ctx.ticks(3);
    ctx.check(tr::curves().between(0, a.offset(1, 0, 0), b.offset(0, 0, 1)) == nullptr, "breaking an end removes the curve");
    int left = 0;
    for (auto const& p : c->occupiedBlocks()) left += isFakeAt(p) ? 1 : 0;
    ctx.check(left == 0, "fake tracks are removed with the curve");
    clearArea(a.offset(-1, 0, -10), a.offset(10, 0, 1));
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("tracks.connect_errors", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(560, 3);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.give(sp, tracks(64), 0);
    ct::BlockPos3 a = cpos(base);
    ct::BlockPos3 b = a.offset(3, 0, -3);
    ctx.setBlock(gpos(a), std::string(tr::kTrackId), states(ct::Shape::XO));
    ctx.setBlock(gpos(b), std::string(tr::kTrackId), states(ct::Shape::ZO));
    stand(sp, a.offset(-2, 0, 0));
    co_await ctx.ticks(2);
    int before = countTracks(sp);
    lookAt(sp, a.offset(6, 0, 0).center());
    co_await ctx.ticks(2);
    useOn(sp, a);
    co_await ctx.ticks(6);
    lookAt(sp, b.center());
    co_await ctx.ticks(2);
    useOn(sp, b); // too sharp
    co_await ctx.ticks(3);
    ctx.check(tr::curves().curvesAt(0, a).empty() && tr::curves().curvesAt(0, a.offset(1, 0, 0)).empty(), "no curve for a too sharp turn");
    ctx.check(countTracks(sp) == before, "nothing taken");
    ctx.check(tr::state().selections.contains(sp.getUuid().asString()), "the selection stays");
    // sneak-click clears it
    sp.setSneaking(true);
    co_await ctx.ticks(6);
    lookAt(sp, b.center());
    co_await ctx.ticks(2);
    useOn(sp, b);
    co_await ctx.ticks(2);
    sp.setSneaking(false);
    ctx.check(!tr::state().selections.contains(sp.getUuid().asString()), "sneak-click clears the selection");
    clearArea(a.offset(-1, 0, -4), a.offset(4, 0, 1));
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("tracks.graph_api", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(575);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ct::BlockPos3 b = cpos(base);
    for (int i = 0; i < 12; ++i) ctx.setBlock(gpos(b.offset(i, 0, 0)), std::string(tr::kTrackId), states(ct::Shape::XO));
    co_await ctx.ticks(2);
    auto start = tr::targetAt({0, b.x + 1, b.y, b.z}, Vec3{1, 0, 0});
    ctx.require(start.has_value(), "targetAt finds the track");
    auto& rail = tr::railways();
    auto* g    = rail.graph(start->graph);
    ctx.require(g != nullptr, "graph");
    auto moved = rail.advance(*start, 5);
    auto p     = rail.positionOf(moved);
    ctx.check(p && std::abs(p->x - (b.x + 6.5)) < 1e-6, "advance moves 5 blocks east");
    ct::PathQuery q;
    q.start        = *start;
    q.destinations = {moved};
    auto path      = rail.findPath(q);
    ctx.check(path && std::abs(path->distance - 5) < 1e-6, "path of 5 blocks");
    // edge points
    ct::EdgePoint pt;
    pt.id       = "tracks.selftest.point";
    pt.type     = "levitate:test";
    pt.node1    = g->node(start->node1)->location;
    pt.node2    = g->node(start->node2)->location;
    pt.position = 4;
    ctx.check(rail.addPoint(pt), "a point can be added on the edge");
    int invalidated = 0;
    rail.onPointInvalidated([&invalidated](ct::EdgePoint const& e) {
        if (e.id == "tracks.selftest.point") ++invalidated;
    });
    clearArea(b.offset(6, 0, 0), b.offset(6, 0, 0));
    co_await ctx.ticks(2);
    ctx.check(rail.point("tracks.selftest.point") == nullptr && invalidated == 1, "breaking the edge invalidates the point");
    clearArea(b.offset(-1, 0, 0), b.offset(12, 0, 0));
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("tracks.persistence", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(590, 3);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ct::BlockPos3 a = cpos(base);
    ctx.setBlock(gpos(a), std::string(tr::kTrackId), states(ct::Shape::XO));
    ctx.setBlock(gpos(a.offset(9, 0, -9)), std::string(tr::kTrackId), states(ct::Shape::ZO));
    co_await ctx.ticks(2);
    ct::PlacementRequest req;
    req.from   = ct::selectEnd(a, ct::Shape::XO, {1, 0, 0});
    req.pos2   = a.offset(9, 0, -9);
    req.shape2 = ct::Shape::ZO;
    req.look   = {0, 0, 1};
    auto plan  = ct::planConnection(req, ct::Shape::XO);
    ctx.require(plan.valid && plan.curve.has_value(), "plan a curve");
    for (auto const& piece : plan.straight) {
        auto const* blk = tr::trackBlock(piece.shape, false);
        if (blk && !shapeAtPos(piece.pos)) lv::game::setBlock(*tr::regionOf(0), tr::toGame(piece.pos), *blk);
    }
    tr::addCurve(0, *plan.curve);
    co_await ctx.ticks(2);
    size_t curvesBefore = tr::curves().size();
    size_t nodesBefore  = 0;
    for (auto* g : tr::railways().graphs()) nodesBefore += g->nodeCount();
    tr::saveWorld();
    tr::loadWorld();
    size_t nodesAfter = 0;
    for (auto* g : tr::railways().graphs()) nodesAfter += g->nodeCount();
    ctx.check(tr::curves().size() == curvesBefore, "curves reload");
    ctx.check(nodesAfter == nodesBefore, "graph nodes reload (" + std::to_string(nodesAfter) + "/" + std::to_string(nodesBefore) + ")");
    ctx.check(tr::curves().between(0, plan.target1, plan.target2) != nullptr, "the curve is found after reload");
    clearArea(a.offset(-1, 0, -10), a.offset(10, 0, 1));
    co_await ctx.ticks(2);
});
