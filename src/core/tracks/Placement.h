#pragma once

// Connecting two track ends with the track item (Create's TrackPlacement.tryConnect), as a pure
// function: the caller supplies the selected end, the target track and the player's look; the
// result says whether the connection is valid (or why not: a `create.track.*` message key), which
// straight blocks to place and the curve, if any.

#include <optional>
#include <string>
#include <vector>

#include "core/tracks/Curve.h"
#include "core/tracks/Shape.h"

namespace lv::core::tracks {

/// The end a player selected with the track item (Create's ConnectingFrom).
struct TrackEnd {
    BlockPos3 pos;
    Vec3d     axis;   ///< nearest track axis, pointing along the player's look
    Vec3d     normal; ///< up normal of the track
    Vec3d     end;    ///< curve start (edge of the block along `axis`)

    [[nodiscard]] nlohmann::json         toJson() const;
    [[nodiscard]] static std::optional<TrackEnd> fromJson(nlohmann::json const& j);
};

/// Selects the end of track `shape` at `pos` nearest to the look direction.
[[nodiscard]] TrackEnd selectEnd(BlockPos3 const& pos, Shape shape, Vec3d const& look);

struct PlacementRequest {
    TrackEnd  from;                 ///< the selected first end
    bool      fromIsTrack = true;   ///< the block at `from.pos` is still a track
    BlockPos3 pos2;                 ///< the clicked track, or where a new track will be placed
    Shape     shape2 = Shape::ZO;   ///< its (existing or placement) shape
    bool      pos2Tilted = false;   ///< target is a tilted track (smoothing between slopes)
    Vec3d     look{0, 0, 1};        ///< player look vector
    int       maxLength    = 32;    ///< `maxTrackPlacementLength`
    bool      maximiseTurn = false; ///< sprint held: biggest possible turn
    bool      girder       = false; ///< metal girder in the off hand
    std::string material   = "andesite";
};

/// One straight track block of a placement.
struct StraightPiece {
    BlockPos3 pos;
    Shape     shape;   ///< shape to place (overlaid onto an existing track by the caller)
    bool      fromEnd1; ///< belongs to the first end's run
};

struct PlacementPlan {
    bool        valid = false;
    std::string message; ///< "create.track.*" key, empty when nothing to say
    int         end1Extent = 0;
    int         end2Extent = 0;
    Vec3d       axis1, axis2;
    BlockPos3   pos1, pos2;
    BlockPos3   target1, target2;   ///< where the curve starts (after the straight extents)
    std::optional<Curve> curve;     ///< the curve to build (none for a straight run)
    std::vector<StraightPiece> straight; ///< straight blocks to place (both runs)
};

/// Runs Create's placement rules. `shape1` is the shape of the track at `from.pos`.
[[nodiscard]] PlacementPlan planConnection(PlacementRequest const& req, Shape shape1);

/// Straight track shape to place along a run from a track of shape `s` (portal shapes become
/// straight, everything else keeps its shape).
[[nodiscard]] Shape runShape(Shape s);

} // namespace lv::core::tracks
