#include "core/trains/Physics.h"

#include <algorithm>
#include <cmath>

namespace lv::core::trains {

Stats statsOf(SpeedConfig const& cfg, bool powered) {
    Stats s;
    s.maxSpeed     = (powered ? cfg.poweredTrainTopSpeed : cfg.trainTopSpeed) / 20.0;
    s.maxTurnSpeed = (powered ? cfg.poweredTrainTurningTopSpeed : cfg.trainTurningTopSpeed) / 20.0;
    s.acceleration = (powered ? cfg.poweredTrainAcceleration : cfg.trainAcceleration) / 400.0;
    return s;
}

double approach(double speed, double target, double acceleration, double mod) {
    if (tracks::nearlyEqual(target, speed)) return speed;
    if (speed < target) return std::min(speed + acceleration * mod, target);
    return std::max(speed - acceleration * mod, target);
}

double passiveSlowdown(double speed, double acceleration) {
    if (speed == 0) return 0;
    if (speed > 0) return std::max(0.0, speed - acceleration);
    return std::min(0.0, speed + acceleration);
}

double derailedSlowdown(double speed) {
    speed /= 3;
    return tracks::nearlyEqual(speed, 0) ? 0 : speed;
}

double brakingDistance(double speed, double acceleration) {
    if (acceleration <= 0) return speed == 0 ? 0 : 1e9;
    return speed * speed / (2 * acceleration);
}

NavigationStep navigate(double speed, Stats const& stats, NavigationInput const& in) {
    NavigationStep out;
    double const dir            = in.direction < 0 ? -1.0 : 1.0;
    double const targetDistance = in.distanceToTarget + 0.25; // Create always overshoots a little
    // final approach: snap onto the destination
    if (targetDistance - std::abs(speed) < 1 / 32.0) {
        out.speed         = std::max(targetDistance, 1 / 32.0) * dir;
        out.finalApproach = true;
        return out;
    }
    double topSpeed = stats.maxSpeed;
    if (targetDistance < 10) {
        double maxApproach = topSpeed * targetDistance / 10;
        if (speed * dir > maxApproach) {
            out.speed = speed + (maxApproach - std::abs(speed)) * 0.5 * dir;
            return out;
        }
    }
    topSpeed *= std::clamp(in.throttle, 0.0, 1.0);
    double turnTopSpeed = std::min(topSpeed, stats.maxTurnSpeed);
    double braking      = brakingDistance(speed, stats.acceleration);
    double target       = targetDistance > braking ? topSpeed * dir : 0;
    if (in.distanceToNextCurve >= 0) {
        double slowingDistance = braking - brakingDistance(turnTopSpeed, stats.acceleration);
        double curveTarget     = (in.distanceToNextCurve > slowingDistance ? topSpeed : turnTopSpeed) * dir;
        if (std::abs(curveTarget) < std::abs(target)) target = curveTarget;
    }
    out.speed = approach(speed, target, stats.acceleration, 1);
    return out;
}

double manualTargetSpeed(int keys, Stats const& stats, double manualModifier, double throttle, bool onTurn, bool slow) {
    if (keys == 0) return 0;
    double topSpeed = stats.maxSpeed * manualModifier;
    double cap      = topSpeed * std::clamp(throttle, 0.0, 1.0);
    if (onTurn) topSpeed = stats.maxTurnSpeed;
    if (slow) topSpeed /= 4;
    return std::min(topSpeed, cap) * (keys > 0 ? 1 : -1);
}

double manualStep(double speed, double target, Stats const& stats) {
    bool against = target != 0 && speed != 0 && (target > 0) != (speed > 0);
    return approach(speed, target, stats.acceleration, against ? 2 : 1);
}

std::optional<EntityHit> entityHit(Vec3d const& trainMotion, Vec3d const& entityMotion, bool monster) {
    Vec3d diff = trainMotion - entityMotion;
    if (diff.length() <= 0.35 || trainMotion.length() <= 0.35) return std::nullopt;
    double damage = diff.length() * (monster ? 2 : 1);
    EntityHit hit;
    hit.damage    = int(damage * 16);
    Vec3d flat    = Vec3d{trainMotion.x, 0, trainMotion.z}.normalize();
    Vec3d motion  = entityMotion + (flat + Vec3d{0, 0.25, 0}) * (damage * 4) + diff;
    double len    = motion.length();
    if (len > 3) motion = motion * (3 / len);
    hit.motion = motion;
    return hit;
}

std::optional<double> collisionExplosion(double speedA, double speedB) {
    double combined = std::abs(speedA) + std::abs(speedB);
    if (combined <= 0.2) return std::nullopt;
    return std::min(3 * combined, 5.0);
}

double crashBounce(double speed) { return -std::clamp(speed, -0.5, 0.5); }

int predictTransitTicks(double distance, Stats const& stats, double throttle) {
    double v = std::min(std::clamp(throttle, 0.0, 1.0) * stats.maxSpeed, (stats.maxSpeed + stats.maxTurnSpeed) / 2);
    if (v <= 0) return -1;
    return int(std::abs(distance) / v * 2);
}

} // namespace lv::core::trains
