#include <gtest/gtest.h>

#include <map>
#include <set>

#include "core/funnels/Logistics.h"

using namespace lv::core::funnels;

namespace {
/// Outputs with a capacity each (-1: not an output).
OutputProbe capacities(std::vector<int> caps) {
    return [caps](int t, int count) -> std::optional<int> {
        int cap = caps[static_cast<std::size_t>(t)];
        if (cap < 0) return std::nullopt;
        return std::max(0, count - cap);
    };
}
std::map<int, int> given(DistributionPlan const& p) {
    std::map<int, int> m;
    for (auto [t, n] : p.give) m[t] += n;
    return m;
}
} // namespace

TEST(Funnels, AmountLabel) {
    EXPECT_EQ(amountLabel({64, true}, 64, "Any"), "Any");
    EXPECT_EQ(amountLabel({16, true}, 64, "Any"), "≤16");
    EXPECT_EQ(amountLabel({64, false}, 64, "Any"), "=64");
    EXPECT_EQ(amountLabel({0, false}, 64, "Any"), "=1");
}

TEST(Funnels, SplitEvenly) {
    auto p = planDistribution(SelectionMode::Split, 10, 3, 0, capacities({64, 64, 64}));
    auto g = given(p);
    EXPECT_EQ(g[0], 4);
    EXPECT_EQ(g[1], 3);
    EXPECT_EQ(g[2], 3);
    EXPECT_EQ(p.kept, 0);
}

TEST(Funnels, SplitSkipsFullOutputs) {
    // output 1 is full: its share goes to the others
    auto p = planDistribution(SelectionMode::Split, 9, 3, 0, capacities({64, 0, 64}));
    auto g = given(p);
    EXPECT_EQ(g[0] + g[2], 9);
    EXPECT_EQ(g.count(1), 0u);
    // a nearly full output takes what it can, the leftovers go round again
    p = planDistribution(SelectionMode::Split, 9, 3, 0, capacities({64, 1, 64}));
    g = given(p);
    EXPECT_EQ(g[1], 1);
    EXPECT_EQ(g[0] + g[2], 8);
    EXPECT_EQ(p.kept, 0);
}

TEST(Funnels, ForcedSplitWaits) {
    auto p = planDistribution(SelectionMode::ForcedSplit, 9, 3, 0, capacities({64, 0, 64}));
    EXPECT_TRUE(p.give.empty());
    EXPECT_EQ(p.kept, 9);
    p = planDistribution(SelectionMode::ForcedSplit, 9, 3, 0, capacities({64, 64, 64}));
    EXPECT_EQ(given(p)[1], 3);
}

TEST(Funnels, RoundRobin) {
    auto p = planDistribution(SelectionMode::RoundRobin, 8, 3, 1, capacities({64, 64, 64}));
    ASSERT_EQ(p.give.size(), 1u);
    EXPECT_EQ(p.give[0], std::make_pair(1, 8));
    // a full output gives nothing this time (the next distribution moves on)
    p = planDistribution(SelectionMode::RoundRobin, 8, 3, 1, capacities({64, 0, 64}));
    EXPECT_TRUE(p.give.empty());
    EXPECT_EQ(p.kept, 8);
    // partial insertion keeps the rest
    p = planDistribution(SelectionMode::RoundRobin, 8, 3, 1, capacities({64, 5, 64}));
    EXPECT_EQ(given(p)[1], 5);
    EXPECT_EQ(p.kept, 3);
    // forced: waits until the whole stack fits
    p = planDistribution(SelectionMode::ForcedRoundRobin, 8, 3, 1, capacities({64, 5, 64}));
    EXPECT_TRUE(p.give.empty());
}

TEST(Funnels, PreferNearestAndRandomize) {
    auto p = planDistribution(SelectionMode::PreferNearest, 8, 3, 0, capacities({0, -1, 64}));
    EXPECT_EQ(given(p)[2], 8);
    EXPECT_EQ(distributionStart(SelectionMode::PreferNearest, 5, 3, 7), 0);
    EXPECT_EQ(distributionStart(SelectionMode::Randomize, 5, 3, 7), 1);
    EXPECT_EQ(distributionStart(SelectionMode::RoundRobin, 5, 3, 7), 2);
    EXPECT_EQ(selectionModeKey(SelectionMode::Synchronize), "create.tunnel.selection_mode.synchronize");
}

TEST(Funnels, TunnelShapes) {
    EXPECT_EQ(tunnelShape(true, true, true), TunnelShape::Cross);
    EXPECT_EQ(tunnelShape(true, false, false), TunnelShape::TLeft);
    EXPECT_EQ(tunnelShape(false, false, true), TunnelShape::Window);
    EXPECT_EQ(tunnelShape(false, false, false), TunnelShape::Straight);
    // axis x: left is south
    EXPECT_EQ(tunnelLeft(0), kSouth);
    EXPECT_EQ(tunnelLeft(2), kWest);
    auto straight = tunnelSides(0, TunnelShape::Straight);
    EXPECT_EQ(std::set<int>(straight.begin(), straight.end()), (std::set<int>{kWest, kEast}));
    auto tl = tunnelSides(0, TunnelShape::TLeft);
    EXPECT_EQ(std::set<int>(tl.begin(), tl.end()), (std::set<int>{kWest, kEast, kSouth}));
    auto tr = tunnelSides(2, TunnelShape::TRight);
    EXPECT_EQ(std::set<int>(tr.begin(), tr.end()), (std::set<int>{kNorth, kSouth, kEast}));
    auto cross = tunnelSides(2, TunnelShape::Cross);
    EXPECT_EQ(cross.size(), 4u);
}

TEST(Funnels, ChuteMotion) {
    EXPECT_FLOAT_EQ(chuteItemMotion(0, 0), -4.f / 20.f); // gravity: 4 blocks per second down
    EXPECT_FLOAT_EQ(chuteItemMotion(64, 0), 1.f / 20.f);
    EXPECT_FLOAT_EQ(chuteItemMotion(0, 256 * 64), 20.f / 20.f);
    EXPECT_FLOAT_EQ(chuteItemMotion(-64, 0), -5.f / 20.f);
    EXPECT_FLOAT_EQ(chutePullDistance(128), 3);
    EXPECT_FLOAT_EQ(chutePullDistance(64), 2);
    EXPECT_FLOAT_EQ(chutePullDistance(16), 0.5f);
}

TEST(Funnels, ChuteState) {
    std::array<bool, 6> none{};
    auto [f, s] = updateChuteState(kDown, ChuteShape::Intersection, none, false, false);
    EXPECT_EQ(f, kDown);
    EXPECT_EQ(s, ChuteShape::Normal);
    std::array<bool, 6> fromNorth{};
    fromNorth[kNorth] = true;
    EXPECT_EQ(updateChuteState(kDown, ChuteShape::Window, fromNorth, false, false).second, ChuteShape::Intersection);
    // a diagonal chute with no chute below it turns straight
    EXPECT_EQ(updateChuteState(kNorth, ChuteShape::Normal, none, false, false).first, kDown);
    // diagonal chain continuing in the same direction stays normal
    EXPECT_EQ(updateChuteState(kNorth, ChuteShape::Normal, fromNorth, true, false).second, ChuteShape::Normal);
    EXPECT_EQ(updateChuteState(kNorth, ChuteShape::Normal, none, true, false).second, ChuteShape::Intersection);
}

TEST(Funnels, VaultFormation) {
    // a 2x2x4 block of vaults along x: one structure, width 2, length 4
    std::vector<VaultCell> cells;
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) cells.push_back({x, y, z});
    auto axisX = [](VaultCell const&) { return std::optional<int>(0); };
    auto s = formVaults(cells, axisX);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].width, 2);
    EXPECT_EQ(s[0].length, 4);
    EXPECT_EQ(s[0].origin, (VaultCell{0, 0, 0}));
    EXPECT_EQ(s[0].indexOf({3, 1, 1}), 15);
    EXPECT_EQ(s[0].cell(15), (VaultCell{3, 1, 1}));
    // length is limited to 3x the width: 8 single vaults in a row -> 3 + 3 + 2
    std::vector<VaultCell> row;
    for (int x = 0; x < 8; ++x) row.push_back({x, 0, 0});
    auto r = formVaults(row, axisX);
    int total = 0;
    for (auto const& v : r) {
        EXPECT_LE(v.length, 3);
        total += v.size();
    }
    EXPECT_EQ(total, 8);
    // different axes don't join
    auto mixed = formVaults({{0, 0, 0}, {1, 0, 0}}, [](VaultCell const& c) { return std::optional<int>(c.x == 0 ? 0 : 2); });
    EXPECT_EQ(mixed.size(), 2u);
}

TEST(Funnels, VaultLook) {
    VaultStructure s{{0, 0, 0}, 0, 3, 3};
    auto           look = vaultLook(s, {1, 1, 1});
    EXPECT_EQ(look.len, 2);
    EXPECT_EQ(look.u, 2);
    EXPECT_EQ(look.v, 2);
    EXPECT_TRUE(look.large);
    look = vaultLook(s, {0, 2, 0});
    EXPECT_EQ(look.len, 1);
    EXPECT_EQ(look.v, 3);
    EXPECT_EQ(look.u, 1);
    EXPECT_EQ(vaultLook(VaultStructure{{0, 0, 0}, 2, 1, 1}, {0, 0, 0}), (VaultLook{0, 0, 0, false}));
}

TEST(Funnels, ComparatorSignal) {
    EXPECT_EQ(comparatorSignal({}, 20), 0);
    EXPECT_EQ(comparatorSignal({{1, 64}}, 20), 1);
    std::vector<std::pair<int, int>> full(20, {64, 64});
    EXPECT_EQ(comparatorSignal(full, 20), 15);
    std::vector<std::pair<int, int>> half(10, {64, 64});
    EXPECT_EQ(comparatorSignal(half, 20), 8);
}
