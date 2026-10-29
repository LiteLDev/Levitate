#include "core/assembly/Deployer.h"

#include <algorithm>
#include <cmath>

namespace lv::core::assembly {

int deployerTimerSpeed(float speed) {
    if (speed == 0) return 0;
    return static_cast<int>(std::clamp(std::abs(speed * 2.f), 8.f, 512.f));
}

Axis deployerShaftAxis(Face facing, bool alongFirst) {
    switch (kinetics::axisOf(facing)) {
    case Axis::X:
        return alongFirst ? Axis::Y : Axis::Z;
    case Axis::Y:
        return alongFirst ? Axis::X : Axis::Z;
    default:
        return alongFirst ? Axis::X : Axis::Y;
    }
}

bool deployerAlongFirstFor(Face facing, Axis shaft) { return deployerShaftAxis(facing, true) == shaft; }

bool deployerPlacementAlongFirst(Face facing, bool horizontalLookAlongX, PlacementNeighbours const& n) {
    auto towards = [&](Face f) { return n.shaftTowards[static_cast<int>(f)]; };
    Axis faceAxis = kinetics::axisOf(facing);
    if (faceAxis != Axis::Y) {
        bool alongFirst = faceAxis == Axis::Z;
        Face positivePerpendicular = faceAxis == Axis::X ? Face::South : Face::East;
        bool shaftAbove  = towards(Face::Up);
        bool shaftBelow  = towards(Face::Down);
        bool perpendicular = towards(positivePerpendicular) || towards(kinetics::opposite(positivePerpendicular));
        // a vertical shaft next to it wins over the horizontal default, a horizontal one keeps it
        if ((shaftAbove || shaftBelow) && !perpendicular) alongFirst = faceAxis == Axis::X;
        return alongFirst;
    }
    bool alongFirst = horizontalLookAlongX;
    int  preferred  = -1;
    for (Face side : {Face::North, Face::South, Face::West, Face::East}) {
        if (!towards(side)) continue;
        if (preferred >= 0 && kinetics::axisOf(static_cast<Face>(preferred)) != kinetics::axisOf(side)) {
            preferred = -1;
            break;
        }
        preferred = static_cast<int>(side);
    }
    if (preferred >= 0) alongFirst = kinetics::axisOf(static_cast<Face>(preferred)) == Axis::X;
    return alongFirst;
}

HandPose deployerHandPose(DeployerMode mode, bool holding) {
    if (mode == DeployerMode::Punch) return HandPose::Punching;
    return holding ? HandPose::Holding : HandPose::Pointing;
}

float deployerHandOffset(DeployerState state, float timer, int timerSpeed, float partial, float reach, HandPose pose) {
    float progress = 0;
    if (state == DeployerState::Expanding) progress = 1 - (timer - partial * static_cast<float>(timerSpeed)) / 1000.f;
    if (state == DeployerState::Retracting) progress = (timer - partial * static_cast<float>(timerSpeed)) / 1000.f;
    float handLength = pose == HandPose::Pointing ? 0.f : pose == HandPose::Holding ? 4 / 16.f : 3 / 16.f;
    return std::min(std::clamp(progress, 0.f, 1.f) * (reach + handLength), 21 / 16.f);
}

float deployerReach(float hitDistance) { return 0.5f + std::min(hitDistance, 0.75f); }

} // namespace lv::core::assembly
