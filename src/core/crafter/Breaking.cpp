#include "core/crafter/Breaking.h"

#include <algorithm>
#include <cmath>

namespace lv::core::crafter {

float stationaryBreakSpeed(float rpm) { return std::abs(rpm / 100.0f); }

float animationSpeed(double motionLength) {
    constexpr int modifier = 1000;
    double        length   = -motionLength;
    if (std::abs(length) < 1.0 / 512.0) return 0;
    double sign = length > 0 ? 1.0 : -1.0;
    return static_cast<float>((static_cast<int>(length * modifier + 100 * sign) / 100) * 100);
}

float movingBreakSpeed(float animSpeed, float lowerLimit) { return std::clamp(std::abs(animSpeed) / 500.0f, lowerLimit, 16.0f); }

int progressStep(float breakSpeed, float hardness, int progress) {
    int raw = hardness > 0 ? static_cast<int>(breakSpeed / hardness) : 10;
    int hi  = std::max(1, 10 - progress);
    return std::clamp(raw, 1, hi);
}

int ticksUntilNextProgress(float hardness, float breakSpeed) {
    if (breakSpeed <= 0) return 0;
    return static_cast<int>(hardness / breakSpeed);
}

bool breakableHardness(float hardness) { return hardness >= 0; }

int ticksToBreak(float hardness, float breakSpeed) {
    int progress = 0, ticks = 0;
    while (true) {
        ++ticks; // the hit itself
        progress += progressStep(breakSpeed, hardness, progress);
        if (progress >= 10) return ticks;
        ticks += ticksUntilNextProgress(hardness, breakSpeed);
    }
}

double drillDamage(float rpm) {
    double s    = std::abs(rpm);
    double sub1 = std::min(s / 16, 2.0);
    double sub2 = std::min(s / 32, 4.0);
    double sub3 = std::min(s / 64, 4.0);
    return std::clamp(sub1 + sub2 + sub3, 0.0, 20.0);
}

double movingDamage(double relativeMotionLength) { return std::clamp(6 * std::pow(relativeMotionLength, 0.4) + 1, 2.0, 10.0); }

Axis directionalAxis(Face facing, bool alongFirst) {
    switch (kinetics::axisOf(facing)) {
    case Axis::X:
        return alongFirst ? Axis::Y : Axis::Z;
    case Axis::Y:
        return alongFirst ? Axis::X : Axis::Z;
    default:
        return alongFirst ? Axis::X : Axis::Y;
    }
}

float sawProcessingSpeed(float rpm) { return std::clamp(std::abs(rpm) / 24.0f, 1.0f, 128.0f); }

int sawProcessingTime(int recipeTime, int count) { return recipeTime * std::max(1, count / 5); }

kinetics::Offset sawItemMovement(bool alongFirst, float rpm) {
    bool alongX = !alongFirst;
    int  offset = rpm < 0 ? -1 : 1;
    return {alongX ? offset : 0, 0, alongX ? 0 : -offset};
}

} // namespace lv::core::crafter
