#include <gtest/gtest.h>

#include <map>

#include "core/tracks/CurveStore.h"
#include "core/tracks/Graph.h"
#include "core/tracks/Placement.h"

using namespace lv::core::tracks;

namespace {

/// A world made of track shapes and curves (dimension → position → shape).
class FakeWorld : public TrackWorld {
public:
    std::map<std::pair<int, BlockPos3>, Shape>                       shapes;
    std::map<std::pair<int, BlockPos3>, std::pair<int, BlockPos3>> portals;
    CurveStore                                                       curves;

    std::optional<TrackBlockView> trackAt(int dim, BlockPos3 const& pos) const override {
        auto it = shapes.find({dim, pos});
        if (it == shapes.end()) return std::nullopt;
        TrackBlockView v;
        v.shape  = it->second;
        v.curves = curves.curvesAt(dim, pos);
        if (auto p = portals.find({dim, pos}); p != portals.end()) v.portalLink = p->second;
        return v;
    }

    void place(Railways& r, int dim, BlockPos3 p, Shape s) {
        shapes[{dim, p}] = s;
        r.onRailAdded(*this, dim, p);
    }
    void breakAt(Railways& r, int dim, BlockPos3 p) {
        auto view = trackAt(dim, p);
        ASSERT_TRUE(view);
        shapes.erase({dim, p});
        for (auto& c : curves.removeAllAt(dim, p)) {
            (void)c;
        }
        r.onRailRemoved(*this, dim, p, *view);
    }
};

std::function<bool(int, int)> noSupport() {
    return [](int, int) { return false; };
}

} // namespace

// ---- shapes -------------------------------------------------------------------------------------

TEST(TrackShape, PlacementFollowsTheLookDirection) {
    EXPECT_EQ(placementShape({0, 0, 1}, 0, noSupport()), Shape::ZO);
    EXPECT_EQ(placementShape({0, 0, -1}, 180, noSupport()), Shape::ZO);
    EXPECT_EQ(placementShape({1, 0, 0}, -90, noSupport()), Shape::XO);
    EXPECT_EQ(placementShape({1, -0.5, 1.05}, 0, noSupport()), Shape::PD);
    EXPECT_EQ(placementShape({-1, 0, 1}, 0, noSupport()), Shape::ND);
    EXPECT_EQ(placementShape({0.2, 0, 1}, 0, noSupport()), Shape::ZO); // ~11° off: still straight
    // looking straight down: the yaw decides (yaw 90 = west)
    EXPECT_EQ(placementShape({0, -1, 0}, 90, noSupport()), Shape::XO);
}

TEST(TrackShape, AscendsTowardsASolidNeighbour) {
    // solid block to the north (dz = -1) with air above it
    auto north = [](int dx, int dz) { return dx == 0 && dz == -1; };
    EXPECT_EQ(placementShape({0, 0, 1}, 0, north), Shape::AN);
    auto east = [](int dx, int dz) { return dx == 1 && dz == 0; };
    EXPECT_EQ(placementShape({1, 0, 0}, -90, east), Shape::AE);
    // diagonals never ascend
    EXPECT_EQ(placementShape({1, 0, 1}, 0, [](int, int) { return true; }), Shape::PD);
    // both sides solid: the negative side wins (as Create iterates)
    EXPECT_EQ(placementShape({0, 0, 1}, 0, [](int, int) { return true; }), Shape::AN);
}

TEST(TrackShape, OverlaysMakeCrossings) {
    EXPECT_EQ(overlayShape(Shape::ZO, Shape::XO), Shape::CR_O);
    EXPECT_EQ(overlayShape(Shape::XO, Shape::ZO), Shape::CR_O);
    EXPECT_EQ(overlayShape(Shape::PD, Shape::ND), Shape::CR_D);
    EXPECT_EQ(overlayShape(Shape::XO, Shape::PD), Shape::CR_PDX);
    EXPECT_EQ(overlayShape(Shape::ND, Shape::ZO), Shape::CR_NDZ);
    EXPECT_EQ(overlayShape(Shape::ZO, Shape::ZO), Shape::ZO);
    EXPECT_EQ(overlayShape(Shape::AN, Shape::XO), Shape::AN);
    EXPECT_TRUE(isJunction(Shape::CR_PDZ));
    EXPECT_FALSE(isJunction(Shape::PD));
}

TEST(TrackShape, RotationAndMirror) {
    EXPECT_EQ(rotateShape(Shape::ZO, 1), Shape::XO);
    EXPECT_EQ(rotateShape(Shape::AN, 1), Shape::AE);
    EXPECT_EQ(rotateShape(Shape::AN, 4), Shape::AN);
    EXPECT_EQ(mirrorShape(Shape::AE, true), Shape::AW);
    EXPECT_EQ(mirrorShape(Shape::AE, false), Shape::AE);
    EXPECT_EQ(mirrorShape(Shape::PD, false), Shape::ND);
}

TEST(TrackShape, NearestAxisPointsAlongTheLook) {
    EXPECT_TRUE(nearestTrackAxis(Shape::ZO, {0, 0, -1}).near({0, 0, -1}));
    EXPECT_TRUE(nearestTrackAxis(Shape::ZO, {0.1, -0.3, 0.9}).near({0, 0, 1}));
    EXPECT_TRUE(nearestTrackAxis(Shape::CR_O, {0.9, 0, 0.1}).near({1, 0, 0}));
    auto end = curveStart({3, 64, 5}, {0, 0, 1});
    EXPECT_TRUE(end.near({3.5, 64, 6}));
    auto up = curveStart({0, 0, 0}, {0, 1, 1});
    EXPECT_TRUE(up.near({0.5, 1, 1}));
}

// ---- curves -------------------------------------------------------------------------------------

namespace {
Curve quarterTurn(double r) {
    CurveSpec s;
    s.blocks[0]  = {0, 0, 0};
    s.blocks[1]  = {int(r), 0, int(r)};
    s.starts[0]  = {0, 0, 0};
    s.starts[1]  = {r, 0, r};
    s.axes[0]    = {0, 0, 1};
    s.axes[1]    = {-1, 0, 0};
    s.normals[0] = s.normals[1] = {0, 1, 0};
    return Curve(s);
}
} // namespace

TEST(TrackCurve, QuarterCircleLengthAndCost) {
    Curve c = quarterTurn(7);
    EXPECT_NEAR(c.radius(), 7, 1e-6);
    EXPECT_NEAR(c.length(), 3.14159265 / 2 * 7, 0.05);
    EXPECT_EQ(c.segmentCount(), int(c.length() * 2));
    EXPECT_EQ(c.trackItemCost(), (c.segmentCount() + 1) / 2);
    EXPECT_EQ(c.girderItemCost(), 0);
    EXPECT_TRUE(c.position(0).near({0, 0, 0}));
    EXPECT_TRUE(c.position(1).near({7, 0, 7}));
    // the middle of the curve lies close to the circle around (7, 0, 0)
    Vec3d mid = c.position(c.tAtDistance(c.length() / 2));
    EXPECT_NEAR(mid.distanceTo({7, 0, 0}), 7, 0.05);
    EXPECT_TRUE(c.normal(0.5).near({0, 1, 0}, 1e-6));
}

TEST(TrackCurve, SecondaryIsTheSameCurveReversed) {
    Curve c = quarterTurn(9);
    Curve s = c.secondary();
    EXPECT_FALSE(s.primary());
    EXPECT_NEAR(s.length(), c.length(), 1e-6);
    EXPECT_TRUE(s.position(0).near(c.position(1)));
    EXPECT_TRUE(c.sameShape(s));
    auto j    = c.toJson();
    auto back = Curve::fromJson(j);
    ASSERT_TRUE(back);
    EXPECT_TRUE(back->sameShape(c));
}

TEST(TrackCurve, RasterisedBlocksFollowTheCurve) {
    Curve c      = quarterTurn(7);
    auto  blocks = c.occupiedBlocks();
    EXPECT_GE(blocks.size(), 8u);
    for (auto const& b : blocks) {
        Vec3d  centre{b.x + .5, 0, b.z + .5};
        double d = centre.distanceTo({7, 0, 0});
        EXPECT_NEAR(d, 7, 1.3) << b.str();
        EXPECT_EQ(b.y, 0) << b.str();
    }
}

// ---- placement ----------------------------------------------------------------------------------

namespace {
PlacementRequest request(BlockPos3 from, Shape fromShape, Vec3d fromLook, BlockPos3 to, Shape toShape, Vec3d look) {
    PlacementRequest r;
    r.from   = selectEnd(from, fromShape, fromLook);
    r.pos2   = to;
    r.shape2 = toShape;
    r.look   = look;
    return r;
}
} // namespace

TEST(TrackPlacement, StraightRun) {
    auto plan = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {0, 0, 10}, Shape::ZO, {0, 0, 1}), Shape::ZO);
    ASSERT_TRUE(plan.valid) << plan.message;
    EXPECT_FALSE(plan.curve);
    EXPECT_EQ(plan.end1Extent, 11);
    ASSERT_EQ(plan.straight.size(), 11u);
    EXPECT_EQ(plan.straight.front().pos, (BlockPos3{0, 0, 0}));
    EXPECT_EQ(plan.straight.back().pos, (BlockPos3{0, 0, 10}));
}

TEST(TrackPlacement, Errors) {
    auto same = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {0, 0, 0}, Shape::ZO, {0, 0, 1}), Shape::ZO);
    EXPECT_EQ(same.message, "create.track.second_point");
    auto far = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {0, 0, 40}, Shape::ZO, {0, 0, 1}), Shape::ZO);
    EXPECT_EQ(far.message, "create.track.too_far");
    auto sharp = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {4, 0, 4}, Shape::XO, {1, 0, 0}), Shape::ZO);
    EXPECT_EQ(sharp.message, "create.track.too_sharp");
    auto sCurve = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {5, 0, -10}, Shape::ZO, {0, 0, 1}), Shape::ZO);
    EXPECT_EQ(sCurve.message, "create.track.too_sharp");
    auto shallow = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {12, 0, 4}, Shape::PD, {1, 0, 1}), Shape::ZO);
    EXPECT_FALSE(shallow.valid);
    auto missing = request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {0, 0, 10}, Shape::ZO, {0, 0, 1});
    missing.fromIsTrack = false;
    EXPECT_EQ(planConnection(missing, Shape::ZO).message, "create.track.original_missing");
}

TEST(TrackPlacement, NinetyDegreeTurn) {
    auto plan = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {10, 0, 10}, Shape::XO, {1, 0, 0}), Shape::ZO);
    ASSERT_TRUE(plan.valid) << plan.message;
    ASSERT_TRUE(plan.curve);
    EXPECT_EQ(plan.end1Extent, 2);
    EXPECT_EQ(plan.end2Extent, 2);
    EXPECT_EQ(plan.target1, (BlockPos3{0, 0, 2}));
    EXPECT_EQ(plan.target2, (BlockPos3{8, 0, 10}));
    EXPECT_NEAR(plan.curve->radius(), 7.5, 1e-6); // from block edge (0.5, 3) to (8, 10.5)
    EXPECT_EQ(plan.straight.size(), 6u);

    // maximised: no straight extensions, the turn takes the whole space
    auto req         = request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {10, 0, 10}, Shape::XO, {1, 0, 0});
    req.maximiseTurn = true;
    auto maxed       = planConnection(req, Shape::ZO);
    ASSERT_TRUE(maxed.valid);
    EXPECT_EQ(maxed.end1Extent, 0);
    EXPECT_NEAR(maxed.curve->radius(), 9.5, 1e-6);
}

TEST(TrackPlacement, SCurve) {
    auto plan = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {5, 0, -14}, Shape::ZO, {0, 0, 1}), Shape::ZO);
    ASSERT_TRUE(plan.valid) << plan.message;
    ASSERT_TRUE(plan.curve);
    EXPECT_EQ(plan.end1Extent, 2);
    EXPECT_EQ(plan.end2Extent, 1);
}

TEST(TrackPlacement, SlopesNeedRoom) {
    // straight connection up 2 blocks over 4: too steep; over 10: a vertical S curve
    auto steep = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {0, 2, 4}, Shape::ZO, {0, 0, 1}), Shape::ZO);
    EXPECT_EQ(steep.message, "create.track.too_steep");
    auto ok = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {0, 2, 10}, Shape::ZO, {0, 0, 1}), Shape::ZO);
    ASSERT_TRUE(ok.valid) << ok.message;
    EXPECT_TRUE(ok.curve);
}

TEST(TrackPlacement, SelectionRoundTrip) {
    TrackEnd e    = selectEnd({1, 2, 3}, Shape::XO, {-1, 0, 0});
    auto     back = TrackEnd::fromJson(e.toJson());
    ASSERT_TRUE(back);
    EXPECT_EQ(back->pos, e.pos);
    EXPECT_TRUE(back->axis.near({-1, 0, 0}));
    EXPECT_TRUE(back->end.near({1, 2, 3.5}));
}

// ---- graph --------------------------------------------------------------------------------------

namespace {
void line(FakeWorld& w, Railways& r, int z0, int z1, int x = 0, int dim = 0) {
    for (int z = z0; z <= z1; ++z) w.place(r, dim, {x, 0, z}, Shape::ZO);
}
} // namespace

TEST(TrackGraph, StraightLineIsOneEdge) {
    FakeWorld w;
    Railways  r;
    line(w, r, 0, 9);
    ASSERT_EQ(r.graphs().size(), 1u);
    Graph* g = r.graphs().front();
    EXPECT_EQ(g->nodeCount(), 2u);
    EXPECT_EQ(g->edgeCount(), 1u);
    auto nodes = g->nodes();
    auto const* e = g->edge(nodes[0]->id, nodes[1]->id);
    ASSERT_TRUE(e);
    EXPECT_NEAR(g->edgeLength(*e), 10, 1e-9);
    EXPECT_FALSE(e->isTurn());
}

TEST(TrackGraph, LongRunsGetANodeEvery16Blocks) {
    FakeWorld w;
    Railways  r;
    line(w, r, 0, 39); // ends at z = 0 and z = 40, grid nodes at z = 16 and z = 32
    ASSERT_EQ(r.graphs().size(), 1u);
    EXPECT_EQ(r.graphs().front()->nodeCount(), 4u);
}

TEST(TrackGraph, BreakingSplitsAndJoining) {
    FakeWorld w;
    Railways  r;
    line(w, r, 0, 9);
    w.breakAt(r, 0, {0, 0, 5});
    ASSERT_EQ(r.graphs().size(), 2u);
    for (auto* g : r.graphs()) EXPECT_EQ(g->nodeCount(), 2u);
    w.place(r, 0, {0, 0, 5}, Shape::ZO);
    ASSERT_EQ(r.graphs().size(), 1u);
    EXPECT_EQ(r.graphs().front()->nodeCount(), 2u);
}

TEST(TrackGraph, JunctionCreatesNodes) {
    FakeWorld w;
    Railways  r;
    line(w, r, 0, 8);
    for (int x = -4; x <= 4; ++x) {
        if (x == 0) continue;
        w.place(r, 0, {x, 0, 4}, Shape::XO);
    }
    w.shapes[{0, {0, 0, 4}}] = Shape::CR_O;
    r.onRailAdded(w, 0, {0, 0, 4});
    ASSERT_EQ(r.graphs().size(), 1u);
    Graph* g = r.graphs().front();
    // 4 dead ends + the centre + the half-block end at x = 0 (a 16-block grid line)
    EXPECT_EQ(g->nodeCount(), 6u);
}

TEST(TrackGraph, CurvesAreTurnEdges) {
    FakeWorld w;
    Railways  r;
    auto      plan = planConnection(request({0, 0, 0}, Shape::ZO, {0, 0, 1}, {10, 0, 10}, Shape::XO, {1, 0, 0}), Shape::ZO);
    ASSERT_TRUE(plan.valid);
    w.curves.add(0, *plan.curve);
    for (auto const& p : plan.straight) w.place(r, 0, p.pos, p.shape);
    ASSERT_EQ(r.graphs().size(), 1u);
    Graph* g     = r.graphs().front();
    int    turns = 0;
    for (auto const* n : g->nodes())
        for (auto const& [to, e] : g->connectionsFrom(n->id)) turns += e.isTurn() ? 1 : 0;
    EXPECT_EQ(turns, 2); // both directions
    EXPECT_EQ(g->nodeCount(), 4u);

    // a train path from the start of the line to the far end goes through the curve
    auto start = r.nearestEdge(0, {0.5, 0, 0.5});
    ASSERT_TRUE(start);
    Node const* farEnd = g->locateNode(NodeLocation::of({11, 0, 10.5}, 0));
    ASSERT_TRUE(farEnd);
    // destination: the edge arriving at the far end
    std::optional<GraphLocation> dest;
    for (auto const* n : g->nodes()) {
        if (auto const* e = g->edge(n->id, farEnd->id)) dest = GraphLocation{g->id(), n->id, farEnd->id, g->edgeLength(*e)};
    }
    ASSERT_TRUE(dest);
    // orient the start towards the curve
    GraphLocation s = *start;
    if (g->node(s.node2)->location.location().z < g->node(s.node1)->location.location().z) {
        auto const* e = g->edge(s.node1, s.node2);
        s             = GraphLocation{s.graph, s.node2, s.node1, g->edgeLength(*e) - s.position};
    }
    PathQuery q;
    q.start        = s;
    q.destinations = {*dest};
    auto path      = r.findPath(q);
    ASSERT_TRUE(path);
    EXPECT_NEAR(path->distance, 2.5 + plan.curve->length() + 3, 0.6);
    EXPECT_EQ(path->edges.size(), 2u);

    // the reverse direction can't reach it (no U-turns)
    PathQuery back = q;
    auto const* se = g->edge(s.node1, s.node2);
    back.start     = GraphLocation{s.graph, s.node2, s.node1, g->edgeLength(*se) - s.position};
    EXPECT_FALSE(r.findPath(back));

    // advancing along the graph follows the curve
    auto moved = r.advance(s, 2.5 + plan.curve->length() / 2);
    auto pos   = r.positionOf(moved);
    ASSERT_TRUE(pos);
    EXPECT_NEAR(pos->distanceTo({8, 0, 3}), 7.5, 0.05);
}

TEST(TrackGraph, EdgePointsSurviveRebuildsAndAreInvalidated) {
    FakeWorld w;
    Railways  r;
    line(w, r, 0, 9);
    Graph* g     = r.graphs().front();
    auto   nodes = g->nodes();
    EdgePoint p;
    p.id       = "signal:test";
    p.type     = "levitate:signal";
    p.node1    = nodes[0]->location;
    p.node2    = nodes[1]->location;
    p.position = 3;
    ASSERT_TRUE(r.addPoint(p));
    int invalidated = 0;
    r.onPointInvalidated([&](EdgePoint const&) { ++invalidated; });

    // re-adding a track of the same line rebuilds the edge: the point stays
    r.onRailAdded(w, 0, {0, 0, 4});
    EXPECT_TRUE(r.point("signal:test"));
    EXPECT_EQ(invalidated, 0);

    // breaking the line removes the edge: the point is invalidated
    w.breakAt(r, 0, {0, 0, 6});
    EXPECT_FALSE(r.point("signal:test"));
    EXPECT_EQ(invalidated, 1);
}

TEST(TrackGraph, PersistenceRoundTrip) {
    FakeWorld w;
    Railways  r;
    line(w, r, 0, 20);
    line(w, r, 0, 5, 10);
    auto plan = planConnection(request({20, 0, 0}, Shape::ZO, {0, 0, 1}, {30, 0, 10}, Shape::XO, {1, 0, 0}), Shape::ZO);
    ASSERT_TRUE(plan.valid);
    w.curves.add(0, *plan.curve);
    for (auto const& p : plan.straight) w.place(r, 0, p.pos, p.shape);

    Railways loaded;
    for (int dim : r.dimensions()) loaded.loadJson(nlohmann::json::parse(r.toJson(dim).dump()));
    loaded.finishLoading();
    ASSERT_EQ(loaded.graphs().size(), r.graphs().size());
    size_t nodesA = 0, nodesB = 0, edgesA = 0, edgesB = 0;
    for (auto* g : r.graphs()) nodesA += g->nodeCount(), edgesA += g->edgeCount();
    for (auto* g : loaded.graphs()) nodesB += g->nodeCount(), edgesB += g->edgeCount();
    EXPECT_EQ(nodesA, nodesB);
    EXPECT_EQ(edgesA, edgesB);
    // new nodes get fresh ids
    EXPECT_GT(loaded.nextNodeId(), 0);
}

TEST(TrackGraph, PortalLinksDimensions) {
    FakeWorld w;
    Railways  r;
    // overworld line ending at a portal track facing north at z = 0; nether line likewise
    line(w, r, 1, 5, 0, 0);
    line(w, r, 1, 5, 0, 1);
    w.shapes[{0, {0, 0, 0}}]  = Shape::TN;
    w.shapes[{1, {0, 0, 0}}]  = Shape::TN;
    w.portals[{0, {0, 0, 0}}] = {1, BlockPos3{0, 0, 0}};
    w.portals[{1, {0, 0, 0}}] = {0, BlockPos3{0, 0, 0}};
    r.onRailAdded(w, 0, {0, 0, 0});
    r.onRailAdded(w, 1, {0, 0, 0});
    ASSERT_EQ(r.graphs().size(), 1u);
    Graph* g     = r.graphs().front();
    bool   inter = false;
    for (auto const* n : g->nodes())
        for (auto const& [to, e] : g->connectionsFrom(n->id)) inter |= e.interDimensional;
    EXPECT_TRUE(inter);
    EXPECT_EQ(r.dimensions(), (std::set<int>{0, 1}));
}

TEST(TrackCurveStore, IndexesEndsAndBlocks) {
    CurveStore s;
    Curve      c = quarterTurn(7);
    s.add(0, c);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(s.curvesAt(0, {0, 0, 0}).size(), 1u);
    auto other = s.curvesAt(0, {7, 0, 7});
    ASSERT_EQ(other.size(), 1u);
    EXPECT_EQ(other[0]->block(0), (BlockPos3{7, 0, 7}));
    auto through = c.occupiedBlocks();
    ASSERT_FALSE(through.empty());
    EXPECT_EQ(s.curvesThrough(0, through[0]).size(), 1u);
    EXPECT_TRUE(s.remove(0, {7, 0, 7}, {0, 0, 0}));
    EXPECT_EQ(s.size(), 0u);
    EXPECT_TRUE(s.curvesThrough(0, through[0]).empty());
}
