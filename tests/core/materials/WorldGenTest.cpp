#include <gtest/gtest.h>

#include <map>
#include <set>
#include <tuple>

#include "core/materials/WorldGen.h"

using namespace lv::core::materials;

namespace {

/// A solid world: stone above y 0, deepslate below, surface at `surfaceY`; records every write.
struct FakeWorld {
    int                                         surfaceY = 64;
    Ground                                      fill     = Ground::Stone; ///< used everywhere when not Stone
    bool                                        layeredByDepth = true;
    std::map<std::tuple<int, int, int>, Mat>    writes;
    int                                         writeRadius = 1 << 30; ///< |x|,|z| limit of writable area

    World world() {
        World w;
        w.ground = [this](int x, int y, int z) {
            if (auto it = writes.find({x, y, z}); it != writes.end()) return Ground::Other; // already placed
            if (y >= surfaceY) return Ground::Other;
            if (fill == Ground::Nether) return Ground::Nether;
            return (layeredByDepth && y < 0) ? Ground::Deepslate : Ground::Stone;
        };
        w.canWrite = [this](int x, int, int z) { return std::abs(x) <= writeRadius && std::abs(z) <= writeRadius; };
        w.set      = [this](int x, int y, int z, Mat m) { writes[{x, y, z}] = m; };
        w.surface  = [this](int, int) { return surfaceY; };
        return w;
    }
};

} // namespace

TEST(MaterialsWorldGen, RngIsDeterministicAndInRange) {
    Rng a(42), b(42), c(43);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        auto x = a.next();
        EXPECT_EQ(x, b.next());
        differs |= x != c.next();
        float f = a.nextFloat();
        EXPECT_GE(f, 0.0f);
        EXPECT_LT(f, 1.0f);
        int n = a.nextInt(7);
        EXPECT_GE(n, 0);
        EXPECT_LT(n, 7);
        b.nextFloat();
        b.nextInt(7);
    }
    EXPECT_TRUE(differs);
    EXPECT_NE(featureSeed(1, 0, 0, 1), featureSeed(1, 1, 0, 1));
    EXPECT_NE(featureSeed(1, 0, 0, 1), featureSeed(1, 0, 0, 2));
}

TEST(MaterialsWorldGen, SimplexStaysInRange) {
    Rng     rng(7);
    Simplex s(rng);
    double  lo = 1, hi = -1;
    for (int i = 0; i < 4000; ++i) {
        double v = s.value(i * 0.137, i * 0.071, i * 0.293);
        lo       = std::min(lo, v);
        hi       = std::max(hi, v);
    }
    EXPECT_GE(lo, -1.01);
    EXPECT_LE(hi, 1.01);
    EXPECT_LT(lo, -0.3);
    EXPECT_GT(hi, 0.3);
}

TEST(MaterialsWorldGen, RollNextLayerNeverRepeatsAndFollowsWeights) {
    auto const& p = overworldPatterns()[3]; // veridium: weights 4, 2, 2, 3
    Rng         rng(9);
    std::map<int, int> counts;
    int prev = -1;
    for (int i = 0; i < 20000; ++i) {
        int next = rollNextLayer(p, prev, rng);
        ASSERT_NE(next, prev);
        ASSERT_GE(next, 0);
        ASSERT_LT(next, static_cast<int>(p.size()));
        if (prev == -1) counts[next]++;
        prev = (i % 2) ? -1 : next;
    }
    // from "no previous" the first layer (weight 4 of 11) is the most likely
    EXPECT_GT(counts[0], counts[1]);
    EXPECT_GT(counts[0], counts[2]);
}

TEST(MaterialsWorldGen, PatternsMatchCreate) {
    ASSERT_EQ(overworldPatterns().size(), 6u);
    ASSERT_EQ(netherPatterns().size(), 2u);
    // every overworld pattern places one of Create's stones
    std::set<Mat> stones;
    for (auto const& p : overworldPatterns())
        for (auto const& l : p)
            for (auto const& alt : l.targets)
                for (auto const& t : alt) stones.insert(t.block);
    for (Mat m : {Mat::Scoria, Mat::Crimsite, Mat::Asurine, Mat::Veridium, Mat::Limestone, Mat::Ochrum})
        EXPECT_TRUE(stones.count(m)) << int(m);
    EXPECT_EQ(netherPatterns()[1][1].weight, 8); // scorchia
    EXPECT_EQ(netherPatterns()[1][1].targets[0][0].block, Mat::Scorchia);
    EXPECT_EQ(overworldPatterns()[0][0].maxSize, 5);
}

TEST(MaterialsWorldGen, ZincOrePlacesOnlyIntoReplaceables) {
    FakeWorld fw;
    auto      w   = fw.world();
    Rng       rng(5);
    int       placed = placeOre(w, rng, 0, 0, 0, 12, zincTargets());
    EXPECT_GT(placed, 0);
    EXPECT_LE(placed, 12 * 12);
    for (auto const& [pos, m] : fw.writes) {
        int y = std::get<1>(pos);
        EXPECT_EQ(m, y < 0 ? Mat::DeepslateZincOre : Mat::ZincOre);
        EXPECT_LE(std::abs(std::get<0>(pos)), 6);
        EXPECT_LE(std::abs(std::get<2>(pos)), 6);
    }
}

TEST(MaterialsWorldGen, OreAboveTheSurfaceIsSkipped) {
    FakeWorld fw;
    fw.surfaceY = -50;
    auto w      = fw.world();
    Rng  rng(5);
    EXPECT_EQ(placeOre(w, rng, 0, 40, 0, 12, zincTargets()), 0);
    EXPECT_TRUE(fw.writes.empty());
}

TEST(MaterialsWorldGen, LayeredPlacesPatternBlocksInASphere) {
    int total = 0;
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        FakeWorld fw;
        auto      w = fw.world();
        Rng       rng(seed);
        total += placeLayered(w, rng, 0, 20, 0, 32, overworldPatterns());
        for (auto const& [pos, m] : fw.writes) {
            auto [x, y, z] = pos;
            EXPECT_LE(x * x + (y - 20) * (y - 20) + z * z, 16 * 16 + 1);
            EXPECT_NE(m, Mat::ZincOre);
        }
    }
    EXPECT_GT(total, 1000); // a size-32 vein is big
}

TEST(MaterialsWorldGen, LayeredRespectsTheWritableArea) {
    FakeWorld fw;
    fw.writeRadius = 4;
    auto w         = fw.world();
    Rng  rng(3);
    placeLayered(w, rng, 0, 20, 0, 32, overworldPatterns());
    for (auto const& [pos, m] : fw.writes) {
        EXPECT_LE(std::abs(std::get<0>(pos)), 4);
        EXPECT_LE(std::abs(std::get<2>(pos)), 4);
    }
}

TEST(MaterialsWorldGen, NetherLayersOnlyReplaceNetherStone) {
    FakeWorld fw;
    fw.fill = Ground::Nether;
    auto w  = fw.world();
    Rng  rng(11);
    int  placed = placeLayered(w, rng, 0, 60, 0, 32, netherPatterns());
    EXPECT_GT(placed, 0);
    for (auto const& [pos, m] : fw.writes) EXPECT_NE(m, Mat::Stone);
}

TEST(MaterialsWorldGen, PlacementsFollowCreateRates) {
    int zincVeins = 0, striated = 0, chunks = 18000;
    for (int i = 0; i < chunks; ++i) {
        Rng z(featureSeed(1, i, -i, 1));
        auto o = zincOrigins(z, i, -i);
        zincVeins += static_cast<int>(o.size());
        for (auto const& p : o) {
            EXPECT_GE(p.y, -63);
            EXPECT_LE(p.y, 70);
            EXPECT_GE(p.x, i * 16);
            EXPECT_LT(p.x, i * 16 + 16);
        }
        Rng s(featureSeed(1, i, -i, 2));
        for (auto const& p : striatedOrigins(s, i, -i, false)) {
            ++striated;
            EXPECT_GE(p.y, -30);
            EXPECT_LE(p.y, 70);
        }
    }
    EXPECT_EQ(zincVeins, chunks * 8);
    EXPECT_GT(striated, chunks / 18 * 8 / 10);
    EXPECT_LT(striated, chunks / 18 * 12 / 10);
}

TEST(MaterialsWorldGen, DecorateChunkIsDeterministic) {
    FakeWorld a, b;
    auto      wa = a.world(), wb = b.world();
    int       na = decorateChunk(wa, 1234, 0, 3, -7);
    int       nb = decorateChunk(wb, 1234, 0, 3, -7);
    EXPECT_EQ(na, nb);
    EXPECT_EQ(a.writes, b.writes);
    EXPECT_GT(na, 0);
    FakeWorld end;
    auto      we = end.world();
    EXPECT_EQ(decorateChunk(we, 1234, 2, 3, -7), 0);
}
