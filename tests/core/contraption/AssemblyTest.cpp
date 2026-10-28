#include <gtest/gtest.h>

#include <algorithm>
#include <map>

#include "core/contraption/Assembly.h"
#include "core/contraption/Glue.h"

using namespace lv::core::contraption;

namespace {

/// A tiny world: a map of positions to block infos; everything else is air.
struct FakeWorld {
    std::map<Pos, BlockInfo> blocks;
    std::vector<GlueBox>     glue;

    BlockInfo& solid(Pos p, std::string id = "stone") {
        auto& b     = blocks[p];
        b.necessary = true;
        b.id        = std::move(id);
        return b;
    }
    WorldQuery query() const {
        return [this](Pos const& p) {
            auto it = blocks.find(p);
            return it == blocks.end() ? BlockInfo{} : it->second;
        };
    }
    GlueQuery glueQuery() const {
        return [this](Pos const& p, Dir d) { return isGlued(glue, p, d); };
    }
    AssemblyResult run(std::vector<Pos> start, AssemblyLimits limits = {}) const {
        return assemble(start, query(), glueQuery(), limits);
    }
};

bool contains(AssemblyResult const& r, Pos p) {
    // (std::find would pick the STL's vectorized path for this 12-byte type and fail to compile)
    return std::any_of(r.blocks.begin(), r.blocks.end(), [&](Pos const& b) { return b == p; });
}

} // namespace

TEST(ContraptionAssembly, SingleBlockWithoutGlue) {
    FakeWorld w;
    w.solid({0, 0, 0});
    w.solid({1, 0, 0});
    auto r = w.run({{0, 0, 0}});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.blocks.size(), 1u); // plain neighbours are not taken without glue
}

TEST(ContraptionAssembly, GlueRegionTakesNeighbours) {
    FakeWorld w;
    w.solid({0, 0, 0});
    w.solid({1, 0, 0});
    w.solid({2, 0, 0});
    w.glue.push_back(GlueBox::of({0, 0, 0}, {1, 0, 0}));
    auto r = w.run({{0, 0, 0}});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.blocks.size(), 2u);
    EXPECT_FALSE(contains(r, {2, 0, 0}));
}

TEST(ContraptionAssembly, AttachedBlocksAndSlime) {
    FakeWorld w;
    w.solid({0, 0, 0});
    auto& torch           = w.solid({0, 1, 0}, "torch");
    torch.brittle         = true;
    torch.attachedTowards = bit(Dir::Down);
    auto& slime           = w.solid({1, 0, 0}, "slime");
    slime.sticky          = StickyKind::Slime;
    w.solid({2, 0, 0});
    auto& honey  = w.solid({1, 0, 1}, "honey");
    honey.sticky = StickyKind::Honey;
    w.glue.push_back(GlueBox::of({0, 0, 0}, {1, 0, 0}));
    auto r = w.run({{0, 0, 0}});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(contains(r, {0, 1, 0}));  // torch hangs on the block
    EXPECT_TRUE(contains(r, {2, 0, 0}));  // slime drags its neighbour
    EXPECT_FALSE(contains(r, {1, 0, 1})); // slime doesn't stick to honey
}

TEST(ContraptionAssembly, UnmovableAndLimits) {
    FakeWorld w;
    w.solid({0, 0, 0});
    auto& bedrock   = w.solid({1, 0, 0}, "bedrock");
    bedrock.movable = false;
    w.glue.push_back(GlueBox::of({0, 0, 0}, {1, 0, 0}));
    // glued to an unmovable block: skipped (only the forced direction fails)
    auto r = w.run({{0, 0, 0}});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.blocks.size(), 1u);
    AssemblyLimits push;
    push.forced = Dir::East;
    auto f      = w.run({{0, 0, 0}}, push);
    EXPECT_EQ(f.error, AssemblyError::Unmovable);
    EXPECT_EQ(f.errorPos, (Pos{1, 0, 0}));
    EXPECT_EQ(f.errorBlock, "bedrock");

    FakeWorld big;
    for (int x = 0; x < 10; ++x) big.solid({x, 0, 0});
    big.glue.push_back(GlueBox::of({0, 0, 0}, {9, 0, 0}));
    AssemblyLimits small;
    small.maxBlocks = 5;
    EXPECT_EQ(big.run({{0, 0, 0}}, small).error, AssemblyError::TooLarge);
    EXPECT_TRUE(big.run({{0, 0, 0}}).ok());

    FakeWorld unloaded;
    unloaded.solid({0, 0, 0}).loaded = false;
    EXPECT_EQ(unloaded.run({{0, 0, 0}}).error, AssemblyError::UnloadedChunk);
}

TEST(ContraptionAssembly, LinearChassis) {
    FakeWorld w;
    auto&     c   = w.solid({0, 0, 0}, "linear_chassis");
    c.chassis      = ChassisKind::Linear;
    c.chassisAxis  = Axis::Y;
    c.chassisRange = 2;
    c.stickyFaces  = bit(Dir::Up);
    auto& c2       = w.solid({1, 0, 0}, "linear_chassis");
    c2             = c;
    c2.stickyFaces = 0;
    auto& sec      = w.solid({-1, 0, 0}, "secondary_linear_chassis");
    sec            = c;
    sec.chassis    = ChassisKind::SecondaryLinear;
    for (int y = 1; y <= 4; ++y) w.solid({0, y, 0});
    w.solid({0, -1, 0}); // below: bottom face not sticky
    w.solid({1, 1, 0});  // above the non-sticky neighbour chassis
    auto r = w.run({{0, 0, 0}});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(contains(r, {1, 0, 0}));   // connected chassis of the same kind
    EXPECT_FALSE(contains(r, {-1, 0, 0})); // a secondary chassis doesn't connect
    EXPECT_TRUE(contains(r, {0, 1, 0}));
    EXPECT_TRUE(contains(r, {0, 2, 0}));
    EXPECT_FALSE(contains(r, {0, 3, 0})); // range 2
    EXPECT_FALSE(contains(r, {0, -1, 0}));
    EXPECT_FALSE(contains(r, {1, 1, 0}));
    // pushing down collects the non-sticky bottom end too
    AssemblyLimits down;
    down.forced = Dir::Down;
    EXPECT_TRUE(contains(w.run({{0, 0, 0}}, down), {0, -1, 0}));
}

TEST(ContraptionAssembly, RadialChassis) {
    FakeWorld w;
    auto&     c   = w.solid({0, 0, 0}, "radial_chassis");
    c.chassis      = ChassisKind::Radial;
    c.chassisAxis  = Axis::Y;
    c.chassisRange = 2;
    c.stickyFaces  = bit(Dir::East);
    for (int x = -3; x <= 3; ++x)
        for (int z = -3; z <= 3; ++z)
            if (x || z) w.solid({x, 0, z});
    auto r = w.run({{0, 0, 0}});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(contains(r, {1, 0, 0}));
    EXPECT_TRUE(contains(r, {2, 0, 0}));
    EXPECT_TRUE(contains(r, {1, 0, 2}));   // within radius 2.5
    EXPECT_FALSE(contains(r, {2, 0, 2}));  // distance sqrt(8) > 2.5
    EXPECT_FALSE(contains(r, {3, 0, 0}));
    EXPECT_TRUE(contains(r, {-1, 0, 0}));  // the disc spreads around the chassis
}

TEST(ContraptionAssembly, StickerAndLinkedParts) {
    FakeWorld w;
    auto& sticker         = w.solid({0, 0, 0}, "sticker");
    sticker.stickerFacing = Dir::East;
    w.solid({1, 0, 0});
    auto& door  = w.solid({0, 0, 1}, "door");
    door.linked = {{0, 1, 1}};
    w.solid({0, 1, 1}, "door");
    w.glue.push_back(GlueBox::of({0, 0, 0}, {0, 0, 1}));
    auto r = w.run({{0, 0, 0}});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(contains(r, {1, 0, 0}));
    EXPECT_TRUE(contains(r, {0, 1, 1}));
}

TEST(ContraptionGlue, SelectionChecks) {
    auto solid = [](Pos const& p) { return p.y == 0 && p.z == 0; }; // a line along x
    EXPECT_EQ(checkSelection({0, 0, 0}, {5, 0, 0}, 24, solid), GlueCheck::Ok);
    EXPECT_EQ(checkSelection({0, 0, 0}, {30, 0, 0}, 24, solid), GlueCheck::TooFar);
    EXPECT_EQ(checkSelection({0, 0, 0}, {3, 0, 2}, 24, solid), GlueCheck::CannotReach);
    std::vector<GlueBox> boxes{GlueBox::of({0, 0, 0}, {2, 1, 0})};
    EXPECT_TRUE(isGlued(boxes, {0, 0, 0}, Dir::Up));
    EXPECT_FALSE(isGlued(boxes, {2, 0, 0}, Dir::East));
    EXPECT_EQ(boxes[0].size(), (Pos{3, 2, 1}));
}
