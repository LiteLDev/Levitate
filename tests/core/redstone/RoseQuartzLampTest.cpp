#include <gtest/gtest.h>

#include <set>

#include "core/redstone/RoseQuartzLamp.h"

using namespace lv::core::redstone;

TEST(RoseQuartzLamp, RisingInputActivates) {
    LampState off{};
    EXPECT_EQ(lampNeighbourChanged(off, false), LampChange::None);
    EXPECT_EQ(lampNeighbourChanged(off, true), LampChange::Activate);
    LampState on{true, true, false};
    EXPECT_EQ(lampNeighbourChanged(on, true), LampChange::None);
    EXPECT_EQ(lampNeighbourChanged(on, false), LampChange::Unpower);
}

TEST(RoseQuartzLamp, ScheduledTick) {
    // the activated lamp stays lit, a lamp darkened by the cluster stays dark
    EXPECT_EQ(lampTick({true, true, true}), (LampState{true, true, false}));
    EXPECT_EQ(lampTick({false, false, false}), (LampState{false, false, false}));
    // a lamp activated in the same tick as another lights up again
    EXPECT_EQ(lampTick({true, false, true}), (LampState{true, true, false}));
    // a lit lamp without activation (wrenched on) keeps... nothing: it goes dark
    EXPECT_EQ(lampTick({false, true, false}), (LampState{false, false, false}));
}

TEST(RoseQuartzLamp, ClusterFollowsConnectedLamps) {
    std::set<LampPos> lamps{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {5, 0, 0}};
    auto              c = lampCluster({0, 0, 0}, [&](LampPos const& p) { return lamps.contains(p); });
    std::set<LampPos> got(c.begin(), c.end());
    EXPECT_EQ(got, (std::set<LampPos>{{1, 0, 0}, {2, 0, 0}, {2, 1, 0}}));
    // a line of 20 lamps: only 16 blocks away count
    auto line = lampCluster({0, 0, 0}, [](LampPos const& p) { return p[1] == 0 && p[2] == 0 && p[0] >= 0 && p[0] < 20; });
    EXPECT_EQ(line.size(), 16u);
}

TEST(RoseQuartzLamp, ComparatorDistance) {
    // lit lamp next to the comparator: 15
    EXPECT_EQ(lampDistanceToPowered([](int i) { return i == 0 ? 1 : 0; }), 15);
    // the third lamp is lit: 13
    EXPECT_EQ(lampDistanceToPowered([](int i) { return i < 2 ? 0 : 1; }), 13);
    // the column ends before a lit lamp
    EXPECT_EQ(lampDistanceToPowered([](int i) { return i < 3 ? 0 : -1; }), 0);
    // nothing lit in 15 lamps
    EXPECT_EQ(lampDistanceToPowered([](int) { return 0; }), 0);
}

TEST(DeskBell, Pose) {
    auto start = deskBellPose(1, 0);
    EXPECT_NEAR(start.plunger, .75f / 16, 1e-6); // pressed
    EXPECT_NEAR(start.tiltZ, 8.f, 1e-4);         // cos(4 pi) * 8
    auto half = deskBellPose(.5f, 0);
    EXPECT_NEAR(half.plunger, 0.f, 1e-6);
    auto rest = deskBellPose(0, 1.f);
    EXPECT_NEAR(rest.tiltX, 0.f, 1e-6);
    EXPECT_NEAR(rest.tiltZ, 0.f, 1e-6);
}
