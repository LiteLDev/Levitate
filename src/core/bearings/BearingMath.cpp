#include "core/bearings/BearingMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lv::core::bearings {

namespace {
double sign(double v) { return v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0; }
} // namespace

bool isNearInitialAngle(double angle) { return std::abs(angle) < 45 || std::abs(angle) > 7 * 45; }

bool canDisassemble(RotationMode mode, double angle) {
    return mode == RotationMode::RotatePlace || (mode == RotationMode::RotatePlaceReturned && isNearInitialAngle(angle));
}

double advanceAngle(double angle, double delta) {
    // Create keeps the angle as a float: (angle + speed) % 360
    return static_cast<double>(std::fmod(static_cast<float>(angle + delta), 360.f));
}

float windmillSpeed(int sails, int sailsPerRpm, bool clockwise) {
    int per = std::max(1, sailsPerRpm);
    int rpm = std::clamp(sails / per, 1, 16);
    return static_cast<float>(rpm) * (clockwise ? 1.f : -1.f);
}

double shortestAngleDiff(double current, double target) {
    current = std::fmod(current, 360.0);
    target  = std::fmod(target, 360.0);
    return static_cast<double>(static_cast<float>(std::fmod(std::fmod(target - current, 360.0) + 540.0, 360.0) - 180.0));
}

double shortestAngleDiff(double current, double target, double hint) {
    double diff = shortestAngleDiff(current, target);
    if (std::abs(std::abs(diff) - 180.0) < 1e-5 && sign(diff) != sign(hint)) return diff + 360.0 * sign(hint);
    return diff;
}

double hourTarget(long long dayTime, int axisStep, bool twentyFour) {
    int time  = static_cast<int>(((dayTime % 24000) + 24000) % 24000);
    int hours = (time / 1000 + 6) % 24;
    int step  = twentyFour ? 24 : 12;
    return static_cast<double>(static_cast<float>(axisStep) * -360.f / static_cast<float>(step) * static_cast<float>(hours % step));
}

double minuteTarget(long long dayTime, int axisStep) {
    int time    = static_cast<int>(((dayTime % 24000) + 24000) % 24000);
    int minutes = (time % 1000) * 60 / 1000;
    return static_cast<double>(static_cast<float>(axisStep) * -360.f / 60.f * static_cast<float>(minutes));
}

double handStep(double angle, double target, double maxSpeed) {
    double speed = -std::abs(maxSpeed);
    if (speed == 0) return 0;
    double diff = shortestAngleDiff(angle, target);
    if (diff < 0) return std::max(speed, diff);
    return std::min(-speed, diff);
}

CartAction actionFor(RailType type, bool powered, bool cartHasPassengers) {
    switch (type) {
    case RailType::Activator:
        return powered ? CartAction::Disassemble : CartAction::Pass;
    case RailType::Controller:
        return powered ? CartAction::AssembleAccelerateDirectional : CartAction::DisassembleBrake;
    case RailType::Detector:
        return cartHasPassengers ? CartAction::Disassemble : CartAction::AssembleAccelerate;
    case RailType::Powered:
        return powered ? CartAction::AssembleAccelerate : CartAction::DisassembleBrake;
    case RailType::Regular:
        return powered ? CartAction::Assemble : CartAction::Disassemble;
    }
    return CartAction::Pass;
}

bool shouldAssemble(CartAction a) {
    return a == CartAction::Assemble || a == CartAction::AssembleAccelerate || a == CartAction::AssembleAccelerateDirectional;
}

bool shouldDisassemble(CartAction a) { return a == CartAction::Disassemble || a == CartAction::DisassembleBrake; }

cc::Dir railDirection(bool axisX, bool backwards) {
    cc::Dir pointing = axisX ? cc::Dir::West : cc::Dir::North;
    return backwards ? cc::opposite(pointing) : pointing;
}

double headingOf(double dx, double dz) { return std::atan2(-dz, dx) * 180.0 / std::numbers::pi; }

double headingOf(cc::Dir d) {
    auto o = cc::offset(d);
    return headingOf(o.x, o.z);
}

std::optional<cc::Vec3d> controllerVelocity(cc::Vec3d const& motion, cc::Dir accel, int power, double maxSpeed) {
    auto      o           = cc::offset(accel);
    cc::Vec3d a           = cc::Vec3d::of(o);
    double    targetSpeed = maxSpeed * std::clamp(power, 0, 15) / 15.0;
    double    dot         = motion.x * a.x + motion.y * a.y + motion.z * a.z;
    if ((dot >= 0 || motion.lengthSqr() < 0.0001) && targetSpeed > 0) return a * targetSpeed;
    return std::nullopt;
}

double maxYawApproach(double motionLength, double widthX) { return motionLength * 12.0 / std::max(1.0, widthX / 6.0); }

std::vector<cc::Dir> orderedByDistanceExceptAxis(cc::Vec3d const& hit, cc::Axis axis) {
    cc::Vec3d                            centerToHit = hit - cc::Vec3d{0.5, 0.5, 0.5};
    std::vector<std::pair<cc::Dir, double>> list;
    for (auto d : cc::kAllDirs) {
        if (cc::axisOf(d) == axis) continue;
        list.emplace_back(d, (cc::Vec3d::of(cc::offset(d)) - centerToHit).length());
    }
    std::stable_sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.second < b.second; });
    std::vector<cc::Dir> out;
    for (auto const& [d, dist] : list) out.push_back(d);
    return out;
}

} // namespace lv::core::bearings
