#pragma once

// The Ponder scene camera (U30): an orthographic view of the scene, turned around the vertical
// axis (yaw) and tilted down (pitch), as Create's Ponder shows its scenes. Pure math, shared by the
// client renderer (block meshes, outlines, text anchors, picking) and the unit tests.

#include <array>

#include "core/ponder/Scene.h"

namespace lv::core::ponder {

struct Projected {
    float x = 0, y = 0; ///< GUI pixels
    float depth = 0;    ///< towards the viewer is larger
};

struct View {
    float yaw     = 45.f; ///< degrees; 45 = seen from the south-west corner
    float pitch   = 30.f; ///< degrees; looking down
    float scale   = 20.f; ///< GUI pixels per block
    float screenX = 0, screenY = 0; ///< where `center` appears
    Vec3f center;         ///< scene point at the screen position

    /// Default camera for a scene drawn into a `w` x `h` area centred at (cx, cy).
    static View fit(Scene const& scene, float cx, float cy, float w, float h);

    [[nodiscard]] Projected project(Vec3f const& p) const;
    /// The same transform as a column-major 4x4 matrix (scene coordinates -> GUI x, y, depth scaled
    /// by `depthScale`): used as the world matrix of block meshes.
    [[nodiscard]] std::array<float, 16> matrix(float depthScale) const;
    /// Turns the camera by a mouse drag of (dx, dy) GUI pixels (pitch clamped to 0..90).
    void drag(float dx, float dy);
};

} // namespace lv::core::ponder
