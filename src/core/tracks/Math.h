#pragma once

// Small vector maths for the track system (pure C++, double precision).
//
// Conventions follow Minecraft: +y is up, +z is south, +x is east. Block positions are integer
// corners; the centre of block (x, y, z) is (x + .5, y + .5, z + .5).

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace lv::core::tracks {

/// `Mth.equal`: |a - b| < 1e-5.
[[nodiscard]] inline bool nearlyEqual(double a, double b) { return std::abs(b - a) < 1.0e-5; }

struct Vec3d {
    double x = 0, y = 0, z = 0;

    constexpr Vec3d() = default;
    constexpr Vec3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    [[nodiscard]] constexpr Vec3d operator+(Vec3d const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    [[nodiscard]] constexpr Vec3d operator-(Vec3d const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    [[nodiscard]] constexpr Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
    [[nodiscard]] constexpr Vec3d operator-() const { return {-x, -y, -z}; }
    Vec3d&                        operator+=(Vec3d const& o) { return *this = *this + o; }
    Vec3d&                        operator-=(Vec3d const& o) { return *this = *this - o; }

    [[nodiscard]] constexpr double dot(Vec3d const& o) const { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vec3d  cross(Vec3d const& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] constexpr double lengthSqr() const { return dot(*this); }
    [[nodiscard]] double           length() const { return std::sqrt(lengthSqr()); }
    /// Unit vector (zero stays zero, like Minecraft's `Vec3.normalize`).
    [[nodiscard]] Vec3d normalize() const {
        double l = length();
        return l < 1.0e-4 ? Vec3d{} : Vec3d{x / l, y / l, z / l};
    }
    [[nodiscard]] double distanceTo(Vec3d const& o) const { return (*this - o).length(); }
    [[nodiscard]] double distanceToSqr(Vec3d const& o) const { return (*this - o).lengthSqr(); }
    [[nodiscard]] constexpr Vec3d multiply(double mx, double my, double mz) const { return {x * mx, y * my, z * mz}; }
    /// Component-wise equality within `eps`.
    [[nodiscard]] bool near(Vec3d const& o, double eps = 1.0e-6) const {
        return std::abs(x - o.x) < eps && std::abs(y - o.y) < eps && std::abs(z - o.z) < eps;
    }

    friend constexpr bool operator==(Vec3d const&, Vec3d const&) = default;
};

/// Integer block position (no dimension).
struct BlockPos3 {
    int x = 0, y = 0, z = 0;

    constexpr BlockPos3() = default;
    constexpr BlockPos3(int x_, int y_, int z_) : x(x_), y(y_), z(z_) {}

    [[nodiscard]] constexpr BlockPos3 offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
    [[nodiscard]] constexpr BlockPos3 operator+(BlockPos3 const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    [[nodiscard]] constexpr BlockPos3 operator-(BlockPos3 const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    [[nodiscard]] constexpr BlockPos3 below(int n = 1) const { return {x, y - n, z}; }
    [[nodiscard]] constexpr BlockPos3 above(int n = 1) const { return {x, y + n, z}; }
    /// Lower (north-west-bottom) corner as a vector.
    [[nodiscard]] constexpr Vec3d corner() const { return {double(x), double(y), double(z)}; }
    /// Centre of the block.
    [[nodiscard]] constexpr Vec3d center() const { return {x + .5, y + .5, z + .5}; }
    /// Bottom centre of the block.
    [[nodiscard]] constexpr Vec3d bottomCenter() const { return {x + .5, double(y), z + .5}; }
    /// Squared distance between the block corners (`BlockPos.distSqr`).
    [[nodiscard]] constexpr double distSqr(BlockPos3 const& o) const {
        double dx = x - o.x, dy = y - o.y, dz = z - o.z;
        return dx * dx + dy * dy + dz * dz;
    }

    friend constexpr bool operator==(BlockPos3 const&, BlockPos3 const&)  = default;
    friend constexpr auto operator<=>(BlockPos3 const&, BlockPos3 const&) = default;

    [[nodiscard]] std::string str() const {
        return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
    }
};

/// The block containing `v` (`BlockPos.containing`: floor of each component).
[[nodiscard]] inline BlockPos3 containing(Vec3d const& v) {
    return {int(std::floor(v.x)), int(std::floor(v.y)), int(std::floor(v.z))};
}

[[nodiscard]] inline Vec3d lerp(double t, Vec3d const& from, Vec3d const& to) { return from + (to - from) * t; }

/// Cubic bezier from `p1` to `p2` with control points `q1` (after p1) and `q2` (before p2).
[[nodiscard]] Vec3d bezier(Vec3d const& p1, Vec3d const& p2, Vec3d const& q1, Vec3d const& q2, double t);
/// Derivative of `bezier` with respect to t.
[[nodiscard]] Vec3d bezierDerivative(Vec3d const& p1, Vec3d const& p2, Vec3d const& q1, Vec3d const& q2, double t);
/// Spherical interpolation between two unit vectors (as Create's VecHelper.slerp, including its
/// quirks, so normals along sloped curves match).
[[nodiscard]] Vec3d slerp(double t, Vec3d const& from, Vec3d const& to);

enum class Axis { X, Y, Z };

/// Intersection of the 2D lines p1 + t·r and p2 + u·s, projected onto the plane perpendicular to
/// `plane` (Y = the horizontal plane). Returns {t, u}, or nullopt when the lines are parallel.
struct LineHit {
    double t = 0, u = 0;
};
[[nodiscard]] std::optional<LineHit>
intersect(Vec3d const& p1, Vec3d const& p2, Vec3d const& r, Vec3d const& s, Axis plane);
/// Intersection of segments p1→p2 and q1→q2 (in the plane), as distances along each segment.
[[nodiscard]] std::optional<LineHit>
intersectRanged(Vec3d const& p1, Vec3d const& q1, Vec3d const& p2, Vec3d const& q2, Axis plane);

/// Axis-aligned box.
struct Box {
    Vec3d min, max;
    [[nodiscard]] bool  contains(Vec3d const& p, double inflate = 0) const {
        return p.x >= min.x - inflate && p.x <= max.x + inflate && p.y >= min.y - inflate && p.y <= max.y + inflate
            && p.z >= min.z - inflate && p.z <= max.z + inflate;
    }
    [[nodiscard]] bool intersects(Box const& o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y && min.z <= o.max.z
            && max.z >= o.min.z;
    }
    void include(Vec3d const& p) {
        min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
};

} // namespace lv::core::tracks

template <>
struct std::hash<lv::core::tracks::BlockPos3> {
    size_t operator()(lv::core::tracks::BlockPos3 const& p) const noexcept {
        return (size_t(uint32_t(p.x)) * 73856093u) ^ (size_t(uint32_t(p.y)) * 19349663u)
             ^ (size_t(uint32_t(p.z)) * 83492791u);
    }
};
