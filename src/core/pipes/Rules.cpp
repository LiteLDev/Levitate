#include "core/pipes/Rules.h"

#include <cmath>
#include <limits>

namespace lv::core::pipes {

int count(FaceMask m) {
    int n = 0;
    for (Face f : kFaces) n += has(m, f) ? 1 : 0;
    return n;
}

FaceMask updatePipeConnections(
    FaceMask                   previous,
    std::array<bool, 6> const& canConnect,
    std::optional<Face>        preferred,
    std::optional<Face>        ignored
) {
    FaceMask next = previous;
    for (Face f : kFaces) {
        if (ignored && f == *ignored) continue;
        if (canConnect[static_cast<int>(f)]) next |= bit(f);
        else next &= static_cast<FaceMask>(~bit(f));
    }
    int open = count(next);
    if (open >= 2) return next;
    if (open == 1) {
        for (Face f : kFaces)
            if (has(next, f)) return static_cast<FaceMask>(next | bit(opposite(f)));
    }
    // no connection at all: keep a previously straight pipe, else open along the preferred direction
    if (count(previous) == 2) return previous;
    Face p = preferred.value_or(Face::Up);
    return static_cast<FaceMask>(bit(p) | bit(opposite(p)));
}

std::optional<Axis> straightAxis(FaceMask open) {
    if (count(open) != 2) return std::nullopt;
    for (Axis a : kAxes)
        if ((open & axisMask(a)) == axisMask(a)) return a;
    return std::nullopt;
}

bool drawsCasing(FaceMask open) {
    for (Axis a : kAxes) {
        int around = 0;
        for (Face f : kFaces)
            if (axisOf(f) != a && has(open, f)) ++around;
        if (around >= 3) return true;
    }
    return false;
}

bool cornerOrEnd(FaceMask open) { return !straightAxis(open) && !drawsCasing(open); }

std::optional<Axis> wrenchGlassAxis(FaceMask open, Face clicked, double hitX, double hitY, double hitZ) {
    auto axis = straightAxis(open);
    if (!axis) {
        double closest = std::numeric_limits<double>::max();
        for (Axis a : kAxes) {
            if (axisOf(clicked) == a) continue;
            double d = std::abs(a == Axis::X ? hitX : a == Axis::Y ? hitY : hitZ);
            if (d < closest) {
                closest = d;
                axis    = a;
            }
        }
    }
    if (!axis || axisOf(clicked) == *axis) return std::nullopt;
    return axis;
}

FaceMask regularPipeFromAxis(Axis axis, std::array<bool, 6> const& canConnect) {
    Face side = faceOf(axis, true);
    return updatePipeConnections(axisMask(axis), canConnect, side, std::nullopt);
}

std::array<int, 3> normal(Face f) {
    auto o = offsetOf(f);
    return {o.x, o.y, o.z};
}

Face nearestFace(double x, double y, double z) {
    Face   best  = Face::North;
    double score = -std::numeric_limits<double>::max();
    for (Face f : kFaces) {
        auto   n = normal(f);
        double s = n[0] * x + n[1] * y + n[2] * z;
        if (s > score) {
            score = s;
            best  = f;
        }
    }
    return best;
}

Face horizontalFace(double x, double z) {
    if (std::abs(x) > std::abs(z)) return x > 0 ? Face::East : Face::West;
    return z > 0 ? Face::South : Face::North;
}

Face clockwise(Face f) {
    switch (f) {
    case Face::North:
        return Face::East;
    case Face::East:
        return Face::South;
    case Face::South:
        return Face::West;
    case Face::West:
        return Face::North;
    default:
        return f;
    }
}

Face pumpPlacement(Face nearestLook, bool sneaking, std::optional<Face> kineticPreferred, std::array<bool, 6> const& pipeAt) {
    // DirectionalKineticBlock: towards a shaft that faces it, else facing the player (away when sneaking)
    Face placed = (!kineticPreferred || sneaking) ? (sneaking ? nearestLook : opposite(nearestLook)) : opposite(*kineticPreferred);
    Face target = sneaking ? nearestLook : opposite(nearestLook);
    std::optional<Face> best;
    double              bestDistance = std::numeric_limits<double>::max();
    auto                t            = normal(target);
    for (Face d : kFaces) {
        if (!pipeAt[static_cast<int>(d)]) continue;
        auto   n        = normal(d);
        double distance = std::sqrt(double((n[0] - t[0]) * (n[0] - t[0]) + (n[1] - t[1]) * (n[1] - t[1]) + (n[2] - t[2]) * (n[2] - t[2])));
        if (distance >= bestDistance) continue;
        bestDistance = distance;
        best         = d;
    }
    if (!best || axisOf(*best) == axisOf(target) || sneaking) return placed;
    return *best;
}

ValveAxes valveAxes(Face facing, bool alongFirst) {
    switch (axisOf(facing)) {
    case Axis::X:
        return alongFirst ? ValveAxes{Axis::Z, Axis::Y} : ValveAxes{Axis::Y, Axis::Z};
    case Axis::Y:
        return alongFirst ? ValveAxes{Axis::Z, Axis::X} : ValveAxes{Axis::X, Axis::Z};
    default:
        return alongFirst ? ValveAxes{Axis::Y, Axis::X} : ValveAxes{Axis::X, Axis::Y};
    }
}

ValvePlacement valvePlacement(
    Face                       nearestLook,
    Face                       horizontalLook,
    bool                       sneaking,
    std::array<bool, 6> const& shaftAt,
    std::array<bool, 6> const& pipeAt
) {
    Face facing     = sneaking ? nearestLook : opposite(nearestLook);
    bool alongFirst = false;
    Axis faceAxis   = axisOf(facing);
    auto at         = [](std::array<bool, 6> const& a, Face f) { return a[static_cast<int>(f)]; };
    if (faceAxis != Axis::Y) {
        alongFirst          = faceAxis == Axis::Z;
        Face positivePerp   = faceAxis == Axis::X ? Face::South : Face::East;
        bool shaftAbove     = at(shaftAt, Face::Up);
        bool shaftBelow     = at(shaftAt, Face::Down);
        bool preferLeft     = at(pipeAt, positivePerp);
        bool preferRight    = at(pipeAt, opposite(positivePerp));
        if (shaftAbove || shaftBelow || preferLeft || preferRight) alongFirst = faceAxis == Axis::X;
    } else {
        alongFirst = axisOf(horizontalLook) == Axis::X;
        std::optional<Face> preferred;
        for (Face side : {Face::North, Face::South, Face::West, Face::East}) {
            if (!at(shaftAt, side) && !at(pipeAt, clockwise(side))) continue;
            if (preferred && axisOf(*preferred) != axisOf(side)) {
                preferred.reset();
                break;
            }
            preferred = side;
        }
        if (preferred) alongFirst = axisOf(*preferred) == Axis::X;
    }
    return {facing, alongFirst};
}

Face rotateAround(Face f, Axis axis) {
    if (axisOf(f) == axis) return f;
    auto n = normal(f);
    int  x = n[0], y = n[1], z = n[2], rx = x, ry = y, rz = z;
    // right-hand quarter turn around the positive axis
    switch (axis) {
    case Axis::X:
        ry = -z, rz = y;
        break;
    case Axis::Y:
        rx = z, rz = -x;
        break;
    default:
        rx = -y, ry = x;
        break;
    }
    for (Face c : kFaces) {
        auto m = normal(c);
        if (m[0] == rx && m[1] == ry && m[2] == rz) return c;
    }
    return f;
}

ValvePlacement valveWrenched(ValvePlacement state, Face clicked) {
    if (axisOf(clicked) == axisOf(state.facing)) return {state.facing, !state.alongFirst};
    auto before = valveAxes(state.facing, state.alongFirst);
    Face next   = rotateAround(state.facing, axisOf(clicked));
    // keep the shaft on the axis it had when that is still possible (the pipe turns around it)
    for (bool along : {state.alongFirst, !state.alongFirst}) {
        if (valveAxes(next, along).shaft == before.shaft) return {next, along};
    }
    return {next, state.alongFirst};
}

Axis smartPipeAxis(SmartPipeState s) { return s.face == AttachFace::Wall ? Axis::Y : axisOf(s.facing); }

SmartPipeState smartPipePlacement(Face nearestLook, Face horizontalLook, std::array<bool, 6> const& pipeAt) {
    SmartPipeState s;
    if (axisOf(nearestLook) == Axis::Y) {
        s.face   = nearestLook == Face::Up ? AttachFace::Ceiling : AttachFace::Floor;
        s.facing = horizontalLook;
    } else {
        s.face   = AttachFace::Wall;
        s.facing = opposite(nearestLook);
    }
    std::optional<Axis> preferredAxis;
    for (Face f : kFaces) {
        if (!pipeAt[static_cast<int>(f)]) continue;
        if (preferredAxis && *preferredAxis != axisOf(f)) {
            preferredAxis.reset();
            break;
        }
        preferredAxis = axisOf(f);
    }
    if (!preferredAxis) return s;
    if (*preferredAxis == Axis::Y) {
        s.face   = AttachFace::Wall;
        s.facing = opposite(s.facing);
        return s;
    }
    if (s.face == AttachFace::Wall) s.face = AttachFace::Floor;
    // the last matching entry of the nearest-looking list wins: the end less aligned with the look,
    // so the pipe faces along the look
    Face positive = faceOf(*preferredAxis, true);
    s.facing      = (axisOf(horizontalLook) == *preferredAxis) ? horizontalLook : positive;
    return s;
}

} // namespace lv::core::pipes
