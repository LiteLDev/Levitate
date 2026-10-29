#include "core/trains/Motion.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lv::core::trains {

using tracks::Edge;
using tracks::Graph;
using Json = nlohmann::json;

namespace {

struct Resolved {
    Graph*      graph = nullptr;
    Edge const* edge  = nullptr;
    GraphLocation loc;
};

std::optional<Resolved> resolveEdge(Railways const& rail, TrackPos const& p) {
    Graph* g = rail.graphAt(p.from);
    if (!g) return std::nullopt;
    auto const* a = g->locateNode(p.from);
    auto const* b = g->locateNode(p.to);
    if (!a || !b) return std::nullopt;
    auto const* e = g->edge(a->id, b->id);
    if (!e) return std::nullopt;
    return Resolved{g, e, GraphLocation{g->id(), a->id, b->id, p.pos}};
}

constexpr size_t kMaxRoute = 512;

bool contains(std::vector<NodeLocation> const& v, NodeLocation const& n) {
    return std::any_of(v.begin(), v.end(), [&](NodeLocation const& o) { return o == n; });
}

} // namespace

Json nodeToJson(NodeLocation const& n) { return Json::array({n.x2, n.y2, n.z2, n.dim, n.yOffsetPixels}); }

NodeLocation nodeFromJson(Json const& j) {
    NodeLocation n;
    if (!j.is_array() || j.size() < 4) return n;
    n.x2  = j[0].get<int>();
    n.y2  = j[1].get<int>();
    n.z2  = j[2].get<int>();
    n.dim = j[3].get<int>();
    if (j.size() > 4) n.yOffsetPixels = j[4].get<int>();
    return n;
}

Json TrackPos::toJson() const { return {{"a", nodeToJson(from)}, {"b", nodeToJson(to)}, {"p", pos}}; }

TrackPos TrackPos::fromJson(Json const& j) {
    TrackPos p;
    if (!j.is_object()) return p;
    p.from = nodeFromJson(j.value("a", Json::array()));
    p.to   = nodeFromJson(j.value("b", Json::array()));
    p.pos  = j.value("p", 0.0);
    return p;
}

std::optional<GraphLocation> resolve(Railways const& rail, TrackPos const& p) {
    auto r = resolveEdge(rail, p);
    if (!r) return std::nullopt;
    return r->loc;
}

TrackPos fromGraph(Railways const& rail, GraphLocation const& g) {
    TrackPos p;
    p.pos      = g.position;
    Graph* gr  = rail.graph(g.graph);
    if (!gr) return p;
    if (auto const* a = gr->node(g.node1)) p.from = a->location;
    if (auto const* b = gr->node(g.node2)) p.to = b->location;
    return p;
}

double edgeLength(Railways const& rail, TrackPos const& p) {
    auto r = resolveEdge(rail, p);
    return r ? r->graph->edgeLength(*r->edge) : 0;
}

TrackPos reverse(Railways const& rail, TrackPos const& p) {
    double len = edgeLength(rail, p);
    return TrackPos{p.to, p.from, std::max(0.0, len - p.pos)};
}

std::optional<Vec3d> positionOf(Railways const& rail, TrackPos const& p) {
    auto r = resolveEdge(rail, p);
    if (!r) return std::nullopt;
    double len = r->graph->edgeLength(*r->edge);
    return r->graph->edgePositionAt(*r->edge, std::clamp(p.pos, 0.0, len));
}

bool onTurn(Railways const& rail, TrackPos const& p) {
    auto r = resolveEdge(rail, p);
    return r && r->edge->isTurn();
}

// ---- route --------------------------------------------------------------------------------------

void Route::record(NodeLocation const& a, NodeLocation const& b, NodeLocation const& next) {
    auto put = [&](Key k, NodeLocation const& v) {
        auto it = mNext.find(k);
        if (it == mNext.end()) mOrder.push_back(k);
        mNext[k] = v;
    };
    put({a, b}, next);
    put({next, b}, a);
    while (mOrder.size() > kMaxRoute) {
        mNext.erase(mOrder.front());
        mOrder.pop_front();
    }
}

std::optional<NodeLocation> Route::after(NodeLocation const& a, NodeLocation const& b) const {
    auto it = mNext.find({a, b});
    if (it == mNext.end()) return std::nullopt;
    return it->second;
}

Json Route::toJson() const {
    Json arr = Json::array();
    for (auto const& k : mOrder) {
        auto it = mNext.find(k);
        if (it == mNext.end()) continue;
        arr.push_back(Json::array({nodeToJson(k.first), nodeToJson(k.second), nodeToJson(it->second)}));
    }
    return arr;
}

void Route::loadJson(Json const& j) {
    clear();
    if (!j.is_array()) return;
    for (auto const& e : j) {
        if (!e.is_array() || e.size() < 3) continue;
        Key k{nodeFromJson(e[0]), nodeFromJson(e[1])};
        if (!mNext.contains(k)) mOrder.push_back(k);
        mNext[k] = nodeFromJson(e[2]);
    }
}

Chooser follow(Route const& route) {
    return [&route](TrackPos const& at, std::vector<NodeLocation> const& options) -> std::optional<NodeLocation> {
        auto next = route.after(at.from, at.to);
        if (next && contains(options, *next)) return next;
        return std::nullopt;
    };
}

// ---- walking ------------------------------------------------------------------------------------

WalkResult walk(
    Railways const&                             rail,
    TrackPos                                    loc,
    double                                      distance,
    Chooser const&                              choose,
    Route*                                      route,
    std::function<void(EdgeSpan const&)> const& onSpan
) {
    WalkResult r;
    r.loc    = loc;
    distance = std::max(0.0, distance);
    for (int guard = 0; guard < 100000; ++guard) {
        auto res = resolveEdge(rail, r.loc);
        if (!res) {
            r.lost = true;
            return r;
        }
        Graph&      g   = *res->graph;
        Edge const& e   = *res->edge;
        double      len = g.edgeLength(e);
        double      pos = std::clamp(r.loc.pos, 0.0, len);
        if (pos + distance <= len) {
            if (onSpan && distance > 0) onSpan({r.loc.from, r.loc.to, pos, pos + distance});
            r.loc.pos = pos + distance;
            r.moved += distance;
            return r;
        }
        if (onSpan && len > pos) onSpan({r.loc.from, r.loc.to, pos, len});
        r.moved += len - pos;
        distance -= len - pos;
        r.loc.pos = len;

        std::vector<NodeLocation> options;
        std::vector<Edge const*>  edges;
        for (auto const& [to, next] : g.connectionsFrom(res->loc.node2)) {
            if (to == res->loc.node1) continue; // no U-turns
            if (!g.canTravelTo(e, next)) continue;
            auto const* n = g.node(to);
            if (!n) continue;
            options.push_back(n->location);
            edges.push_back(&next);
        }
        if (options.empty()) {
            r.blocked = true;
            return r;
        }
        std::optional<NodeLocation> pick;
        if (choose) pick = choose(r.loc, options);
        if (pick && !contains(options, *pick)) pick.reset();
        if (!pick) {
            double best = -2;
            Vec3d  dir  = g.edgeDirection(e, false);
            for (size_t i = 0; i < edges.size(); ++i) {
                double d = dir.dot(g.edgeDirection(*edges[i], true));
                if (d > best) {
                    best = d;
                    pick = options[i];
                }
            }
        }
        if (route) route->record(r.loc.from, r.loc.to, *pick);
        r.loc = TrackPos{r.loc.to, *pick, 0};
    }
    return r;
}

// ---- body ---------------------------------------------------------------------------------------

std::optional<TrackPos> Body::pointAt(Railways const& rail, double offset) const {
    if (offset <= 0) return front;
    auto r = walk(rail, reverse(rail, front), offset, follow(route), nullptr);
    if (r.lost) return std::nullopt;
    return reverse(rail, r.loc);
}

std::vector<TrackPos> Body::points(Railways const& rail) const {
    std::vector<TrackPos> out;
    out.reserve(offsets.size());
    TrackPos rev     = reverse(rail, front);
    double   walked  = 0;
    for (double o : offsets) {
        auto r = walk(rail, rev, o - walked, follow(route), nullptr);
        rev    = r.loc;
        walked = o;
        out.push_back(reverse(rail, rev));
    }
    return out;
}

WalkResult Body::move(Railways const& rail, double distance, Chooser const& choose, std::function<void(EdgeSpan const&)> const& onSpan) {
    if (distance >= 0) {
        auto r = walk(rail, front, distance, choose, &route, onSpan);
        if (!r.lost) front = r.loc;
        return r;
    }
    auto back = pointAt(rail, length());
    if (!back) {
        WalkResult lost;
        lost.loc  = front;
        lost.lost = true;
        return lost;
    }
    auto r = walk(rail, reverse(rail, *back), -distance, choose, &route, onSpan);
    if (r.lost) return r;
    auto fw = walk(rail, reverse(rail, r.loc), length(), follow(route), nullptr);
    if (!fw.lost) front = fw.loc;
    return r;
}

Json Body::toJson() const { return {{"front", front.toJson()}, {"offsets", offsets}, {"route", route.toJson()}}; }

Body Body::fromJson(Json const& j) {
    Body b;
    if (!j.is_object()) return b;
    b.front   = TrackPos::fromJson(j.value("front", Json::object()));
    b.offsets = j.value("offsets", std::vector<double>{});
    b.route.loadJson(j.value("route", Json::array()));
    return b;
}

// ---- paths --------------------------------------------------------------------------------------

Chooser Path::chooser() {
    return [this](TrackPos const& at, std::vector<NodeLocation> const& options) -> std::optional<NodeLocation> {
        // skip edges we already passed (e.g. after a re-path)
        for (size_t i = next; i < edges.size(); ++i) {
            if (edges[i].first == at.to && contains(options, edges[i].second)) {
                next = i + 1;
                return edges[i].second;
            }
        }
        return std::nullopt;
    };
}

Json Path::toJson() const {
    Json arr = Json::array();
    for (auto const& [a, b] : edges) arr.push_back(Json::array({nodeToJson(a), nodeToJson(b)}));
    return {{"edges", arr}, {"next", next}, {"distance", distance}, {"cost", cost}, {"dest", destination}};
}

Path Path::fromJson(Json const& j) {
    Path p;
    if (!j.is_object()) return p;
    for (auto const& e : j.value("edges", Json::array()))
        if (e.is_array() && e.size() >= 2) p.edges.emplace_back(nodeFromJson(e[0]), nodeFromJson(e[1]));
    p.next        = j.value("next", size_t{0});
    p.distance    = j.value("distance", 0.0);
    p.cost        = j.value("cost", 0.0);
    p.destination = j.value("dest", size_t{0});
    return p;
}

std::optional<Path> findPath(
    Railways const&                                       rail,
    TrackPos const&                                       start,
    std::vector<TrackPos> const&                          destinations,
    std::function<double(Graph const&, Edge const&)>      penalty,
    double                                                maxCost
) {
    auto s = resolveEdge(rail, start);
    if (!s) return std::nullopt;
    tracks::PathQuery q;
    q.start = s->loc;
    std::vector<size_t> indexOf;
    for (size_t i = 0; i < destinations.size(); ++i) {
        auto d = resolveEdge(rail, destinations[i]);
        if (!d || d->loc.graph != s->loc.graph) continue;
        q.destinations.push_back(d->loc);
        indexOf.push_back(i);
    }
    if (q.destinations.empty()) return std::nullopt;
    q.edgePenalty = std::move(penalty);
    q.maxCost     = maxCost;
    auto r        = rail.findPath(q);
    if (!r) return std::nullopt;
    Path p;
    p.distance    = r->distance;
    p.cost        = r->cost;
    p.destination = indexOf[r->destinationIndex];
    for (auto const& step : r->edges) {
        auto const* a = s->graph->node(step.node1);
        auto const* b = s->graph->node(step.node2);
        if (a && b) p.edges.emplace_back(a->location, b->location);
    }
    return p;
}

double distanceToCurve(Railways const& rail, TrackPos const& at, Path const& path, double lookahead) {
    auto cur = resolveEdge(rail, at);
    if (!cur) return -1;
    if (cur->edge->isTurn()) return 0;
    double d = cur->graph->edgeLength(*cur->edge) - at.pos;
    for (size_t i = path.next; i < path.edges.size() && d <= lookahead; ++i) {
        auto e = resolveEdge(rail, TrackPos{path.edges[i].first, path.edges[i].second, 0});
        if (!e) break;
        if (e->edge->isTurn()) return d;
        d += e->graph->edgeLength(*e->edge);
    }
    return -1;
}

double yawOf(Vec3d const& dir) { return std::atan2(-dir.z, dir.x) * 180.0 / std::numbers::pi; }

} // namespace lv::core::trains
