// Selftests of tilted tracks (U48, R18; testPos dx 4800..4829): a flat track between two sloped
// curves tilts (Create's TrackBlockEntityTilt), the graph climbs over it, a connection onto it is
// refused, and it goes flat again when one of its curves goes.

#include <cmath>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/tracks/Tilt.h"
#include "feature/common/BlockData.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::tracks::tilttests {

using lv::selftest::Context;

namespace {

ct::BlockPos3 cpos(core::GlobalPos const& p) { return {p.x, p.y, p.z}; }
core::GlobalPos gpos(int dim, ct::BlockPos3 const& p) { return {dim, p.x, p.y, p.z}; }

std::vector<std::pair<std::string, int>> trackStates(ct::Shape shape) {
    int s = int(shape);
    return {{std::string(kShapeState), s % 16}, {std::string(kShapeHiState), s / 16}, {std::string(kTurnState), 0}};
}

/// A flat east-west track `m` with a slope rising towards it from the west (2 blocks, the curve
/// arrives 2 blocks lower) and one rising away from it to the east: the layout of the gtests,
/// along x. Returns the two curve plans (from `m`).
struct Ramp {
    ct::BlockPos3                    m, lowSlope, highSlope;
    std::optional<ct::PlacementPlan> low, high;
};

Ramp buildRamp(Context& ctx, int dim, ct::BlockPos3 m) {
    Ramp r;
    r.m         = m;
    r.highSlope = m.offset(5, 2, 0);
    r.lowSlope  = m.offset(-5, -3, 0);
    ctx.setBlock(gpos(dim, m), std::string(kTrackId), trackStates(ct::Shape::XO));
    for (int i = 0; i < 2; ++i) {
        ctx.setBlock(gpos(dim, r.highSlope.offset(i, i, 0)), std::string(kTrackId), trackStates(ct::Shape::AE));
        ctx.setBlock(gpos(dim, r.lowSlope.offset(-i, -i, 0)), std::string(kTrackId), trackStates(ct::Shape::AE));
    }
    return r;
}

std::optional<ct::PlacementPlan> plan(ct::BlockPos3 m, ct::Vec3d look, ct::BlockPos3 to) {
    ct::PlacementRequest req;
    req.from   = ct::selectEnd(m, ct::Shape::XO, look);
    req.pos2   = to;
    req.shape2 = ct::Shape::AE;
    req.look   = look;
    auto p     = ct::planConnection(req, ct::Shape::XO);
    if (!p.valid || !p.curve) return std::nullopt;
    return p;
}

void clearRamp(int dim, Ramp const& r) {
    for (auto const& c : state().curves.curvesAt(dim, r.m)) destroyCurve(dim, *c, false, nullptr);
    auto* region    = regionOf(dim);
    auto const* air = game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (int x = -8; x <= 8; ++x)
        for (int y = -5; y <= 5; ++y) {
            BlockPos bp{r.m.x + x, r.m.y + y, r.m.z};
            if (region->getBlock(bp).isAir()) continue;
            state().noDrops.insert({dim, {bp.x, bp.y, bp.z}});
            game::setBlock(*region, bp, *air);
        }
}

/// Walks the graph from the low slope over the ramp: heights along the way (every 1/4 block).
std::vector<ct::Vec3d> walk(int dim, Ramp const& r) {
    std::vector<ct::Vec3d> out;
    auto&                  rail  = railways();
    auto                   start = rail.nearestEdge(dim, r.lowSlope.offset(-1, -1, 0).bottomCenter() + ct::Vec3d{0, .5, 0}, 1.5);
    if (!start) return out;
    ct::GraphLocation loc = *start;
    // face east (towards the ramp)
    auto a = rail.positionOf(loc);
    auto b = rail.positionOf(rail.advance(loc, 0.25));
    if (a && b && b->x < a->x) {
        auto* g = rail.graph(loc.graph);
        auto const* e = g ? g->edge(loc.node1, loc.node2) : nullptr;
        if (!e) return out;
        loc = ct::GraphLocation{loc.graph, loc.node2, loc.node1, g->edgeLength(*e) - loc.position};
    }
    for (int i = 0; i < 80; ++i) {
        if (auto p = rail.positionOf(loc)) out.push_back(*p);
        loc = rail.advance(loc, 0.25);
    }
    return out;
}

} // namespace

LV_REGISTER_SELFTEST("tracks.tilt", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(4800, 6);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    int  dim = base.dim;
    Ramp r   = buildRamp(ctx, dim, cpos(base).offset(8, 4, 0));
    co_await ctx.ticks(2);
    r.high = plan(r.m, {1, 0, 0}, r.highSlope);
    r.low  = plan(r.m, {-1, 0, 0}, r.lowSlope);
    ctx.require(r.high && r.low, "both sloped curves can be planned from the flat track");
    ctx.check(r.high->curve->block(0) == r.m && r.low->curve->block(0) == r.m, "both curves start at the flat track");

    addCurve(dim, *r.high->curve);
    co_await ctx.ticks(2);
    ctx.check(!tiltAt(dim, r.m), "one sloped curve doesn't tilt the track");
    addCurve(dim, *r.low->curve);
    co_await ctx.ticks(3);

    auto angle = tiltAt(dim, r.m);
    ctx.require(angle.has_value(), "the second sloped curve tilts the track");
    double m = (r.high->curve->spec().starts[1].y - r.low->curve->spec().starts[1].y) / (r.high->curve->length() + r.low->curve->length());
    ctx.check(std::abs(*angle - std::atan(m) * 180 / 3.141592653589793) < 1e-6, "Create's smoothing angle (" + std::to_string(*angle) + ")");
    auto const* doc = common::blockdata::get(gpos(dim, r.m));
    ctx.check(doc && doc->contains(kBlockDataKey), "the tilt is kept in the track's BlockData");

    auto at = state().curves.curvesAt(dim, r.m);
    ctx.require(at.size() == 2, "the track keeps its two curves");
    int raised = 0;
    for (auto const& c : at) {
        ctx.check(c->spec().axes[0].y != 0, "each curve leaves along the ramp");
        raised = std::max(raised, c->spec().smoothing[0]);
    }
    ctx.check(raised == std::clamp(int(m * 16), 0, 15) && raised > 0, "the high end is raised by " + std::to_string(raised) + " px");

    // the graph: a raised node at the high end of the block, and a smooth climb over the ramp
    ct::NodeLocation top = ct::NodeLocation::of({r.m.x + 1.0, double(r.m.y), r.m.z + .5}, dim);
    top.yOffsetPixels    = raised;
    bool found           = false;
    for (auto* g : railways().graphs()) found = found || g->locateNode(top);
    ctx.check(found, "the graph has the raised node at the top of the ramp");
    auto path = walk(dim, r);
    ctx.require(path.size() > 40, "the graph runs from the low slope over the ramp (" + std::to_string(path.size()) + " points)");
    bool   rising = true, smooth = true, crossed = false;
    double maxStep = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        double dy = path[i].y - path[i - 1].y;
        maxStep   = std::max(maxStep, std::abs(dy));
        if (dy < -1e-6) rising = false;
        if (std::abs(dy) > 0.2) smooth = false;
        if (path[i].x > r.m.x + .4 && path[i].x < r.m.x + .6) {
            crossed = true;
            ctx.check(path[i].y > r.m.y + 1e-3 && path[i].y < r.m.y + raised / 16.0, "the middle of the block is on the ramp");
        }
    }
    ctx.check(crossed, "the walk crosses the tilted block");
    ctx.check(rising, "the walk climbs all the way");
    ctx.check(smooth, "no step over 0.2 blocks (max " + std::to_string(maxStep) + ")");

    // Create: "Cannot start connection from a Turn"
    ct::PlacementRequest onto;
    onto.from       = ct::selectEnd(r.m.offset(0, 0, 6), ct::Shape::ZO, {0, 0, -1});
    onto.pos2       = r.m;
    onto.shape2     = ct::Shape::XO;
    onto.pos2Tilted = tiltAt(dim, r.m).has_value();
    ctx.check(ct::planConnection(onto, ct::Shape::ZO).message == "create.track.turn_start", "a connection onto the tilted track is refused");

    clearRamp(dim, r);
    co_await ctx.ticks(3);
    ctx.check(!tiltAt(dim, r.m), "breaking the ramp clears the tilt");
});

LV_REGISTER_SELFTEST("tracks.tilt_undo", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(4815, 6);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    int  dim = base.dim;
    Ramp r   = buildRamp(ctx, dim, cpos(base).offset(8, 4, 0));
    co_await ctx.ticks(2);
    r.high = plan(r.m, {1, 0, 0}, r.highSlope);
    r.low  = plan(r.m, {-1, 0, 0}, r.lowSlope);
    ctx.require(r.high && r.low, "both sloped curves can be planned");
    addCurve(dim, *r.high->curve);
    addCurve(dim, *r.low->curve);
    co_await ctx.ticks(3);
    ctx.require(tiltAt(dim, r.m).has_value(), "tilted");

    // breaking the high slope removes its curve: the track is flat again, the low curve restored
    auto* region    = regionOf(dim);
    auto const* air = game::blockWith("minecraft:air");
    ctx.require(region && air, "region");
    state().noDrops.insert({dim, r.highSlope});
    game::setBlock(*region, BlockPos{r.highSlope.x, r.highSlope.y, r.highSlope.z}, *air);
    co_await ctx.ticks(3);
    ctx.check(!tiltAt(dim, r.m), "the track is flat again");
    auto at = state().curves.curvesAt(dim, r.m);
    ctx.require(at.size() == 1, "the low curve stays");
    ctx.check(at[0]->spec().axes[0].y == 0 && !at[0]->hasSmoothing(), "the low curve leaves the track flat again");
    ctx.check(at[0]->spec().starts[0].near(r.low->curve->spec().starts[0]), "the low curve starts where it was placed");
    ctx.check(std::abs(at[0]->length() - r.low->curve->length()) < 1e-6, "the low curve has its original shape");
    bool flatNode = false;
    for (auto* g : railways().graphs()) flatNode = flatNode || g->locateNode(ct::NodeLocation::of({r.m.x + 1.0, double(r.m.y), r.m.z + .5}, dim));
    ctx.check(flatNode, "the graph has the flat end node back");
    clearRamp(dim, r);
    co_await ctx.ticks(2);
});

} // namespace lv::feature::tracks::tilttests
