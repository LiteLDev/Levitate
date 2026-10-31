#include <gtest/gtest.h>

#include "core/contraption/Collision.h"

using namespace lv::core::contraption;

namespace {
bool covered(std::vector<Box> const& boxes, Vec3d const& p) {
    for (auto const& b : boxes)
        if (b.contains(p)) return true;
    return false;
}
} // namespace

TEST(ContraptionCollision, QuarterTurnsKeepOneBox) {
    Transform t;
    t.angle  = 90;
    auto out = worldBoxes(t, Box::unit({0, 0, 0}));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_NEAR(out[0].size().x, 1.0, 1e-9);
    EXPECT_NEAR(out[0].size().z, 1.0, 1e-9);
}

TEST(ContraptionCollision, TurnedBoxesHugTheBlock) {
    // a block turned 45 degrees about Y (pivot at its centre): its footprint is a diamond of
    // half-diagonal 0.707; the bounding box would reach the square's corners
    Transform t;
    t.angle  = 45;
    t.pivot  = {0.5, 0.5, 0.5};
    auto out = worldBoxes(t, Box::unit({0, 0, 0}));
    EXPECT_EQ(out.size(), 4u);
    EXPECT_TRUE(covered(out, {0.5, 0.5, 0.5}));        // the centre
    EXPECT_TRUE(covered(out, {0.5 + 0.65, 0.5, 0.5})); // near a tip of the diamond
    // the corner region of the plain bounding box is left out (0.7, 0.7 off the centre)
    EXPECT_FALSE(covered(out, {0.5 + 0.6, 0.5, 0.5 + 0.6}));
    // the vertical extent stays the block's
    for (auto const& b : out) {
        EXPECT_NEAR(b.min.y, 0.0, 1e-9);
        EXPECT_NEAR(b.max.y, 1.0, 1e-9);
    }
}
