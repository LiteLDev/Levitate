#include "core/tracks/Placement.h"

#include <cmath>
#include <numbers>

namespace lv::core::tracks {

namespace {
nlohmann::json vecJson(Vec3d const& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
Vec3d          jsonVec(nlohmann::json const& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }

PlacementPlan& fail(PlacementPlan& p, char const* key, bool tooJumbly = false) {
    p.valid   = false;
    p.message = std::string("create.track.") + key;
    if (tooJumbly) p.curve.reset();
    return p;
}

float deg(double rad) { return float(rad * 180.0 / std::numbers::pi); }
} // namespace

nlohmann::json TrackEnd::toJson() const {
    return {
        {"pos",    nlohmann::json::array({pos.x, pos.y, pos.z})},
        {"axis",   vecJson(axis)                                },
        {"normal", vecJson(normal)                              },
        {"end",    vecJson(end)                                 },
    };
}

std::optional<TrackEnd> TrackEnd::fromJson(nlohmann::json const& j) {
    try {
        TrackEnd e;
        auto     p = j.at("pos");
        e.pos      = {p.at(0).get<int>(), p.at(1).get<int>(), p.at(2).get<int>()};
        e.axis     = jsonVec(j.at("axis"));
        e.normal   = jsonVec(j.at("normal"));
        e.end      = jsonVec(j.at("end"));
        return e;
    } catch (...) {
        return std::nullopt;
    }
}

TrackEnd selectEnd(BlockPos3 const& pos, Shape shape, Vec3d const& look) {
    TrackEnd e;
    e.pos    = pos;
    e.axis   = nearestTrackAxis(shape, look);
    e.end    = curveStart(pos, e.axis);
    e.normal = shapeNormal(shape).normalize();
    return e;
}

Shape runShape(Shape s) {
    switch (s) {
    case Shape::TE:
    case Shape::TW:
        return Shape::XO;
    case Shape::TN:
    case Shape::TS:
        return Shape::ZO;
    default:
        return s;
    }
}

PlacementPlan planConnection(PlacementRequest const& req, Shape shape1) {
    PlacementPlan info;
    Shape const   shape2 = req.shape2;

    Vec3d axis2       = nearestTrackAxis(shape2, req.look);
    Vec3d normal2     = shapeNormal(shape2).normalize();
    Vec3d normedAxis2 = axis2.normalize();
    Vec3d end2        = curveStart(req.pos2, axis2);

    BlockPos3 pos1        = req.from.pos;
    BlockPos3 pos2        = req.pos2;
    Vec3d     axis1       = req.from.axis;
    Vec3d     normedAxis1 = axis1.normalize();
    Vec3d     end1        = req.from.end;
    Vec3d     normal1     = req.from.normal;

    info.pos1  = pos1;
    info.pos2  = pos2;
    info.axis1 = axis1;
    info.axis2 = axis2;

    if (pos1 == pos2) return fail(info, "second_point");
    if (pos1.distSqr(pos2) > double(req.maxLength) * req.maxLength) return fail(info, "too_far", true);
    if (!req.fromIsTrack) return fail(info, "original_missing");
    if (req.pos2Tilted) return fail(info, "turn_start");

    if (axis1.dot(end2 - end1) < 0) {
        axis1       = -axis1;
        normedAxis1 = -normedAxis1;
        end1        = curveStart(pos1, axis1);
    }

    auto intersectY = intersect(end1, end2, normedAxis1, normedAxis2, Axis::Y);
    bool parallel   = !intersectY;
    bool skipCurve  = false;

    if ((parallel && normedAxis1.dot(normedAxis2) > 0) || (!parallel && (intersectY->t < 0 || intersectY->u < 0))) {
        axis2       = -axis2;
        normedAxis2 = -normedAxis2;
        end2        = curveStart(pos2, axis2);
    }

    Vec3d  cross2    = normedAxis2.cross(Vec3d{0, 1, 0});
    double a1        = std::atan2(normedAxis2.z, normedAxis2.x);
    double a2        = std::atan2(normedAxis1.z, normedAxis1.x);
    double angle     = a1 - a2;
    double ascend    = (end2 - end1).y;
    double absAscend = std::abs(ascend);
    bool   slope     = !(normal1 == normal2) && !normal1.near(normal2, 1e-9);

    info.axis1 = axis1;
    info.axis2 = axis2;

    // ---- S curve or straight ----
    double dist = 0;
    if (parallel) {
        if (auto sTest = intersect(end1, end2, normedAxis1, cross2, Axis::Y)) {
            double t  = std::abs(sTest->t);
            double u  = std::abs(sTest->u);
            skipCurve = nearlyEqual(u, 0);

            if (!skipCurve && sTest->t < 0) return fail(info, "perpendicular", true);

            if (skipCurve) {
                dist            = pos1.center().distanceTo(pos2.center());
                info.end1Extent = int(std::round((dist + 1) / axis1.length()));
            } else {
                if (!nearlyEqual(ascend, 0) || normedAxis1.y != 0) return fail(info, "ascending_s_curve");
                double targetT = u <= 1 ? 3 : u * 2;
                if (t < targetT) return fail(info, "too_sharp");
                // standardise S-curve sizes
                if (t > targetT) {
                    int correction  = int((t - targetT) / axis1.length());
                    info.end1Extent = req.maximiseTurn ? 0 : correction / 2 + (correction % 2);
                    info.end2Extent = req.maximiseTurn ? 0 : correction / 2;
                }
            }
        }
    }

    // ---- slope ----
    if (slope) {
        if (!skipCurve) return fail(info, "slope_turn");
        if (nearlyEqual(normal1.dot(normal2), 0)) return fail(info, "opposing_slopes");
        if ((axis1.y < 0 || axis2.y > 0) && ascend > 0) return fail(info, "leave_slope_ascending");
        if ((axis1.y > 0 || axis2.y < 0) && ascend < 0) return fail(info, "leave_slope_descending");

        skipCurve       = false;
        info.end1Extent = 0;
        info.end2Extent = 0;

        Axis plane = nearlyEqual(axis1.x, 0) ? Axis::X : Axis::Z;
        auto hit   = intersect(end1, end2, normedAxis1, normedAxis2, plane);
        if (!hit) return fail(info, "too_sharp", true);
        double dist1 = std::abs(hit->t / axis1.length());
        double dist2 = std::abs(hit->u / axis2.length());
        if (dist1 > dist2) info.end1Extent = int(std::round(dist1 - dist2));
        if (dist2 > dist1) info.end2Extent = int(std::round(dist2 - dist1));

        double turnSize = std::min(dist1, dist2);
        if (hit->t < 0 || hit->u < 0) return fail(info, "too_sharp", true);
        if (turnSize < 2) return fail(info, "too_sharp");

        // standardise curve sizes (Java's int += double narrows)
        if (turnSize > 2 && !req.maximiseTurn) {
            info.end1Extent = int(info.end1Extent + turnSize - 2);
            info.end2Extent = int(info.end2Extent + turnSize - 2);
        }
    }

    // ---- straight ascend ----
    if (skipCurve && !nearlyEqual(ascend, 0)) {
        int hDistance = info.end1Extent;
        if (axis1.y == 0 || !nearlyEqual(absAscend + 1, dist / axis1.length())) {
            if (axis1.y != 0 && axis1.y == -axis2.y) return fail(info, "ascending_s_curve");

            info.end1Extent     = 0;
            double minHDistance = std::max(absAscend < 4 ? absAscend * 4 : absAscend * 3, 6.0) / axis1.length();
            if (hDistance < minHDistance) return fail(info, "too_steep");
            if (hDistance > minHDistance) {
                int correction  = int(hDistance - minHDistance);
                info.end1Extent = req.maximiseTurn ? 0 : correction / 2 + (correction % 2);
                info.end2Extent = req.maximiseTurn ? 0 : correction / 2;
            }
            skipCurve = false;
        }
    }

    // ---- turn ----
    if (!parallel) {
        float absAngle = std::abs(deg(angle));
        if (absAngle < 60 || absAngle > 300) return fail(info, "turn_90", true);

        auto   hit   = intersect(end1, end2, normedAxis1, normedAxis2, Axis::Y);
        double dist1 = std::abs(hit->t);
        double dist2 = std::abs(hit->u);
        float  ex1   = 0;
        float  ex2   = 0;
        if (dist1 > dist2) ex1 = float((dist1 - dist2) / axis1.length());
        if (dist2 > dist1) ex2 = float((dist2 - dist1) / axis2.length());

        double turnSize = std::min(dist1, dist2) - .1;
        bool   ninety   = std::fmod(absAngle + .25f, 90.0f) < 1;

        if (hit->t < 0 || hit->u < 0) return fail(info, "too_sharp", true);

        double minTurnSize = ninety ? 7 : 3.25;
        double turnSizeToFitAscend =
            minTurnSize + (ninety ? std::max(0.0, absAscend - 3) * 2.0 : std::max(0.0, absAscend - 1.5) * 1.5);

        if (turnSize < minTurnSize) return fail(info, "too_sharp");
        if (turnSize < turnSizeToFitAscend) return fail(info, "too_steep");

        // standardise curve sizes
        if (!req.maximiseTurn) {
            ex1 += float((turnSize - turnSizeToFitAscend) / axis1.length());
            ex2 += float((turnSize - turnSizeToFitAscend) / axis2.length());
        }
        info.end1Extent = int(std::floor(ex1));
        info.end2Extent = int(std::floor(ex2));
    }

    Vec3d offset1 = axis1 * info.end1Extent;
    Vec3d offset2 = axis2 * info.end2Extent;
    info.target1  = pos1 + containing(offset1);
    info.target2  = pos2 + containing(offset2);

    if (!skipCurve) {
        CurveSpec spec;
        spec.blocks[0]  = info.target1;
        spec.blocks[1]  = info.target2;
        spec.starts[0]  = end1 + offset1;
        spec.starts[1]  = end2 + offset2;
        spec.axes[0]    = normedAxis1;
        spec.axes[1]    = normedAxis2;
        spec.normals[0] = normal1;
        spec.normals[1] = normal2;
        spec.girder     = req.girder;
        spec.material   = req.material;
        info.curve.emplace(std::move(spec), true);
    }

    info.valid = true;

    // straight runs (Create's placeTracks): blocks i = 0 .. extent (inclusive when a curve follows)
    for (bool first : {true, false}) {
        int       extent = first ? info.end1Extent : info.end2Extent;
        Vec3d     axis   = first ? axis1 : axis2;
        BlockPos3 pos    = first ? pos1 : pos2;
        Shape     shape  = runShape(first ? shape1 : shape2);
        int       count  = info.curve ? extent + 1 : extent;
        for (int i = 0; i < count; ++i) info.straight.push_back({pos + containing(axis * i), shape, first});
    }
    return info;
}

} // namespace lv::core::tracks
