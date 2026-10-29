#include "core/generators/Generators.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace lv::core::generators {

using kinetics::axisOf;
using kinetics::isPositive;
using kinetics::kFaces;
using kinetics::offsetOf;
using kinetics::opposite;

int dyeColourOf(std::string_view itemId) {
    constexpr std::string_view prefix = "minecraft:";
    constexpr std::string_view suffix = "_dye";
    if (!itemId.starts_with(prefix) || !itemId.ends_with(suffix)) return -1;
    auto colour = itemId.substr(prefix.size(), itemId.size() - prefix.size() - suffix.size());
    for (std::size_t i = 0; i < kDyeColours.size(); ++i)
        if (kDyeColours[i] == colour) return static_cast<int>(i);
    return -1;
}

int woodOfPlanks(std::string_view id) {
    constexpr std::string_view prefix = "minecraft:";
    constexpr std::string_view suffix = "_planks";
    if (!id.starts_with(prefix) || !id.ends_with(suffix)) return -1;
    auto wood = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    for (std::size_t i = 0; i < kWoods.size(); ++i)
        if (kWoods[i] == wood) return static_cast<int>(i);
    return -1;
}

float convertToDirection(float axisSpeed, Face facing) { return isPositive(facing) ? axisSpeed : -axisSpeed; }

Face clockWise(Face face, Axis axis) {
    if (axisOf(face) == axis) return face;
    switch (axis) {
    case Axis::X: // Direction.getClockWiseX
        switch (face) {
        case Face::Down:
            return Face::South;
        case Face::South:
            return Face::Up;
        case Face::Up:
            return Face::North;
        default:
            return Face::Down;
        }
    case Axis::Y: // Direction.getClockWise
        switch (face) {
        case Face::North:
            return Face::East;
        case Face::East:
            return Face::South;
        case Face::South:
            return Face::West;
        default:
            return Face::North;
        }
    default: // Direction.getClockWiseZ
        switch (face) {
        case Face::Down:
            return Face::West;
        case Face::West:
            return Face::Up;
        case Face::Up:
            return Face::East;
        default:
            return Face::Down;
        }
    }
}

std::optional<Face> preferredFacing(std::function<bool(Face side)> const& neighbourShaftTowardsUs) {
    std::optional<Face> preferred;
    for (Face side : kFaces) {
        if (!neighbourShaftTowardsUs(side)) continue;
        if (preferred && axisOf(*preferred) != axisOf(side)) return std::nullopt;
        preferred = side;
    }
    return preferred;
}

std::optional<Axis> preferredAxis(std::function<bool(Face side)> const& neighbourShaftTowardsUs) {
    std::optional<Axis> preferred;
    for (Face side : kFaces) {
        if (!neighbourShaftTowardsUs(side)) continue;
        if (preferred && *preferred != axisOf(side)) return std::nullopt;
        preferred = axisOf(side);
    }
    return preferred;
}

Face nearestLooking(double dx, double dy, double dz) {
    // Direction.orderedByNearest(entity)[0]: the dominant component of the view vector
    double ax = std::abs(dx), ay = std::abs(dy), az = std::abs(dz);
    Face   fx = dx > 0 ? Face::East : Face::West;
    Face   fy = dy > 0 ? Face::Up : Face::Down;
    Face   fz = dz > 0 ? Face::South : Face::North;
    if (ax > az) return ay > ax ? fy : fx;
    return ay > az ? fy : fz;
}

Face crankFacing(std::optional<Face> preferred, Face clickedFace, bool sneaking) {
    if (!preferred || sneaking) return clickedFace;
    return opposite(*preferred);
}

Face directionalFacing(std::optional<Face> preferred, Face nearestLook, bool sneaking) {
    if (!preferred || sneaking) return sneaking ? nearestLook : opposite(nearestLook);
    return opposite(*preferred);
}

Face motorFacing(std::optional<Face> preferred, Face nearestLook, bool sneaking) {
    if (sneaking || !preferred) return directionalFacing(preferred, nearestLook, sneaking);
    return *preferred;
}

Axis pillarAxis(std::optional<Axis> preferred, Face clickedFace, Face nearestLook, bool sneaking) {
    if (preferred && !sneaking) return *preferred;
    return preferred && sneaking ? axisOf(clickedFace) : axisOf(nearestLook);
}

float crankSpeed(bool inUse, bool clockwise, Face facing) {
    int speed = (!inUse ? 0 : clockwise ? -1 : 1) * kCrankSpeed;
    return convertToDirection(static_cast<float>(speed), facing);
}

ValveTurn valveTurn(int angle, bool sneaking) {
    ValveTurn t;
    int       target = std::abs(angle);
    float     perTick = convertToAngular(static_cast<float>(kCrankSpeed));
    t.useTicks        = static_cast<int>(std::ceil(static_cast<float>(target) / perTick)) + 2;
    int sign          = angle > 0 ? 1 : angle < 0 ? -1 : 0;
    // startAngle is 0 on the server (its independent angle is always 0)
    t.targetAngle = static_cast<int>(std::lround(static_cast<float>((target > 135 ? 180 : 90) * sign) / 90.f)) * 90;
    t.clockwise   = (angle < 0) != sneaking;
    return t;
}

float valveAngle(int targetAngle, int total, float elapsed, bool backwards, Face facing) {
    float frac = total > 0 ? std::min(static_cast<float>(total), std::max(0.f, elapsed)) / static_cast<float>(total) : 1.f;
    float step = isPositive(facing) ? 1.f : -1.f;
    return static_cast<float>(targetAngle) * frac * (backwards ? -1.f : 1.f) * step;
}

float crankChase(float chasingVelocity, float speed) { return chasingVelocity + ((speed * 10.f / 3.f) - chasingVelocity) * .25f; }

std::vector<Offset> wheelOffsets(Axis axis, bool large) {
    std::vector<Offset> out;
    for (Face d : kFaces) {
        if (axisOf(d) == axis) continue;
        auto o = offsetOf(d);
        if (!large) {
            out.push_back(o);
            continue;
        }
        Offset central{o.x * 2, o.y * 2, o.z * 2};
        out.push_back(central);
        for (Face d2 : kFaces) {
            if (axisOf(d2) == axis || axisOf(d2) == axisOf(d)) continue;
            auto o2 = offsetOf(d2);
            out.push_back({central.x + o2.x, central.y + o2.y, central.z + o2.z});
        }
    }
    return out;
}

namespace {
/// Create's VecHelper.rotate (right-hand rotation about the positive axis).
Vec3d rotate(Vec3d v, double degrees, Axis axis) {
    double a = degrees / 180.0 * 3.14159265358979323846;
    double s = std::sin(a), c = std::cos(a);
    switch (axis) {
    case Axis::X:
        return {v.x, v.y * c - v.z * s, v.z * c + v.y * s};
    case Axis::Y:
        return {v.x * c + v.z * s, v.y, v.z * c - v.x * s};
    default:
        return {v.x * c - v.y * s, v.y * c + v.x * s, v.z};
    }
}
double length(Vec3d const& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
} // namespace

int flowScore(Axis axis, bool large, std::function<Vec3d(Offset const&)> const& flowAt) {
    int score = 0;
    for (auto const& o : wheelOffsets(axis, large)) {
        Vec3d flow = flowAt(o);
        if (axis == Axis::X) flow.x = 0;
        if (axis == Axis::Y) flow.y = 0;
        if (axis == Axis::Z) flow.z = 0;
        double len = length(flow);
        if (len == 0) continue;
        flow = {flow.x / len, flow.y / len, flow.z / len};
        Vec3d  normal{static_cast<double>(o.x), static_cast<double>(o.y), static_cast<double>(o.z)};
        double nl = length(normal);
        normal    = {normal.x / nl, normal.y / nl, normal.z / nl};
        Vec3d  positiveMotion = rotate(normal, 90, axis);
        double dot            = flow.x * positiveMotion.x + flow.y * positiveMotion.y + flow.z * positiveMotion.z;
        if (std::abs(dot) > .5) score += dot > 0 ? 1 : -1;
    }
    return score;
}

float wheelSpeed(int score, bool large) {
    return static_cast<float>(std::clamp(score, -1, 1) * kWheelSpeed) / (large ? 2.f : 1.f);
}

namespace {
constexpr std::array<std::pair<int, int>, 8> kSegmentCells{{{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}}};

/// The canonical (a, b) of an in-plane offset: Y -> (x, z), X -> (y, z), Z -> (x, y).
std::pair<int, int> cellOf(Offset const& o, Axis axis) {
    switch (axis) {
    case Axis::X:
        return {o.y, o.z};
    case Axis::Y:
        return {o.x, o.z};
    default:
        return {o.x, o.y};
    }
}
} // namespace

std::pair<int, int> segmentCell(int segment) { return kSegmentCells[static_cast<std::size_t>(std::clamp(segment, 0, 7))]; }

Offset segmentOffset(int segment, Axis axis) {
    auto [a, b] = segmentCell(segment);
    switch (axis) {
    case Axis::X:
        return {0, a, b};
    case Axis::Y:
        return {a, 0, b};
    default:
        return {a, b, 0};
    }
}

std::array<StructureCell, 8> structureCells(Axis axis) {
    std::array<StructureCell, 8> out{};
    std::size_t                  n = 0;
    for (Face side : kFaces) {
        if (axisOf(side) == axis) continue;
        for (bool secondary : {false, true}) {
            Face   target = secondary ? clockWise(side, axis) : side;
            auto   s      = offsetOf(side);
            auto   t      = offsetOf(target);
            Offset o      = secondary ? Offset{s.x + t.x, s.y + t.y, s.z + t.z} : t;
            int    segment = 0;
            auto   cell    = cellOf(o, axis);
            for (int i = 0; i < 8; ++i)
                if (kSegmentCells[static_cast<std::size_t>(i)] == cell) segment = i;
            out[n++] = StructureCell{o, opposite(target), segment};
        }
    }
    return out;
}

} // namespace lv::core::generators
