#pragma once

// A curved track (Create's BezierConnection): a cubic bezier between two track blocks.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/tracks/Math.h"

namespace lv::core::tracks {

/// The two ends of a curve, in world coordinates. End 0 is the "first" block (where the curve is
/// stored as primary), end 1 the other one.
struct CurveSpec {
    BlockPos3   blocks[2];   ///< track blocks the curve starts from (Create's bePositions)
    Vec3d       starts[2];   ///< curve end points (edge of the track block)
    Vec3d       axes[2];     ///< unit direction leaving each end into the curve
    Vec3d       normals[2];  ///< up normals at each end
    bool        girder  = false; ///< built with metal girders under it
    std::string material = "andesite";
};

/// One sampled point of a curve.
struct CurveSegment {
    int   index = 0;
    Vec3d position;   ///< world position
    Vec3d derivative; ///< unit tangent
    Vec3d faceNormal; ///< up normal
    Vec3d normal;     ///< sideways unit vector (faceNormal × derivative)
};

class Curve {
public:
    explicit Curve(CurveSpec spec, bool primary = true);

    [[nodiscard]] CurveSpec const& spec() const { return mSpec; }
    [[nodiscard]] bool             primary() const { return mPrimary; }
    [[nodiscard]] bool             girder() const { return mSpec.girder; }
    /// The same curve seen from the other end.
    [[nodiscard]] Curve secondary() const;
    /// Block at end `i` (0 or 1).
    [[nodiscard]] BlockPos3 const& block(int i) const { return mSpec.blocks[i]; }
    [[nodiscard]] BlockPos3 const& key() const { return mSpec.blocks[1]; }

    [[nodiscard]] double length() const { return mLength; }
    [[nodiscard]] int    segmentCount() const { return mSegments; }
    [[nodiscard]] double radius() const { return mRadius; }
    [[nodiscard]] double handleLength() const { return mHandleLength; }
    [[nodiscard]] Box const& bounds() const { return mBounds; } ///< inflated by 1.375 as Create

    /// Position at bezier parameter t (0..1).
    [[nodiscard]] Vec3d position(double t) const;
    /// Unit tangent at t.
    [[nodiscard]] Vec3d tangent(double t) const;
    /// Up normal at t (as Create's getNormal).
    [[nodiscard]] Vec3d normal(double t) const;
    /// Parameter t of segment `index` (arc-length corrected through the step LUT).
    [[nodiscard]] double segmentT(int index) const;
    /// The parameter reached when moving `distance` blocks from `t` (Create's incrementT).
    [[nodiscard]] double incrementT(double t, double distance) const;
    /// Parameter at an arc-length distance from end 0 (0..length), through the step LUT.
    [[nodiscard]] double tAtDistance(double distance) const;
    /// Segments 0..segmentCount (inclusive), positions in world coordinates.
    [[nodiscard]] std::vector<CurveSegment> segments() const;

    /// Track items it takes (and refunds): (segments + 1) / 2.
    [[nodiscard]] int trackItemCost() const { return (mSegments + 1) / 2; }
    /// Metal girders it takes: twice the track cost when built with girders.
    [[nodiscard]] int girderItemCost() const { return mSpec.girder ? trackItemCost() * 2 : 0; }

    /// The block columns the curve passes through: (x, z) → lowest y of the rail middle there
    /// (Create's `rasterise`, used to place the invisible fake track blocks).
    [[nodiscard]] std::map<std::pair<int, int>, double> rasterise() const;
    /// The blocks (fake tracks) a curve occupies: for each rasterised column the block at
    /// floor(y) + 1, excluding the two end blocks.
    [[nodiscard]] std::vector<BlockPos3> occupiedBlocks() const;

    /// True when both describe the same curve (either direction), ignoring the material.
    [[nodiscard]] bool sameShape(Curve const& other) const;

    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] static std::optional<Curve> fromJson(nlohmann::json const& j);

private:
    void resolve();

    CurveSpec mSpec;
    bool      mPrimary;

    Vec3d              mFinish1, mFinish2;
    double             mLength       = 0;
    double             mRadius       = 0;
    double             mHandleLength = 0;
    int                mSegments     = 0;
    std::vector<float> mStepLut;
    Box                mBounds;
};

} // namespace lv::core::tracks
