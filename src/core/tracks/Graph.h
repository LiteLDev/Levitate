#pragma once

// The track graph (Create's TrackGraph / TrackPropagator / GlobalRailwayManager), pure C++.
//
// Nodes sit at track ends, junctions, curve ends, portal crossings and every 16 blocks along long
// straight runs; edges are straight or curved (a `Curve`) pieces between them. Graphs are the
// connected components; they may span dimensions through nether-portal tracks. The world is read
// through `TrackWorld`, so the same code runs in the game and in unit tests.
//
// Everything here runs on the server thread.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/tracks/Curve.h"
#include "core/tracks/Shape.h"

namespace lv::core::tracks {

// ---- locations ---------------------------------------------------------------------------------

/// A node position: x and z on a half-block grid, y on whole blocks (both stored doubled), plus
/// the dimension and a pixel offset for tilted tracks (Create's TrackNodeLocation).
struct NodeLocation {
    int x2 = 0, y2 = 0, z2 = 0;
    int dim           = 0;
    int yOffsetPixels = 0;

    [[nodiscard]] static NodeLocation of(Vec3d const& v, int dim);
    [[nodiscard]] Vec3d               location() const;
    /// The 8 blocks around the location (where track blocks touching it can be).
    [[nodiscard]] std::vector<BlockPos3> allAdjacent() const;
    [[nodiscard]] std::string            str() const;

    friend bool operator==(NodeLocation const&, NodeLocation const&)  = default;
    friend auto operator<=>(NodeLocation const&, NodeLocation const&) = default;
};

struct NodeLocationHash {
    size_t operator()(NodeLocation const& l) const noexcept {
        return ((size_t(uint32_t(l.y2)) + (size_t(uint32_t(l.z2)) + size_t(uint32_t(l.yOffsetPixels)) * 31u) * 31u
                 + size_t(uint32_t(l.dim)))
                * 31u)
             * 31u
             + size_t(uint32_t(l.x2));
    }
};

/// A location found while walking the tracks (Create's DiscoveredLocation).
struct DiscoveredLocation : NodeLocation {
    std::shared_ptr<Curve const> turn;          ///< reached through this curve (oriented from the walk's start)
    bool                         forceNode = false;
    std::optional<Vec3d>         direction;      ///< unit track direction here
    Vec3d                        normal{0, 1, 0};
    std::string                  materialA = "andesite", materialB = "andesite";

    [[nodiscard]] bool differentMaterials() const { return materialA != materialB; }
    [[nodiscard]] bool notInLineWith(Vec3d const& dir) const;
};

// ---- world access ------------------------------------------------------------------------------

/// What the graph needs to know about one track block.
struct TrackBlockView {
    Shape shape = Shape::ZO;
    /// Curves attached to this block, each oriented from this block (`block(0)` is this block).
    std::vector<std::shared_ptr<Curve const>> curves;
    /// Portal tracks: the bound track on the other side (dimension, position).
    std::optional<std::pair<int, BlockPos3>> portalLink;
    std::string                              material = "andesite";
};

class TrackWorld {
public:
    virtual ~TrackWorld() = default;
    /// The track at `pos` in `dim` (nullopt when there is none or the chunk isn't loaded).
    [[nodiscard]] virtual std::optional<TrackBlockView> trackAt(int dim, BlockPos3 const& pos) const = 0;
};

/// Ends of a track block reachable from `connectedTo` (or all of them), Create's getConnected.
/// `linear` skips curves and portal links.
[[nodiscard]] std::vector<DiscoveredLocation> connectedEnds(
    TrackWorld const&               world,
    int                             dim,
    BlockPos3 const&                pos,
    TrackBlockView const&           view,
    bool                            linear,
    DiscoveredLocation const*       connectedTo
);
/// Every location connected to `location` through the tracks touching it.
[[nodiscard]] std::vector<DiscoveredLocation>
walkConnectedTracks(TrackWorld const& world, NodeLocation const& location, bool linear);

// ---- graph -------------------------------------------------------------------------------------

using NodeId = int;

struct Node {
    NodeId       id = 0;
    NodeLocation location;
    Vec3d        normal{0, 1, 0};
};

/// A point registered on an edge by another unit (signal, station, observer, ...).
struct EdgePoint {
    std::string  id;    ///< unique id (the owner chooses it, e.g. "signal:0:12,64,-3:front")
    std::string  type;  ///< "levitate:signal", "levitate:station", "levitate:observer", ...
    NodeLocation node1; ///< edge it lies on, measured from node1 towards node2
    NodeLocation node2;
    double       position = 0;    ///< distance from node1 (blocks)
    bool         facingNode2 = true; ///< which way the point acts (signals face one direction)
    nlohmann::json data;          ///< owner data (persisted with the graph)
};

struct Edge {
    NodeId                       node1 = 0;
    NodeId                       node2 = 0;
    std::shared_ptr<Curve const> turn; ///< curve oriented from node1, null for straight edges
    bool                         interDimensional = false;
    std::string                  material         = "andesite";

    [[nodiscard]] bool isTurn() const { return turn != nullptr; }
};

class Railways;

class Graph {
public:
    explicit Graph(std::string id, int netId);

    [[nodiscard]] std::string const& id() const { return mId; }
    [[nodiscard]] int                netId() const { return mNetId; }
    [[nodiscard]] bool               empty() const { return mNodes.empty(); }

    [[nodiscard]] Node const* locateNode(NodeLocation const& loc) const;
    [[nodiscard]] Node const* node(NodeId id) const;
    [[nodiscard]] std::vector<Node const*> nodes() const;
    [[nodiscard]] size_t                   nodeCount() const { return mNodes.size(); }
    /// Directed edges leaving `id` (target node id → edge).
    [[nodiscard]] std::map<NodeId, Edge> const& connectionsFrom(NodeId id) const;
    [[nodiscard]] Edge const*                   edge(NodeId from, NodeId to) const;
    [[nodiscard]] size_t                        edgeCount() const; ///< directed edges / 2

    // ---- geometry of an edge ----
    [[nodiscard]] double edgeLength(Edge const& e) const;
    /// Position at parameter t (0..1, linear in length for straight edges; bezier t for curves).
    [[nodiscard]] Vec3d edgePosition(Edge const& e, double t) const;
    /// Position at a distance (blocks) from node1, arc-length exact on curves.
    [[nodiscard]] Vec3d edgePositionAt(Edge const& e, double distance) const;
    [[nodiscard]] Vec3d edgeNormal(Edge const& e, double t) const;
    /// Unit direction at the start (`fromFirst`) or the end of the edge.
    [[nodiscard]] Vec3d edgeDirection(Edge const& e, bool fromFirst) const;
    /// Trains may go from `e` onto `next` (no sharp kinks; Create's canTravelTo).
    [[nodiscard]] bool canTravelTo(Edge const& e, Edge const& next) const;

private:
    friend class Railways;

    bool   addNodeIfAbsent(Node node);
    void   addNode(Node node);
    bool   removeNode(NodeLocation const& loc, std::vector<EdgePoint>* removedPoints);
    void   putConnection(NodeId a, NodeId b, Edge e);
    void   transferNode(Node const& node, Graph& target);
    [[nodiscard]] std::vector<std::unique_ptr<Graph>> splitDisconnected(Railways& rail);

    std::string                                                mId;
    int                                                        mNetId;
    std::unordered_map<NodeLocation, NodeId, NodeLocationHash> mNodesByLocation;
    std::map<NodeId, Node>                                     mNodes;
    std::map<NodeId, std::map<NodeId, Edge>>                   mConnections;
};

// ---- the set of all graphs ---------------------------------------------------------------------

/// Where something is on the graph: on the directed edge node1 → node2, `position` blocks from node1.
struct GraphLocation {
    std::string graph;
    NodeId      node1 = 0;
    NodeId      node2 = 0;
    double      position = 0;
};

struct PathStep {
    NodeId node1 = 0, node2 = 0;
};

struct PathResult {
    std::string           graph;
    std::vector<PathStep> edges;     ///< edges after the start edge, in travel order
    double                distance = 0; ///< from the start to the destination along the tracks
    double                cost     = 0; ///< distance + penalties
    size_t                destinationIndex = 0;
};

struct PathQuery {
    GraphLocation              start;        ///< travelling towards start.node2
    std::vector<GraphLocation> destinations; ///< reached when passing their position on their directed edge
    double maxDistance = 1e9;
    double maxCost     = 1e9;
    /// Extra cost for entering an edge (signals, occupied blocks, ...); 0 by default.
    std::function<double(Graph const&, Edge const&)> edgePenalty;
};

/// Holds every track graph of a world. Owned by the feature module; tests own their own.
class Railways {
public:
    Railways();

    // ---- graphs ----
    [[nodiscard]] std::vector<Graph*> graphs() const;
    [[nodiscard]] Graph*              graph(std::string const& id) const;
    [[nodiscard]] Graph*              graphAt(NodeLocation const& loc) const;
    void                              clear();

    // ---- incremental updates (TrackPropagator) ----
    /// A track block was placed or changed (call after the world shows it). Returns its graph.
    Graph* onRailAdded(TrackWorld const& world, int dim, BlockPos3 const& pos);
    /// A track block is about to be removed: `view` is what it was.
    void onRailRemoved(TrackWorld const& world, int dim, BlockPos3 const& pos, TrackBlockView const& view);

    // ---- queries ----
    /// Nearest edge point to `pos` (within `maxDistance` blocks) in `dim`.
    [[nodiscard]] std::optional<GraphLocation> nearestEdge(int dim, Vec3d const& pos, double maxDistance = 2) const;
    [[nodiscard]] std::optional<Vec3d>         positionOf(GraphLocation const& loc) const;
    /// Moves `distance` blocks forward from `loc` along the graph, choosing at junctions with
    /// `choose` (default: the straightest continuation). Stops at dead ends.
    [[nodiscard]] GraphLocation advance(
        GraphLocation                                                                         loc,
        double                                                                                distance,
        std::function<NodeId(Graph const&, Edge const& from, std::vector<Edge const*> const&)> choose = {}
    ) const;
    /// A* over directed edges (Create's Navigation.search: distance + penalties, heuristic =
    /// straight-line distance to the nearest destination).
    [[nodiscard]] std::optional<PathResult> findPath(PathQuery const& q) const;

    // ---- edge points ----
    /// Adds (or replaces) a point; false when its edge doesn't exist.
    bool addPoint(EdgePoint point);
    bool removePoint(std::string const& id);
    [[nodiscard]] EdgePoint const*              point(std::string const& id) const;
    [[nodiscard]] std::vector<EdgePoint const*> pointsOn(NodeLocation const& a, NodeLocation const& b) const;
    [[nodiscard]] std::vector<EdgePoint const*> points(std::string const& type = {}) const;
    /// Called with every point whose edge disappeared (it is removed; owners re-add it).
    void onPointInvalidated(std::function<void(EdgePoint const&)> fn);

    // ---- persistence ----
    /// Everything stored for nodes in dimension `dim` (edges are stored with their node1).
    [[nodiscard]] nlohmann::json toJson(int dim) const;
    [[nodiscard]] std::set<int>  dimensions() const;
    /// Loads one dimension's data; call `finishLoading()` after the last one.
    void loadJson(nlohmann::json const& j);
    void finishLoading();

    /// Bumped on every change (for "dirty" checks and client sync).
    [[nodiscard]] uint64_t version() const { return mVersion; }

    [[nodiscard]] NodeId nextNodeId() { return ++mNodeIdCounter; }

private:
    friend class Graph;

    Graph& newGraph(std::string id = {});
    void   removeGraph(Graph* g);
    void   connectNodes(Graph& g, DiscoveredLocation const& a, DiscoveredLocation const& b, std::shared_ptr<Curve const> turn);
    bool   createNodeIfAbsent(Graph& g, DiscoveredLocation const& loc);
    void   updateSplitGraph(Graph& g);
    void   invalidatePoints(std::vector<EdgePoint> const& removed);
    [[nodiscard]] std::string newGraphId();

    std::map<std::string, std::unique_ptr<Graph>>  mGraphs;
    std::map<std::string, EdgePoint>               mPoints;
    std::vector<std::function<void(EdgePoint const&)>> mPointListeners;
    std::vector<nlohmann::json>                    mPending; ///< loadJson buffer
    NodeId                                         mNodeIdCounter = 0;
    int                                            mGraphNetCounter = 0;
    uint64_t                                       mVersion         = 0;
    uint64_t                                       mIdSeed;
};

/// Whether `location` must be a graph node, given the ends `next` reachable from it (Create's
/// TrackPropagator.isValidGraphNodeLocation).
[[nodiscard]] bool isValidGraphNodeLocation(DiscoveredLocation const& location, std::vector<DiscoveredLocation> const& next, bool first);

} // namespace lv::core::tracks
