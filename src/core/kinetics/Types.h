#pragma once

// Geometry vocabulary of the kinetic network (pure C++): axes, faces in Create's order, offsets.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/GlobalPos.h"

namespace lv::core::kinetics {

enum class Axis : std::uint8_t { X = 0, Y = 1, Z = 2 };

/// Faces in Create's `Direction` order, which is also the value order of Bedrock's
/// `minecraft:block_face` state ("down", "up", "north", "south", "west", "east").
enum class Face : std::uint8_t { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };

inline constexpr std::array<Face, 6> kFaces{Face::Down, Face::Up, Face::North, Face::South, Face::West, Face::East};
inline constexpr std::array<Axis, 3> kAxes{Axis::X, Axis::Y, Axis::Z};

/// Integer block offset.
struct Offset {
    int x = 0, y = 0, z = 0;
    friend bool operator==(Offset const&, Offset const&) = default;
};

[[nodiscard]] constexpr Face opposite(Face f) { return static_cast<Face>(static_cast<int>(f) ^ 1); }

[[nodiscard]] constexpr Axis axisOf(Face f) {
    switch (f) {
    case Face::Down:
    case Face::Up:
        return Axis::Y;
    case Face::North:
    case Face::South:
        return Axis::Z;
    default:
        return Axis::X;
    }
}

/// Up, South and East point along the positive axis.
[[nodiscard]] constexpr bool isPositive(Face f) { return f == Face::Up || f == Face::South || f == Face::East; }

[[nodiscard]] constexpr Face faceOf(Axis axis, bool positive) {
    switch (axis) {
    case Axis::X:
        return positive ? Face::East : Face::West;
    case Axis::Y:
        return positive ? Face::Up : Face::Down;
    default:
        return positive ? Face::South : Face::North;
    }
}

[[nodiscard]] constexpr Offset offsetOf(Face f) {
    switch (f) {
    case Face::Down:
        return {0, -1, 0};
    case Face::Up:
        return {0, 1, 0};
    case Face::North:
        return {0, 0, -1};
    case Face::South:
        return {0, 0, 1};
    case Face::West:
        return {-1, 0, 0};
    default:
        return {1, 0, 0};
    }
}

/// The component of (x, y, z) along `axis` (Create's `Axis.choose`).
[[nodiscard]] constexpr int choose(Axis axis, int x, int y, int z) {
    return axis == Axis::X ? x : axis == Axis::Y ? y : z;
}
[[nodiscard]] constexpr int choose(Axis axis, Offset const& o) { return choose(axis, o.x, o.y, o.z); }

/// The face whose direction is closest to (x, y, z); ties go to the first face in `kFaces` order
/// (Create's `Direction.getNearest`). (0, 0, 0) gives North, as in Minecraft.
[[nodiscard]] constexpr Face nearestFace(int x, int y, int z) {
    Face      best    = Face::North;
    long long bestDot = 0;
    for (Face f : kFaces) {
        auto      o   = offsetOf(f);
        long long dot = static_cast<long long>(o.x) * x + static_cast<long long>(o.y) * y + static_cast<long long>(o.z) * z;
        if (dot > bestDot) {
            best    = f;
            bestDot = dot;
        }
    }
    return best;
}

[[nodiscard]] inline Offset diff(GlobalPos const& from, GlobalPos const& to) {
    return {to.x - from.x, to.y - from.y, to.z - from.z};
}
[[nodiscard]] inline GlobalPos shifted(GlobalPos p, Offset const& o) {
    p.x += o.x;
    p.y += o.y;
    p.z += o.z;
    return p;
}
[[nodiscard]] inline GlobalPos shifted(GlobalPos const& p, Face f) { return shifted(p, offsetOf(f)); }

[[nodiscard]] char const* faceName(Face f);
[[nodiscard]] char const* axisName(Axis a);

struct GlobalPosHash {
    std::size_t operator()(GlobalPos const& p) const noexcept {
        std::size_t h = static_cast<std::size_t>(p.x) * 73856093u;
        h ^= static_cast<std::size_t>(p.y) * 19349663u;
        h ^= static_cast<std::size_t>(p.z) * 83492791u;
        h ^= static_cast<std::size_t>(p.dim) * 2654435761u;
        return h;
    }
};

/// Snapshot of the kinetically relevant states of a block: its id and the integer value of every
/// state the block's spec lists (`KineticSpec::states`). Bedrock string-enum states (such as
/// `minecraft:block_face`) are their value index.
struct BlockState {
    std::string                              id;
    std::vector<std::pair<std::string, int>> values;

    [[nodiscard]] int get(std::string_view name, int fallback = 0) const {
        for (auto const& [k, v] : values)
            if (k == name) return v;
        return fallback;
    }
    [[nodiscard]] bool has(std::string_view name) const {
        for (auto const& [k, v] : values)
            if (k == name) return true;
        return false;
    }
    BlockState& set(std::string_view name, int value) {
        for (auto& [k, v] : values) {
            if (k == name) {
                v = value;
                return *this;
            }
        }
        values.emplace_back(std::string(name), value);
        return *this;
    }
    friend bool operator==(BlockState const&, BlockState const&) = default;
};

} // namespace lv::core::kinetics
