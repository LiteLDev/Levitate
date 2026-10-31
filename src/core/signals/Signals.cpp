#include "core/signals/Signals.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>

namespace lv::core::signals {

namespace {

constexpr double kEps = 1e-6;

/// Union-find over integers.
struct Dsu {
    std::vector<int> parent;
    explicit Dsu(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    }
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
};

/// The edge between two node locations in `rail` (and its graph), or nulls.
std::pair<ct::Graph const*, ct::Edge const*>
edgeBetween(ct::Railways const& rail, ct::NodeLocation const& a, ct::NodeLocation const& b) {
    ct::Graph const* g = rail.graphAt(a);
    if (!g) return {nullptr, nullptr};
    auto const* na = g->locateNode(a);
    auto const* nb = g->locateNode(b);
    if (!na || !nb) return {g, nullptr};
    return {g, g->edge(na->id, nb->id)};
}

/// 2D segment intersection in the xz plane: parameters (s, t) along both segments.
std::optional<std::pair<double, double>>
intersect2d(ct::Vec3d const& p0, ct::Vec3d const& p1, ct::Vec3d const& q0, ct::Vec3d const& q1) {
    double rx = p1.x - p0.x, rz = p1.z - p0.z;
    double sx = q1.x - q0.x, sz = q1.z - q0.z;
    double den = rx * sz - rz * sx;
    if (std::abs(den) < 1e-9) return std::nullopt;
    double qpx = q0.x - p0.x, qpz = q0.z - p0.z;
    double t   = (qpx * sz - qpz * sx) / den;
    double u   = (qpx * rz - qpz * rx) / den;
    if (t < -kEps || t > 1 + kEps || u < -kEps || u > 1 + kEps) return std::nullopt;
    return std::pair{t, u};
}

} // namespace

// ---- configuration ------------------------------------------------------------------------------

SignalSystem::EdgeKey SignalSystem::keyOf(ct::NodeLocation const& x, ct::NodeLocation const& y) {
    return x < y ? EdgeKey{x, y} : EdgeKey{y, x};
}

SignalSystem::Placed SignalSystem::place(ct::NodeLocation const& n1, ct::NodeLocation const& n2, double pos) const {
    Placed p;
    p.key = keyOf(n1, n2);
    auto it = mEdges.find(p.key);
    if (it == mEdges.end()) return p;
    p.alongAB = p.key.a == n1;
    p.pos     = std::clamp(p.alongAB ? pos : it->second.length - pos, 0.0, it->second.length);
    p.valid   = true;
    return p;
}

void SignalSystem::rebuild(ct::Railways const& rail, std::vector<Boundary> boundaries, std::vector<Observer> observers) {
    mBoundaries = std::move(boundaries);
    mObservers  = std::move(observers);
    mBoundaryIndex.clear();
    for (size_t i = 0; i < mBoundaries.size(); ++i) mBoundaryIndex[mBoundaries[i].id] = i;
    mEdges.clear();
    mOut.clear();
    mDirs.clear();

    // every undirected edge of every graph, with its length and topology
    for (auto const* g : rail.graphs()) {
        for (auto const* n : g->nodes()) {
            for (auto const& [to, e] : g->connectionsFrom(n->id)) {
                auto const* m = g->node(to);
                if (!m) continue;
                EdgeKey key = keyOf(n->location, m->location);
                if (!mEdges.contains(key)) mEdges[key].length = g->edgeLength(e);
                ct::Vec3d d0 = e.interDimensional ? ct::Vec3d{} : g->edgeDirection(e, true);
                ct::Vec3d d1 = e.interDimensional ? ct::Vec3d{} : g->edgeDirection(e, false);
                mOut[n->location].push_back({m->location, d0});
                mDirs[{n->location, m->location}] = {d0, d1};
            }
        }
    }

    // boundaries cut their edges into segments
    mBoundaryGroups.assign(mBoundaries.size(), {kNoGroup, kNoGroup});
    std::vector<Placed> placed(mBoundaries.size());
    for (size_t i = 0; i < mBoundaries.size(); ++i) {
        auto const& b = mBoundaries[i];
        placed[i]     = place(b.node1, b.node2, b.position);
        if (placed[i].valid) mEdges[placed[i].key].cuts.push_back({placed[i].pos, i});
    }
    int segments = 0;
    for (auto& [key, info] : mEdges) {
        std::stable_sort(info.cuts.begin(), info.cuts.end(), [](auto const& x, auto const& y) { return x.first < y.first; });
        info.firstSegment = segments;
        segments += int(info.cuts.size()) + 1;
    }

    // segments touching the same node are one section
    Dsu                                           dsu(size_t(std::max(segments, 1)));
    std::map<ct::NodeLocation, std::vector<int>> atNode;
    for (auto const& [key, info] : mEdges) {
        atNode[key.a].push_back(info.firstSegment);
        atNode[key.b].push_back(info.firstSegment + int(info.cuts.size()));
    }
    for (auto const& [loc, segs] : atNode)
        for (size_t i = 1; i < segs.size(); ++i) dsu.unite(segs[0], segs[i]);

    // number the sections that have a boundary (in boundary order, for stable ids)
    std::map<int, GroupId> rootGroup;
    mGroupCount = 0;
    auto groupOfSegment = [&](int seg) {
        int  root = dsu.find(seg);
        auto it   = rootGroup.find(root);
        if (it == rootGroup.end()) it = rootGroup.emplace(root, GroupId(mGroupCount++)).first;
        return it->second;
    };
    for (size_t i = 0; i < mBoundaries.size(); ++i) {
        if (!placed[i].valid) continue;
        auto const& info = mEdges[placed[i].key];
        int         k    = 0;
        while (k < int(info.cuts.size()) && info.cuts[k].second != i) ++k;
        GroupId towardsA = groupOfSegment(info.firstSegment + k);
        GroupId towardsB = groupOfSegment(info.firstSegment + k + 1);
        // side 1 = towards node2
        mBoundaryGroups[i] = placed[i].alongAB ? std::array{towardsA, towardsB} : std::array{towardsB, towardsA};
    }
    mSegmentGroup.assign(size_t(std::max(segments, 0)), kNoGroup);
    for (int s = 0; s < segments; ++s) {
        auto it = rootGroup.find(dsu.find(s));
        if (it != rootGroup.end()) mSegmentGroup[s] = it->second;
    }

    computeLinks(rail);

    // exits of every boundary side
    for (int side = 0; side < 2; ++side) {
        mExitCache[side].assign(mBoundaries.size(), {});
        for (size_t i = 0; i < mBoundaries.size(); ++i) {
            if (!placed[i].valid) continue;
            auto const& b    = mBoundaries[i];
            auto&       out  = mExitCache[side][i];
            ct::NodeLocation from = side == 1 ? b.node1 : b.node2;
            ct::NodeLocation to   = side == 1 ? b.node2 : b.node1;
            double           at   = side == 1 ? b.position : mEdges[placed[i].key].length - b.position;
            std::set<std::pair<ct::NodeLocation, ct::NodeLocation>> visited;
            std::deque<std::tuple<ct::NodeLocation, ct::NodeLocation, double>> frontier{{from, to, at}};
            int guard = 4096;
            while (!frontier.empty() && guard-- > 0) {
                auto [f, t, start] = frontier.front();
                frontier.pop_front();
                if (!visited.insert({f, t}).second) continue;
                EdgeKey key  = keyOf(f, t);
                auto    eit  = mEdges.find(key);
                if (eit == mEdges.end()) continue;
                bool    ab   = key.a == f;
                double  len  = eit->second.length;
                // the nearest boundary after `start` in travel direction
                std::optional<std::pair<double, size_t>> next;
                for (auto const& [cpos, idx] : eit->second.cuts) {
                    if (idx == i) continue;
                    double tp = ab ? cpos : len - cpos;
                    if (tp < start - kEps) continue;
                    if (!next || tp < next->first) next = std::pair{tp, idx};
                }
                if (next) {
                    auto const& nb = mBoundaries[next->second];
                    out.push_back({nb.id, nb.node1 == f ? 1 : 0});
                    continue;
                }
                auto dirs = mDirs.find({f, t});
                for (auto const& [nxt, d0] : mOut[t]) {
                    if (nxt == f) continue;
                    if (dirs != mDirs.end()) {
                        ct::Vec3d endDir = dirs->second.second;
                        bool      ok     = endDir.lengthSqr() < 1e-9 || d0.lengthSqr() < 1e-9 || endDir.dot(d0) > 7 / 8.0;
                        if (!ok) continue;
                    }
                    frontier.push_back({t, nxt, 0.0});
                }
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
    }

    remapTrains();
    mStates.assign(mBoundaries.size(), {SignalState::Invalid, SignalState::Invalid});
    updateStates();
}

bool SignalSystem::setSides(std::string const& boundaryId, std::array<BoundarySide, 2> const& sides) {
    auto it = mBoundaryIndex.find(boundaryId);
    if (it == mBoundaryIndex.end()) return false;
    mBoundaries[it->second].sides = sides;
    return true;
}

Boundary const* SignalSystem::boundary(std::string const& id) const {
    auto it = mBoundaryIndex.find(id);
    return it == mBoundaryIndex.end() ? nullptr : &mBoundaries[it->second];
}

void SignalSystem::computeLinks(ct::Railways const& rail) {
    // Create's TrackEdgeIntersection: tracks crossing without a common node (curves over other
    // tracks) link their sections
    struct Poly {
        EdgeKey                key;
        int                    dim = 0;
        std::vector<ct::Vec3d> pts;
        std::vector<double>    dist; ///< arc length from key.a at each point
        ct::Vec3d              min, max;
    };
    std::vector<Poly> polys;
    for (auto const& [key, info] : mEdges) {
        if (info.length <= kEps || key.a.dim != key.b.dim) continue;
        auto [g, e] = edgeBetween(rail, key.a, key.b);
        if (!g || !e) continue;
        Poly p;
        p.key   = key;
        p.dim   = key.a.dim;
        int n   = e->isTurn() ? std::max(2, int(std::ceil(info.length * 2))) : 1;
        for (int i = 0; i <= n; ++i) {
            double d = info.length * i / n;
            p.pts.push_back(g->edgePositionAt(*e, d));
            p.dist.push_back(d);
        }
        p.min = p.max = p.pts.front();
        for (auto const& v : p.pts) {
            p.min = {std::min(p.min.x, v.x), std::min(p.min.y, v.y), std::min(p.min.z, v.z)};
            p.max = {std::max(p.max.x, v.x), std::max(p.max.y, v.y), std::max(p.max.z, v.z)};
        }
        polys.push_back(std::move(p));
    }
    Dsu links(std::max<size_t>(mGroupCount, 1));
    for (size_t i = 0; i < polys.size(); ++i) {
        for (size_t j = i + 1; j < polys.size(); ++j) {
            auto const& p = polys[i];
            auto const& q = polys[j];
            if (p.dim != q.dim) continue;
            if (p.key.a == q.key.a || p.key.a == q.key.b || p.key.b == q.key.a || p.key.b == q.key.b) continue;
            if (p.max.x < q.min.x || q.max.x < p.min.x || p.max.z < q.min.z || q.max.z < p.min.z) continue;
            if (p.max.y + 1 < q.min.y || q.max.y + 1 < p.min.y) continue;
            for (size_t a = 0; a + 1 < p.pts.size(); ++a) {
                for (size_t b = 0; b + 1 < q.pts.size(); ++b) {
                    auto hit = intersect2d(p.pts[a], p.pts[a + 1], q.pts[b], q.pts[b + 1]);
                    if (!hit) continue;
                    double ya = p.pts[a].y + (p.pts[a + 1].y - p.pts[a].y) * hit->first;
                    double yb = q.pts[b].y + (q.pts[b + 1].y - q.pts[b].y) * hit->second;
                    if (std::abs(ya - yb) > 0.5) continue;
                    double  da = p.dist[a] + (p.dist[a + 1] - p.dist[a]) * hit->first;
                    double  db = q.dist[b] + (q.dist[b + 1] - q.dist[b]) * hit->second;
                    GroupId ga = segmentGroup(p.key, da, true);
                    GroupId gb = segmentGroup(q.key, db, true);
                    if (ga != kNoGroup && gb != kNoGroup) links.unite(ga, gb);
                }
            }
        }
    }
    mLinked.assign(mGroupCount, {});
    std::map<int, std::vector<GroupId>> byRoot;
    for (GroupId g = 0; g < GroupId(mGroupCount); ++g) byRoot[links.find(g)].push_back(g);
    for (auto const& [root, list] : byRoot)
        for (GroupId g : list) mLinked[g] = list;
}

// ---- sections -----------------------------------------------------------------------------------

GroupId SignalSystem::segmentGroup(EdgeKey const& key, double pos, bool towardsB) const {
    auto it = mEdges.find(key);
    if (it == mEdges.end()) return kNoGroup;
    auto const& info = it->second;
    int         k    = 0;
    for (auto const& [cpos, idx] : info.cuts) {
        if (towardsB ? pos >= cpos - kEps : pos > cpos + kEps) ++k;
        else break;
    }
    size_t seg = size_t(info.firstSegment + k);
    return seg < mSegmentGroup.size() ? mSegmentGroup[seg] : kNoGroup;
}

GroupId SignalSystem::groupOf(std::string const& boundaryId, int side) const {
    auto it = mBoundaryIndex.find(boundaryId);
    if (it == mBoundaryIndex.end() || side < 0 || side > 1) return kNoGroup;
    return mBoundaryGroups[it->second][side];
}

GroupId SignalSystem::groupAt(ct::Railways const& rail, ct::GraphLocation const& loc) const {
    ct::Graph const* g = rail.graph(loc.graph);
    if (!g) return kNoGroup;
    auto const* a = g->node(loc.node1);
    auto const* b = g->node(loc.node2);
    if (!a || !b) return kNoGroup;
    auto p = place(a->location, b->location, loc.position);
    if (!p.valid) return kNoGroup;
    return segmentGroup(p.key, p.pos, p.alongAB);
}

std::vector<GroupId> SignalSystem::groupsOn(Span const& span) const {
    std::vector<GroupId> out;
    auto                 key = keyOf(span.node1, span.node2);
    auto                 it  = mEdges.find(key);
    if (it == mEdges.end()) return out;
    auto const& info = it->second;
    double      len  = info.length;
    bool        ab   = key.a == span.node1;
    double      lo   = std::clamp(ab ? span.from : len - span.to, 0.0, len);
    double      hi   = std::clamp(ab ? span.to : len - span.from, 0.0, len);
    if (lo > hi) std::swap(lo, hi);
    bool   point = hi - lo < kEps;
    double prev  = 0;
    for (size_t k = 0; k <= info.cuts.size(); ++k) {
        double next = k < info.cuts.size() ? info.cuts[k].first : len;
        bool   hit  = point ? (lo >= prev - kEps && lo <= next + kEps) : (lo < next - kEps && hi > prev + kEps);
        if (hit) {
            size_t seg = size_t(info.firstSegment) + k;
            if (seg < mSegmentGroup.size() && mSegmentGroup[seg] != kNoGroup) out.push_back(mSegmentGroup[seg]);
        }
        prev = next;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<GroupId> const& SignalSystem::linked(GroupId g) const {
    static std::vector<GroupId> const none;
    if (g < 0 || size_t(g) >= mLinked.size()) return none;
    return mLinked[size_t(g)];
}

std::vector<std::pair<std::string, int>> SignalSystem::exits(std::string const& boundaryId, int side) const {
    auto it = mBoundaryIndex.find(boundaryId);
    if (it == mBoundaryIndex.end() || side < 0 || side > 1 || it->second >= mExitCache[side].size()) return {};
    return mExitCache[side][it->second];
}

std::vector<Crossing> SignalSystem::boundariesAlong(
    ct::Railways const&              rail,
    ct::GraphLocation const&         start,
    std::vector<ct::PathStep> const& path,
    double                           maxDistance
) const {
    std::vector<Crossing> out;
    ct::Graph const*      g = rail.graph(start.graph);
    if (!g) return out;
    std::vector<std::tuple<ct::NodeId, ct::NodeId, double>> legs{{start.node1, start.node2, start.position}};
    for (auto const& s : path) legs.push_back({s.node1, s.node2, 0.0});
    double walked = 0;
    for (size_t li = 0; li < legs.size(); ++li) {
        auto [n1, n2, from] = legs[li];
        auto const* a = g->node(n1);
        auto const* b = g->node(n2);
        auto const* e = g->edge(n1, n2);
        if (!a || !b || !e) break;
        EdgeKey key = keyOf(a->location, b->location);
        auto    it  = mEdges.find(key);
        double  len = g->edgeLength(*e);
        if (it != mEdges.end()) {
            bool                                ab = key.a == a->location;
            std::vector<std::pair<double, size_t>> hits;
            for (auto const& [cpos, idx] : it->second.cuts) {
                double tp = ab ? cpos : len - cpos;
                if (tp < from - kEps) continue;
                hits.push_back({tp, idx});
            }
            std::sort(hits.begin(), hits.end());
            for (auto const& [tp, idx] : hits) {
                double d = walked + tp - from;
                if (d > maxDistance) return out;
                auto const& bd = mBoundaries[idx];
                out.push_back({bd.id, bd.node1 == a->location ? 1 : 0, std::max(0.0, d)});
            }
        }
        walked += len - from;
        if (walked > maxDistance) break;
    }
    return out;
}

// ---- trains -------------------------------------------------------------------------------------

void SignalSystem::setTrainSpans(std::string const& train, std::vector<Span> spans) {
    std::set<GroupId> groups;
    for (auto const& s : spans)
        for (GroupId g : groupsOn(s)) groups.insert(g);
    mTrainSpans[train] = std::move(spans);
    mTrainGroups[train] = groups;
    // a train entering a section it reserved releases the reservation
    std::erase_if(mReservations, [&](Reservation const& r) {
        return r.train == train && groups.contains(groupOf(r.boundary, r.side));
    });
}

void SignalSystem::removeTrain(std::string const& train) {
    mTrainSpans.erase(train);
    mTrainGroups.erase(train);
    releaseReservations(train);
}

std::vector<std::string> SignalSystem::trains() const {
    std::vector<std::string> out;
    for (auto const& [id, s] : mTrainSpans) out.push_back(id);
    return out;
}

std::set<GroupId> SignalSystem::trainGroups(std::string const& train) const {
    auto it = mTrainGroups.find(train);
    return it == mTrainGroups.end() ? std::set<GroupId>{} : it->second;
}

std::vector<std::string> SignalSystem::trainsIn(GroupId g) const {
    std::vector<std::string> out;
    for (auto const& [id, groups] : mTrainGroups)
        if (groups.contains(g)) out.push_back(id);
    return out;
}

void SignalSystem::remapTrains() {
    for (auto& [id, spans] : mTrainSpans) {
        std::set<GroupId> groups;
        for (auto const& s : spans)
            for (GroupId g : groupsOn(s)) groups.insert(g);
        mTrainGroups[id] = std::move(groups);
    }
    std::erase_if(mReservations, [&](Reservation const& r) { return groupOf(r.boundary, r.side) == kNoGroup; });
}

std::optional<Reservation> SignalSystem::reservation(GroupId g) const {
    for (auto const& r : mReservations)
        if (groupOf(r.boundary, r.side) == g) return r;
    return std::nullopt;
}

bool SignalSystem::occupiedFor(GroupId g, std::string const& train) const {
    if (g == kNoGroup) return false;
    for (GroupId h : linked(g)) {
        for (auto const& [id, groups] : mTrainGroups)
            if (id != train && groups.contains(h)) return true;
        for (auto const& r : mReservations)
            if (r.train != train && groupOf(r.boundary, r.side) == h) return true;
    }
    return false;
}

bool SignalSystem::occupiedUnless(GroupId g, std::string const& boundaryId, int side) const {
    if (g == kNoGroup) return false;
    for (GroupId h : linked(g)) {
        for (auto const& [id, groups] : mTrainGroups)
            if (groups.contains(h)) return true;
        for (auto const& r : mReservations)
            if (groupOf(r.boundary, r.side) == h && (r.boundary != boundaryId || r.side != side)) return true;
    }
    return false;
}

void SignalSystem::releaseReservations(std::string const& train) {
    std::erase_if(mReservations, [&](Reservation const& r) { return r.train == train; });
}

Authority SignalSystem::checkAhead(
    ct::Railways const&              rail,
    std::string const&               train,
    ct::GraphLocation const&         front,
    std::vector<ct::PathStep> const& path,
    double                           lookahead
) {
    releaseReservations(train);
    Authority a;
    auto      crossings = boundariesAlong(rail, front, path);

    std::optional<Crossing>   chainStart; // the first cross signal of an unresolved chain
    std::vector<Reservation> pending;
    auto commit = [&] {
        for (auto const& r : pending) {
            GroupId g = groupOf(r.boundary, r.side);
            if (g == kNoGroup) continue;
            mReservations.push_back(r);
            a.reserved.push_back(g);
        }
        pending.clear();
        chainStart.reset();
    };

    for (auto const& c : crossings) {
        auto const* b = boundary(c.boundary);
        if (!b) continue;
        auto const& side = b->sides[size_t(c.side)];
        if (!side.hasSignal) {
            // no signal for this direction: the train passes freely, which ends a cross chain
            if (chainStart) commit();
            continue;
        }
        if (!chainStart && c.distance > lookahead) break;
        GroupId g        = groupOf(c.boundary, c.side);
        bool    occupied = side.forcedRed || occupiedFor(g, train);
        if (occupied) {
            Crossing const& at = chainStart ? *chainStart : c;
            a.clear            = false;
            a.stopDistance     = at.distance;
            a.waitingFor       = at.boundary;
            a.waitingSide      = at.side;
            pending.clear();
            chainStart.reset();
            break;
        }
        pending.push_back({train, c.boundary, c.side});
        if (side.type == SignalType::Cross) {
            if (!chainStart) chainStart = c;
            continue;
        }
        commit(); // a free entry signal ends the chain
    }
    // the path ends inside the chain (destination in a junction section): nothing left to wait for
    if (a.clear && chainStart) commit();
    return a;
}

SignalSystem::EdgeCost SignalSystem::edgeCost(
    ct::NodeLocation const& from,
    ct::NodeLocation const& to,
    std::string const&      train,
    double&                 redWeight,
    double                  redstonePenalty
) const {
    EdgeCost out;
    auto     key = keyOf(from, to);
    auto     it  = mEdges.find(key);
    if (it == mEdges.end()) return out;
    bool                                   ab = key.a == from;
    std::vector<std::pair<double, size_t>> hits; // (distance from `from`, boundary index)
    for (auto const& [cpos, idx] : it->second.cuts) hits.push_back({ab ? cpos : it->second.length - cpos, idx});
    std::sort(hits.begin(), hits.end());
    for (auto const& [d, idx] : hits) {
        auto const& b    = mBoundaries[idx];
        int         side = b.node1 == from ? 1 : 0;
        auto const& s    = b.sides[size_t(side)];
        if (!s.hasSignal) { // Create's SignalBoundary.canNavigateVia: no signal faces this way
            out.barrier = d;
            break;
        }
        if (s.forcedRed) {
            out.penalty += redstonePenalty;
        } else if (occupiedFor(mBoundaryGroups[idx][size_t(side)], train)) {
            out.penalty += redWeight;
            redWeight /= 2;
        }
    }
    return out;
}

double SignalSystem::edgePenalty(
    ct::NodeLocation const& from,
    ct::NodeLocation const& to,
    std::string const&      train,
    double                  redPenalty,
    double                  redstonePenalty
) const {
    double weight = redPenalty;
    double total  = 0;
    auto   key    = keyOf(from, to);
    auto   it     = mEdges.find(key);
    if (it == mEdges.end()) return 0;
    for (auto const& [cpos, idx] : it->second.cuts) {
        auto const& b    = mBoundaries[idx];
        int         side = b.node1 == from ? 1 : 0;
        auto const& s    = b.sides[size_t(side)];
        if (!s.hasSignal) continue;
        if (s.forcedRed) total += redstonePenalty;
        else if (occupiedFor(mBoundaryGroups[idx][size_t(side)], train)) total += weight;
    }
    return total;
}

// ---- states -------------------------------------------------------------------------------------

void SignalSystem::updateStates() {
    mStates.resize(mBoundaries.size(), {SignalState::Invalid, SignalState::Invalid});
    for (size_t i = 0; i < mBoundaries.size(); ++i) {
        auto const& b = mBoundaries[i];
        for (int s = 0; s < 2; ++s) {
            auto const& side = b.sides[size_t(s)];
            GroupId     g    = mBoundaryGroups[i][size_t(s)];
            if (!side.hasSignal || g == kNoGroup) {
                mStates[i][size_t(s)] = SignalState::Invalid;
                continue;
            }
            if (g == mBoundaryGroups[i][size_t(1 - s)]) { // Create: both sides in one section (a loop)
                mStates[i][size_t(s)] = SignalState::Invalid;
                continue;
            }
            bool red              = side.forcedRed || occupiedUnless(g, b.id, s);
            mStates[i][size_t(s)] = red ? SignalState::Red : SignalState::Green;
        }
    }
    // cross signals: Create's resolveSignalChain over the signals leading out of their section
    // (green when all are green or invalid, red when all are red, yellow when mixed). Chained cross
    // signals read each other's results: repeat until nothing changes.
    std::vector<std::array<bool, 2>> free(mBoundaries.size(), {false, false});
    for (size_t i = 0; i < mBoundaries.size(); ++i)
        for (int s = 0; s < 2; ++s) free[i][size_t(s)] = mStates[i][size_t(s)] == SignalState::Green;
    for (size_t pass = 0; pass < mBoundaries.size() + 1; ++pass) {
        bool changed = false;
        for (size_t i = 0; i < mBoundaries.size(); ++i) {
            for (int s = 0; s < 2; ++s) {
                if (!free[i][size_t(s)] || mBoundaries[i].sides[size_t(s)].type != SignalType::Cross) continue;
                bool allFree = true, noneFree = true;
                for (auto const& [id, es] : mExitCache[s].size() > i ? mExitCache[s][i] : std::vector<std::pair<std::string, int>>{}) {
                    auto j = mBoundaryIndex.find(id);
                    if (j == mBoundaryIndex.end() || !mBoundaries[j->second].sides[size_t(es)].hasSignal) continue;
                    auto other = mStates[j->second][size_t(es)];
                    allFree &= other == SignalState::Green || other == SignalState::Invalid;
                    noneFree &= other == SignalState::Red;
                }
                auto next = allFree ? SignalState::Green : noneFree ? SignalState::Red : SignalState::Yellow;
                if (next != mStates[i][size_t(s)]) {
                    mStates[i][size_t(s)] = next;
                    changed               = true;
                }
            }
        }
        if (!changed) break;
    }
}

SignalState SignalSystem::state(std::string const& boundaryId, int side) const {
    auto it = mBoundaryIndex.find(boundaryId);
    if (it == mBoundaryIndex.end() || side < 0 || side > 1 || it->second >= mStates.size()) return SignalState::Invalid;
    return mStates[it->second][size_t(side)];
}

// ---- observers ----------------------------------------------------------------------------------

std::vector<std::string> SignalSystem::trainsOver(std::string const& observerId) const {
    std::vector<std::string> out;
    auto obs = std::find_if(mObservers.begin(), mObservers.end(), [&](Observer const& o) { return o.id == observerId; });
    if (obs == mObservers.end()) return out;
    auto key = keyOf(obs->node1, obs->node2);
    auto it  = mEdges.find(key);
    if (it == mEdges.end()) return out;
    double len = it->second.length;
    for (auto const& [id, spans] : mTrainSpans) {
        for (auto const& s : spans) {
            if (keyOf(s.node1, s.node2) != key) continue;
            double p = s.node1 == obs->node1 ? obs->position : len - obs->position;
            if (p >= s.from - kEps && p <= s.to + kEps) {
                out.push_back(id);
                break;
            }
        }
    }
    return out;
}

} // namespace lv::core::signals
