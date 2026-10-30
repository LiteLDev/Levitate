#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include "core/equipment/Bells.h"

using namespace lv::core::equipment;

namespace {

BellSupport support(std::set<int> sturdy, bool above = false) {
    return BellSupport{[sturdy](int d) { return sturdy.contains(d); }, [above] { return above; }};
}

} // namespace

TEST(Bells, PlacementOnFloorCeilingAndWalls) {
    // clicking the top of a block: a floor bell turned like the player
    auto s = placeBell(1, 4, support({0}));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->attach, BellAttach::Floor);
    EXPECT_EQ(s->facing, 4);
    // clicking the bottom of a block: hanging from the ceiling
    s = placeBell(0, 2, support({}, true));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->attach, BellAttach::Ceiling);
    EXPECT_FALSE(placeBell(0, 2, support({})));
    // the north face of a wall: the bell is north of it and faces south, into the wall
    s = placeBell(2, 4, support({3}));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->attach, BellAttach::SingleWall);
    EXPECT_EQ(s->facing, 3);
    // a wall on both sides
    s = placeBell(2, 4, support({2, 3}));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->attach, BellAttach::DoubleWall);
    // no wall there (a non-sturdy side): standing on the floor below instead, facing into the wall
    s = placeBell(2, 4, support({0}));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->attach, BellAttach::Floor);
    EXPECT_EQ(s->facing, 3);
    EXPECT_FALSE(placeBell(2, 4, support({})));
}

TEST(Bells, NeighbourChanges) {
    BellState floor{BellAttach::Floor, 3};
    EXPECT_FALSE(bellAfterNeighbourChange(floor, 0, support({})));
    EXPECT_EQ(*bellAfterNeighbourChange(floor, 0, support({0})), floor);
    EXPECT_EQ(*bellAfterNeighbourChange(floor, 2, support({})), floor); // other sides don't matter
    BellState wall{BellAttach::SingleWall, 3};
    EXPECT_FALSE(bellAfterNeighbourChange(wall, 3, support({})));
    // a block appears on the other side: a double wall bell
    auto s = bellAfterNeighbourChange(wall, 2, support({2, 3}));
    EXPECT_EQ(s->attach, BellAttach::DoubleWall);
    // a double wall bell loses its south wall: it hangs on the north one
    s = bellAfterNeighbourChange(BellState{BellAttach::DoubleWall, 3}, 3, support({2}));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->attach, BellAttach::SingleWall);
    EXPECT_EQ(s->facing, 2);
}

TEST(Bells, RingingSides) {
    BellState floor{BellAttach::Floor, 3};
    EXPECT_TRUE(bellRingsFrom(floor, 2, .5));
    EXPECT_TRUE(bellRingsFrom(floor, 3, .5));
    EXPECT_FALSE(bellRingsFrom(floor, 4, .5));
    EXPECT_FALSE(bellRingsFrom(floor, 1, .5));     // never from above
    EXPECT_FALSE(bellRingsFrom(floor, 2, .9));     // the top of the frame
    BellState wall{BellAttach::SingleWall, 3};
    EXPECT_TRUE(bellRingsFrom(wall, 4, .5));
    EXPECT_FALSE(bellRingsFrom(wall, 2, .5));
    EXPECT_EQ(bellRedstoneDirection(floor), 3);
    EXPECT_EQ(bellRedstoneDirection(wall), 4); // south turned clockwise
}

TEST(Bells, SwingAngle) {
    EXPECT_FLOAT_EQ(bellSwingAngle(0), 0.f);
    EXPECT_GT(bellSwingAngle(5), 0.f);
    EXPECT_LT(std::abs(bellSwingAngle(74)), .25f); // dies down
}

TEST(Bells, SoulLayers) {
    // every offset within 11 blocks (rounded) is in exactly one layer
    std::size_t total = 0;
    for (int i = 0; i < kSoulMaxDistance; ++i) total += soulLayer(i).size();
    std::size_t expected = 0;
    for (int x = -10; x <= 10; ++x)
        for (int y = -10; y <= 10; ++y)
            for (int z = -10; z <= 10; ++z)
                if (std::lround(std::sqrt(double(x * x + y * y + z * z))) <= kSoulMaxDistance) ++expected;
    EXPECT_EQ(total, expected);
    // layer 0: the centre and its 6 + 12 + 8 neighbours (distances 0, 1, 1.41 and 1.73 round to <= 1... 2)
    std::set<Pos> first(soulLayer(0).begin(), soulLayer(0).end());
    EXPECT_TRUE(first.contains(Pos{0, 0, 0}));
    EXPECT_TRUE(first.contains(Pos{1, 0, 0}));
    EXPECT_TRUE(first.contains(Pos{1, 1, 0}));  // 1.41 rounds to 1
    EXPECT_FALSE(first.contains(Pos{1, 1, 1})); // 1.73 rounds to 2
    EXPECT_TRUE(soulLayer(11).empty());
}

TEST(Bells, PulseExpandsAndWaits) {
    SoulPulse p({0, 0, 0}, 3, false);
    std::vector<int> layerTicks;
    int tick = 0;
    while (!p.finished()) {
        ++tick;
        if (p.tick([](Pos const&) { return true; })) layerTicks.push_back(tick);
    }
    // a layer every 6 ticks (layers 1..3), then 100 ticks of waiting
    EXPECT_EQ(layerTicks, (std::vector<int>{6, 12, 18}));
    EXPECT_EQ(tick, 18 + 100);
    // empty layers are skipped at once
    SoulPulse q({0, 0, 0}, 3, false);
    std::optional<std::vector<Pos>> got;
    for (int i = 0; i < 6; ++i) got = q.tick([](Pos const& o) { return std::abs(o[0]) + std::abs(o[1]) + std::abs(o[2]) >= 4; });
    ASSERT_TRUE(got);
    EXPECT_FALSE(got->empty()); // layer 1 was empty: layer 2 came in the same tick
    for (auto const& o : *got) EXPECT_GE(std::abs(o[0]) + std::abs(o[1]) + std::abs(o[2]), 4);
    EXPECT_TRUE(q.expanding()); // layer 3 is still to come
}

TEST(Bells, PulsesDontMarkTwice) {
    SoulPulses all;
    all.add(SoulPulse({0, 0, 0}, 2, false));
    all.add(SoulPulse({0, 0, 0}, 2, false));
    std::multiset<Pos> marked;
    for (int i = 0; i < 200; ++i) all.tick([](Pos const&) { return true; }, [&](Pos const& p) { marked.insert(p); });
    for (auto const& p : marked) EXPECT_EQ(marked.count(p), 1u);
    EXPECT_EQ(all.size(), 0u);
    // overlapping pulses mark again
    all.add(SoulPulse({0, 0, 0}, 1, true));
    all.add(SoulPulse({0, 0, 0}, 1, true));
    std::size_t n = 0;
    for (int i = 0; i < 10; ++i) all.tick([](Pos const&) { return true; }, [&](Pos const&) { ++n; });
    EXPECT_EQ(n, 2 * soulLayer(0).size());
}

TEST(Bells, HeldPulses) {
    for (int t = 1; t < kHeldPulseWarmup; ++t) EXPECT_FALSE(heldBellPulses(t, 3));
    EXPECT_TRUE(heldBellPulses(kHeldPulseWarmup, 16)); // the first pulse comes whatever the time
    EXPECT_FALSE(heldBellPulses(kHeldPulseWarmup + 1, 16));
    EXPECT_TRUE(heldBellPulses(kHeldPulseWarmup + 1, 17));
}
