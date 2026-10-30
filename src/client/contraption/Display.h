#pragma once

// Client mod internals of the contraption unit (not a contract): how a display entity is drawn.

#ifdef LV_CLIENT

#include <string>
#include <utility>
#include <vector>

#include "core/contraption/Geometry.h"

class Actor;

namespace lv::client::contraption {

/// How a display entity is drawn at `partialTick` (0..1), from the contraption motion the server
/// sent. The game draws it on the straight line between its old and new place; `shift` moves it
/// onto the contraption's arc. `axis` / `degrees`: its turn about its block centre (the angle
/// left after the quarter turns the displayed block already shows).
struct DisplayPose {
    int                     axis    = 1;
    float                   degrees = 0;
    core::contraption::Vec3d shift;
    int                     quarter  = 0; ///< quarter turns the displayed block shows (about axis)
    int                     tiltAxis = 0; ///< tilt (applied in the contraption frame before the turn)
    float                   tilt     = 0;
};

/// False when the actor isn't a display of a contraption whose motion is known.
[[nodiscard]] bool displayPose(Actor const& actor, float partialTick, DisplayPose& out);

/// Adds the world boxes (bounding boxes of the turned block boxes) of the known contraption
/// blocks in dimension `dim` that intersect `query`: the local player collides with them.
void appendCollision(int dim, core::contraption::Box const& query, std::vector<std::pair<core::contraption::Box, std::string>>& out);

} // namespace lv::client::contraption

#endif // LV_CLIENT
