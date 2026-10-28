#include "core/contraption/Collision.h"

#include <algorithm>
#include <cmath>

namespace lv::core::contraption {

namespace {

/// Half extents of a world box seen in local space (bounding box of the rotated box).
Vec3d localHalfExtents(Transform const& t, Vec3d const& half) {
    Vec3d ex = t.unrotateVector({half.x, 0, 0});
    Vec3d ey = t.unrotateVector({0, half.y, 0});
    Vec3d ez = t.unrotateVector({0, 0, half.z});
    return {
        std::abs(ex.x) + std::abs(ey.x) + std::abs(ez.x),
        std::abs(ex.y) + std::abs(ey.y) + std::abs(ez.y),
        std::abs(ex.z) + std::abs(ey.z) + std::abs(ez.z),
    };
}

} // namespace

CollisionResult collide(Transform const& t, std::vector<Box> const& boxes, Box const& entity, double stepHeight) {
    CollisionResult r;
    Vec3d           half   = entity.size() * 0.5;
    Vec3d           center = t.toLocal(entity.center());
    Vec3d           lh     = localHalfExtents(t, half);
    Vec3d           worldUpLocal = t.unrotateVector({0, 1, 0});
    Vec3d           total;
    for (int iteration = 0; iteration < 4; ++iteration) {
        Box  e{center - lh, center + lh};
        bool any = false;
        for (auto const& b : boxes) {
            if (!e.intersects(b)) continue;
            any = true;
            // penetration needed along each local axis (signed)
            double best = 1e9;
            Vec3d  move;
            for (int a = 0; a < 3; ++a) {
                auto   axis = static_cast<Axis>(a);
                double pos  = b.max.get(axis) - e.min.get(axis); // push towards +
                double neg  = e.max.get(axis) - b.min.get(axis); // push towards -
                // prefer "up" in world space for small steps
                double upBias = worldUpLocal.get(axis);
                double cost   = pos;
                if (upBias > 0.5 && pos <= stepHeight) cost = pos * 0.25;
                if (cost < best) {
                    best = cost;
                    move = {};
                    move.set(axis, pos);
                }
                cost = neg;
                if (upBias < -0.5 && neg <= stepHeight) cost = neg * 0.25;
                if (cost < best) {
                    best = cost;
                    move = {};
                    move.set(axis, -neg);
                }
            }
            center += move;
            total += move;
            e = Box{center - lh, center + lh};
        }
        if (!any) break;
        r.hit = true;
    }
    if (r.hit) {
        r.push  = t.rotateVector(total);
        r.onTop = r.push.y > 1e-6 && std::abs(r.push.y) >= std::abs(r.push.x) && std::abs(r.push.y) >= std::abs(r.push.z);
    }
    return r;
}

bool standsOn(Transform const& t, std::vector<Box> const& boxes, Vec3d const& feet, double probe) {
    Vec3d p = t.toLocal(feet - Vec3d{0, probe, 0});
    return std::any_of(boxes.begin(), boxes.end(), [&](Box const& b) { return b.contains(p); });
}

} // namespace lv::core::contraption
