#pragma once

// Train track block shapes (Create's TrackShape), their axes and placement rules.

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "core/tracks/Math.h"

namespace lv::core::tracks {

/// The `levitate:shape` block state value of `levitate:track` (same order as Create's TrackShape).
enum class Shape : int {
    None   = 0,
    ZO     = 1,  ///< straight north-south
    XO     = 2,  ///< straight east-west
    PD     = 3,  ///< diagonal (+x+z)
    ND     = 4,  ///< diagonal (-x+z)
    AN     = 5,  ///< ascending towards north
    AS     = 6,  ///< ascending towards south
    AE     = 7,  ///< ascending towards east
    AW     = 8,  ///< ascending towards west
    TN     = 9,  ///< portal track facing north
    TS     = 10, ///< portal track facing south
    TE     = 11, ///< portal track facing east
    TW     = 12, ///< portal track facing west
    CR_O   = 13, ///< crossing of ZO and XO
    CR_D   = 14, ///< crossing of PD and ND
    CR_PDX = 15, ///< crossing of XO and PD
    CR_PDZ = 16, ///< crossing of ZO and PD
    CR_NDX = 17, ///< crossing of XO and ND
    CR_NDZ = 18, ///< crossing of ZO and ND
};
inline constexpr int kShapeCount = 19;

/// Java horizontal directions in Levitate's 2D order (0 S, 1 W, 2 N, 3 E), see game::playerFacing.
enum class Dir2 : int { South = 0, West = 1, North = 2, East = 3 };
[[nodiscard]] Vec3d dirVec(Dir2 d);
[[nodiscard]] Dir2  dirOpposite(Dir2 d);

[[nodiscard]] std::string_view shapeName(Shape s); ///< Create's serialized name ("zo", "cr_pdx", ...)
[[nodiscard]] std::optional<Shape> shapeFromInt(int v);

/// Track axes (1 for simple shapes, 2 for crossings). Not normalized (diagonals are (±1,0,1)).
[[nodiscard]] std::vector<Vec3d> const& shapeAxes(Shape s);
/// Up normal (normalized; ascending shapes tilt it).
[[nodiscard]] Vec3d shapeNormal(Shape s);
[[nodiscard]] bool  isJunction(Shape s);
[[nodiscard]] bool  isPortal(Shape s);
[[nodiscard]] bool  isSlope(Shape s);
/// Portal shape facing `d` (TN, TS, TE, TW).
[[nodiscard]] Shape asPortal(Dir2 d);
/// Direction the portal shape faces (towards the portal).
[[nodiscard]] Dir2 portalFacing(Shape s);

/// Shape after rotating the block clockwise by 90° `quarterTurns` times.
[[nodiscard]] Shape rotateShape(Shape s, int quarterTurns);
/// Mirror front-back (x → -x) or left-right (z → -z).
[[nodiscard]] Shape mirrorShape(Shape s, bool frontBack);

/// Combines an existing track with one placed onto it (crossings). Returns `existing` if the
/// shapes don't combine.
[[nodiscard]] Shape overlayShape(Shape existing, Shape placed);

/// The shape a track gets when placed by a player looking along `look` (any length; the
/// horizontal part is used, falling back to `yawDegrees` when looking straight up/down).
/// `sturdyTop(dx, dz)` tells whether the block at (pos.x+dx, pos.y, pos.z+dz) has a sturdy top
/// face and the block above it has no sturdy bottom: such a neighbour along a straight track
/// turns it into an ascending track towards that neighbour.
[[nodiscard]] Shape placementShape(Vec3d look, float yawDegrees, std::function<bool(int dx, int dz)> const& sturdyTop);

/// `ITrackBlock.getNearestTrackAxis` resolved to the axis pointing along the look direction
/// (what Create uses as the selection / connection axis).
[[nodiscard]] Vec3d nearestTrackAxis(Shape s, Vec3d const& look);
/// Where a curve leaves the block along `axis` (`TrackBlock.getCurveStart`).
[[nodiscard]] Vec3d curveStart(BlockPos3 const& pos, Vec3d const& axis);

} // namespace lv::core::tracks
