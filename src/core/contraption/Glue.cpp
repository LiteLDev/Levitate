#include "core/contraption/Glue.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace lv::core::contraption {

GlueBox GlueBox::of(Pos const& a, Pos const& b) {
    return {
        {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)},
        {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}
    };
}

bool GlueBox::contains(Pos const& p) const {
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
}

bool GlueBox::intersects(GlueBox const& o) const {
    return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y && min.z <= o.max.z
        && max.z >= o.min.z;
}

bool isGlued(std::vector<GlueBox> const& boxes, Pos const& a, Dir d) {
    Pos b = a + offset(d);
    return std::any_of(boxes.begin(), boxes.end(), [&](GlueBox const& box) { return box.contains(a) && box.contains(b); });
}

GlueCheck checkSelection(Pos const& first, Pos const& second, int maxRange, std::function<bool(Pos const&)> const& solid) {
    auto box = GlueBox::of(first, second);
    // Create's BlockPos#closerThan(hovered, 24): the corners' distance, not each side
    Pos  d   = second - first;
    long long dist2 = static_cast<long long>(d.x) * d.x + static_cast<long long>(d.y) * d.y + static_cast<long long>(d.z) * d.z;
    if (dist2 >= static_cast<long long>(maxRange) * maxRange) return GlueCheck::TooFar;
    if (first == second) return GlueCheck::SingleBlock; // searchGlueGroup: no 1-block glue
    std::deque<Pos>                  frontier{first};
    std::unordered_set<Pos, PosHash> visited{first};
    while (!frontier.empty()) {
        Pos p = frontier.front();
        frontier.pop_front();
        if (p == second) return GlueCheck::Ok;
        for (auto d : kAllDirs) {
            Pos n = p + offset(d);
            if (!box.contains(n) || visited.contains(n)) continue;
            visited.insert(n);
            if (n != second && !solid(n)) continue;
            frontier.push_back(n);
        }
    }
    return GlueCheck::CannotReach;
}

GlueBox transformBox(GlueBox const& box, std::function<Pos(Pos const&)> const& place) {
    return GlueBox::of(place(box.min), place(box.max));
}

} // namespace lv::core::contraption
