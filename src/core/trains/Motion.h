#pragma once

// Trains on the track graph (pure C++): positions on directed edges, walking along the graph
// with junction choices, and a train body whose points (bogey wheels) follow the route the
// leading point took (Create's TravellingPoint / Carriage.travel).
//
// Positions are kept by node *locations* (not node ids), so they survive the graph rebuilds of
// the track unit (TrackPropagator gives rebuilt nodes new ids).

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/tracks/Graph.h"

namespace lv::core::trains {

using tracks::GraphLocation;
using tracks::NodeLocation;
using tracks::Railways;
using tracks::Vec3d;

/// A point on the directed edge `from` -> `to`, `pos` blocks from `from`.
struct TrackPos {
    NodeLocation from, to;
    double       pos = 0;

    [[nodiscard]] nlohmann::json   toJson() const;
    [[nodiscard]] static TrackPos  fromJson(nlohmann::json const& j);
    friend bool operator==(TrackPos const&, TrackPos const&) = default;
};

[[nodiscard]] nlohmann::json nodeToJson(NodeLocation const& n);
[[nodiscard]] NodeLocation   nodeFromJson(nlohmann::json const& j);

/// The graph edge of a position (nullopt when its nodes or edge are gone).
[[nodiscard]] std::optional<GraphLocation> resolve(Railways const& rail, TrackPos const& p);
[[nodiscard]] TrackPos                     fromGraph(Railways const& rail, GraphLocation const& g);
/// Length of the edge of `p` (0 when gone).
[[nodiscard]] double edgeLength(Railways const& rail, TrackPos const& p);
/// The same point travelling the other way.
[[nodiscard]] TrackPos reverse(Railways const& rail, TrackPos const& p);
[[nodiscard]] std::optional<Vec3d> positionOf(Railways const& rail, TrackPos const& p);
/// Whether the edge of `p` is a curve.
[[nodiscard]] bool onTurn(Railways const& rail, TrackPos const& p);

/// Junction choices a train made (Create keeps them in its travelling points): after directed
/// edge a -> b comes b -> next. Recorded both ways so points can follow in either direction.
class Route {
public:
    void record(NodeLocation const& a, NodeLocation const& b, NodeLocation const& next);
    [[nodiscard]] std::optional<NodeLocation> after(NodeLocation const& a, NodeLocation const& b) const;
    void clear() { mNext.clear(), mOrder.clear(); }
    [[nodiscard]] nlohmann::json toJson() const;
    void                         loadJson(nlohmann::json const& j);
    [[nodiscard]] size_t         size() const { return mNext.size(); }

private:
    using Key = std::pair<NodeLocation, NodeLocation>;
    std::map<Key, NodeLocation> mNext;
    std::deque<Key>             mOrder; ///< oldest first, pruned
};

/// Picks the next node at the end of edge `at.from -> at.to` among `options`; nullopt lets the
/// walk take the straightest one.
using Chooser = std::function<std::optional<NodeLocation>(TrackPos const& at, std::vector<NodeLocation> const& options)>;

/// A traversed piece of a directed edge, [start, end] blocks from its first node.
struct EdgeSpan {
    NodeLocation from, to;
    double       start = 0, end = 0;
};

struct WalkResult {
    TrackPos loc;
    double   moved   = 0;
    bool     blocked = false; ///< reached a dead end
    bool     lost    = false; ///< the edge doesn't exist (derailed)
};

/// Moves `distance` (>= 0) blocks forward. Records junction choices into `route` when given.
[[nodiscard]] WalkResult walk(
    Railways const&                           rail,
    TrackPos                                  loc,
    double                                    distance,
    Chooser const&                            choose,
    Route*                                    route,
    std::function<void(EdgeSpan const&)> const& onSpan = {}
);

/// A train on the tracks: its leading point and the distances of its other points behind it.
struct Body {
    TrackPos            front;
    std::vector<double> offsets; ///< ascending distances from the front point (offsets[0] = 0)
    Route               route;

    [[nodiscard]] double length() const { return offsets.empty() ? 0 : offsets.back(); }
    /// The point `offset` blocks behind the front (following the route).
    [[nodiscard]] std::optional<TrackPos> pointAt(Railways const& rail, double offset) const;
    /// Every point (offsets order).
    [[nodiscard]] std::vector<TrackPos> points(Railways const& rail) const;
    /// Moves the train by `distance` (negative: backwards, the last point leads). `onSpan` sees
    /// the leading point's spans (in its travel direction).
    WalkResult move(Railways const& rail, double distance, Chooser const& choose, std::function<void(EdgeSpan const&)> const& onSpan = {});

    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] static Body    fromJson(nlohmann::json const& j);
};

/// A chooser that follows `route` (and otherwise lets the walk go straight).
[[nodiscard]] Chooser follow(Route const& route);

/// A planned path as node location pairs (directed edges after the start edge).
struct Path {
    std::vector<std::pair<NodeLocation, NodeLocation>> edges;
    size_t                                             next = 0; ///< first edge not entered yet
    double                                             distance = 0;
    double                                             cost     = 0;
    size_t                                             destination = 0;

    /// A chooser that takes the path's next edge.
    [[nodiscard]] Chooser chooser();
    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] static Path    fromJson(nlohmann::json const& j);
};

/// Railways::findPath from `start` to any of `destinations` (positions on their directed edges).
[[nodiscard]] std::optional<Path> findPath(
    Railways const&                                                  rail,
    TrackPos const&                                                  start,
    std::vector<TrackPos> const&                                     destinations,
    std::function<double(tracks::Graph const&, tracks::Edge const&)> penalty = {},
    double                                                           maxCost = 1e9
);

/// Distance along the path to the first curved edge ahead (-1 if none within `lookahead`).
[[nodiscard]] double distanceToCurve(Railways const& rail, TrackPos const& at, Path const& path, double lookahead);

/// Horizontal yaw (degrees, Create/Bedrock: counter-clockwise from east looking down, as the
/// contraption transform's rotation about +Y) of a direction.
[[nodiscard]] double yawOf(Vec3d const& dir);

} // namespace lv::core::trains
