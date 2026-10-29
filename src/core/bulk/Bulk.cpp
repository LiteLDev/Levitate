#include "core/bulk/Bulk.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lv::core::bulk {

double Vec3d::length() const { return std::sqrt(x * x + y * y + z * z); }

Vec3d Vec3d::normalized() const {
    double l = length();
    return l < 1e-4 ? Vec3d{} : Vec3d{x / l, y / l, z / l};
}

Vec3d normalOf(Face f) {
    auto o = kinetics::offsetOf(f);
    return {double(o.x), double(o.y), double(o.z)};
}

// ---- fan processing types ------------------------------------------------------------------------------------

int priorityOf(FanType t) {
    switch (t) {
    case FanType::Splashing:
        return 400;
    case FanType::Haunting:
        return 300;
    case FanType::Smoking:
        return 200;
    case FanType::Blasting:
        return 100;
    default:
        return 0;
    }
}

std::string_view fanTypeName(FanType t) {
    switch (t) {
    case FanType::Splashing:
        return "splashing";
    case FanType::Haunting:
        return "haunting";
    case FanType::Smoking:
        return "smoking";
    case FanType::Blasting:
        return "blasting";
    default:
        return "";
    }
}

FanType fanTypeByName(std::string_view name) {
    for (auto t : {FanType::Splashing, FanType::Haunting, FanType::Smoking, FanType::Blasting})
        if (fanTypeName(t) == name) return t;
    return FanType::None;
}

FanType bestType(std::vector<FanType> const& valid) {
    FanType best = FanType::None;
    for (auto t : valid)
        if (t != FanType::None && priorityOf(t) > priorityOf(best)) best = t;
    return best;
}

// ---- fan source ------------------------------------------------------------------------------------------------

float fanMaxDistance(float speed, FanConfig const& config) {
    float factor = std::min(std::abs(speed) / std::max(config.rotationArgmax, 1.f), 1.f);
    float push   = 3.f + factor * (config.pushDistance - 3.f);
    float pull   = 3.f + factor * (config.pullDistance - 3.f);
    return speed > 0 ? push : pull;
}

bool fanPushing(Face facing, float speed) {
    float converted = kinetics::isPositive(facing) ? speed : -speed;
    return converted > 0;
}

int fanProcessingTime(int count, int baseTime) {
    int modifier = (std::max(count, 1) - 1) / 16 + 1;
    return baseTime * modifier + 1;
}

// ---- air current -------------------------------------------------------------------------------------------------

double findMaxDepth(FlowBlock const& box, Face face) {
    static constexpr double kTests[5][2] = {
        {0.25, 0.25},
        {0.25, 0.75},
        {0.5,  0.5 },
        {0.75, 0.25},
        {0.75, 0.75}
    };
    int axis = static_cast<int>(kinetics::axisOf(face));
    int a    = (axis + 1) % 3, b = (axis + 2) % 3;
    double maxDepth = 0;
    for (auto const& t : kTests) {
        bool inside = box.min[a] <= t[0] && t[0] <= box.max[a] && box.min[b] <= t[1] && t[1] <= box.max[b];
        if (!inside) return std::numeric_limits<double>::infinity();
        double depth = kinetics::isPositive(face) ? box.min[axis] : 1.0 - box.max[axis];
        maxDepth     = std::max(maxDepth, depth);
    }
    return maxDepth;
}

float flowLimit(float max, Face facing, std::function<FlowBlock(int i)> const& blockAt) {
    for (int i = 0; i < max; ++i) {
        auto b = blockAt(i + 1);
        switch (b.kind) {
        case FlowBlock::Kind::Unloaded:
            return float(i);
        case FlowBlock::Kind::Pass:
            continue;
        case FlowBlock::Kind::Full:
            return float(i);
        case FlowBlock::Kind::Partial: {
            double depth = findMaxDepth(b, facing);
            if (std::isinf(depth)) continue;
            return std::min(float(i + depth + 1 / 32.0), max);
        }
        }
    }
    return max;
}

int limitOf(float maxDistance) {
    int whole = static_cast<int>(maxDistance);
    return float(whole) == maxDistance ? whole : whole + 1;
}

void AirCurrent::buildSegments(std::function<FanType(int i)> const& catalystAt) {
    segments.clear();
    int  limit       = limitOf(maxDistance);
    int  searchStart = pushing ? 1 : limit;
    int  searchEnd   = pushing ? limit : 1;
    int  searchStep  = pushing ? 1 : -1;
    int  toOffset    = pushing ? -1 : 0;
    bool open        = false;
    Segment current;
    FanType type = FanType::None;
    for (int i = searchStart; i * searchStep <= searchEnd * searchStep; i += searchStep) {
        if (auto t = catalystAt(i); t != FanType::None) type = t;
        if (!open) {
            current = Segment{type, i + toOffset, 0};
            open    = true;
        } else if (current.type != type) {
            current.endOffset = i + toOffset;
            segments.push_back(current);
            current = Segment{type, i + toOffset, 0};
        }
    }
    if (open) {
        current.endOffset = searchEnd + searchStep + toOffset;
        segments.push_back(current);
    }
}

FanType AirCurrent::typeAt(float offset) const {
    if (offset < 0 || offset > maxDistance) return FanType::None;
    for (auto const& s : segments) {
        if (pushing ? offset <= s.endOffset : offset >= s.endOffset) return s.type;
    }
    return FanType::None;
}

Box currentBounds(int x, int y, int z, Face direction, float maxDistance) {
    if (maxDistance < 0.25f) return Box{};
    auto  n     = normalOf(direction);
    Vec3d start{double(x) + n.x, double(y) + n.y, double(z) + n.z};
    Box   box{start, start + Vec3d{1, 1, 1}};
    double factor = maxDistance - 1.0;
    Vec3d  scale  = n * factor;
    auto   each   = [&](auto&& fn) {
        fn(box.min.x, box.max.x, scale.x);
        fn(box.min.y, box.max.y, scale.y);
        fn(box.min.z, box.max.z, scale.z);
    };
    if (factor > 0) {
        // AABB.expandTowards
        each([](double& lo, double& hi, double s) {
            if (s < 0) lo += s;
            else hi += s;
        });
    } else {
        // AABB.contract(scale) then move(scale)
        each([](double& lo, double& hi, double s) {
            if (s < 0) lo -= s;
            else if (s > 0) hi -= s;
            lo += s;
            hi += s;
        });
    }
    return box;
}

double alignedDistanceToFace(Vec3d const& p, int bx, int by, int bz, Face face) {
    auto   axis = kinetics::axisOf(face);
    double base = axis == Axis::X ? bx : axis == Axis::Y ? by : bz;
    return std::abs(p.get(axis) - (base + (kinetics::isPositive(face) ? 1 : 0)));
}

Vec3d pushedMotion(Vec3d const& motion, AirCurrent const& current, float speed, bool sneaking, double distanceToCenter) {
    Face   flowFace = current.pushing ? current.direction : kinetics::opposite(current.direction);
    auto   flow     = normalOf(flowFace);
    double sneakMod = sneaking ? 4096.0 : 512.0;
    double dist     = std::max(distanceToCenter, 1e-3);
    double accel    = std::abs(speed) / sneakMod / (dist / std::max(current.maxDistance, 1e-3f));
    auto   in       = [&](double f, double m) { return std::clamp(f * accel - m, -5.0, 5.0); };
    Vec3d  add{in(flow.x, motion.x), in(flow.y, motion.y), in(flow.z, motion.z)};
    return motion + add * (1.0 / 8.0);
}

// ---- nozzle ----------------------------------------------------------------------------------------------------

std::optional<Vec3d> nozzlePush(Vec3d const& diff, float range, bool pushing, bool isItem) {
    double distance = diff.length();
    if (distance > range) return std::nullopt;
    if (!pushing && distance < 1.5) return std::nullopt;
    double factor = isItem ? 1 / 128.0 : 1 / 32.0;
    return diff.normalized() * ((range - distance) * (pushing ? 1 : -1) * factor);
}

// ---- millstone -------------------------------------------------------------------------------------------------

int millstoneProcessingSpeed(float speed) { return std::clamp(static_cast<int>(std::abs(speed / 16.f)), 1, 512); }

// ---- crushing wheels -------------------------------------------------------------------------------------------

float crushingSpeedOf(float wheelSpeed) { return std::abs(wheelSpeed / 50.f); }

float crusherProcessingSpeed(float crushingSpeed, int count, bool appliedRecipe) {
    float speed = crushingSpeed * 4;
    if (speed <= 0) return 0;
    float denom = appliedRecipe ? 1.f : std::log(float(std::max(count, 1))) / std::log(2.f);
    float value = denom <= 0 ? std::numeric_limits<float>::infinity() : speed / denom;
    return std::clamp(value, .25f, 20.f);
}

Face controllerFacing(Axis wheelAxis, Face side, float speed) {
    int  sign = speed > 0 ? 1 : speed < 0 ? -1 : 0;
    int  ado  = sign * (kinetics::isPositive(side) ? 1 : -1);
    auto unit = [](Axis a) {
        return std::array<int, 3>{a == Axis::X ? 1 : 0, a == Axis::Y ? 1 : 0, a == Axis::Z ? 1 : 0};
    };
    auto w = unit(wheelAxis), s = unit(kinetics::axisOf(side));
    std::array<int, 3> c{w[1] * s[2] - w[2] * s[1], w[2] * s[0] - w[0] * s[2], w[0] * s[1] - w[1] * s[0]};
    return kinetics::nearestFace(c[0] * ado, c[1] * ado, c[2] * ado);
}

Vec3d crusherEntityMotion(Face facing, float crushingSpeed, Vec3d const& entityPos, int bx, int bz, bool sneaking) {
    float  speed  = crushingSpeed * 4;
    int    offset = kinetics::isPositive(facing) ? 1 : -1;
    double xm     = (bx + .5 - entityPos.x) / 2.0;
    double zm     = (bz + .5 - entityPos.z) / 2.0;
    if (sneaking) xm = zm = 0;
    double movement = std::max(-speed / 4.0, -.5) * -offset;
    auto   axis     = kinetics::axisOf(facing);
    return {axis == Axis::X ? movement : xm, axis == Axis::Y ? movement : 0.0, axis == Axis::Z ? movement : zm};
}

Vec3d crusherOutPos(int bx, int by, int bz, Face facing) {
    int  offset = kinetics::isPositive(facing) ? 1 : -1;
    auto axis   = kinetics::axisOf(facing);
    return {
        bx + .5 + (axis == Axis::X ? .55 * offset : 0.0),
        by + .5 + (axis == Axis::Y ? .55 * offset : 0.0),
        bz + .5 + (axis == Axis::Z ? .55 * offset : 0.0)
    };
}

Vec3d crusherOutSpeed(Face facing) {
    int  offset = kinetics::isPositive(facing) ? 1 : -1;
    auto axis   = kinetics::axisOf(facing);
    return {
        (axis == Axis::X ? .25 : 0.0) * offset,
        offset == 1 ? (axis == Axis::Y ? .5 : 0.0) : 0.0,
        (axis == Axis::Z ? .25 : 0.0) * offset
    };
}

bool crusherIntakeReached(Face facing, Vec3d const& entityPos, int bx, int by, int bz) {
    int    offset = kinetics::isPositive(facing) ? 1 : -1;
    auto   axis   = kinetics::axisOf(facing);
    double centre = (axis == Axis::X ? bx : axis == Axis::Y ? by : bz) + .5;
    return entityPos.get(axis) * -offset < (centre - .25) * -offset;
}

} // namespace lv::core::bulk
