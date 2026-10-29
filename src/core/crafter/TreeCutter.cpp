#include "core/crafter/TreeCutter.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>

namespace lv::core::crafter {

namespace {

TreePos at(TreePos p, int dx, int dy, int dz) { return {p.x + dx, p.y + dy, p.z + dz}; }

bool logOrRoot(TreeKind k) { return k == TreeKind::Log || k == TreeKind::Root; }

/// Create's validateCut: no log above the cut layer may stand on another log (the cut must free
/// the whole tree).
bool validateCut(TreeReader const& read, TreePos pos, int limit) {
    std::set<TreePos>   visited;
    std::deque<TreePos> frontier{pos, at(pos, 0, 1, 0)};
    while (!frontier.empty() && static_cast<int>(visited.size()) < limit) {
        TreePos cur = frontier.front();
        frontier.pop_front();
        TreePos below = at(cur, 0, -1, 0);
        visited.insert(cur);
        bool lowerLayer = cur.y == pos.y;
        if (!logOrRoot(read(cur).kind)) continue;
        if (!lowerLayer && !(below == pos) && logOrRoot(read(below).kind)) return false;
        for (auto f : kinetics::kFaces) {
            if (f == kinetics::Face::Down) continue;
            if (f == kinetics::Face::Up && !lowerLayer) continue;
            auto    o    = kinetics::offsetOf(f);
            TreePos next = at(cur, o.x, o.y, o.z);
            if (visited.contains(next)) continue;
            frontier.push_back(next);
        }
    }
    return true;
}

/// Positions of the box x/z +-1, y minY..maxY around p, in BlockPos.betweenClosed order (x fastest,
/// then y... Mojang iterates x, then y, then z).
template <class F>
void box(TreePos p, int minY, int maxY, F&& fn) {
    for (int z = -1; z <= 1; ++z)
        for (int y = minY; y <= maxY; ++y)
            for (int x = -1; x <= 1; ++x) fn(at(p, x, y, z));
}

} // namespace

int computeLeafDistance(TreeReader const& read, TreePos pos) {
    // BFS through face-adjacent leaves, distance to the first log (capped at 7, as vanilla)
    std::map<TreePos, int> dist{{pos, 0}};
    std::deque<TreePos>    q{pos};
    while (!q.empty()) {
        TreePos cur = q.front();
        q.pop_front();
        int d = dist[cur];
        if (d >= 7) continue;
        for (auto f : kinetics::kFaces) {
            auto    o = kinetics::offsetOf(f);
            TreePos n = at(cur, o.x, o.y, o.z);
            if (dist.contains(n)) continue;
            auto k = read(n).kind;
            if (k == TreeKind::Log) return d + 1;
            if (k != TreeKind::Leaf) continue;
            dist[n] = d + 1;
            q.push_back(n);
        }
    }
    return 7;
}

Tree findTree(TreeReader const& read, TreePos pos, TreeBlock broken, int maxBlocks) {
    Tree    tree;
    TreePos above      = at(pos, 0, 1, 0);
    auto    aboveBlock = read(above);

    // bamboo, sugar cane, cactus, kelp: everything above the cut, top first
    if (broken.kind == TreeKind::VerticalPlant) {
        if (aboveBlock.kind != TreeKind::VerticalPlant) return {};
        for (int i = 1; i < 256; ++i) {
            TreePos cur = at(pos, 0, i, 0);
            if (read(cur).kind != TreeKind::VerticalPlant) break;
            tree.logs.push_back(cur);
        }
        std::reverse(tree.logs.begin(), tree.logs.end());
        return tree;
    }

    // chorus: the connected plant above the cut, last found first
    if (broken.kind == TreeKind::Chorus) {
        if (aboveBlock.kind != TreeKind::Chorus) return {};
        std::set<TreePos>   visited;
        std::deque<TreePos> frontier{above};
        while (!frontier.empty() && static_cast<int>(tree.logs.size()) < maxBlocks) {
            TreePos cur = frontier.front();
            frontier.pop_front();
            if (!visited.insert(cur).second) continue;
            tree.logs.push_back(cur);
            for (auto f : kinetics::kFaces) {
                auto    o = kinetics::offsetOf(f);
                TreePos n = at(cur, o.x, o.y, o.z);
                if (visited.contains(n) || read(n).kind != TreeKind::Chorus) continue;
                frontier.push_back(n);
            }
        }
        std::reverse(tree.logs.begin(), tree.logs.end());
        return tree;
    }

    // regular trees
    if (!validateCut(read, pos, maxBlocks)) return {};

    std::set<TreePos>   visited{pos};
    std::deque<TreePos> frontier;
    box(pos, 0, 1, [&](TreePos p) { frontier.push_back(p); });

    std::set<TreePos> logSet;
    bool              hasRoots = false;
    while (!frontier.empty() && static_cast<int>(logSet.size()) < maxBlocks) {
        TreePos cur = frontier.front();
        frontier.pop_front();
        if (!visited.insert(cur).second) continue;
        auto k = read(cur).kind;
        if (k == TreeKind::Root) hasRoots = true;
        else if (k != TreeKind::Log) continue;
        tree.logs.push_back(cur);
        logSet.insert(cur);
        box(cur, 0, 1, [&](TreePos p) {
            if (!visited.contains(p)) frontier.push_back(p);
        });
    }

    auto restart = [&] {
        visited = logSet;
        frontier.assign(tree.logs.begin(), tree.logs.end());
    };
    restart();

    if (hasRoots) {
        while (!frontier.empty() && static_cast<int>(logSet.size()) < maxBlocks) {
            TreePos cur = frontier.front();
            frontier.pop_front();
            if (!logSet.contains(cur) && !visited.insert(cur).second) continue;
            if (read(cur).kind != TreeKind::Root) continue;
            if (logSet.insert(cur).second) tree.logs.push_back(cur);
            box(cur, -1, 0, [&](TreePos p) {
                if (!visited.contains(p)) frontier.push_back(p);
            });
        }
        restart();
    }

    // leaves and attachments
    std::map<TreePos, int> leafDistances;
    auto leafDistance = [&](TreePos p, TreeBlock const& b) {
        if (b.leafDistance >= 0) return b.leafDistance;
        auto it = leafDistances.find(p);
        if (it != leafDistances.end()) return it->second;
        int d            = computeLeafDistance(read, p);
        leafDistances[p] = d;
        return d;
    };
    int budget = maxBlocks;
    while (!frontier.empty() && budget-- > 0) {
        TreePos prev = frontier.front();
        frontier.pop_front();
        if (!logSet.contains(prev) && !visited.insert(prev).second) continue;
        auto prevBlock    = read(prev);
        int  prevDistance = prevBlock.kind == TreeKind::Leaf ? leafDistance(prev, prevBlock) : 0;
        box(prev, -1, 1, [&](TreePos cur) {
            if (visited.contains(cur)) return;
            auto b = read(cur);
            if (b.kind == TreeKind::Attachment) {
                tree.attachments.push_back(cur);
                visited.insert(cur);
                return;
            }
            int horizontal = std::max(std::abs(cur.x - pos.x), std::abs(cur.z - pos.z));
            if (horizontal <= b.nonDecaying) {
                tree.leaves.push_back(cur);
                frontier.push_back(cur);
                return;
            }
            if (b.kind == TreeKind::Leaf && leafDistance(cur, b) > prevDistance) {
                tree.leaves.push_back(cur);
                frontier.push_back(cur);
            }
        });
    }
    // a leaf can be queued from several neighbours before it is visited
    auto dedupe = [](std::vector<TreePos>& v) {
        std::set<TreePos>    seen;
        std::vector<TreePos> out;
        for (auto const& p : v)
            if (seen.insert(p).second) out.push_back(p);
        v = std::move(out);
    };
    dedupe(tree.leaves);
    dedupe(tree.attachments);
    return tree;
}

} // namespace lv::core::crafter
