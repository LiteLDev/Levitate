#include <cmath>

#include <gtest/gtest.h>

#include "core/contraption/Motion.h"

using namespace lv::core::contraption;

namespace {
Transform at(double x, double angle, Axis axis = Axis::Y) {
    Transform t;
    t.origin = {10, 64, 10};
    t.offset = {x, 0, 0};
    t.axis   = axis;
    t.angle  = angle;
    t.pivot  = {0.5, 0.5, 0.5};
    return t;
}
} // namespace

TEST(ContraptionMotion, LerpTranslationAndAngle) {
    auto a = at(0, 10), b = at(1, 30);
    auto m = lerp(a, b, 0.25);
    auto w = m.toWorld({0.5, 0.5, 0.5}); // the pivot only translates
    EXPECT_NEAR(w.x, 10.75, 1e-9);
    EXPECT_NEAR(w.y, 64.5, 1e-9);
    EXPECT_NEAR(m.angle, 15, 1e-9);
    // the ends are the transforms themselves
    EXPECT_NEAR(lerp(a, b, 0).toWorld({3, 0, 0}).x, a.toWorld({3, 0, 0}).x, 1e-9);
    EXPECT_NEAR(lerp(a, b, 0).toWorld({3, 0, 0}).z, a.toWorld({3, 0, 0}).z, 1e-9);
    EXPECT_NEAR(lerp(a, b, 1).toWorld({3, 0, 0}).z, b.toWorld({3, 0, 0}).z, 1e-9);
}

TEST(ContraptionMotion, LerpAngleTakesTheShortWay) {
    // an anchor that wraps its angle (350 -> 10) turns 20 degrees, not -340
    auto m = lerp(at(0, 350), at(0, 10), 0.5);
    EXPECT_NEAR(std::remainder(m.angle - 0, 360.0), 0, 1e-9);
    // accumulated angles stay as they are
    EXPECT_NEAR(lerp(at(0, 715), at(0, 725), 0.5).angle, 720, 1e-9);
}

TEST(ContraptionMotion, LerpAxisChangeJumps) {
    auto m = lerp(at(0, 10, Axis::X), at(1, 30, Axis::Z), 0.5);
    EXPECT_EQ(m.axis, Axis::Z);
    EXPECT_NEAR(m.angle, 30, 1e-9);
    EXPECT_NEAR(m.toWorld({0, 0, 0}).x, at(1, 30, Axis::Z).toWorld({0, 0, 0}).x, 1e-9);
}

TEST(ContraptionMotion, BufferOnePerTick) {
    MotionBuffer b;
    EXPECT_FALSE(b.ready());
    b.push(at(0, 0)); // starts the buffer
    ASSERT_TRUE(b.ready());
    EXPECT_EQ(b.queued(), 0u);
    b.push(at(1, 0));
    b.step();
    EXPECT_NEAR(b.previous().offset.x, 0, 1e-9);
    EXPECT_NEAR(b.current().offset.x, 1, 1e-9);
    EXPECT_NEAR(b.at(0.5).toWorld({0, 0, 0}).x, 10.5, 1e-9);
    // nothing new: standing still at the last transform
    b.step();
    EXPECT_NEAR(b.at(0).toWorld({0, 0, 0}).x, 11, 1e-9);
    EXPECT_NEAR(b.at(1).toWorld({0, 0, 0}).x, 11, 1e-9);
}

TEST(ContraptionMotion, BufferSkipsOldTransformsAfterALagSpike) {
    MotionBuffer b;
    b.reset(at(0, 0));
    for (int i = 1; i <= 5; ++i) b.push(at(i, 0));
    EXPECT_EQ(b.queued(), MotionBuffer::kMaxQueued);
    b.step();
    EXPECT_NEAR(b.current().offset.x, 4, 1e-9);
    b.step();
    EXPECT_NEAR(b.current().offset.x, 5, 1e-9);
    EXPECT_EQ(b.queued(), 0u);
    b.reset(at(9, 0));
    EXPECT_NEAR(b.previous().offset.x, 9, 1e-9);
    EXPECT_EQ(b.queued(), 0u);
}
