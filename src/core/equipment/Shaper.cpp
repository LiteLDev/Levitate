#include "core/equipment/Shaper.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

namespace lv::core::equipment {

nlohmann::json ShaperSettings::toJson() const {
    return {
        {"brush", static_cast<int>(brush)},
        {"params", {params[0], params[1], params[2]}},
        {"tool", static_cast<int>(tool)},
        {"placement", static_cast<int>(placement)},
        {"pattern", static_cast<int>(pattern)},
        {"configured", configured},
        {"material", material},
    };
}

ShaperSettings ShaperSettings::fromJson(nlohmann::json const& j) {
    ShaperSettings s;
    if (!j.is_object()) return s;
    s.brush = static_cast<Brush>(std::clamp(j.value("brush", 0), 0, 4));
    auto p  = j.value("params", nlohmann::json::array());
    if (p.is_array() && p.size() == 3)
        for (std::size_t i = 0; i < 3; ++i) s.params[i] = p[i].is_number_integer() ? p[i].get<int>() : 1;
    s.tool       = static_cast<ShaperTool>(std::clamp(j.value("tool", 0), 0, 5));
    s.placement  = static_cast<Placement>(std::clamp(j.value("placement", 0), 0, 2));
    s.pattern    = static_cast<Pattern>(std::clamp(j.value("pattern", 0), 0, 5));
    s.configured = j.value("configured", false);
    s.material   = j.value("material", std::string{});
    sanitize(s);
    return s;
}

std::vector<ParamSpec> brushParams(Brush b) {
    switch (b) {
    case Brush::Cuboid:
        return {{"create.generic.width", "Width", 1, 32, 1}, {"create.generic.height", "Height", 1, 32, 1}, {"create.generic.length", "Length", 1, 32, 1}};
    case Brush::Sphere:
        return {{"create.generic.radius", "Radius", 0, 10, 1}};
    case Brush::Cylinder:
        return {{"create.generic.radius", "Radius", 0, 8, 1}, {"create.generic.height", "Height", 1, 8, 1}};
    case Brush::Surface:
    case Brush::Cluster:
        return {
            {"create.generic.range", "Range", 1, 10, 1},
            {"create.gui.terrainzapper.searchDiagonal", "Follow Diagonals", 0, 1, 1, true},
            {"create.gui.terrainzapper.searchFuzzy", "Ignore Material Borders", 0, 1, 1, true},
        };
    }
    return {};
}

std::vector<ShaperTool> brushTools(Brush b) {
    using T = ShaperTool;
    if (b == Brush::Surface) return {T::Overlay, T::Replace, T::Clear};
    if (b == Brush::Cluster) return {T::Replace, T::Clear};
    return {T::Fill, T::Place, T::Replace, T::Clear, T::Overlay, T::Flatten};
}

bool hasPlacement(Brush b) { return b == Brush::Cuboid || b == Brush::Sphere || b == Brush::Cylinder; }

void sanitize(ShaperSettings& s) {
    auto specs = brushParams(s.brush);
    for (std::size_t i = 0; i < 3; ++i) {
        if (i < specs.size()) s.params[i] = std::clamp(s.params[i], specs[i].min, specs[i].max);
        else s.params[i] = std::max(0, s.params[i]);
    }
    auto tools = brushTools(s.brush);
    if (std::find(tools.begin(), tools.end(), s.tool) == tools.end()) s.tool = tools.front();
}

std::vector<IPos> brushShape(ShaperSettings const& s) {
    std::vector<IPos> out;
    switch (s.brush) {
    case Brush::Cuboid: {
        int w = s.params[0], h = s.params[1], l = s.params[2];
        for (int x = (w - 1) / -2; x <= w / 2; ++x)
            for (int y = (h - 1) / -2; y <= h / 2; ++y)
                for (int z = (l - 1) / -2; z <= l / 2; ++z) out.push_back({x, y, z});
        break;
    }
    case Brush::Sphere: {
        int r = s.params[0];
        for (int x = -r - 1; x <= r + 1; ++x)
            for (int y = -r - 1; y <= r + 1; ++y)
                for (int z = -r - 1; z <= r + 1; ++z)
                    if (std::sqrt(double(x * x + y * y + z * z)) < r + 0.5) out.push_back({x, y, z});
        break;
    }
    case Brush::Cylinder: {
        int r = s.params[0], h = s.params[1];
        std::vector<IPos> disc;
        for (int x = -r - 1; x <= r + 1; ++x)
            for (int z = -r - 1; z <= r + 1; ++z)
                if (std::sqrt(double(x * x + z * z)) < r + 0.42) disc.push_back({x, 0, z});
        for (int layer = 0; layer < h; ++layer)
            for (auto p : disc) out.push_back({p.x, layer - h / 2, p.z});
        break;
    }
    default:
        break;
    }
    return out;
}

namespace {
// Bedrock face index -> (axis 0 x / 1 y / 2 z, positive)
int axisOfFace(int face) { return face <= 1 ? 1 : face <= 3 ? 2 : 0; }
bool positiveFace(int face) { return face == 1 || face == 3 || face == 5; }
IPos faceStep(int face, int n) {
    IPos p;
    int  v = positiveFace(face) ? n : -n;
    switch (axisOfFace(face)) {
    case 0: p.x = v; break;
    case 1: p.y = v; break;
    default: p.z = v; break;
    }
    return p;
}
} // namespace

IPos placementOffset(ShaperSettings const& s, int face) {
    if (s.placement == Placement::Merged || !hasPlacement(s.brush) || face < 0 || face > 5) return {};
    bool attached = s.placement == Placement::Attached;
    bool negative = !positiveFace(face);
    int  axis     = axisOfFace(face);
    int  amount   = 0;
    switch (s.brush) {
    case Brush::Cuboid: {
        int offset = attached ? (negative ? 2 : 1) : 0;
        int p      = s.params[axis == 0 ? 0 : axis == 1 ? 1 : 2];
        amount     = (p + (p == 0 ? 0 : offset)) / 2;
        break;
    }
    case Brush::Sphere:
        amount = s.params[0] + 1 + (attached ? 0 : -1);
        break;
    case Brush::Cylinder: {
        int r      = s.params[0] + 1 + (attached ? 0 : -1);
        int yOff   = attached ? (negative ? 1 : 2) : (negative ? 0 : -1);
        int y      = (s.params[1] + (s.params[1] == 0 ? 0 : yOff)) / 2;
        amount     = axis == 1 ? y : r;
        break;
    }
    default:
        break;
    }
    return faceStep(face, attached ? amount : -amount);
}

std::vector<IPos> floodFill(ShaperSettings const& s, IPos start, int face, std::function<bool(IPos const&)> const& include) {
    bool surface   = s.brush == Brush::Surface;
    bool diagonals = followDiagonals(s);
    int  range     = s.params[0];
    int  axis      = axisOfFace(face);
    std::vector<IPos> offsets;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z) {
                if (!(std::abs(x) + std::abs(y) + std::abs(z) < 2 || diagonals)) continue;
                int along = axis == 0 ? x : axis == 1 ? y : z;
                if (along != 0 && surface) continue;
                offsets.push_back({x, y, z});
            }
    std::vector<IPos> out;
    std::set<IPos>    visited;
    std::deque<IPos>  frontier{start};
    while (!frontier.empty()) {
        IPos cur = frontier.front();
        frontier.pop_front();
        if (!visited.insert(cur).second) continue;
        double dx = cur.x - start.x, dy = cur.y - start.y, dz = cur.z - start.z;
        if (!(dx * dx + dy * dy + dz * dz < double(range) * range)) continue;
        if (!include(cur)) continue;
        out.push_back(cur);
        for (auto o : offsets) frontier.push_back({cur.x + o.x, cur.y + o.y, cur.z + o.z});
    }
    return out;
}

bool patternKeeps(Pattern p, IPos const& pos, std::function<bool()> const& rnd) {
    switch (p) {
    case Pattern::Chance25: return !(rnd() || rnd());
    case Pattern::Chance50: return !rnd();
    case Pattern::Chance75: return !(rnd() && rnd());
    case Pattern::Checkered: return (pos.x + pos.y + pos.z) % 2 != 0;
    case Pattern::InverseCheckered: return (pos.x + pos.y + pos.z) % 2 == 0;
    default: return true;
    }
}

int toolAction(ShaperTool t, bool replaceable) {
    switch (t) {
    case ShaperTool::Clear: return 2;
    case ShaperTool::Fill: return replaceable ? 1 : 0;
    case ShaperTool::Place: return 1;
    case ShaperTool::Replace: return replaceable ? 0 : 1;
    default: return 0;
    }
}

std::vector<int> smoothHeights(std::vector<int> const& values, int rows, int cols) {
    static constexpr float kernel[5][5] = {
        {0.003765f, 0.015019f, 0.023792f, 0.015019f, 0.003765f},
        {0.015019f, 0.059912f, 0.094907f, 0.059912f, 0.015019f},
        {0.023792f, 0.094907f, 0.150342f, 0.094907f, 0.023792f},
        {0.015019f, 0.059912f, 0.094907f, 0.059912f, 0.015019f},
        {0.003765f, 0.015019f, 0.023792f, 0.015019f, 0.003765f},
    };
    std::vector<int> out(values.size());
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) {
            int   value = values[static_cast<std::size_t>(i * cols + j)];
            float v     = 0;
            for (int a = -2; a <= 2; ++a)
                for (int b = -2; b <= 2; ++b) {
                    int ti = i + a, tj = j + b;
                    int ref = (ti < 0 || ti >= rows || tj < 0 || tj >= cols) ? value : values[static_cast<std::size_t>(ti * cols + tj)];
                    if (ref == kNoHeight) ref = value;
                    v += kernel[a + 2][b + 2] * static_cast<float>(ref);
                }
            out[static_cast<std::size_t>(i * cols + j)] = static_cast<int>(std::floor(v + 0.5f));
        }
    return out;
}

std::string brushKey(Brush b) {
    static char const* k[] = {"cuboid", "sphere", "cylinder", "surface", "cluster"};
    return std::string("create.gui.terrainzapper.brush.") + k[static_cast<int>(b)];
}
std::string toolKey(ShaperTool t) {
    static char const* k[] = {"fill", "place", "replace", "clear", "overlay", "flatten"};
    return std::string("create.gui.terrainzapper.tool.") + k[static_cast<int>(t)];
}
std::string placementKey(Placement p) {
    static char const* k[] = {"merged", "attached", "inserted"};
    return std::string("create.gui.terrainzapper.placement.") + k[static_cast<int>(p)];
}
std::string patternKey(Pattern p) {
    static char const* k[] = {"solid", "checkered", "inversecheckered", "chance25", "chance50", "chance75"};
    return std::string("create.gui.terrainzapper.pattern.") + k[static_cast<int>(p)];
}
std::string brushFallback(Brush b) {
    static char const* k[] = {"Cuboid", "Sphere", "Cylinder", "Surface", "Cluster"};
    return k[static_cast<int>(b)];
}
std::string toolFallback(ShaperTool t) {
    static char const* k[] = {"Fill", "Place", "Replace", "Clear", "Overlay", "Flatten"};
    return k[static_cast<int>(t)];
}
std::string placementFallback(Placement p) {
    static char const* k[] = {"Merged", "Attached", "Inserted"};
    return k[static_cast<int>(p)];
}
std::string patternFallback(Pattern p) {
    static char const* k[] = {"Solid", "Checkerboard", "Inverted Checkerboard", "25% Roll", "50% Roll", "75% Roll"};
    return k[static_cast<int>(p)];
}

} // namespace lv::core::equipment
