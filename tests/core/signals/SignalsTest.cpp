#include <gtest/gtest.h>

#include <map>

#include "core/signals/Signals.h"
#include "core/tracks/CurveStore.h"
#include "core/tracks/Graph.h"
#include "core/tracks/Placement.h"

using namespace lv::core::signals;
using namespace lv::core::tracks;

namespace {

/// Track shapes and curves by position (as the tracks unit's tests).
class World : public TrackWorld {
public:
    std::map<std::pair<int, BlockPos3>, Shape> shapes;
    CurveStore                                 curves;

    std::optional<TrackBlockView> trackAt(int dim, BlockPos3 const& pos) const override {
        auto it = shapes.find({dim, pos});
        if (it == shapes.end()) return std::nullopt;
        TrackBlockView v;
        v.shape  = it->second;
        v.curves = curves.curvesAt(dim, pos);
        return v;
    }
    void place(Railways& r, BlockPos3 p, Shape s) {
        shapes[{0, p}] = s;
        r.onRailAdded(*this, 0, p);
    }
};

/// A north-south line at x along z0..z1 (inclusive).
void lineZ(World& w, Railways& r, int z0, int z1, int x = 0) {
    for (int z = z0; z <= z1; ++z) w.place(r, {x, 0, z}, Shape::ZO);
}

/// The graph location nearest to `pos`, travelling along `dir` (Create's track targeting).
GraphLocation at(Railways const& r, Vec3d pos, Vec3d dir) {
    auto loc = r.nearestEdge(0, pos, 1.0);
    EXPECT_TRUE(loc.has_value());
    if (!loc) return {};
    Graph*      g = r.graph(loc->graph);
    auto const* e = g->edge(loc->node1, loc->node2);
    if (g->edgeDirection(*e, true).dot(dir) < 0) return {loc->graph, loc->node2, loc->node1, g->edgeLength(*e) - loc->position};
    return *loc;
}

/// A boundary at `loc` with a signal for trains travelling along it (side 1) and optionally one
/// for the opposite direction (side 0).
Boundary boundary(Railways const& r, std::string id, GraphLocation const& loc, SignalType type = SignalType::Entry, bool back = false) {
    Graph*   g = r.graph(loc.graph);
    Boundary b;
    b.id                 = std::move(id);
    b.node1              = g->node(loc.node1)->location;
    b.node2              = g->node(loc.node2)->location;
    b.position           = loc.position;
    b.sides[1].hasSignal = true;
    b.sides[1].type      = type;
    b.sides[0].hasSignal = back;
    return b;
}

/// The train covering `length` blocks behind `front` (straight runs only: within one edge or two).
std::vector<Span> spanBehind(Railways const& r, GraphLocation const& front, double length) {
    Graph* g = r.graph(front.graph);
    Span   s;
    s.node1 = g->node(front.node1)->location;
    s.node2 = g->node(front.node2)->location;
    s.from  = std::max(0.0, front.position - length);
    s.to    = front.position;
    return {s};
}

/// Edges of the path from `start` to the end of the edge leading to the node nearest `dest`.
std::vector<PathStep> pathTo(Railways const& r, GraphLocation const& start, Vec3d dest, Vec3d destDir) {
    PathQuery q;
    q.start        = start;
    q.destinations = {at(r, dest, destDir)};
    auto p         = r.findPath(q);
    EXPECT_TRUE(p.has_value());
    return p ? p->edges : std::vector<PathStep>{};
}

} // namespace

TEST(SignalSections, BoundariesSplitALineIntoSections) {
    World    w;
    Railways r;
    lineZ(w, r, 0, 29);
    SignalSystem s;
    auto         b1 = at(r, {0.5, 0, 10.5}, {0, 0, 1});
    auto         b2 = at(r, {0.5, 0, 20.5}, {0, 0, 1});
    s.rebuild(r, {boundary(r, "a", b1), boundary(r, "b", b2)});
    EXPECT_EQ(s.groupCount(), 3u);
    GroupId south = s.groupOf("a", 0), middle = s.groupOf("a", 1), north = s.groupOf("b", 1);
    EXPECT_NE(south, middle);
    EXPECT_NE(middle, north);
    EXPECT_EQ(s.groupOf("b", 0), middle);
    EXPECT_EQ(s.groupAt(r, at(r, {0.5, 0, 15}, {0, 0, 1})), middle);
    EXPECT_EQ(s.groupAt(r, at(r, {0.5, 0, 25}, {0, 0, -1})), north);
    EXPECT_EQ(s.groupAt(r, at(r, {0.5, 0, 2}, {0, 0, 1})), south);

    // without boundaries the network has no sections (Create's passive group)
    SignalSystem none;
    none.rebuild(r, {});
    EXPECT_EQ(none.groupCount(), 0u);
    EXPECT_EQ(none.groupAt(r, b1), kNoGroup);
}

TEST(SignalSections, OccupiedSectionsTurnTheirSignalsRed) {
    World    w;
    Railways r;
    lineZ(w, r, 0, 29);
    SignalSystem s;
    s.rebuild(r, {boundary(r, "a", at(r, {0.5, 0, 10.5}, {0, 0, 1})), boundary(r, "b", at(r, {0.5, 0, 20.5}, {0, 0, 1}))});
    EXPECT_EQ(s.state("a", 1), SignalState::Green);
    EXPECT_EQ(s.state("a", 0), SignalState::Invalid); // no signal block for that direction

    // a 6-block train in the middle section
    s.setTrainSpans("t", spanBehind(r, at(r, {0.5, 0, 18}, {0, 0, 1}), 6));
    s.updateStates();
    EXPECT_EQ(s.state("a", 1), SignalState::Red);
    EXPECT_EQ(s.state("b", 1), SignalState::Green);
    EXPECT_EQ(s.trainsIn(s.groupOf("a", 1)), std::vector<std::string>{"t"});

    // straddling a boundary occupies both sections
    s.setTrainSpans("t", spanBehind(r, at(r, {0.5, 0, 23}, {0, 0, 1}), 6));
    s.updateStates();
    EXPECT_EQ(s.trainGroups("t").size(), 2u);
    EXPECT_EQ(s.state("a", 1), SignalState::Red);
    EXPECT_EQ(s.state("b", 1), SignalState::Red);

    // redstone forces a signal red
    s.removeTrain("t");
    auto sides         = s.boundary("a")->sides;
    sides[1].forcedRed = true;
    s.setSides("a", sides);
    s.updateStates();
    EXPECT_EQ(s.state("a", 1), SignalState::Red);
    EXPECT_EQ(s.state("b", 1), SignalState::Green);
}

TEST(SignalSections, EntrySignalsReserveForOneTrain) {
    World    w;
    Railways r;
    lineZ(w, r, 0, 39);
    SignalSystem s;
    // a section from z 10.5 to z 20.5 guarded in both directions
    auto north = at(r, {0.5, 0, 20.5}, {0, 0, -1});
    s.rebuild(r, {boundary(r, "a", at(r, {0.5, 0, 10.5}, {0, 0, 1})), boundary(r, "b", north)});
    ASSERT_EQ(s.groupOf("a", 1), s.groupOf("b", 1));

    // train A heading north is 5 blocks before signal a
    auto frontA = at(r, {0.5, 0, 5.5}, {0, 0, 1});
    s.setTrainSpans("A", spanBehind(r, frontA, 4));
    auto a = s.checkAhead(r, "A", frontA, pathTo(r, frontA, {0.5, 0, 38}, {0, 0, 1}), 12);
    EXPECT_TRUE(a.clear);
    ASSERT_EQ(a.reserved.size(), 1u);
    EXPECT_EQ(a.reserved[0], s.groupOf("a", 1));
    s.updateStates();
    EXPECT_EQ(s.state("a", 1), SignalState::Green); // reserved through itself
    EXPECT_EQ(s.state("b", 1), SignalState::Red);   // reserved through another signal

    // train B heading south must wait at signal b
    auto frontB = at(r, {0.5, 0, 25.5}, {0, 0, -1});
    s.setTrainSpans("B", spanBehind(r, frontB, 4));
    auto b = s.checkAhead(r, "B", frontB, pathTo(r, frontB, {0.5, 0, 1}, {0, 0, -1}), 12);
    EXPECT_FALSE(b.clear);
    EXPECT_EQ(b.waitingFor, "b");
    EXPECT_NEAR(b.stopDistance, 5, 1e-6);

    // A checks again (keeps its reservation) and enters: the reservation turns into occupancy
    a = s.checkAhead(r, "A", frontA, pathTo(r, frontA, {0.5, 0, 38}, {0, 0, 1}), 12);
    EXPECT_TRUE(a.clear);
    auto inside = at(r, {0.5, 0, 12}, {0, 0, 1});
    s.setTrainSpans("A", spanBehind(r, inside, 4));
    EXPECT_FALSE(s.reservation(s.groupOf("a", 1)).has_value());
    s.updateStates();
    EXPECT_EQ(s.state("a", 1), SignalState::Red);
    EXPECT_FALSE(s.checkAhead(r, "B", frontB, pathTo(r, frontB, {0.5, 0, 1}, {0, 0, -1}), 12).clear);

    // A leaves the section: B may go
    auto beyond = at(r, {0.5, 0, 30}, {0, 0, 1});
    s.setTrainSpans("A", spanBehind(r, beyond, 4));
    b = s.checkAhead(r, "B", frontB, pathTo(r, frontB, {0.5, 0, 1}, {0, 0, -1}), 12);
    EXPECT_TRUE(b.clear);

    // signals beyond the look-ahead are not checked yet
    s.removeTrain("B");
    s.setTrainSpans("A", spanBehind(r, inside, 4));
    auto far = at(r, {0.5, 0, 2}, {0, 0, 1});
    s.setTrainSpans("C", spanBehind(r, far, 1));
    EXPECT_TRUE(s.checkAhead(r, "C", far, pathTo(r, far, {0.5, 0, 38}, {0, 0, 1}), 4).clear);
    EXPECT_FALSE(s.checkAhead(r, "C", far, pathTo(r, far, {0.5, 0, 38}, {0, 0, 1}), 12).clear);
}

namespace {
/// A crossing at (0, 0, 10): a north-south line z 0..20 and an east-west line x -10..10.
void crossing(World& w, Railways& r) {
    lineZ(w, r, 0, 20);
    for (int x = -10; x <= 10; ++x)
        if (x != 0) w.place(r, {x, 0, 10}, Shape::XO);
    w.shapes[{0, {0, 0, 10}}] = Shape::CR_O;
    r.onRailAdded(w, 0, {0, 0, 10});
}
} // namespace

TEST(SignalSections, CrossSignalsChainThroughJunctions) {
    World    w;
    Railways r;
    crossing(w, r);
    ASSERT_EQ(r.graphs().size(), 1u);
    SignalSystem s;
    // cross signals into the crossing from the south and the west, entry signals out of it
    s.rebuild(
        r,
        {boundary(r, "south", at(r, {0.5, 0, 6.5}, {0, 0, 1}), SignalType::Cross),
         boundary(r, "west", at(r, {-4.5, 0, 10.5}, {1, 0, 0}), SignalType::Cross),
         boundary(r, "north", at(r, {0.5, 0, 14.5}, {0, 0, 1})),
         boundary(r, "east", at(r, {5.5, 0, 10.5}, {1, 0, 0}))}
    );
    GroupId junction = s.groupOf("south", 1);
    EXPECT_EQ(s.groupOf("west", 1), junction);
    EXPECT_EQ(s.groupOf("north", 0), junction);
    EXPECT_EQ(s.groupOf("east", 0), junction);
    EXPECT_EQ(s.groupCount(), 5u);
    // going north from the south signal leads out through the north signal only (no 90° turns)
    auto ex = s.exits("south", 1);
    ASSERT_EQ(ex.size(), 1u);
    EXPECT_EQ(ex[0], (std::pair<std::string, int>{"north", 1}));

    // a train standing north of the crossing: the only exit is red, so the cross signal is red
    // too (Create's resolveSignalChain: red when no chained signal is free, yellow when mixed)
    auto parked = at(r, {0.5, 0, 19}, {0, 0, 1});
    s.setTrainSpans("parked", spanBehind(r, parked, 3));
    s.updateStates();
    EXPECT_EQ(s.state("north", 1), SignalState::Red);
    EXPECT_EQ(s.state("south", 1), SignalState::Red);
    EXPECT_EQ(s.state("west", 1), SignalState::Green); // its way out (east) is free

    // a northbound train waits at the cross signal, not inside the crossing
    auto front = at(r, {0.5, 0, 3}, {0, 0, 1});
    s.setTrainSpans("t", spanBehind(r, front, 2));
    auto path = pathTo(r, front, {0.5, 0, 17}, {0, 0, 1});
    auto a    = s.checkAhead(r, "t", front, path, 8);
    EXPECT_FALSE(a.clear);
    EXPECT_EQ(a.waitingFor, "south");
    EXPECT_NEAR(a.stopDistance, 3.5, 1e-6);
    EXPECT_TRUE(a.reserved.empty());

    // once the exit is free it reserves the crossing and the section after it
    s.removeTrain("parked");
    a = s.checkAhead(r, "t", front, path, 8);
    EXPECT_TRUE(a.clear);
    EXPECT_EQ(a.reserved.size(), 2u);
    s.updateStates();
    EXPECT_EQ(s.state("south", 1), SignalState::Green);
    EXPECT_EQ(s.state("west", 1), SignalState::Red); // the crossing is reserved through another signal

    // an eastbound train now waits at the west signal
    auto east = at(r, {-8, 0, 10.5}, {1, 0, 0});
    s.setTrainSpans("e", spanBehind(r, east, 1));
    auto b = s.checkAhead(r, "e", east, pathTo(r, east, {9, 0, 10.5}, {1, 0, 0}), 8);
    EXPECT_FALSE(b.clear);
    EXPECT_EQ(b.waitingFor, "west");

    // giving up releases the reservations
    s.releaseReservations("t");
    EXPECT_TRUE(s.checkAhead(r, "e", east, pathTo(r, east, {9, 0, 10.5}, {1, 0, 0}), 8).clear);
}

TEST(SignalSections, CrossingCurvesLinkTheirSections) {
    World    w;
    Railways r;
    // a quarter turn from (0,0,0) heading south to (10,0,10) heading east...
    PlacementRequest req;
    req.from   = selectEnd({0, 0, 0}, Shape::ZO, {0, 0, 1});
    req.pos2   = {10, 0, 10};
    req.shape2 = Shape::XO;
    req.look   = {1, 0, 0};
    auto plan  = planConnection(req, Shape::ZO);
    ASSERT_TRUE(plan.valid);
    w.curves.add(0, *plan.curve);
    w.place(r, {0, 0, 0}, Shape::ZO);
    for (auto const& p : plan.straight) w.place(r, p.pos, p.shape);
    w.place(r, {10, 0, 10}, Shape::XO);
    for (int x = 11; x <= 16; ++x) w.place(r, {x, 0, 10}, Shape::XO);
    // ...and a separate north-south line crossing it at x = 6
    lineZ(w, r, -2, 14, 6);
    ASSERT_EQ(r.graphs().size(), 2u);

    SignalSystem s;
    s.rebuild(
        r,
        {boundary(r, "curve", at(r, {13.5, 0, 10.5}, {1, 0, 0})), boundary(r, "line", at(r, {6.5, 0, 12.5}, {0, 0, 1}))}
    );
    GroupId curveSide = s.groupOf("curve", 0);
    GroupId lineSide  = s.groupOf("line", 0);
    ASSERT_NE(curveSide, kNoGroup);
    ASSERT_NE(lineSide, kNoGroup);
    auto const& linked = s.linked(lineSide);
    EXPECT_NE(std::find(linked.begin(), linked.end(), curveSide), linked.end());
    EXPECT_EQ(s.linked(s.groupOf("line", 1)).size(), 1u);

    // a train on the curve's section turns the line's signal into that section red
    auto sides         = s.boundary("line")->sides;
    sides[0].hasSignal = true;
    s.setSides("line", sides);
    auto front = at(r, {1, 0, 1}, {0, 0, 1});
    s.setTrainSpans("t", spanBehind(r, front, 0.5));
    s.updateStates();
    EXPECT_EQ(s.state("line", 0), SignalState::Red);
    EXPECT_EQ(s.state("line", 1), SignalState::Green);
}

TEST(SignalSections, ObserversSeeTrainsOverThem) {
    World    w;
    Railways r;
    lineZ(w, r, 0, 19);
    SignalSystem s;
    auto         loc = at(r, {0.5, 0, 8.5}, {0, 0, 1});
    Graph*       g   = r.graph(loc.graph);
    Observer     o{"obs", g->node(loc.node1)->location, g->node(loc.node2)->location, loc.position};
    s.rebuild(r, {}, {o});
    EXPECT_TRUE(s.trainsOver("obs").empty());
    s.setTrainSpans("t", spanBehind(r, at(r, {0.5, 0, 6}, {0, 0, 1}), 3));
    EXPECT_TRUE(s.trainsOver("obs").empty());
    s.setTrainSpans("t", spanBehind(r, at(r, {0.5, 0, 10}, {0, 0, 1}), 3));
    EXPECT_EQ(s.trainsOver("obs"), std::vector<std::string>{"t"});
    // reported in the other direction along the same edge
    s.setTrainSpans("t", spanBehind(r, at(r, {0.5, 0, 7}, {0, 0, -1}), 3));
    EXPECT_EQ(s.trainsOver("obs"), std::vector<std::string>{"t"});
}

TEST(SignalSections, PathPenaltiesAndRebuildsKeepTrains) {
    World    w;
    Railways r;
    lineZ(w, r, 0, 29);
    SignalSystem s;
    auto         la = at(r, {0.5, 0, 10.5}, {0, 0, 1});
    s.rebuild(r, {boundary(r, "a", la)});
    s.setTrainSpans("t", spanBehind(r, at(r, {0.5, 0, 20}, {0, 0, 1}), 4));
    Graph* g = r.graph(la.graph);
    auto   n1 = g->node(la.node1)->location, n2 = g->node(la.node2)->location;
    EXPECT_EQ(s.edgePenalty(n1, n2, "other", 25, 400), 25);
    EXPECT_EQ(s.edgePenalty(n1, n2, "t", 25, 400), 0);
    EXPECT_EQ(s.edgePenalty(n2, n1, "other", 25, 400), 0); // no signal that way

    // a second boundary: the train is re-mapped to the new sections
    s.rebuild(r, {boundary(r, "a", la), boundary(r, "b", at(r, {0.5, 0, 25.5}, {0, 0, 1}))});
    EXPECT_EQ(s.trainGroups("t"), std::set<GroupId>{s.groupOf("a", 1)});
    s.updateStates();
    EXPECT_EQ(s.state("a", 1), SignalState::Red);
    EXPECT_EQ(s.state("b", 1), SignalState::Green);
    auto sides         = s.boundary("a")->sides;
    sides[1].forcedRed = true;
    s.setSides("a", sides);
    EXPECT_EQ(s.edgePenalty(n1, n2, "t", 25, 400), 400);
}

TEST(SignalSections, PathsDontPassASignalFromBehind) {
    // Create's SignalBoundary.canNavigateVia: no signal faces trains going south here
    World    w;
    Railways r;
    lineZ(w, r, 0, 29);
    SignalSystem s;
    s.rebuild(r, {boundary(r, "a", at(r, {0.5, 0, 10.5}, {0, 0, 1}))});
    auto query = [&](GraphLocation start, GraphLocation dest) {
        PathQuery q;
        q.start        = start;
        q.destinations = {dest};
        q.edgeCost     = [&](Graph const& g, Edge const& e) {
            double weight = 25;
            return s.edgeCost(g.node(e.node1)->location, g.node(e.node2)->location, "t", weight, 400);
        };
        return r.findPath(q);
    };
    auto south = [&](double z) { return at(r, {0.5, 0, z}, {0, 0, -1}); };
    auto north = [&](double z) { return at(r, {0.5, 0, z}, {0, 0, 1}); };
    EXPECT_FALSE(query(south(25), south(3)).has_value()) << "behind the signal";
    EXPECT_TRUE(query(south(25), south(15)).has_value()) << "a destination before the signal is reached";
    EXPECT_TRUE(query(north(3), north(25)).has_value()) << "the signal faces northbound trains";
    // without the barrier (old edgePenalty) the way exists
    PathQuery plain;
    plain.start        = south(25);
    plain.destinations = {south(3)};
    EXPECT_TRUE(r.findPath(plain).has_value());
}

TEST(SignalSections, RedSignalWeightHalvesPerRedSignal) {
    // Create's Navigation.search: one signalWeight for the whole search, halved after each red signal
    World    w;
    Railways r;
    lineZ(w, r, 0, 39);
    SignalSystem s;
    // three boundaries on one graph edge (long straight lines are split into edges)
    auto la = at(r, {0.5, 0, 3.5}, {0, 0, 1});
    s.rebuild(r, {boundary(r, "a", la), boundary(r, "b", at(r, {0.5, 0, 8.5}, {0, 0, 1})), boundary(r, "c", at(r, {0.5, 0, 13.5}, {0, 0, 1}))});
    s.setTrainSpans("x", spanBehind(r, at(r, {0.5, 0, 7}, {0, 0, 1}), 2));
    s.setTrainSpans("y", spanBehind(r, at(r, {0.5, 0, 12}, {0, 0, 1}), 2));
    Graph* g  = r.graph(la.graph);
    auto   n1 = g->node(la.node1)->location, n2 = g->node(la.node2)->location;
    ASSERT_GE(n2.location().z - n1.location().z, 14) << "the boundaries share an edge";
    double weight = 40;
    auto   cost   = s.edgeCost(n1, n2, "t", weight, 400);
    EXPECT_DOUBLE_EQ(cost.penalty, 40 + 20);
    EXPECT_DOUBLE_EQ(weight, 10);
    EXPECT_EQ(cost.barrier, std::numeric_limits<double>::infinity());
    // travelling south every boundary is passed from behind: the first one stops the way
    weight    = 40;
    auto back = s.edgeCost(n2, n1, "t", weight, 400);
    EXPECT_NEAR(back.barrier, n2.location().z - 13.5, 1e-6);
}
