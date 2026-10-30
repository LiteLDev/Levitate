#include "core/contraption/Geometry.h"

#include <numbers>

namespace lv::core::contraption {

Pos offset(Dir d) {
    switch (d) {
    case Dir::Down:
        return {0, -1, 0};
    case Dir::Up:
        return {0, 1, 0};
    case Dir::North:
        return {0, 0, -1};
    case Dir::South:
        return {0, 0, 1};
    case Dir::West:
        return {-1, 0, 0};
    default:
        return {1, 0, 0};
    }
}

Axis axisOf(Dir d) {
    switch (d) {
    case Dir::Down:
    case Dir::Up:
        return Axis::Y;
    case Dir::North:
    case Dir::South:
        return Axis::Z;
    default:
        return Axis::X;
    }
}

bool positive(Dir d) { return d == Dir::Up || d == Dir::South || d == Dir::East; }

Dir opposite(Dir d) { return static_cast<Dir>(static_cast<int>(d) ^ 1); }

Dir dirOf(Axis a, bool pos) {
    switch (a) {
    case Axis::X:
        return pos ? Dir::East : Dir::West;
    case Axis::Y:
        return pos ? Dir::Up : Dir::Down;
    default:
        return pos ? Dir::South : Dir::North;
    }
}

std::string_view dirName(Dir d) {
    static constexpr std::array<std::string_view, 6> names{"down", "up", "north", "south", "west", "east"};
    return names[static_cast<int>(d)];
}

std::optional<Dir> dirFromName(std::string_view name) {
    for (auto d : kAllDirs)
        if (dirName(d) == name) return d;
    return std::nullopt;
}

std::string_view axisName(Axis a) { return a == Axis::X ? "x" : a == Axis::Y ? "y" : "z"; }

std::optional<Axis> axisFromName(std::string_view name) {
    if (name == "x") return Axis::X;
    if (name == "y") return Axis::Y;
    if (name == "z") return Axis::Z;
    return std::nullopt;
}

std::optional<Dir> dirFromOffset(Pos const& p) {
    for (auto d : kAllDirs)
        if (offset(d) == p) return d;
    return std::nullopt;
}

Dir fromHorizontalIndex(int i) {
    switch (normQuarter(i)) {
    case 0:
        return Dir::South;
    case 1:
        return Dir::West;
    case 2:
        return Dir::North;
    default:
        return Dir::East;
    }
}

std::optional<int> horizontalIndex(Dir d) {
    switch (d) {
    case Dir::South:
        return 0;
    case Dir::West:
        return 1;
    case Dir::North:
        return 2;
    case Dir::East:
        return 3;
    default:
        return std::nullopt;
    }
}

Pos rotate(Pos const& p, Axis axis, int quarterTurns) {
    Pos r = p;
    for (int i = 0; i < normQuarter(quarterTurns); ++i) {
        // one quarter turn, right-hand rule
        switch (axis) {
        case Axis::X:
            r = {r.x, -r.z, r.y};
            break;
        case Axis::Y:
            r = {r.z, r.y, -r.x};
            break;
        case Axis::Z:
            r = {-r.y, r.x, r.z};
            break;
        }
    }
    return r;
}

Dir rotate(Dir d, Axis axis, int quarterTurns) { return *dirFromOffset(rotate(offset(d), axis, quarterTurns)); }

Axis rotate(Axis a, Axis about, int quarterTurns) {
    if (a == about || normQuarter(quarterTurns) % 2 == 0) return a;
    return axisOf(rotate(dirOf(a, true), about, 1));
}

Vec3d rotate(Vec3d const& v, Axis axis, double degrees) {
    if (degrees == 0) return v;
    double rad = degrees * std::numbers::pi / 180.0;
    double c = std::cos(rad), s = std::sin(rad);
    // snap exact quarter turns so grid positions stay exact
    int q = quarterTurnsOf(degrees);
    if (std::abs(degrees - q * 90.0) < 1e-9) {
        static constexpr std::array<std::pair<int, int>, 4> cs{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
        c = cs[normQuarter(q)].first;
        s = cs[normQuarter(q)].second;
    }
    switch (axis) {
    case Axis::X:
        return {v.x, v.y * c - v.z * s, v.y * s + v.z * c};
    case Axis::Y:
        return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
    default:
        return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
    }
}

int roundHalfUp(double v) { return static_cast<int>(std::floor(v + 0.5)); }

int quarterTurnsOf(double degrees) { return roundHalfUp(degrees / 90.0); }

Vec3d Transform::rotateVector(Vec3d const& v) const {
    return rotate(tilt == 0 ? v : rotate(v, tiltAxis, tilt), axis, angle);
}

Vec3d Transform::unrotateVector(Vec3d const& v) const {
    Vec3d u = rotate(v, axis, -angle);
    return tilt == 0 ? u : rotate(u, tiltAxis, -tilt);
}

Vec3d Transform::toWorld(Vec3d const& local) const {
    return origin + offset + pivot + rotateVector(local - pivot);
}

Vec3d Transform::toLocal(Vec3d const& world) const {
    return pivot + unrotateVector(world - origin - offset - pivot);
}

Pos Transform::originBlock() const {
    return {roundHalfUp(origin.x), roundHalfUp(origin.y), roundHalfUp(origin.z)};
}

Pos Transform::placeBlock(Pos const& p) const {
    Vec3d rounded{double(roundHalfUp(offset.x)), double(roundHalfUp(offset.y)), double(roundHalfUp(offset.z))};
    Vec3d c = origin + rounded + pivot + rotate(Vec3d::center(p) - pivot, axis, placeQuarterTurns() * 90.0);
    return {int(std::floor(c.x + 1e-6)), int(std::floor(c.y + 1e-6)), int(std::floor(c.z + 1e-6))};
}

std::pair<Axis, double> pitchTilt(int ax, int az, Vec3d const& dir) {
    double flat  = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    double pitch = std::atan2(dir.y, flat) * 180.0 / 3.14159265358979323846;
    if (std::abs(pitch) < 1e-4) pitch = 0;
    // (1,0,0) turned about Z rises with a positive angle; (0,0,1) turned about X sinks
    if (ax != 0) return {Axis::Z, ax > 0 ? pitch : -pitch};
    return {Axis::X, az > 0 ? -pitch : pitch};
}

bool Box::intersects(Box const& o, double eps) const {
    return min.x < o.max.x - eps && max.x > o.min.x + eps && min.y < o.max.y - eps && max.y > o.min.y + eps
        && min.z < o.max.z - eps && max.z > o.min.z + eps;
}

bool Box::contains(Vec3d const& p) const {
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
}

} // namespace lv::core::contraption
