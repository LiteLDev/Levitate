#pragma once

// Contraption geometry (pure C++): directions, axes, block positions, vectors, the transform of
// a moving structure and its grid-aligned placement.
//
// Conventions (same as the game):
// - Directions use Bedrock's facing order: 0 down, 1 up, 2 north (-z), 3 south (+z), 4 west (-x),
//   5 east (+x).
// - Rotations follow the right-hand rule: a positive angle about an axis turns counter-clockwise
//   when looking from the positive end of the axis towards the origin. A quarter turn about Y
//   takes east to north.

#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace lv::core::contraption {

enum class Axis : std::uint8_t { X = 0, Y = 1, Z = 2 };
enum class Dir : std::uint8_t { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };

inline constexpr std::array<Dir, 6> kAllDirs{Dir::Down, Dir::Up, Dir::North, Dir::South, Dir::West, Dir::East};

struct Pos {
    int x = 0, y = 0, z = 0;

    friend bool operator==(Pos const&, Pos const&)  = default;
    friend auto operator<=>(Pos const&, Pos const&) = default;
    Pos         operator+(Pos const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Pos         operator-(Pos const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Pos         operator*(int k) const { return {x * k, y * k, z * k}; }
    [[nodiscard]] int get(Axis a) const { return a == Axis::X ? x : a == Axis::Y ? y : z; }
};

struct PosHash {
    std::size_t operator()(Pos const& p) const noexcept {
        auto h = static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

struct Vec3d {
    double x = 0, y = 0, z = 0;

    Vec3d  operator+(Vec3d const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3d  operator-(Vec3d const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3d  operator*(double k) const { return {x * k, y * k, z * k}; }
    Vec3d  operator-() const { return {-x, -y, -z}; }
    Vec3d& operator+=(Vec3d const& o) { return *this = *this + o; }
    [[nodiscard]] double lengthSqr() const { return x * x + y * y + z * z; }
    [[nodiscard]] double length() const { return std::sqrt(lengthSqr()); }
    [[nodiscard]] double get(Axis a) const { return a == Axis::X ? x : a == Axis::Y ? y : z; }
    void                 set(Axis a, double v) { (a == Axis::X ? x : a == Axis::Y ? y : z) = v; }
    static Vec3d         of(Pos const& p) { return {double(p.x), double(p.y), double(p.z)}; }
    static Vec3d         center(Pos const& p) { return {p.x + 0.5, p.y + 0.5, p.z + 0.5}; }
};

// ---- directions -------------------------------------------------------------------------------
[[nodiscard]] Pos              offset(Dir d);
[[nodiscard]] Axis             axisOf(Dir d);
[[nodiscard]] bool             positive(Dir d);
[[nodiscard]] Dir              opposite(Dir d);
[[nodiscard]] Dir              dirOf(Axis a, bool positive);
[[nodiscard]] std::string_view dirName(Dir d); ///< "down", "up", "north", ...
[[nodiscard]] std::optional<Dir> dirFromName(std::string_view name);
[[nodiscard]] std::string_view axisName(Axis a); ///< "x", "y", "z"
[[nodiscard]] std::optional<Axis> axisFromName(std::string_view name);
/// The direction pointing along an integer unit vector (nullopt if it isn't one).
[[nodiscard]] std::optional<Dir> dirFromOffset(Pos const& p);
/// Horizontal (Java 2D) data value 0 S, 1 W, 2 N, 3 E <-> direction.
[[nodiscard]] Dir             fromHorizontalIndex(int i);
[[nodiscard]] std::optional<int> horizontalIndex(Dir d);

// ---- rotations --------------------------------------------------------------------------------
/// Normalizes a number of quarter turns to 0..3.
[[nodiscard]] inline int normQuarter(int q) { return ((q % 4) + 4) % 4; }
/// Rotates a direction by `quarterTurns` (right-hand rule) about `axis`.
[[nodiscard]] Dir rotate(Dir d, Axis axis, int quarterTurns);
/// Rotates an integer vector by quarter turns about the origin.
[[nodiscard]] Pos rotate(Pos const& p, Axis axis, int quarterTurns);
/// Rotates a vector by `degrees` about the origin.
[[nodiscard]] Vec3d rotate(Vec3d const& v, Axis axis, double degrees);
/// Axis after rotating by quarter turns (X stays X when rotating about X, ...).
[[nodiscard]] Axis rotate(Axis a, Axis about, int quarterTurns);
/// Nearest number of quarter turns for an angle (Create rounds the angle at disassembly).
[[nodiscard]] int quarterTurnsOf(double degrees);
/// Rounds to the nearest integer, halves away from zero (Java's Math.round rounds halves up;
/// both agree except for exact negative halves, which never happen for real offsets).
[[nodiscard]] int roundHalfUp(double v);

/// The placement of a moving structure: local block coordinates (relative to the anchor block)
/// to world coordinates. `world(p) = origin + offset + pivot + R(p - pivot)`, with R the rotation
/// by `angle` degrees about `axis`.
struct Transform {
    Vec3d  origin;              ///< world position of local (0,0,0)'s min corner when assembled
    Vec3d  offset;              ///< translation since assembly
    Axis   axis  = Axis::Y;     ///< rotation axis
    double angle = 0;           ///< degrees
    Vec3d  pivot{0.5, 0.5, 0.5}; ///< rotation center in local coordinates
    /// A second turn applied first, about a local axis through the pivot (a train carriage pitched
    /// on a slope): `world(p) = origin + offset + pivot + R(axis, angle) R(tiltAxis, tilt) (p - pivot)`.
    /// Disassembly ignores it (placement stays grid aligned).
    Axis   tiltAxis = Axis::X;
    double tilt     = 0; ///< degrees

    [[nodiscard]] Vec3d toWorld(Vec3d const& local) const;
    [[nodiscard]] Vec3d toLocal(Vec3d const& world) const;
    /// Rotates a direction vector (no translation).
    [[nodiscard]] Vec3d rotateVector(Vec3d const& v) const;
    [[nodiscard]] Vec3d unrotateVector(Vec3d const& v) const;

    /// Grid-aligned placement used at disassembly: the angle rounded to quarter turns and the
    /// offset rounded to whole blocks. Returns the world block of local block `p`.
    [[nodiscard]] Pos  placeBlock(Pos const& p) const;
    [[nodiscard]] int  placeQuarterTurns() const { return normQuarter(quarterTurnsOf(angle)); }
    [[nodiscard]] Pos  originBlock() const;
};

/// An axis-aligned box in double coordinates.
struct Box {
    Vec3d min, max;

    [[nodiscard]] bool  intersects(Box const& o, double eps = 1e-7) const;
    [[nodiscard]] bool  contains(Vec3d const& p) const;
    [[nodiscard]] Vec3d center() const { return (min + max) * 0.5; }
    [[nodiscard]] Vec3d size() const { return max - min; }
    [[nodiscard]] Box   moved(Vec3d const& d) const { return {min + d, max + d}; }
    static Box          unit(Pos const& p) { return {Vec3d::of(p), Vec3d::of(p) + Vec3d{1, 1, 1}}; }
};

} // namespace lv::core::contraption
