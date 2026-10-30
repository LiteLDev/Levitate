#include "core/tracks/Tilt.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lv::core::tracks {

namespace {

/// TrackNodeLocation(v).getLocation() without a pixel offset: x / z on the half-block grid, y on
/// the block.
Vec3d nodeLocation(Vec3d const& v) {
    return {std::floor(v.x * 2 + 0.5) / 2.0, std::floor(v.y), std::floor(v.z * 2 + 0.5) / 2.0};
}

double toRad(double deg) { return deg / 180.0 * std::numbers::pi; }

/// Create's VecHelper.rotate (about the origin; float angle as Java).
Vec3d rotate(Vec3d const& v, double deg, Axis axis) {
    if (deg == 0) return v;
    float  angle = float(toRad(deg));
    double sin   = std::sin(angle);
    double cos   = std::cos(angle);
    switch (axis) {
    case Axis::X: return {v.x, v.y * cos - v.z * sin, v.z * cos + v.y * sin};
    case Axis::Y: return {v.x * cos + v.z * sin, v.y, v.z * cos - v.x * sin};
    case Axis::Z: return {v.x * cos - v.y * sin, v.y * cos + v.x * sin, v.z};
    }
    return v;
}

Vec3d rotateCentered(Vec3d const& v, double deg, Axis axis) {
    Vec3d const c{.5, .5, .5};
    return rotate(v - c, deg, axis) + c;
}

} // namespace

bool compareHandles(Vec3d const& a, Vec3d const& b) {
    Vec3d la = nodeLocation(a).multiply(1, 0, 1);
    Vec3d lb = nodeLocation(b).multiply(1, 0, 1);
    return la.distanceToSqr(lb) < 1 / 512.0;
}

std::optional<Smoothing>
trySmoothing(BlockPos3 const& pos, Shape shape, bool portalBound, std::vector<std::shared_ptr<Curve const>> const& curves) {
    auto const& axes = shapeAxes(shape);
    if (axes.size() != 1) return std::nullopt;
    if (axes[0].y != 0) return std::nullopt;
    if (portalBound || isPortal(shape)) return std::nullopt;
    if (curves.size() != 2) return std::nullopt;

    std::shared_ptr<Curve const> first, second;
    std::optional<Vec3d>         axis;
    for (auto const& c : curves) {
        if (!c || !(c->block(0) == pos)) return std::nullopt;
        auto const& sp = c->spec();
        if (sp.starts[0].y == sp.starts[1].y) continue;
        Vec3d normedAxis = sp.axes[0].normalize();
        if (axis) {
            if (second) return std::nullopt;
            if (normedAxis.dot(*axis) > -1 + 1 / 64.0) return std::nullopt;
            second = c;
            continue;
        }
        axis  = normedAxis;
        first = c;
    }
    if (!first || !second) return std::nullopt;
    if (first->spec().starts[1].y > second->spec().starts[1].y) std::swap(first, second);

    CurveSpec low  = first->spec();
    CurveSpec high = second->spec();
    Vec3d     lowestPoint  = low.starts[1];
    Vec3d     highestPoint = high.starts[1];
    if (lowestPoint.y > low.starts[0].y) return std::nullopt;
    if (highestPoint.y < high.starts[0].y) return std::nullopt;

    double hDistance      = first->length() + second->length();
    Vec3d  baseAxis       = low.axes[0];
    double baseAxisLength = baseAxis.x != 0 && baseAxis.z != 0 ? std::sqrt(2.0) : 1;
    double vDistance      = highestPoint.y - lowestPoint.y;
    double m              = vDistance / hDistance;

    Vec3d diff         = high.starts[0] - low.starts[0];
    bool  flipRotation = diff.dot(Vec3d{1, 0, 2}.normalize()) <= 0;

    Smoothing out{
        std::atan2(m, 1) * 180.0 / std::numbers::pi * (flipRotation ? -1 : 1),
        *first,
        *second,
    };

    int   smoothingParam = std::clamp(int(m * baseAxisLength * 16), 0, 15);
    Vec3d raisedOffset   = (diff.normalize() + Vec3d{0, std::clamp(m, 0.0, 1 - 1 / 512.0), 0}).normalize() * baseAxisLength;
    high.starts[0]       = low.starts[0] + raisedOffset;

    low.smoothing[0]  = 0;
    high.smoothing[0] = smoothingParam;
    low.axes[0]       = (low.axes[0] + Vec3d{0, -m, 0}).normalize();
    high.axes[0]      = (high.axes[0] + Vec3d{0, m, 0}).normalize();

    out.low  = Curve(std::move(low), first->primary());
    out.high = Curve(std::move(high), second->primary());
    return out;
}

Curve restoreToOriginalCurve(Curve const& curve) {
    CurveSpec s    = curve.spec();
    s.smoothing[0] = 0;
    s.starts[0]    = nodeLocation(s.starts[0]);
    s.axes[0]      = s.axes[0].multiply(1, 0, 1).normalize();
    return Curve(std::move(s), curve.primary());
}

int yOffsetForAxisEnd(std::vector<std::shared_ptr<Curve const>> const& curves, Vec3d const& end) {
    for (auto const& c : curves)
        if (c && compareHandles(c->spec().starts[0], end)) return c->yOffsetAt(end);
    return 0;
}

Vec3d tiltModelPoint(Vec3d const& vIn, Shape shape, double angleIn) {
    double angle = std::abs(angleIn);
    bool   flip  = angleIn < 0;
    double hAngle = 0;
    switch (shape) {
    case Shape::XO: hAngle = 0; break;
    case Shape::PD: hAngle = 45; break;
    case Shape::ZO: hAngle = 90; break;
    case Shape::ND: hAngle = 135; break;
    default: break;
    }
    Vec3d const verticalOffset{0, -0.25, 0};
    Vec3d const diagonalRotationPoint =
        (shape == Shape::ND || shape == Shape::PD) ? Vec3d{(std::sqrt(2.0) - 1) / 2, 0, 0} : Vec3d{0, 0, 0};
    Vec3d v = vIn + verticalOffset;
    v       = rotateCentered(v, hAngle, Axis::Y);
    v       = v + diagonalRotationPoint;
    v       = rotate(v, angle, Axis::Z);
    v       = v - diagonalRotationPoint;
    v       = rotateCentered(v, -hAngle + (flip ? 180 : 0), Axis::Y);
    v       = v - verticalOffset;
    return v;
}

} // namespace lv::core::tracks
