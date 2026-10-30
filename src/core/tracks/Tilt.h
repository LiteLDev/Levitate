#pragma once

// Tilted tracks (Create's TrackBlockEntityTilt), pure maths.
//
// A flat straight track with exactly two curves leaving it on opposite sides, one going down and
// one going up (two sloped curves meeting at it), is tilted: both curves are re-shaped so the
// track block becomes a short ramp between them. The low curve keeps its start and leaves the
// block along the ramp (its axis gets -m vertically), the high curve starts at the raised end of
// the ramp (axis +m), with m = (height of the high far end - height of the low far end) / (sum of
// the two curve lengths). The graph node at the raised end is lifted by `smoothing` pixels
// (Curve::yOffsetAt), the block model turns by `angle` degrees (Create's TrackModel). When one of
// the curves goes, the other one is restored to its original shape.

#include <memory>
#include <optional>
#include <vector>

#include "core/tracks/Curve.h"
#include "core/tracks/Shape.h"

namespace lv::core::tracks {

/// Create's TrackBlockEntityTilt.compareHandles: same node location ignoring height.
[[nodiscard]] bool compareHandles(Vec3d const& a, Vec3d const& b);

struct Smoothing {
    double angle = 0; ///< Create's smoothingAngle (degrees, signed: the model's turn)
    Curve  low;       ///< the re-shaped curves, oriented from the tilted block
    Curve  high;
};

/// Create's tryApplySmoothing for the track at `pos` (`shape`, bound to a portal or not) with the
/// curves leaving it (each oriented from `pos`). nullopt when it doesn't tilt. Unlike Create, a
/// block with more than the two sloped curves is left alone (Create would drop the extra one).
[[nodiscard]] std::optional<Smoothing>
trySmoothing(BlockPos3 const& pos, Shape shape, bool portalBound, std::vector<std::shared_ptr<Curve const>> const& curves);

/// Create's restoreToOriginalCurve: end 0 of `curve` back on its flat start (no smoothing there,
/// horizontal axis).
[[nodiscard]] Curve restoreToOriginalCurve(Curve const& curve);

/// Create's getYOffsetForAxisEnd of a tilted track: the pixels the straight end at `end` is
/// raised by (the smoothing of the curve starting there; curves oriented from the block).
[[nodiscard]] int yOffsetForAxisEnd(std::vector<std::shared_ptr<Curve const>> const& curves, Vec3d const& end);

/// Create's TrackModel transform of a tilted track block: a point of the block model (block
/// units, 0..1, the block's corner at the origin) moved onto the ramp. `shape` is the block's
/// flat shape (XO, ZO, PD, ND), `angle` the smoothing angle.
[[nodiscard]] Vec3d tiltModelPoint(Vec3d const& v, Shape shape, double angle);

} // namespace lv::core::tracks
