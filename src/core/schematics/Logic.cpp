#include "core/schematics/Logic.h"

#include <cmath>

namespace lv::core::schematics {

// ---- selection ------------------------------------------------------------------------------------

std::array<Pos, 2> Selection::box() const {
    Pos a = first.value_or(Pos{}), b = second.value_or(a);
    return {Pos{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}, Pos{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}};
}

Pos Selection::size() const {
    auto [lo, hi] = box();
    return {hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
}

void Selection::scrollRange(int delta) { range = std::clamp(range + delta, 1, 100); }

void Selection::resize(int face, int delta, bool cameraInside) {
    if (!complete() || face < 0 || face > 5 || delta == 0) return;
    static constexpr std::array<Pos, 6> kNormal{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
    auto [lo, hi] = box();
    if (cameraInside) delta = -delta;
    Pos  n    = kNormal[static_cast<std::size_t>(face)];
    int  step = (n.x + n.y + n.z) > 0 ? 1 : -1;
    Pos  v{n.x * delta, n.y * delta, n.z * delta};
    if (step < 0) {
        lo = lo - v;
        hi = hi - v;
    }
    hi = {std::max(hi.x - v.x * step, lo.x), std::max(hi.y - v.y * step, lo.y), std::max(hi.z - v.z * step, lo.z)};
    first  = lo;
    second = hi;
}

// ---- schematicannon ---------------------------------------------------------------------------------

bool shouldPlace(PlaceQuery const& q) {
    if (q.nearCannon) return false;
    if (q.protectData && q.worldHasData) return false;
    if (q.ignored) return false;
    switch (q.mode) {
    case ReplaceMode::ReplaceWithEmpty:
        return true;
    case ReplaceMode::ReplaceWithAny:
        return !q.placingAir;
    case ReplaceMode::ReplaceWithSolid:
        return !q.placingAir && (q.schematicSolid || (!q.worldSolid && !q.worldOtherSolid));
    case ReplaceMode::DontReplaceSolid:
        return !q.placingAir && !q.worldSolid && !q.worldOtherSolid;
    }
    return false;
}

int flightTicks(Pos from, Pos to) {
    double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    double d2 = dx * dx + dy * dy + dz * dz;
    return static_cast<int>(std::max(10.0, std::sqrt(std::sqrt(d2)) * 4.0));
}

bool PrintCursor::advance() {
    if (mMax.x < mMin.x || mMax.y < mMin.y || mMax.z < mMin.z) return false;
    ++mCur.x;
    if (mCur.x > mMax.x) {
        mCur.x = mMin.x;
        ++mCur.z;
    }
    if (mCur.z > mMax.z) {
        mCur.z = mMin.z;
        ++mCur.y;
    }
    return mCur.y <= mMax.y;
}

std::vector<int> PrintCursor::save() const {
    std::vector<int> out{mStage, mCur.x, mCur.y, mCur.z, static_cast<int>(mDeferredIndex), static_cast<int>(mDeferred.size())};
    for (auto const& p : mDeferred) {
        out.push_back(p.x);
        out.push_back(p.y);
        out.push_back(p.z);
    }
    return out;
}

PrintCursor PrintCursor::load(Pos min, Pos max, std::vector<int> const& d) {
    PrintCursor c(min, max);
    if (d.size() < 6) return c;
    c.mStage         = std::clamp(d[0], 0, 2);
    c.mCur           = {d[1], d[2], d[3]};
    c.mDeferredIndex = static_cast<std::size_t>(std::max(0, d[4]));
    auto n           = static_cast<std::size_t>(std::max(0, d[5]));
    for (std::size_t i = 0; i < n && 6 + i * 3 + 2 < d.size(); ++i) c.mDeferred.push_back({d[6 + i * 3], d[7 + i * 3], d[8 + i * 3]});
    return c;
}

// ---- checklist --------------------------------------------------------------------------------------

void Checklist::require(std::string const& item, int count) {
    if (item.empty() || item == "minecraft:air" || count <= 0) return;
    required[item] += count;
}

void Checklist::collect(std::string const& item, int count) {
    if (required.contains(item)) gathered[item] += count;
}

int Checklist::missing(std::string const& item) const {
    auto r = required.find(item);
    if (r == required.end()) return 0;
    auto g = gathered.find(item);
    return r->second - (g == gathered.end() ? 0 : g->second);
}

std::string checklistAmount(int amount) {
    return " x" + std::to_string(amount) + " | " + std::to_string(amount / 64) + "▤ +" + std::to_string(amount % 64);
}

std::vector<std::string> checklistPages(
    std::vector<Checklist::Entry> const& entries,
    std::vector<std::string> const&      names,
    bool                                 blocksNotLoaded,
    std::string const&                   notLoadedText
) {
    // Bedrock formatting codes: blue §9, dark green §2, black §0, gray §7, red §c
    std::vector<std::string> pages;
    if (blocksNotLoaded) pages.push_back("\n§c" + notLoadedText);
    std::string page;
    int         written = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto const& e = entries[i];
        if (written == 5) {
            written = 0;
            page += e.complete ? "\n§2 >>>" : "\n§9 >>>";
            pages.push_back(page);
            page.clear();
        }
        ++written;
        std::string const& name = i < names.size() ? names[i] : e.item;
        page += (e.complete ? "§2" : "§9") + name + (e.complete ? " ✔" : "");
        page += "\n§0 x" + std::to_string(e.amount) + "§7 | " + std::to_string(e.amount / 64) + "▤ +" + std::to_string(e.amount % 64) + "\n";
    }
    pages.push_back(page);
    return pages;
}

// ---- blueprint ----------------------------------------------------------------------------------------

std::vector<Pos> blueprintCells(Pos at, int face, int horizontal, int size) {
    static constexpr std::array<Pos, 6> kDir{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
    auto dir = [](int f) { return kDir[static_cast<std::size_t>(std::clamp(f, 0, 5))]; };
    // counter-clockwise / clockwise seen from above (north -> west -> south -> east)
    auto ccw = [](int f) {
        switch (f) {
        case 2:
            return 4;
        case 4:
            return 3;
        case 3:
            return 5;
        default:
            return 2;
        }
    };
    auto cw  = [&](int f) { return ccw(ccw(ccw(f))); };
    auto opp = [](int f) { return f ^ 1; };
    if (horizontal < 2 || horizontal > 5) horizontal = 2;
    Pos up, right;
    if (face >= 2) {
        up    = {0, 1, 0};
        right = dir(ccw(face));
    } else if (face == 1) {
        up    = dir(horizontal);
        right = dir(cw(horizontal));
    } else {
        up    = dir(opp(horizontal));
        right = dir(cw(horizontal));
    }
    size     = std::clamp(size, 1, 3);
    int base = size == 3 ? -1 : 0;
    std::vector<Pos> out;
    for (int row = 0; row < size; ++row) {
        for (int col = 0; col < size; ++col) {
            int c = base + col, r = base + row;
            out.push_back({at.x + right.x * c + up.x * r, at.y + right.y * c + up.y * r, at.z + right.z * c + up.z * r});
        }
    }
    return out;
}

} // namespace lv::core::schematics
