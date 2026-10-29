#include "core/crafter/Crafter.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

namespace lv::core::crafter {

int xRotation(Pointing p) {
    switch (p) {
    case Pointing::Up:
        return 0;
    case Pointing::Left:
        return 270;
    case Pointing::Down:
        return 180;
    default:
        return 90;
    }
}

Pointing cycle(Pointing p) { return static_cast<Pointing>((static_cast<int>(p) + 1) % 4); }

namespace {
/// AngleHelper.horizontalAngle: south 0, west -90, north 180, east 90 (degrees).
double horizontalAngle(Face f) {
    switch (f) {
    case Face::South:
        return 0;
    case Face::West:
        return -90;
    case Face::North:
        return 180;
    case Face::East:
        return 90;
    default:
        return 0;
    }
}
double rad(double deg) { return deg * std::numbers::pi / 180.0; }
} // namespace

Face targetDirection(Face facing, Pointing p) {
    // (0, 1, 0) rotated by -xRot about Z, then by the horizontal angle about Y (Create's VecHelper.rotate)
    double a = rad(-xRotation(p));
    double x = -std::sin(a), y = std::cos(a), z = 0;
    double b = rad(horizontalAngle(facing));
    double x2 = x * std::cos(b) + z * std::sin(b);
    double z2 = z * std::cos(b) - x * std::sin(b);
    auto   r  = [](double v) { return static_cast<int>(std::lround(v)); };
    return kinetics::nearestFace(r(x2), r(y), r(z2));
}

Pointing pointingFromFacing(Face clickedFace, Face blockFacing) {
    bool positive = kinetics::isPositive(blockFacing);
    switch (clickedFace) {
    case Face::Up:
        return Pointing::Down;
    case Face::Down:
        return Pointing::Up;
    case Face::East:
        return positive ? Pointing::Left : Pointing::Right;
    case Face::West:
        return positive ? Pointing::Right : Pointing::Left;
    case Face::North:
        return positive ? Pointing::Left : Pointing::Right;
    default: // south
        return positive ? Pointing::Right : Pointing::Left;
    }
}

Face horizontalOpposite(Face lookFacing) { return kinetics::opposite(lookFacing); }

bool opposed(Pointing a, Pointing b) { return std::abs(xRotation(a) - xRotation(b)) == 180; }

Cell mergeOffset(Pointing p) {
    switch (p) {
    case Pointing::Left:
        return {1, 0};
    case Pointing::Right:
        return {-1, 0};
    case Pointing::Down:
        return {0, 1};
    default:
        return {0, -1};
    }
}

GridStats statsOf(std::vector<Cell> const& cells) {
    GridStats s;
    for (auto const& [x, y] : cells) {
        s.minX = std::min(s.minX, x);
        s.minY = std::min(s.minY, y);
        s.maxX = std::max(s.maxX, x);
        s.maxY = std::max(s.maxY, y);
    }
    s.width  = s.maxX - s.minX + 1;
    s.height = s.maxY - s.minY + 1;
    return s;
}

int InputMatrix::count() const {
    int n = 0;
    for (int c : cells)
        if (c >= 0) ++n;
    return n;
}

InputMatrix inputMatrix(std::vector<Cell> const& cells, std::vector<bool> const& present) {
    auto        s = statsOf(cells);
    InputMatrix full;
    full.width  = s.width;
    full.height = s.height;
    full.cells.assign(static_cast<std::size_t>(s.width * s.height), -1);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i >= present.size() || !present[i]) continue;
        int col = cells[i].first - s.minX;
        int row = s.maxY - cells[i].second; // row 0 = top
        full.cells[static_cast<std::size_t>(row * s.width + col)] = static_cast<int>(i);
    }
    // trim empty outer rows / columns
    int x0 = full.width, x1 = -1, y0 = full.height, y1 = -1;
    for (int y = 0; y < full.height; ++y)
        for (int x = 0; x < full.width; ++x)
            if (full.at(x, y) >= 0) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    InputMatrix out;
    if (x1 < 0) return out;
    out.width  = x1 - x0 + 1;
    out.height = y1 - y0 + 1;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) out.cells.push_back(full.at(x, y));
    return out;
}

namespace {
/// The pattern without empty outer rows / columns (vanilla ShapedRecipePattern.shrink).
ShapedPattern trimmed(ShapedPattern const& p) {
    int x0 = p.width, x1 = -1, y0 = p.height, y1 = -1;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            if (p.at(x, y) >= 0) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    ShapedPattern out;
    if (x1 < 0) return out;
    out.width  = x1 - x0 + 1;
    out.height = y1 - y0 + 1;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) out.cells.push_back(p.at(x, y));
    return out;
}
} // namespace

bool matchShaped(ShapedPattern const& raw, InputMatrix const& input, bool mirror, std::function<bool(int, int)> const& test) {
    ShapedPattern pattern = trimmed(raw);
    if (pattern.width == 0 || pattern.width != input.width || pattern.height != input.height) return false;
    auto attempt = [&](bool mirrored) {
        for (int y = 0; y < input.height; ++y) {
            for (int x = 0; x < input.width; ++x) {
                int ing  = pattern.at(mirrored ? pattern.width - 1 - x : x, y);
                int cell = input.at(x, y);
                if ((ing < 0) != (cell < 0)) return false;
                if (ing < 0) continue;
                if (!test(ing, cell)) return false;
            }
        }
        return true;
    };
    return attempt(false) || (mirror && attempt(true));
}

bool matchShapeless(int ingredientCount, InputMatrix const& input, std::function<bool(int, int)> const& test) {
    std::vector<int> items;
    for (int c : input.cells)
        if (c >= 0) items.push_back(c);
    if (static_cast<int>(items.size()) != ingredientCount) return false;
    // Kuhn's bipartite matching: items -> ingredients
    std::vector<int> owner(static_cast<std::size_t>(ingredientCount), -1);
    std::function<bool(int, std::vector<bool>&)> assign = [&](int item, std::vector<bool>& seen) -> bool {
        for (int ing = 0; ing < ingredientCount; ++ing) {
            if (seen[static_cast<std::size_t>(ing)] || !test(ing, items[static_cast<std::size_t>(item)])) continue;
            seen[static_cast<std::size_t>(ing)] = true;
            auto& o = owner[static_cast<std::size_t>(ing)];
            if (o < 0 || assign(o, seen)) {
                o = item;
                return true;
            }
        }
        return false;
    };
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        std::vector<bool> seen(static_cast<std::size_t>(ingredientCount), false);
        if (!assign(i, seen)) return false;
    }
    return true;
}

std::optional<ShapedPattern> parsePattern(std::vector<std::string> const& rows, std::map<char, int> const& key) {
    if (rows.empty()) return std::nullopt;
    ShapedPattern p;
    p.height = static_cast<int>(rows.size());
    for (auto const& r : rows) p.width = std::max(p.width, static_cast<int>(r.size()));
    if (p.width == 0) return std::nullopt;
    for (auto const& r : rows) {
        for (int x = 0; x < p.width; ++x) {
            char c = x < static_cast<int>(r.size()) ? r[static_cast<std::size_t>(x)] : ' ';
            if (c == ' ') {
                p.cells.push_back(-1);
                continue;
            }
            auto it = key.find(c);
            if (it == key.end()) return std::nullopt;
            p.cells.push_back(it->second);
        }
    }
    return p;
}

std::vector<Offset> inventoryOrder(std::vector<Offset> offsets, Face facing) {
    int  axisStep = kinetics::isPositive(facing) ? 1 : -1;
    // the facing's clockwise neighbour: its axis is the row axis
    Axis compareAxis = kinetics::axisOf(facing) == Axis::X ? Axis::Z : Axis::X;
    int  modifier    = axisStep * (compareAxis == Axis::Z ? -1 : 1);
    std::stable_sort(offsets.begin(), offsets.end(), [&](Offset const& a, Offset const& b) {
        if (a.y != b.y) return a.y > b.y;
        int c1 = kinetics::choose(compareAxis, a), c2 = kinetics::choose(compareAxis, b);
        return modifier > 0 ? c1 < c2 : c1 > c2;
    });
    return offsets;
}

std::optional<Face> edgeAt(Face face, double hx, double hy, double hz, std::vector<Face> const& candidates) {
    constexpr double band = 3.0 / 16.0, eps = 1.0 / 256.0;
    for (Face d : candidates) {
        if (kinetics::axisOf(d) == kinetics::axisOf(face)) continue;
        double c = kinetics::axisOf(d) == Axis::X ? hx : kinetics::axisOf(d) == Axis::Y ? hy : hz;
        bool   in = kinetics::isPositive(d) ? c >= 1.0 - band - eps : c <= band + eps;
        if (in) return d;
    }
    return std::nullopt;
}

int countDownSpeed(float rpm) {
    if (rpm == 0) return 0;
    return std::clamp(static_cast<int>(std::abs(rpm)), 4, 250);
}

char const* phaseName(Phase p) {
    switch (p) {
    case Phase::Idle:
        return "IDLE";
    case Phase::Accepting:
        return "ACCEPTING";
    case Phase::Assembling:
        return "ASSEMBLING";
    case Phase::Exporting:
        return "EXPORTING";
    case Phase::Waiting:
        return "WAITING";
    case Phase::Crafting:
        return "CRAFTING";
    default:
        return "INSERTING";
    }
}

Phase phaseFromName(std::string_view name) {
    for (int i = 0; i <= static_cast<int>(Phase::Inserting); ++i)
        if (name == phaseName(static_cast<Phase>(i))) return static_cast<Phase>(i);
    return Phase::Idle;
}

} // namespace lv::core::crafter
