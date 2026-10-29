#include "core/assembly/Arm.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lv::core::assembly {

namespace {
constexpr double kDeg = 180.0 / std::numbers::pi;

using V = std::array<double, 3>;
V add(V a, V b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V rotX(V v, double deg) {
    double r = deg / kDeg, c = std::cos(r), s = std::sin(r);
    return {v[0], v[1] * c - v[2] * s, v[1] * s + v[2] * c};
}
V rotY(V v, double deg) {
    double r = deg / kDeg, c = std::cos(r), s = std::sin(r);
    return {v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c};
}

float lerp(float t, float a, float b) { return a + (b - a) * t; }
float angleLerp(float t, float from, float to) {
    float diff = std::fmod(to - from + 540.f, 360.f) - 180.f;
    return from + diff * t;
}
} // namespace

ArmScan armScanRange(ArmSelectionMode mode, int lastIndex, int count) {
    ArmScan s;
    s.begin = mode == ArmSelectionMode::PreferFirst ? 0 : lastIndex + 1;
    s.end   = mode == ArmSelectionMode::ForcedRoundRobin ? lastIndex + 2 : count;
    s.end   = std::min(s.end, count);
    s.begin = std::max(0, s.begin);
    return s;
}

int armLastIndexAfter(ArmSelectionMode mode, int lastIndex, bool found, int count) {
    if (!found && mode == ArmSelectionMode::RoundRobin) lastIndex = -1;
    if (lastIndex == count - 1) lastIndex = -1;
    return lastIndex;
}

float armProgressPerTick(float speed) { return std::min(256.f, std::abs(speed)) / 1024.f; }

bool armInRange(int dx, int dy, int dz, int range) {
    long long d = static_cast<long long>(dx) * dx + static_cast<long long>(dy) * dy + static_cast<long long>(dz) * dz;
    return d < static_cast<long long>(range) * range;
}

ArmAngles armRestAngles() { return ArmAngles{}; }

ArmAngles armAnglesFor(std::array<int, 3> armPos, Vec3f target, kinetics::Face clawFacing, bool ceiling) {
    V origin{armPos[0] + 0.5, armPos[1] + 0.5 + (ceiling ? -4 / 16.0 : 4 / 16.0), armPos[2] + 0.5};
    V clawTarget = target;
    auto off     = kinetics::offsetOf(kinetics::opposite(clawFacing));
    V    t       = add(target, V{off.x * 0.5, off.y * 0.5, off.z * 0.5});
    V    diff    = sub(t, origin);
    double horizontal = std::sqrt(diff[0] * diff[0] + diff[2] * diff[2]);
    double baseAngle  = std::atan2(diff[0], diff[2]) * kDeg + 180;
    if (ceiling) {
        diff[1]   = -diff[1];
        baseAngle = 180 - baseAngle;
    }
    double alphaOffset = std::atan2(diff[1], horizontal) * kDeg;
    double a = kArmLowerLength, b = kArmUpperLength;
    double len   = std::clamp(std::sqrt(diff[1] * diff[1] + horizontal * horizontal), 1 / 8.0, a + b);
    double alpha = std::acos(std::clamp((-b * b + a * a + len * len) / (2 * a * len), -1.0, 1.0)) * kDeg + alphaOffset;
    double beta  = std::acos(std::clamp((-len * len + a * a + b * b) / (2 * b * a), -1.0, 1.0)) * kDeg;
    if (std::isnan(alpha)) alpha = 0;
    if (std::isnan(beta)) beta = 0;

    V head{0, 0, 0};
    head = rotX(add(head, V{0, b, 0}), beta + 180);
    head = rotX(add(head, V{0, a, 0}), alpha - 90);
    head = rotY(head, baseAngle);
    if (ceiling) head = rotX(head, 180);
    head = add(head, origin);
    V headDiff = sub(clawTarget, head);
    if (ceiling) headDiff[1] = -headDiff[1];
    double hh       = std::sqrt(headDiff[0] * headDiff[0] + headDiff[2] * headDiff[2]);
    double headAngle = alpha + beta + 135 - std::atan2(headDiff[1], hh) * kDeg;

    ArmAngles out;
    out.base  = static_cast<float>(baseAngle);
    out.lower = static_cast<float>(alpha);
    out.upper = static_cast<float>(beta);
    out.head  = static_cast<float>(-headAngle);
    return out;
}

ArmAngles armPoseAt(ArmAngles const& previous, ArmAngles const& target, bool hasTarget, float progress) {
    progress = std::clamp(progress, 0.f, 1.f);
    ArmAngles out;
    out.base = angleLerp(progress, previous.base, hasTarget ? target.base : previous.base);
    // first half: fold from the previous pose back to rest; second half: reach out to the target
    ArmAngles rest = armRestAngles();
    ArmAngles from = progress < .5f ? previous : rest;
    ArmAngles to   = progress < .5f || !hasTarget ? rest : target;
    float     t    = progress >= 1.f ? 1.f : std::fmod(progress, .5f) * 2.f;
    out.lower      = lerp(t, from.lower, to.lower);
    out.upper      = lerp(t, from.upper, to.upper);
    out.head       = lerp(t, from.head, to.head);
    return out;
}

} // namespace lv::core::assembly
