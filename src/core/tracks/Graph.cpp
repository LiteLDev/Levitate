#include "core/tracks/Graph.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <queue>
#include <random>
#include <unordered_set>

namespace lv::core::tracks {

// ================================================================================================
// locations
// ================================================================================================

NodeLocation NodeLocation::of(Vec3d const& v, int dim) {
    NodeLocation l;
    // Java: Mth.floor((double) Math.round(x * 2)), Mth.floor(y) * 2
    l.x2  = int(std::floor(v.x * 2 + 0.5));
    l.y2  = int(std::floor(v.y)) * 2;
    l.z2  = int(std::floor(v.z * 2 + 0.5));
    l.dim = dim;
    return l;
}

Vec3d NodeLocation::location() const { return {x2 / 2.0, y2 / 2.0 + yOffsetPixels / 16.0, z2 / 2.0}; }

std::vector<BlockPos3> NodeLocation::allAdjacent() const {
    std::vector<BlockPos3> out;
    Vec3d                  v = location() - Vec3d{0, yOffsetPixels / 16.0, 0};
    double const           step = 1 / 8.0;
    for (int x : {1, -1})
        for (int y : {1, -1})
            for (int z : {1, -1}) {
                BlockPos3 p = containing(v + Vec3d{x * step, y * step, z * step});
                if (std::none_of(out.begin(), out.end(), [&](BlockPos3 const& q) { return q == p; })) out.push_back(p);
            }
    return out;
}

std::string NodeLocation::str() const {
    auto half = [](int v) {
        std::string s = std::to_string(v / 2);
        if (v % 2 != 0) s = (v < 0 && v / 2 == 0 ? "-" : "") + s + ".5";
        return s;
    };
    return std::to_string(dim) + ":" + half(x2) + "," + std::to_string(y2 / 2) + "," + half(z2);
}

bool DiscoveredLocation::notInLineWith(Vec3d const& dir) const {
    return direction && std::max(dir.dot(*direction), dir.dot(-*direction)) < 7 / 8.0;
}

namespace {

bool sameLocation(NodeLocation const& a, NodeLocation const& b) { return a == b; }

/// Create's ITrackBlock.addToListIfConnected.
template <class OffsetFn, class NormalFn, class DimFn>
void addToListIfConnected(
    NodeLocation const*                 fromEnd,
    std::vector<DiscoveredLocation>&    list,
    OffsetFn const&                     offset,
    NormalFn const&                     normal,
    DimFn const&                        dimension,
    std::optional<Vec3d> const&         axis,
    std::shared_ptr<Curve const> const& viaTurn,
    std::string const&                  material
) {
    auto make = [&](bool b) {
        DiscoveredLocation d;
        static_cast<NodeLocation&>(d) = NodeLocation::of(offset(0.5, b), dimension(b));
        d.turn                        = viaTurn;
        d.forceNode                   = viaTurn != nullptr;
        d.materialA = d.materialB = material;
        d.normal                      = normal(b);
        if (axis) d.direction = axis->normalize();
        return d;
    };
    DiscoveredLocation first  = make(true);
    DiscoveredLocation second = make(false);
    if (first.dim != second.dim) {
        first.forceNode  = true;
        second.forceNode = true;
    }
    bool skipFirst = false, skipSecond = false;
    if (fromEnd) {
        bool equalsFirst  = sameLocation(first, *fromEnd);
        bool equalsSecond = sameLocation(second, *fromEnd);
        if (!equalsFirst && !equalsSecond) return; // not reachable from this end
        skipFirst  = equalsFirst;
        skipSecond = equalsSecond;
    }
    if (!skipFirst) list.push_back(std::move(first));
    if (!skipSecond) list.push_back(std::move(second));
}

double elevationAtCenter(Shape s) { return isSlope(s) ? .5 : 0; }

} // namespace

std::vector<DiscoveredLocation> connectedEnds(
    TrackWorld const&         world,
    int                       dim,
    BlockPos3 const&          pos,
    TrackBlockView const&     view,
    bool                      linear,
    DiscoveredLocation const* connectedTo
) {
    NodeLocation const*             from = connectedTo;
    std::vector<DiscoveredLocation> list;
    {
        Shape const                     shape  = view.shape;
        auto const&                     axes   = shapeAxes(shape);
        Vec3d const                     center = pos.bottomCenter() + Vec3d{0, elevationAtCenter(shape), 0};
        auto                            normal = [&](bool) { return shapeNormal(shape); };
        auto                            sameDim = [&](bool) { return dim; };

        if (axes.size() > 1) {
            for (auto const& axis : axes) {
                for (bool fromCenter : {true, false}) {
                    addToListIfConnected(
                        from,
                        list,
                        [&](double d, bool b) { return axis * (b ? 0 : fromCenter ? -d : d) + center; },
                        normal,
                        sameDim,
                        axis,
                        nullptr,
                        view.material
                    );
                }
            }
        } else {
            for (auto const& axis : axes) {
                addToListIfConnected(
                    from,
                    list,
                    [&](double d, bool b) { return axis * (b ? d : -d) + center; },
                    normal,
                    sameDim,
                    axis,
                    nullptr,
                    view.material
                );
            }
        }

        if (linear) return list;
        for (auto const& bc : view.curves) {
            addToListIfConnected(
                from,
                list,
                [&](double, bool b) { return bc->spec().starts[b ? 0 : 1]; },
                [&](bool b) { return bc->spec().normals[b ? 0 : 1]; },
                sameDim,
                std::nullopt,
                bc,
                bc->spec().material
            );
        }

        if (view.portalLink) {
            auto [otherDim, boundPos] = *view.portalLink;
            auto bound                = world.trackAt(otherDim, boundPos);
            if (bound) {
                Vec3d boundCenter = boundPos.bottomCenter() + Vec3d{0, elevationAtCenter(bound->shape), 0};
                Vec3d boundAxis   = shapeAxes(bound->shape)[0];
                for (auto const& axis : axes) {
                    addToListIfConnected(
                        from,
                        list,
                        [&](double d, bool b) { return (b ? axis : boundAxis) * d + (b ? center : boundCenter); },
                        [&](bool b) { return b ? shapeNormal(shape) : shapeNormal(bound->shape); },
                        [&](bool b) { return b ? dim : otherDim; },
                        axis,
                        nullptr,
                        view.material
                    );
                }
            }
        }
    }
    return list;
}

std::vector<DiscoveredLocation> walkConnectedTracks(TrackWorld const& world, NodeLocation const& location, bool linear) {
    std::vector<DiscoveredLocation> list;
    DiscoveredLocation              from;
    static_cast<NodeLocation&>(from) = location;
    for (auto const& pos : location.allAdjacent()) {
        auto view = world.trackAt(location.dim, pos);
        if (!view) continue;
        auto found = connectedEnds(world, location.dim, pos, *view, linear, &from);
        list.insert(list.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    return list;
}

bool isValidGraphNodeLocation(DiscoveredLocation const& location, std::vector<DiscoveredLocation> const& next, bool first) {
    int size = int(next.size()) - (first ? 1 : 0);
    if (size != 1) return true;
    if (location.forceNode) return true;
    if (location.differentMaterials()) return true;
    for (auto const& n : next)
        if (n.forceNode) return true;
    if (location.direction) {
        for (auto const& n : next)
            if (n.notInLineWith(*location.direction)) return true;
    }
    Vec3d vec       = location.location();
    bool  centeredX = !nearlyEqual(vec.x, std::round(vec.x));
    bool  centeredZ = !nearlyEqual(vec.z, std::round(vec.z));
    // Java: (int) Math.round(v) % 16 == 0 (Math.round = floor(v + 0.5))
    if (centeredX && !centeredZ) return (long long)(std::floor(vec.z + 0.5)) % 16 == 0;
    return (long long)(std::floor(vec.x + 0.5)) % 16 == 0;
}

// ================================================================================================
// graph
// ================================================================================================

Graph::Graph(std::string id, int netId) : mId(std::move(id)), mNetId(netId) {}

Node const* Graph::locateNode(NodeLocation const& loc) const {
    auto it = mNodesByLocation.find(loc);
    return it == mNodesByLocation.end() ? nullptr : node(it->second);
}

Node const* Graph::node(NodeId id) const {
    auto it = mNodes.find(id);
    return it == mNodes.end() ? nullptr : &it->second;
}

std::vector<Node const*> Graph::nodes() const {
    std::vector<Node const*> out;
    out.reserve(mNodes.size());
    for (auto const& [id, n] : mNodes) out.push_back(&n);
    return out;
}

std::map<NodeId, Edge> const& Graph::connectionsFrom(NodeId id) const {
    static std::map<NodeId, Edge> const empty;
    auto                                it = mConnections.find(id);
    return it == mConnections.end() ? empty : it->second;
}

Edge const* Graph::edge(NodeId from, NodeId to) const {
    auto const& c  = connectionsFrom(from);
    auto        it = c.find(to);
    return it == c.end() ? nullptr : &it->second;
}

size_t Graph::edgeCount() const {
    size_t n = 0;
    for (auto const& [id, m] : mConnections) n += m.size();
    return n / 2;
}

bool Graph::addNodeIfAbsent(Node n) {
    if (mNodesByLocation.contains(n.location)) return false;
    mNodesByLocation[n.location] = n.id;
    mNodes[n.id]                 = std::move(n);
    return true;
}

void Graph::addNode(Node n) {
    if (mNodesByLocation.contains(n.location)) removeNode(n.location, nullptr);
    mNodesByLocation[n.location] = n.id;
    mNodes[n.id]                 = std::move(n);
}

bool Graph::removeNode(NodeLocation const& loc, std::vector<EdgePoint>* /*unused*/) {
    auto it = mNodesByLocation.find(loc);
    if (it == mNodesByLocation.end()) return false;
    NodeId id = it->second;
    mNodesByLocation.erase(it);
    mNodes.erase(id);
    auto cit = mConnections.find(id);
    if (cit == mConnections.end()) return true;
    std::map<NodeId, Edge> connections = std::move(cit->second);
    mConnections.erase(cit);
    for (auto const& [other, e] : connections) {
        auto oit = mConnections.find(other);
        if (oit != mConnections.end()) oit->second.erase(id);
    }
    return true;
}

void Graph::putConnection(NodeId a, NodeId b, Edge e) { mConnections[a][b] = std::move(e); }

void Graph::transferNode(Node const& n, Graph& target) {
    target.addNode(n);
    auto cit = mConnections.find(n.id);
    if (cit != mConnections.end()) {
        target.mConnections[n.id] = std::move(cit->second);
        mConnections.erase(cit);
    }
    mNodesByLocation.erase(n.location);
    mNodes.erase(n.id);
}

std::vector<std::unique_ptr<Graph>> Graph::splitDisconnected(Railways& rail) {
    std::vector<std::unique_ptr<Graph>> discovered;
    std::set<NodeId>                    vertices;
    for (auto const& [id, n] : mNodes) vertices.insert(id);
    bool firstComponent = true;
    while (!vertices.empty()) {
        NodeId start = *vertices.begin();
        vertices.erase(vertices.begin());
        std::vector<NodeId> component{start};
        std::deque<NodeId>  frontier{start};
        while (!frontier.empty()) {
            NodeId cur = frontier.front();
            frontier.pop_front();
            for (auto const& [other, e] : connectionsFrom(cur)) {
                if (vertices.erase(other)) {
                    frontier.push_back(other);
                    component.push_back(other);
                }
            }
        }
        if (firstComponent) {
            firstComponent = false;
            continue;
        }
        auto target = std::make_unique<Graph>(rail.newGraphId(), ++rail.mGraphNetCounter);
        for (NodeId id : component) {
            Node copy = mNodes.at(id);
            transferNode(copy, *target);
        }
        discovered.push_back(std::move(target));
    }
    return discovered;
}

double Graph::edgeLength(Edge const& e) const {
    if (e.interDimensional) return 0;
    if (e.turn) return e.turn->length();
    auto const* a = node(e.node1);
    auto const* b = node(e.node2);
    if (!a || !b) return 0;
    return a->location.location().distanceTo(b->location.location());
}

Vec3d Graph::edgePosition(Edge const& e, double t) const {
    if (e.turn) return e.turn->position(std::clamp(t, 0.0, 1.0));
    auto const* a = node(e.node1);
    auto const* b = node(e.node2);
    if (!a || !b) return {};
    return lerp(t, a->location.location(), b->location.location());
}

Vec3d Graph::edgePositionAt(Edge const& e, double distance) const {
    if (e.turn) return e.turn->position(e.turn->tAtDistance(distance));
    double len = edgeLength(e);
    return edgePosition(e, len <= 0 ? 0 : std::clamp(distance / len, 0.0, 1.0));
}

Vec3d Graph::edgeNormal(Edge const& e, double t) const {
    if (e.turn) return e.turn->normal(std::clamp(t, 0.0, 1.0));
    auto const* a = node(e.node1);
    return a ? a->normal : Vec3d{0, 1, 0};
}

Vec3d Graph::edgeDirection(Edge const& e, bool fromFirst) const {
    return (edgePosition(e, fromFirst ? 0.25 : 1) - edgePosition(e, fromFirst ? 0 : 0.75)).normalize();
}

bool Graph::canTravelTo(Edge const& e, Edge const& next) const {
    if (e.interDimensional || next.interDimensional) return true;
    return edgeDirection(e, false).dot(edgeDirection(next, true)) > 7 / 8.0;
}

// ================================================================================================
// railways
// ================================================================================================

Railways::Railways() : mIdSeed(std::random_device{}()) {}

std::string Railways::newGraphId() {
    static char const hex[] = "0123456789abcdef";
    std::mt19937_64   rng(mIdSeed + (++mVersion) * 0x9E3779B97F4A7C15ull);
    uint64_t          a = rng(), b = rng();
    std::string       s;
    for (int i = 0; i < 16; ++i) s += hex[(a >> (i * 4)) & 15];
    for (int i = 0; i < 16; ++i) s += hex[(b >> (i * 4)) & 15];
    return s;
}

std::vector<Graph*> Railways::graphs() const {
    std::vector<Graph*> out;
    for (auto const& [id, g] : mGraphs) out.push_back(g.get());
    return out;
}

Graph* Railways::graph(std::string const& id) const {
    auto it = mGraphs.find(id);
    return it == mGraphs.end() ? nullptr : it->second.get();
}

Graph* Railways::graphAt(NodeLocation const& loc) const {
    for (auto const& [id, g] : mGraphs)
        if (g->locateNode(loc)) return g.get();
    return nullptr;
}

void Railways::clear() {
    mGraphs.clear();
    mPoints.clear();
    mPending.clear();
    ++mVersion;
}

Graph& Railways::newGraph(std::string id) {
    if (id.empty()) id = newGraphId();
    auto  g   = std::make_unique<Graph>(id, ++mGraphNetCounter);
    auto& ref = *g;
    mGraphs[id] = std::move(g);
    ++mVersion;
    return ref;
}

void Railways::removeGraph(Graph* g) {
    if (!g) return;
    mGraphs.erase(g->id());
    ++mVersion;
}

bool Railways::createNodeIfAbsent(Graph& g, DiscoveredLocation const& loc) {
    Node n;
    n.id       = nextNodeId();
    n.location = loc;
    n.normal   = loc.normal;
    if (!g.addNodeIfAbsent(n)) {
        --mNodeIdCounter; // id not used
        return false;
    }
    ++mVersion;
    return true;
}

void Railways::connectNodes(Graph& g, DiscoveredLocation const& a, DiscoveredLocation const& b, std::shared_ptr<Curve const> turn) {
    auto const* n1 = g.locateNode(a);
    auto const* n2 = g.locateNode(b);
    if (!n1 || !n2) return;
    Edge e;
    e.node1            = n1->id;
    e.node2            = n2->id;
    e.turn             = turn;
    e.interDimensional = n1->location.dim != n2->location.dim;
    e.material         = turn ? turn->spec().material : b.materialA;
    Edge r             = e;
    r.node1            = n2->id;
    r.node2            = n1->id;
    r.turn             = turn ? std::make_shared<Curve const>(turn->secondary()) : nullptr;
    g.putConnection(n1->id, n2->id, std::move(e));
    g.putConnection(n2->id, n1->id, std::move(r));
    ++mVersion;
}

void Railways::updateSplitGraph(Graph& g) {
    auto parts = g.splitDisconnected(*this);
    for (auto& p : parts) {
        std::string id = p->id();
        mGraphs[id]    = std::move(p);
    }
    if (!parts.empty()) ++mVersion;
}

namespace {
struct FrontierEntry {
    std::optional<DiscoveredLocation> parent;
    std::optional<DiscoveredLocation> prev;
    DiscoveredLocation                current;
    bool                              isStart = false;
};

void removeFirst(std::vector<DiscoveredLocation>& v, NodeLocation const& loc) {
    auto it = std::find_if(v.begin(), v.end(), [&](DiscoveredLocation const& d) { return sameLocation(d, loc); });
    if (it != v.end()) v.erase(it);
}

std::vector<Graph*> graphsWith(Railways const& r, NodeLocation const& loc) {
    std::vector<Graph*> out;
    for (auto* g : r.graphs())
        if (g->locateNode(loc)) out.push_back(g);
    return out;
}
} // namespace

Graph* Railways::onRailAdded(TrackWorld const& world, int dim, BlockPos3 const& pos) {
    auto view = world.trackAt(dim, pos);
    if (!view) return nullptr;

    // points on edges touched by this update: re-attached at the end if their edge still exists
    std::vector<EdgePoint> detached;
    auto                   detachAround = [&](Graph& g, NodeLocation const& loc) {
        Node const* n = g.locateNode(loc);
        if (!n) return;
        for (auto const& [other, e] : g.connectionsFrom(n->id)) {
            Node const* o = g.node(other);
            if (!o) continue;
            for (auto* p : pointsOn(n->location, o->location)) detached.push_back(*p);
        }
    };

    // 1. remove all immediately reachable node locations
    std::deque<FrontierEntry>                                    frontier;
    std::unordered_set<NodeLocation, NodeLocationHash>           visited;
    std::vector<Graph*>                                          connected;
    for (auto& initial : connectedEnds(world, dim, pos, *view, false, nullptr)) {
        frontier.push_back({std::nullopt, std::nullopt, std::move(initial)});
    }
    int emergencyExit = 1000;
    while (!frontier.empty()) {
        if (emergencyExit-- == 0) break;
        FrontierEntry entry = std::move(frontier.front());
        frontier.pop_front();
        auto intersecting = graphsWith(*this, entry.current);
        for (auto* g : intersecting) {
            detachAround(*g, entry.current);
            g->removeNode(entry.current, nullptr);
            ++mVersion;
            if (std::find(connected.begin(), connected.end(), g) == connected.end()) connected.push_back(g);
        }
        if (!intersecting.empty()) continue;
        auto ends = walkConnectedTracks(world, entry.current, false);
        if (entry.prev) removeFirst(ends, *entry.prev);
        for (auto& loc : ends)
            if (visited.insert(loc).second) frontier.push_back({std::nullopt, entry.current, loc});
    }
    frontier.clear();
    visited.clear();

    // remove empty graphs
    for (auto it = connected.begin(); it != connected.end();) {
        if (!(*it)->empty() || connected.size() == 1) {
            ++it;
            continue;
        }
        removeGraph(*it);
        it = connected.erase(it);
    }

    // merge graphs if more than one
    Graph* graph = nullptr;
    if (connected.size() > 1) {
        for (auto* other : connected) {
            if (!graph) {
                graph = other;
                continue;
            }
            for (auto const* n : other->nodes()) graph->addNodeIfAbsent(*n);
            for (auto const& [from, edges] : other->mConnections) {
                for (auto const& [to, e] : edges) graph->putConnection(from, to, e);
            }
            removeGraph(other);
        }
    } else if (connected.size() == 1) {
        graph = connected.front();
    } else {
        graph = &newGraph();
    }

    // 2. find the first graph node candidate nearby
    std::optional<DiscoveredLocation> startNode;
    for (auto& initial : connectedEnds(world, dim, pos, *view, true, nullptr)) {
        frontier.push_back({std::nullopt, std::nullopt, std::move(initial)});
    }
    emergencyExit = 1000;
    while (!frontier.empty()) {
        if (emergencyExit-- == 0) break;
        FrontierEntry entry = std::move(frontier.front());
        frontier.pop_front();
        auto ends  = walkConnectedTracks(world, entry.current, false);
        bool first = !entry.prev.has_value();
        if (!first) removeFirst(ends, *entry.prev);
        if (isValidGraphNodeLocation(entry.current, ends, first)) {
            startNode = entry.current;
            break;
        }
        for (auto& loc : ends)
            if (visited.insert(loc).second) frontier.push_back({std::nullopt, entry.current, loc});
    }
    frontier.clear();

    if (startNode) {
        createNodeIfAbsent(*graph, *startNode);
        frontier.push_back({*startNode, std::nullopt, *startNode, true});

        // 3. build up the graph via all connected nodes
        emergencyExit = 1000;
        while (!frontier.empty()) {
            if (emergencyExit-- == 0) break;
            FrontierEntry      entry  = std::move(frontier.front());
            DiscoveredLocation parent = entry.parent ? *entry.parent : entry.current;
            frontier.pop_front();
            auto ends  = walkConnectedTracks(world, entry.current, false);
            bool first = !entry.prev.has_value();
            if (!first) removeFirst(ends, *entry.prev);
            if (isValidGraphNodeLocation(entry.current, ends, first) && !entry.isStart) {
                bool nodeIsNew = createNodeIfAbsent(*graph, entry.current);
                connectNodes(*graph, parent, entry.current, entry.current.turn);
                parent = entry.current;
                if (!nodeIsNew) continue;
            }
            for (auto& loc : ends) frontier.push_back({parent, entry.current, loc});
        }
    }

    // re-attach points whose edge survived; invalidate the others
    std::vector<EdgePoint> lost;
    for (auto& p : detached) {
        if (!mPoints.contains(p.id)) continue;
        Graph* g  = graphAt(p.node1);
        auto*  n1 = g ? g->locateNode(p.node1) : nullptr;
        auto*  n2 = g ? g->locateNode(p.node2) : nullptr;
        auto*  e  = (n1 && n2) ? g->edge(n1->id, n2->id) : nullptr;
        if (!e || p.position > g->edgeLength(*e) + 1e-6) lost.push_back(p);
    }
    invalidatePoints(lost);
    ++mVersion;
    return graph;
}

void Railways::onRailRemoved(TrackWorld const& world, int dim, BlockPos3 const& pos, TrackBlockView const& view) {
    auto                   ends = connectedEnds(world, dim, pos, view, false, nullptr);
    std::vector<EdgePoint> removedPoints;

    // 1. remove any nodes this rail was part of
    for (auto const& removed : ends) {
        for (auto* g : graphsWith(*this, removed)) {
            Node const* n = g->locateNode(removed);
            if (n) {
                for (auto const& [other, e] : g->connectionsFrom(n->id)) {
                    Node const* o = g->node(other);
                    if (!o) continue;
                    for (auto* p : pointsOn(n->location, o->location)) removedPoints.push_back(*p);
                }
            }
            g->removeNode(removed, nullptr);
            ++mVersion;
            if (g->empty()) removeGraph(g);
        }
    }

    std::set<BlockPos3> positionsToUpdate;
    for (auto const& e : ends)
        for (auto const& p : e.allAdjacent()) positionsToUpdate.insert(p);

    // 2. re-run railAdded for any track that was disconnected from this track
    std::vector<std::string> toUpdate;
    for (auto const& p : positionsToUpdate) {
        if (p == pos) continue;
        if (Graph* g = onRailAdded(world, dim, p)) {
            if (std::find(toUpdate.begin(), toUpdate.end(), g->id()) == toUpdate.end()) toUpdate.push_back(g->id());
        }
    }

    // 3. ensure any affected graph gets checked for segmentation
    for (auto const& id : toUpdate)
        if (Graph* g = graph(id)) updateSplitGraph(*g);

    // points whose edge is gone
    std::vector<EdgePoint> lost;
    for (auto& p : removedPoints) {
        if (!mPoints.contains(p.id)) continue;
        Graph* g  = graphAt(p.node1);
        auto*  n1 = g ? g->locateNode(p.node1) : nullptr;
        auto*  n2 = g ? g->locateNode(p.node2) : nullptr;
        if (!n1 || !n2 || !g->edge(n1->id, n2->id)) lost.push_back(p);
    }
    invalidatePoints(lost);
    ++mVersion;
}

// ---- queries ------------------------------------------------------------------------------------

std::optional<GraphLocation> Railways::nearestEdge(int dim, Vec3d const& pos, double maxDistance) const {
    std::optional<GraphLocation> best;
    double                       bestDist = maxDistance;
    for (auto const& [gid, g] : mGraphs) {
        for (auto const& [from, edges] : g->mConnections) {
            for (auto const& [to, e] : edges) {
                if (from > to || e.interDimensional) continue;
                auto const* a = g->node(from);
                if (!a || a->location.dim != dim) continue;
                // walk the edge in small pieces and project onto each
                double len    = g->edgeLength(e);
                int    pieces = e.turn ? std::max(1, e.turn->segmentCount()) : 1;
                double walked = 0;
                Vec3d  prev   = g->edgePositionAt(e, 0);
                for (int i = 1; i <= pieces; ++i) {
                    Vec3d  cur  = e.turn ? e.turn->position(e.turn->segmentT(i)) : g->edgePositionAt(e, len);
                    Vec3d  seg  = cur - prev;
                    double sl   = seg.lengthSqr();
                    double f    = sl > 0 ? std::clamp((pos - prev).dot(seg) / sl, 0.0, 1.0) : 0;
                    Vec3d  near = prev + seg * f;
                    double d    = near.distanceTo(pos);
                    if (d < bestDist) {
                        bestDist = d;
                        best     = GraphLocation{gid, from, to, std::min(len, walked + std::sqrt(sl) * f)};
                    }
                    walked += std::sqrt(sl);
                    prev    = cur;
                }
            }
        }
    }
    return best;
}

std::optional<Vec3d> Railways::positionOf(GraphLocation const& loc) const {
    Graph* g = graph(loc.graph);
    if (!g) return std::nullopt;
    Edge const* e = g->edge(loc.node1, loc.node2);
    if (!e) return std::nullopt;
    return g->edgePositionAt(*e, loc.position);
}

GraphLocation Railways::advance(
    GraphLocation                                                                          loc,
    double                                                                                 distance,
    std::function<NodeId(Graph const&, Edge const&, std::vector<Edge const*> const&)> choose
) const {
    Graph* g = graph(loc.graph);
    if (!g) return loc;
    auto reverse = [&](GraphLocation l) {
        Edge const* e = g->edge(l.node1, l.node2);
        double      len = e ? g->edgeLength(*e) : 0;
        return GraphLocation{l.graph, l.node2, l.node1, len - l.position};
    };
    if (distance < 0) return reverse(advance(reverse(loc), -distance, choose));

    for (int guard = 0; guard < 10000; ++guard) {
        Edge const* e = g->edge(loc.node1, loc.node2);
        if (!e) return loc;
        double len = g->edgeLength(*e);
        if (loc.position + distance <= len) {
            loc.position += distance;
            return loc;
        }
        distance -= len - loc.position;
        std::vector<Edge const*> options;
        for (auto const& [to, next] : g->connectionsFrom(loc.node2)) {
            if (to == loc.node1) continue; // no U-turns (also not back through a portal)
            if (g->canTravelTo(*e, next)) options.push_back(&next);
        }
        if (options.empty()) {
            loc.position = len;
            return loc;
        }
        NodeId target = 0;
        if (choose) target = choose(*g, *e, options);
        Edge const* chosen = nullptr;
        for (auto* o : options)
            if (o->node2 == target) chosen = o;
        if (!chosen) {
            double bestDot = -2;
            Vec3d  dir     = g->edgeDirection(*e, false);
            for (auto* o : options) {
                double d = dir.dot(g->edgeDirection(*o, true));
                if (d > bestDot) {
                    bestDot = d;
                    chosen  = o;
                }
            }
        }
        loc = GraphLocation{loc.graph, chosen->node1, chosen->node2, 0};
    }
    return loc;
}

std::optional<PathResult> Railways::findPath(PathQuery const& q) const {
    Graph* g = graph(q.start.graph);
    if (!g) return std::nullopt;
    Edge const* startEdge = g->edge(q.start.node1, q.start.node2);
    if (!startEdge) return std::nullopt;

    struct DestInfo {
        size_t index;
        Vec3d  position;
        int    dim;
    };
    std::map<std::pair<NodeId, NodeId>, std::vector<std::pair<size_t, double>>> destByEdge;
    std::vector<DestInfo>                                                         destPositions;
    for (size_t i = 0; i < q.destinations.size(); ++i) {
        auto const& d = q.destinations[i];
        if (d.graph != q.start.graph) continue;
        destByEdge[{d.node1, d.node2}].push_back({i, d.position});
        if (auto p = positionOf(d)) {
            auto const* n = g->node(d.node1);
            destPositions.push_back({i, *p, n ? n->location.dim : 0});
        }
    }
    if (destByEdge.empty()) return std::nullopt;

    auto remaining = [&](NodeId id) {
        auto const* n = g->node(id);
        if (!n) return 0.0;
        double best = std::numeric_limits<double>::max();
        for (auto const& d : destPositions)
            if (d.dim == n->location.dim) best = std::min(best, n->location.location().distanceTo(d.position));
        return best == std::numeric_limits<double>::max() ? 0.0 : best;
    };
    auto penaltyOf = [&](Edge const& e) { return q.edgePenalty ? q.edgePenalty(*g, e) : 0.0; };

    struct Entry {
        double                  distance, penalty, remaining;
        std::pair<NodeId, NodeId> edge;
        long                    destination = -1; ///< index into q.destinations when this entry is an arrival
        bool operator>(Entry const& o) const {
            return distance + penalty + remaining > o.distance + o.penalty + o.remaining;
        }
    };
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> frontier;
    std::map<std::pair<NodeId, NodeId>, std::pair<NodeId, NodeId>> parentOf;
    std::set<std::pair<NodeId, NodeId>>                            visited;

    auto key0 = std::make_pair(q.start.node1, q.start.node2);
    // destinations ahead on the start edge
    if (auto it = destByEdge.find(key0); it != destByEdge.end()) {
        for (auto const& [idx, position] : it->second) {
            if (position + 1e-9 < q.start.position) continue;
            frontier.push({position - q.start.position, 0, 0, key0, long(idx)});
        }
    }
    double startPenalty = penaltyOf(*startEdge);
    frontier.push({g->edgeLength(*startEdge) - q.start.position, startPenalty, remaining(q.start.node2), key0, -1});

    while (!frontier.empty()) {
        Entry entry = frontier.top();
        frontier.pop();
        if (entry.distance > q.maxDistance || entry.distance + entry.penalty > q.maxCost) continue;
        if (entry.destination >= 0) {
            PathResult r;
            r.graph            = g->id();
            r.distance         = entry.distance;
            r.cost             = entry.distance + entry.penalty;
            r.destinationIndex = size_t(entry.destination);
            for (auto k = entry.edge; k != key0;) {
                r.edges.push_back({k.first, k.second});
                auto it = parentOf.find(k);
                if (it == parentOf.end()) break;
                k = it->second;
            }
            std::reverse(r.edges.begin(), r.edges.end());
            return r;
        }
        if (!visited.insert(entry.edge).second) continue;
        Edge const* e = g->edge(entry.edge.first, entry.edge.second);
        if (!e) continue;
        for (auto const& [to, next] : g->connectionsFrom(entry.edge.second)) {
            if (to == entry.edge.first || !g->canTravelTo(*e, next)) continue; // no U-turns
            auto key = std::make_pair(next.node1, next.node2);
            if (visited.contains(key)) continue;
            if (!parentOf.contains(key)) parentOf[key] = entry.edge;
            double newPenalty = entry.penalty + penaltyOf(next);
            if (auto it = destByEdge.find(key); it != destByEdge.end()) {
                for (auto const& [idx, position] : it->second) {
                    Entry arrival{entry.distance + position, newPenalty, 0, key, long(idx)};
                    frontier.push(arrival);
                }
            }
            frontier.push({entry.distance + g->edgeLength(next), newPenalty, remaining(next.node2), key, -1});
        }
    }
    return std::nullopt;
}

// ---- edge points --------------------------------------------------------------------------------

bool Railways::addPoint(EdgePoint p) {
    Graph* g  = graphAt(p.node1);
    auto*  n1 = g ? g->locateNode(p.node1) : nullptr;
    auto*  n2 = g ? g->locateNode(p.node2) : nullptr;
    if (!n1 || !n2 || !g->edge(n1->id, n2->id)) return false;
    std::string id = p.id;
    mPoints[id]    = std::move(p);
    ++mVersion;
    return true;
}

bool Railways::removePoint(std::string const& id) {
    bool removed = mPoints.erase(id) > 0;
    if (removed) ++mVersion;
    return removed;
}

EdgePoint const* Railways::point(std::string const& id) const {
    auto it = mPoints.find(id);
    return it == mPoints.end() ? nullptr : &it->second;
}

std::vector<EdgePoint const*> Railways::pointsOn(NodeLocation const& a, NodeLocation const& b) const {
    std::vector<EdgePoint const*> out;
    for (auto const& [id, p] : mPoints)
        if ((p.node1 == a && p.node2 == b) || (p.node1 == b && p.node2 == a)) out.push_back(&p);
    return out;
}

std::vector<EdgePoint const*> Railways::points(std::string const& type) const {
    std::vector<EdgePoint const*> out;
    for (auto const& [id, p] : mPoints)
        if (type.empty() || p.type == type) out.push_back(&p);
    return out;
}

void Railways::onPointInvalidated(std::function<void(EdgePoint const&)> fn) { mPointListeners.push_back(std::move(fn)); }

void Railways::invalidatePoints(std::vector<EdgePoint> const& removed) {
    for (auto const& p : removed) {
        if (!mPoints.erase(p.id)) continue;
        ++mVersion;
        for (auto const& fn : mPointListeners) fn(p);
    }
}

// ---- persistence --------------------------------------------------------------------------------

namespace {
nlohmann::json locJson(NodeLocation const& l) { return nlohmann::json::array({l.x2, l.y2, l.z2, l.dim, l.yOffsetPixels}); }
NodeLocation   jsonLoc(nlohmann::json const& j) {
    NodeLocation l;
    l.x2            = j.at(0).get<int>();
    l.y2            = j.at(1).get<int>();
    l.z2            = j.at(2).get<int>();
    l.dim           = j.at(3).get<int>();
    l.yOffsetPixels = j.size() > 4 ? j.at(4).get<int>() : 0;
    return l;
}
} // namespace

std::set<int> Railways::dimensions() const {
    std::set<int> out;
    for (auto const& [id, g] : mGraphs)
        for (auto const& [nid, n] : g->mNodes) out.insert(n.location.dim);
    for (auto const& [id, p] : mPoints) out.insert(p.node1.dim);
    return out;
}

nlohmann::json Railways::toJson(int dim) const {
    nlohmann::json nodes  = nlohmann::json::array();
    nlohmann::json edges  = nlohmann::json::array();
    nlohmann::json points = nlohmann::json::array();
    for (auto const& [gid, g] : mGraphs) {
        for (auto const& [id, n] : g->mNodes) {
            if (n.location.dim != dim) continue;
            nodes.push_back({
                {"id", id                                                     },
                {"g",  gid                                                    },
                {"l",  locJson(n.location)                                    },
                {"n",  nlohmann::json::array({n.normal.x, n.normal.y, n.normal.z})},
            });
            for (auto const& [to, e] : g->connectionsFrom(id)) {
                nlohmann::json je = {
                    {"a", e.node1   },
                    {"b", e.node2   },
                    {"m", e.material},
                };
                if (e.turn) je["turn"] = e.turn->toJson();
                edges.push_back(std::move(je));
            }
        }
    }
    for (auto const& [id, p] : mPoints) {
        if (p.node1.dim != dim) continue;
        points.push_back({
            {"id",   p.id            },
            {"type", p.type          },
            {"a",    locJson(p.node1)},
            {"b",    locJson(p.node2)},
            {"pos",  p.position      },
            {"fwd",  p.facingNode2   },
            {"data", p.data          },
        });
    }
    return {
        {"version", 1     },
        {"dim",     dim   },
        {"nodes",   nodes },
        {"edges",   edges },
        {"points",  points},
    };
}

void Railways::loadJson(nlohmann::json const& j) { mPending.push_back(j); }

void Railways::finishLoading() {
    std::vector<nlohmann::json> pending = std::move(mPending);
    mPending.clear();
    std::map<NodeId, Graph*> owner;
    // nodes first (every dimension), then edges, then points
    for (auto const& j : pending) {
        for (auto const& jn : j.value("nodes", nlohmann::json::array())) {
            try {
                std::string gid = jn.at("g").get<std::string>();
                Graph*      g   = graph(gid);
                if (!g) g = &newGraph(gid);
                Node n;
                n.id       = jn.at("id").get<NodeId>();
                n.location = jsonLoc(jn.at("l"));
                auto nv    = jn.at("n");
                n.normal   = {nv.at(0).get<double>(), nv.at(1).get<double>(), nv.at(2).get<double>()};
                if (g->addNodeIfAbsent(n)) owner[n.id] = g;
                mNodeIdCounter = std::max(mNodeIdCounter, n.id);
            } catch (...) {
            }
        }
    }
    for (auto const& j : pending) {
        for (auto const& je : j.value("edges", nlohmann::json::array())) {
            try {
                NodeId a  = je.at("a").get<NodeId>();
                NodeId b  = je.at("b").get<NodeId>();
                auto   ia = owner.find(a);
                auto   ib = owner.find(b);
                if (ia == owner.end() || ib == owner.end() || ia->second != ib->second) continue;
                Graph* g = ia->second;
                Edge   e;
                e.node1            = a;
                e.node2            = b;
                e.material         = je.value("m", std::string("andesite"));
                e.interDimensional = g->node(a)->location.dim != g->node(b)->location.dim;
                if (je.contains("turn")) {
                    if (auto c = Curve::fromJson(je.at("turn"))) e.turn = std::make_shared<Curve const>(std::move(*c));
                }
                g->putConnection(a, b, std::move(e));
            } catch (...) {
            }
        }
    }
    for (auto const& j : pending) {
        for (auto const& jp : j.value("points", nlohmann::json::array())) {
            try {
                EdgePoint p;
                p.id          = jp.at("id").get<std::string>();
                p.type        = jp.value("type", std::string());
                p.node1       = jsonLoc(jp.at("a"));
                p.node2       = jsonLoc(jp.at("b"));
                p.position    = jp.value("pos", 0.0);
                p.facingNode2 = jp.value("fwd", true);
                p.data        = jp.value("data", nlohmann::json());
                mPoints[p.id] = std::move(p);
            } catch (...) {
            }
        }
    }
    ++mVersion;
}

} // namespace lv::core::tracks
