#include "core/tracks/Math.h"

#include <algorithm>

namespace lv::core::tracks {

Vec3d bezier(Vec3d const& p1, Vec3d const& p2, Vec3d const& q1, Vec3d const& q2, double t) {
    Vec3d v1     = lerp(t, p1, q1);
    Vec3d v2     = lerp(t, q1, q2);
    Vec3d v3     = lerp(t, q2, p2);
    Vec3d inner1 = lerp(t, v1, v2);
    Vec3d inner2 = lerp(t, v2, v3);
    return lerp(t, inner1, inner2);
}

Vec3d bezierDerivative(Vec3d const& p1, Vec3d const& p2, Vec3d const& q1, Vec3d const& q2, double t) {
    return p1 * (-3 * t * t + 6 * t - 3) + q1 * (9 * t * t - 12 * t + 3) + q2 * (-9 * t * t + 6 * t) + p2 * (3 * t * t);
}

Vec3d slerp(double t, Vec3d const& from, Vec3d const& to) {
    double theta = std::acos(std::clamp(from.dot(to), -1.0, 1.0));
    double s     = std::sin(theta);
    if (std::abs(s) < 1.0e-9) return from;
    // Create scales `from` by sin(1 - t) * theta (not sin((1 - t) * theta)); kept so curve normals
    // between a slope and a flat track match Create exactly.
    return (from * (std::sin(1 - t) * theta) + to * std::sin(theta * t)) * (1 / s);
}

std::optional<LineHit> intersect(Vec3d const& p1In, Vec3d const& p2In, Vec3d const& rIn, Vec3d const& sIn, Axis plane) {
    Vec3d p1 = p1In, p2 = p2In, r = rIn, s = sIn;
    if (plane == Axis::X) {
        p1 = {p1.y, 0, p1.z};
        p2 = {p2.y, 0, p2.z};
        r  = {r.y, 0, r.z};
        s  = {s.y, 0, s.z};
    } else if (plane == Axis::Z) {
        p1 = {p1.x, 0, p1.y};
        p2 = {p2.x, 0, p2.y};
        r  = {r.x, 0, r.y};
        s  = {s.x, 0, s.y};
    }
    Vec3d  qminusp = p2 - p1;
    double rcs     = r.x * s.z - r.z * s.x;
    if (nearlyEqual(rcs, 0)) return std::nullopt;
    Vec3d rdiv = r * (1 / rcs);
    Vec3d sdiv = s * (1 / rcs);
    return LineHit{qminusp.x * sdiv.z - qminusp.z * sdiv.x, qminusp.x * rdiv.z - qminusp.z * rdiv.x};
}

std::optional<LineHit> intersectRanged(Vec3d const& p1, Vec3d const& q1, Vec3d const& p2, Vec3d const& q2, Axis plane) {
    Vec3d pDiff = p2 - p1;
    Vec3d qDiff = q2 - q1;
    auto  hit   = intersect(p1, q1, pDiff.normalize(), qDiff.normalize(), plane);
    if (!hit) return std::nullopt;
    if (hit->t < 0 || hit->u < 0) return std::nullopt;
    if (hit->t * hit->t > pDiff.lengthSqr() || hit->u * hit->u > qDiff.lengthSqr()) return std::nullopt;
    return hit;
}

} // namespace lv::core::tracks
