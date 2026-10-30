#include <cmath>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "core/contraption/Collision.h"
#include "core/contraption/Geometry.h"
#include "core/contraption/Motion.h"
#include "core/contraption/StateRotation.h"

using namespace lv::core::contraption;

namespace {
void expectNear(Vec3d const& a, Vec3d const& b, double eps = 1e-9) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}
} // namespace

TEST(ContraptionGeometry, Directions) {
    for (auto d : kAllDirs) {
        EXPECT_EQ(opposite(opposite(d)), d);
        EXPECT_EQ(offset(d) + offset(opposite(d)), (Pos{0, 0, 0}));
        EXPECT_EQ(dirOf(axisOf(d), positive(d)), d);
        EXPECT_EQ(dirFromName(dirName(d)), d);
    }
    EXPECT_EQ(fromHorizontalIndex(0), Dir::South);
    EXPECT_EQ(horizontalIndex(Dir::East), 3);
}

TEST(ContraptionGeometry, QuarterTurnsFollowTheRightHandRule) {
    EXPECT_EQ(rotate(Dir::East, Axis::Y, 1), Dir::North);
    EXPECT_EQ(rotate(Dir::North, Axis::Y, 1), Dir::West);
    EXPECT_EQ(rotate(Dir::Up, Axis::X, 1), Dir::South);
    EXPECT_EQ(rotate(Dir::East, Axis::Z, 1), Dir::Up);
    EXPECT_EQ(rotate(Dir::East, Axis::Y, -1), Dir::South);
    EXPECT_EQ(rotate(Dir::East, Axis::Y, 4), Dir::East);
    EXPECT_EQ(rotate(Axis::X, Axis::Y, 1), Axis::Z);
    EXPECT_EQ(rotate(Axis::X, Axis::Y, 2), Axis::X);
    EXPECT_EQ(rotate(Axis::Y, Axis::Y, 1), Axis::Y);
    for (auto a : {Axis::X, Axis::Y, Axis::Z}) {
        for (int q = -4; q <= 4; ++q) {
            Pos p{3, -2, 5};
            auto v = rotate(Vec3d::of(p), a, q * 90.0);
            auto r = rotate(p, a, q);
            expectNear(v, Vec3d::of(r));
        }
    }
}

TEST(ContraptionGeometry, QuarterTurnRounding) {
    EXPECT_EQ(quarterTurnsOf(44), 0);
    EXPECT_EQ(quarterTurnsOf(45), 1);
    EXPECT_EQ(quarterTurnsOf(-46), -1);
    EXPECT_EQ(quarterTurnsOf(359), 4);
    EXPECT_EQ(roundHalfUp(0.5), 1);
    EXPECT_EQ(roundHalfUp(-0.4), 0);
}

TEST(ContraptionGeometry, TransformRoundTrip) {
    Transform t;
    t.origin = {10, 64, -5};
    t.offset = {0.25, 1.5, 0};
    t.axis   = Axis::Z;
    t.angle  = 33;
    Vec3d p{2.3, -1.2, 4.4};
    expectNear(t.toLocal(t.toWorld(p)), p, 1e-9);
}

TEST(ContraptionGeometry, TiltedCarriage) {
    // a carriage along +X, yawed 90 degrees, pitched 30 degrees up a slope (tilt about local Z)
    Transform t;
    t.origin   = {0, 64, 0};
    t.pivot    = {0.5, 0, 0.5};
    t.axis     = Axis::Y;
    t.angle    = 90;
    t.tiltAxis = Axis::Z;
    t.tilt     = 30;
    // the pivot stays put; a point 2 blocks along the carriage rises by 2 sin 30 = 1
    expectNear(t.toWorld({0.5, 0, 0.5}), {0.5, 64, 0.5});
    auto w = t.toWorld({2.5, 0, 0.5});
    EXPECT_NEAR(w.y, 65, 1e-9);
    EXPECT_NEAR(w.z, 0.5 - 2 * std::cos(30 * 3.14159265358979323846 / 180), 1e-9); // +X turned to north
    Vec3d p{2.3, -1.2, 4.4};
    expectNear(t.toLocal(t.toWorld(p)), p, 1e-9);
    expectNear(t.unrotateVector(t.rotateVector(p)), p, 1e-9);
    // placement ignores the tilt
    Transform level = t;
    level.tilt      = 0;
    EXPECT_EQ(t.placeBlock({3, 1, 0}), level.placeBlock({3, 1, 0}));
    // the client interpolates the tilt too
    EXPECT_NEAR(lerp(level, t, 0.5).tilt, 15, 1e-9);
}

TEST(ContraptionGeometry, PitchTiltFollowsTheSlope) {
    // whatever the assembly direction, the tilted and yawed carriage axis points along the slope
    for (auto [ax, az] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
        for (Vec3d dir : {Vec3d{3, 1, 0}, Vec3d{0, -2, 4}, Vec3d{-2, 0.5, -2}}) {
            Transform t;
            t.pivot = {0, 0, 0};
            // the yaw that turns (ax, az) into the horizontal direction of dir
            double from = std::atan2(-double(az), double(ax)), to = std::atan2(-dir.z, dir.x);
            t.angle            = (to - from) * 180 / 3.14159265358979323846;
            std::tie(t.tiltAxis, t.tilt) = pitchTilt(ax, az, dir);
            auto   u   = t.rotateVector({double(ax), 0, double(az)});
            double len = dir.length();
            expectNear(u, dir * (1 / len), 1e-9);
        }
    }
}

TEST(ContraptionGeometry, PlacementRoundsToTheGrid) {
    Transform t;
    t.origin = {100, 64, 100};
    // a block one east of the pivot block, rotated by 80 degrees about Y: placed north
    t.angle = 80;
    EXPECT_EQ(t.placeBlock({1, 0, 0}), (Pos{100, 64, 99}));
    EXPECT_EQ(t.placeBlock({0, 0, 0}), (Pos{100, 64, 100}));
    t.angle  = 0;
    t.offset = {0, 2.4, 0.6};
    EXPECT_EQ(t.placeBlock({0, 1, 0}), (Pos{100, 67, 101}));
    // pivot at a corner (between four blocks)
    t        = Transform{};
    t.pivot  = {1, 0, 1};
    t.angle  = 90;
    EXPECT_EQ(t.placeBlock({0, 0, 0}), (Pos{0, 0, 1}));
}

TEST(ContraptionCollision, PushesOutAndStandsOnTop) {
    Transform        t;
    t.origin = {0, 10, 0};
    std::vector<Box> boxes{Box::unit({0, 0, 0}), Box::unit({1, 0, 0})};
    // a player slightly sunk into the top of the structure
    Box player{{0.2, 10.9, 0.2}, {0.8, 12.7, 0.8}};
    auto r = collide(t, boxes, player);
    EXPECT_TRUE(r.hit);
    EXPECT_TRUE(r.onTop);
    EXPECT_NEAR(r.push.y, 0.1, 1e-9);
    // a box beside the structure does not collide
    Box away{{3, 10, 0}, {3.6, 11.8, 0.6}};
    EXPECT_FALSE(collide(t, boxes, away).hit);
    // standing: feet on top
    EXPECT_TRUE(standsOn(t, boxes, {0.5, 11.0, 0.5}));
    EXPECT_FALSE(standsOn(t, boxes, {0.5, 11.5, 0.5}));
}

TEST(ContraptionCollision, CarryFollowsRotation) {
    Transform a;
    a.origin = {0, 0, 0};
    Transform b = a;
    b.angle     = 90;
    // a point above block (1,0,0) (east of the pivot block) ends up above (0,0,-1)
    auto p = carry(a, b, {1.5, 1.0, 0.5});
    expectNear(p, {0.5, 1.0, -0.5});
}

TEST(ContraptionStates, RotatesKnownStates) {
    States s{
        {"facing_direction",             std::int64_t{5}        },
        {"minecraft:cardinal_direction", std::string("east")    },
        {"pillar_axis",                  std::string("x")       },
        {"direction",                    std::int64_t{0}        },
        {"weirdo_direction",             std::int64_t{0}        },
        {"torch_facing_direction",       std::string("top")     },
        {"ground_sign_direction",        std::int64_t{0}        },
    };
    EXPECT_TRUE(rotateStates(s, Axis::Y, 1));
    EXPECT_EQ(std::get<std::int64_t>(s["facing_direction"]), 2); // east -> north
    EXPECT_EQ(std::get<std::string>(s["minecraft:cardinal_direction"]), "north");
    EXPECT_EQ(std::get<std::string>(s["pillar_axis"]), "z");
    EXPECT_EQ(std::get<std::int64_t>(s["direction"]), 3);        // south -> east
    EXPECT_EQ(std::get<std::int64_t>(s["weirdo_direction"]), 3); // east -> north
    EXPECT_EQ(std::get<std::string>(s["torch_facing_direction"]), "top");
    EXPECT_EQ(std::get<std::int64_t>(s["ground_sign_direction"]), 12);

    States h{{"minecraft:cardinal_direction", std::string("north")}, {"minecraft:vertical_half", std::string("bottom")}};
    rotateStates(h, Axis::X, 1); // would turn vertical: unchanged
    EXPECT_EQ(std::get<std::string>(h["minecraft:cardinal_direction"]), "north");
    rotateStates(h, Axis::X, 2);
    EXPECT_EQ(std::get<std::string>(h["minecraft:cardinal_direction"]), "south");
    EXPECT_EQ(std::get<std::string>(h["minecraft:vertical_half"]), "top");
}

TEST(ContraptionStates, RotatesChassisStickySides) {
    States linear{
        {"levitate:axis",         std::int64_t{1}},
        {"levitate:sticky_top",   std::int64_t{1}},
        {"levitate:sticky_bottom", std::int64_t{0}},
    };
    rotateStates(linear, Axis::X, 1); // up -> south: still the positive end, axis z
    EXPECT_EQ(std::get<std::int64_t>(linear["levitate:axis"]), 2);
    EXPECT_EQ(std::get<std::int64_t>(linear["levitate:sticky_top"]), 1);
    rotateStates(linear, Axis::Y, 1); // south -> east: positive, axis x
    EXPECT_EQ(std::get<std::int64_t>(linear["levitate:axis"]), 0);
    EXPECT_EQ(std::get<std::int64_t>(linear["levitate:sticky_top"]), 1);
    rotateStates(linear, Axis::Y, 1); // east -> north: negative end now
    EXPECT_EQ(std::get<std::int64_t>(linear["levitate:sticky_top"]), 0);
    EXPECT_EQ(std::get<std::int64_t>(linear["levitate:sticky_bottom"]), 1);

    States radial{
        {"levitate:axis",         std::int64_t{1}},
        {"levitate:sticky_north", std::int64_t{1}},
        {"levitate:sticky_south", std::int64_t{0}},
        {"levitate:sticky_east",  std::int64_t{0}},
        {"levitate:sticky_west",  std::int64_t{0}},
    };
    rotateStates(radial, Axis::Y, 1); // north side -> west side
    EXPECT_EQ(std::get<std::int64_t>(radial["levitate:sticky_west"]), 1);
    EXPECT_EQ(std::get<std::int64_t>(radial["levitate:sticky_north"]), 0);
}
