#include "core/equipment/Symmetry.h"

#include <cmath>

namespace lv::core::equipment {

nlohmann::json Mirror::toJson() const {
    return {{"type", static_cast<int>(type)}, {"orientation", orientation}, {"pos", {x, y, z}}, {"enabled", enabled}, {"placed", placed}, {"dim", dimension}};
}

Mirror Mirror::fromJson(nlohmann::json const& j) {
    Mirror m;
    if (!j.is_object()) return m;
    int t         = j.value("type", 0);
    m.type        = static_cast<MirrorType>(t < 0 || t > 2 ? 0 : t);
    m.orientation = j.value("orientation", 0);
    if (m.orientation < 0 || m.orientation >= orientationCount(m.type)) m.orientation = 0;
    auto p = j.value("pos", nlohmann::json::array());
    if (p.is_array() && p.size() == 3) m.x = p[0].get<int>(), m.y = p[1].get<int>(), m.z = p[2].get<int>();
    m.enabled = j.value("enabled", false);
    m.placed  = j.value("placed", false);
    m.dimension = j.value("dim", 0);
    return m;
}

namespace {
constexpr Mat2 kFlipX{-1, 0, 0, 1};
constexpr Mat2 kFlipZ{1, 0, 0, -1};
constexpr Mat2 kFlipXZ{-1, 0, 0, -1};
constexpr Mat2 kSwap{0, 1, 1, 0};     // mirror across the x = z diagonal
constexpr Mat2 kSwapNeg{0, -1, -1, 0}; // mirror across the x = -z diagonal
constexpr Mat2 kRotCw{0, -1, 1, 0};
constexpr Mat2 kRotCcw{0, 1, -1, 0};
} // namespace

std::vector<Mat2> mirrorTransforms(Mirror const& m) {
    switch (m.type) {
    case MirrorType::Plane:
        return {m.orientation == 0 ? kFlipX : kFlipZ};
    case MirrorType::Cross:
        if (m.orientation == 0) return {kFlipX, kFlipZ, kFlipXZ};
        return {kSwap, kSwapNeg, kFlipXZ};
    case MirrorType::Triple:
        return {kFlipX, kFlipZ, kFlipXZ, kSwap, kSwapNeg, kRotCw, kRotCcw};
    }
    return {};
}

std::vector<MirroredPos> mirrorPositions(Mirror const& m, int x, int y, int z) {
    std::vector<MirroredPos> out;
    int                      dx = x - m.x, dz = z - m.z;
    for (auto const& t : mirrorTransforms(m)) {
        auto [nx, nz] = t.apply(dx, dz);
        if (nx == dx && nz == dz) continue; // on the mirror plane: the same block
        bool dup = false;
        for (auto const& o : out)
            if (o.x == m.x + nx && o.z == m.z + nz) dup = true;
        if (!dup) out.push_back({m.x + nx, y, m.z + nz, t});
    }
    return out;
}

int transformHorizontal(Mat2 const& t, int dir) {
    static constexpr int dx[] = {0, 0, -1, 1}, dz[] = {-1, 1, 0, 0};
    if (dir < 0 || dir > 3) return dir;
    auto [nx, nz] = t.apply(dx[dir], dz[dir]);
    for (int d = 0; d < 4; ++d)
        if (dx[d] == nx && dz[d] == nz) return d;
    return dir;
}

int orientationCount(MirrorType t) { return t == MirrorType::Triple ? 1 : 2; }

int planeOrientationFor(float yaw) {
    float a = std::fmod(std::fabs(yaw), 360.0f);
    if (a > 180.0f) a = 360.0f - a;
    bool northSouth = a < 45.0f || a > 135.0f;
    return northSouth ? 1 : 0;
}

int crossOrientationFor(float yaw) {
    float a = std::fmod(std::fabs(yaw), 90.0f);
    return (a >= 22.0f && a <= 67.0f) ? 1 : 0;
}

std::string mirrorTypeKey(MirrorType t) {
    switch (t) {
    case MirrorType::Plane: return "create.symmetry.mirror.plane";
    case MirrorType::Cross: return "create.symmetry.mirror.doublePlane";
    case MirrorType::Triple: return "create.symmetry.mirror.triplePlane";
    }
    return {};
}

std::string orientationKey(MirrorType t, int orientation) {
    switch (t) {
    case MirrorType::Plane: return orientation == 0 ? "create.orientation.alongX" : "create.orientation.alongZ";
    case MirrorType::Cross: return orientation == 0 ? "create.orientation.orthogonal" : "create.orientation.diagonal";
    case MirrorType::Triple: return "create.orientation.horizontal";
    }
    return {};
}

} // namespace lv::core::equipment
