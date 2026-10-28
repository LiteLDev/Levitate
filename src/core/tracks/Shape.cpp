#include "core/tracks/Shape.h"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace lv::core::tracks {

namespace {

struct ShapeInfo {
    std::string_view   name;
    std::vector<Vec3d> axes;
    Vec3d              normal;
};

std::array<ShapeInfo, kShapeCount> const& infos() {
    static std::array<ShapeInfo, kShapeCount> const table = [] {
        double const s = 1 / std::numbers::sqrt2;
        Vec3d const  up{0, 1, 0};
        return std::array<ShapeInfo, kShapeCount>{{
            {"none", {Vec3d{0, 0, 0}}, up},
            {"zo", {Vec3d{0, 0, 1}}, up},
            {"xo", {Vec3d{1, 0, 0}}, up},
            {"pd", {Vec3d{1, 0, 1}}, up},
            {"nd", {Vec3d{-1, 0, 1}}, up},
            {"an", {Vec3d{0, 1, -1}}, Vec3d{0, s, s}},
            {"as", {Vec3d{0, 1, 1}}, Vec3d{0, s, -s}},
            {"ae", {Vec3d{1, 1, 0}}, Vec3d{-s, s, 0}},
            {"aw", {Vec3d{-1, 1, 0}}, Vec3d{s, s, 0}},
            {"tn", {Vec3d{0, 0, -1}}, up},
            {"ts", {Vec3d{0, 0, 1}}, up},
            {"te", {Vec3d{1, 0, 0}}, up},
            {"tw", {Vec3d{-1, 0, 0}}, up},
            {"cr_o", {Vec3d{0, 0, 1}, Vec3d{1, 0, 0}}, up},
            {"cr_d", {Vec3d{1, 0, 1}, Vec3d{-1, 0, 1}}, up},
            {"cr_pdx", {Vec3d{1, 0, 0}, Vec3d{1, 0, 1}}, up},
            {"cr_pdz", {Vec3d{0, 0, 1}, Vec3d{1, 0, 1}}, up},
            {"cr_ndx", {Vec3d{1, 0, 0}, Vec3d{-1, 0, 1}}, up},
            {"cr_ndz", {Vec3d{0, 0, 1}, Vec3d{-1, 0, 1}}, up},
        }};
    }();
    return table;
}

ShapeInfo const& info(Shape s) { return infos()[static_cast<size_t>(s)]; }

Shape clockwise(Shape s) {
    switch (s) {
    case Shape::PD:
        return Shape::ND;
    case Shape::ND:
        return Shape::PD;
    case Shape::XO:
        return Shape::ZO;
    case Shape::ZO:
        return Shape::XO;
    case Shape::AE:
        return Shape::AS;
    case Shape::AS:
        return Shape::AW;
    case Shape::AW:
        return Shape::AN;
    case Shape::AN:
        return Shape::AE;
    case Shape::CR_PDX:
        return Shape::CR_NDZ;
    case Shape::CR_NDX:
        return Shape::CR_PDZ;
    case Shape::CR_PDZ:
        return Shape::CR_NDX;
    case Shape::CR_NDZ:
        return Shape::CR_PDX;
    default:
        return s;
    }
}

} // namespace

Vec3d dirVec(Dir2 d) {
    switch (d) {
    case Dir2::South:
        return {0, 0, 1};
    case Dir2::West:
        return {-1, 0, 0};
    case Dir2::North:
        return {0, 0, -1};
    default:
        return {1, 0, 0};
    }
}

Dir2 dirOpposite(Dir2 d) { return static_cast<Dir2>((static_cast<int>(d) + 2) & 3); }

std::string_view shapeName(Shape s) { return info(s).name; }

std::optional<Shape> shapeFromInt(int v) {
    if (v < 0 || v >= kShapeCount) return std::nullopt;
    return static_cast<Shape>(v);
}

std::vector<Vec3d> const& shapeAxes(Shape s) { return info(s).axes; }
Vec3d                     shapeNormal(Shape s) { return info(s).normal; }
bool                      isJunction(Shape s) { return info(s).axes.size() > 1; }
bool isPortal(Shape s) { return s == Shape::TN || s == Shape::TS || s == Shape::TE || s == Shape::TW; }
bool isSlope(Shape s) { return info(s).axes[0].y != 0; }

Shape asPortal(Dir2 d) {
    switch (d) {
    case Dir2::East:
        return Shape::TE;
    case Dir2::North:
        return Shape::TN;
    case Dir2::South:
        return Shape::TS;
    default:
        return Shape::TW;
    }
}

Dir2 portalFacing(Shape s) {
    switch (s) {
    case Shape::TE:
        return Dir2::East;
    case Shape::TN:
        return Dir2::North;
    case Shape::TS:
        return Dir2::South;
    default:
        return Dir2::West;
    }
}

Shape rotateShape(Shape s, int quarterTurns) {
    for (int i = 0; i < (quarterTurns & 3); ++i) s = clockwise(s);
    return s;
}

Shape mirrorShape(Shape s, bool frontBack) {
    switch (s) {
    case Shape::PD:
        return Shape::ND;
    case Shape::ND:
        return Shape::PD;
    case Shape::CR_PDX:
        return Shape::CR_NDX;
    case Shape::CR_NDX:
        return Shape::CR_PDX;
    case Shape::CR_PDZ:
        return Shape::CR_NDZ;
    case Shape::CR_NDZ:
        return Shape::CR_PDZ;
    case Shape::AE:
        return frontBack ? Shape::AW : s;
    case Shape::AW:
        return frontBack ? Shape::AE : s;
    case Shape::AN:
        return frontBack ? s : Shape::AS;
    case Shape::AS:
        return frontBack ? s : Shape::AN;
    default:
        return s;
    }
}

Shape overlayShape(Shape existing, Shape placed) {
    std::optional<Shape> combined;
    for (bool flip : {true, false}) {
        Shape s1 = flip ? existing : placed;
        Shape s2 = flip ? placed : existing;
        if (s1 == Shape::XO && s2 == Shape::ZO) combined = Shape::CR_O;
        if (s1 == Shape::PD && s2 == Shape::ND) combined = Shape::CR_D;
        if (s1 == Shape::XO && s2 == Shape::PD) combined = Shape::CR_PDX;
        if (s1 == Shape::ZO && s2 == Shape::PD) combined = Shape::CR_PDZ;
        if (s1 == Shape::XO && s2 == Shape::ND) combined = Shape::CR_NDX;
        if (s1 == Shape::ZO && s2 == Shape::ND) combined = Shape::CR_NDZ;
    }
    return combined.value_or(existing);
}

Shape placementShape(Vec3d look, float yawDegrees, std::function<bool(int, int)> const& sturdyTop) {
    look = look.multiply(1, 0, 1);
    if (nearlyEqual(look.length(), 0)) {
        // (0, 0, 1) rotated by -yaw around Y (Create's VecHelper.rotate)
        double a = -yawDegrees / 180.0 * std::numbers::pi;
        look     = Vec3d{std::sin(a), 0, std::cos(a)};
    }
    look = look.normalize();

    Shape  best      = Shape::ZO;
    double bestValue = std::numeric_limits<float>::max();
    for (int i = 0; i < kShapeCount; ++i) {
        auto shape = static_cast<Shape>(i);
        if (isJunction(shape) || isPortal(shape)) continue;
        Vec3d  axis     = shapeAxes(shape)[0];
        double distance = std::min(axis.distanceToSqr(look), (-axis.normalize()).distanceToSqr(look));
        if (distance > bestValue) continue;
        bestValue = distance;
        best      = shape;
    }

    Vec3d bestAxis = shapeAxes(best)[0];
    if (bestAxis.lengthSqr() == 1 && sturdyTop) {
        for (bool neg : {true, false}) {
            BlockPos3 off = containing(bestAxis * (neg ? -1 : 1));
            if (!sturdyTop(off.x, off.z)) continue;
            if (best == Shape::XO) best = neg ? Shape::AW : Shape::AE;
            if (best == Shape::ZO) best = neg ? Shape::AN : Shape::AS;
        }
    }
    return best;
}

Vec3d nearestTrackAxis(Shape s, Vec3d const& look) {
    Vec3d  best     = shapeAxes(s)[0];
    double bestDiff = std::numeric_limits<double>::max();
    for (auto const& v : shapeAxes(s)) {
        for (int opposite : {1, -1}) {
            double d = v.normalize().distanceTo(look * opposite);
            if (d > bestDiff) continue;
            bestDiff = d;
            best     = v;
        }
    }
    bool positive = look.dot(best.multiply(1, 0, 1).normalize()) < 0;
    return best * (positive ? -1 : 1);
}

Vec3d curveStart(BlockPos3 const& pos, Vec3d const& axis) {
    bool vertical = axis.y != 0;
    return pos.center() + Vec3d{0, vertical ? 0 : -.5, 0} + axis * .5;
}

} // namespace lv::core::tracks
