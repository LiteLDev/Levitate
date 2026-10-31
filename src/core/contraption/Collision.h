#pragma once

// Entity collision with a moving structure (pure C++).
//
// The structure's collision boxes live in local coordinates (blocks relative to the anchor).
// An entity box is brought into local space (its extents grow to the bounding box of the rotated
// box when the structure is not at a quarter turn), pushed out along the axis of least
// penetration (upwards preferred for small steps, so entities end up standing on top) and the
// push is turned back into world space.

#include <vector>

#include "core/contraption/Geometry.h"

namespace lv::core::contraption {

struct CollisionResult {
    bool  hit   = false; ///< the entity overlapped the structure
    Vec3d push;          ///< world-space displacement that resolves the overlap
    bool  onTop = false; ///< resolved upwards: the entity stands on the structure
};

/// Resolves `entity` (world box) against the structure's `localBoxes` at `transform`.
/// `stepHeight`: overlaps up to this height are resolved upwards (players step onto blocks).
[[nodiscard]] CollisionResult
collide(Transform const& transform, std::vector<Box> const& localBoxes, Box const& entity, double stepHeight = 0.6);

/// True when a point just below `feet` (world) is inside a structure box: the entity stands on it.
[[nodiscard]] bool standsOn(Transform const& transform, std::vector<Box> const& localBoxes, Vec3d const& feet, double probe = 0.08);

/// World boxes for the game's axis-aligned collision of a local box at `transform` (Create's
/// ContraptionCollider uses the oriented box). At a quarter turn: its bounding box. Turned by
/// another angle the box is cut into halves along the turned axes and each part gives its bounding
/// box, so a 45 degree plank reaches about 0.1 block past its corners instead of 0.2.
[[nodiscard]] std::vector<Box> worldBoxes(Transform const& transform, Box const& local);

/// Where a carried point moves when the structure goes from `before` to `after`.
[[nodiscard]] inline Vec3d carry(Transform const& before, Transform const& after, Vec3d const& world) {
    return after.toWorld(before.toLocal(world));
}

} // namespace lv::core::contraption
