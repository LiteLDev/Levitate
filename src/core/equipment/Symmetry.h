#pragma once

// Wand of symmetry mirrors (U31, SPEC §3.6.31), independent of the game. Create's SymmetryMirror,
// PlaneMirror ("Mirror Once"), CrossPlaneMirror ("Rectangular") and TriplePlaneMirror
// ("Octagonal"). Mirroring is horizontal only and happens about the mirror block.

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::core::equipment {

enum class MirrorType : int { Plane = 0, Cross = 1, Triple = 2 };

/// Orientations: Plane: 0 "Along X" (flips X), 1 "Along Z" (flips Z); Cross: 0 orthogonal,
/// 1 diagonal; Triple: 0 horizontal.
struct Mirror {
    MirrorType type        = MirrorType::Plane;
    int        orientation = 0;
    int        x = 0, y = 0, z = 0;
    bool       enabled = false;
    bool       placed  = false; ///< a mirror exists (Create: the wand has a SYMMETRY component)
    int        dimension = 0;

    [[nodiscard]] int dim() const { return dimension; }
    void              setDim(int d) { dimension = d; }

    [[nodiscard]] nlohmann::json toJson() const;
    static Mirror                fromJson(nlohmann::json const& j);
    friend bool                  operator==(Mirror const&, Mirror const&) = default;
};

/// A 2x2 integer transform of horizontal offsets: (dx, dz) -> (a·dx + b·dz, c·dx + d·dz).
struct Mat2 {
    int a = 1, b = 0, c = 0, d = 1;
    [[nodiscard]] std::pair<int, int> apply(int dx, int dz) const { return {a * dx + b * dz, c * dx + d * dz}; }
    /// The transform flips handedness (a mirror rather than a rotation).
    [[nodiscard]] bool reflects() const { return a * d - b * c < 0; }
    friend bool operator==(Mat2 const&, Mat2 const&) = default;
};

/// The copies a mirror makes (without the identity).
[[nodiscard]] std::vector<Mat2> mirrorTransforms(Mirror const& m);

struct MirroredPos {
    int  x, y, z;
    Mat2 transform;
};
/// Every mirrored position of block (x, y, z).
[[nodiscard]] std::vector<MirroredPos> mirrorPositions(Mirror const& m, int x, int y, int z);

/// Horizontal direction as Create's Direction: 0 north (-z), 1 south (+z), 2 west (-x), 3 east (+x).
/// Returns the transformed direction.
[[nodiscard]] int transformHorizontal(Mat2 const& t, int dir);

/// Number of orientations of a mirror type (2, 2, 1).
[[nodiscard]] int orientationCount(MirrorType t);

/// Create's placement: a new mirror is a plane; players facing north or south (|yaw| < 45 or
/// > 135) get the XY plane ("Along Z"), others "Along X". Cross mirrors moved by a click become
/// diagonal when |yaw mod 90| is in 22..67. `yaw` in degrees (Bedrock: 0 = south).
[[nodiscard]] int planeOrientationFor(float yaw);
[[nodiscard]] int crossOrientationFor(float yaw);

/// Translation keys of a mirror type / orientation (Create's lang keys).
[[nodiscard]] std::string mirrorTypeKey(MirrorType t);
[[nodiscard]] std::string orientationKey(MirrorType t, int orientation);

} // namespace lv::core::equipment
