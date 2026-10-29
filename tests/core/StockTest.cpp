// Unit tests of the stock network's pure logic (U26, src/core/stock).

#include <gtest/gtest.h>

#include "core/stock/Logistics.h"

namespace cs = lv::core::stock;

TEST(StockSummary, AddsMergesAndKeepsOrder) {
    cs::Summary s;
    s.add("a", 5);
    s.add("b", 10);
    s.add("a", 3);
    s.add("", 7);
    s.add("c", 0);
    EXPECT_EQ(s.countOf("a"), 8);
    EXPECT_EQ(s.countOf("b"), 10);
    EXPECT_EQ(s.countOf("c"), 0);
    EXPECT_EQ(s.total(), 18);
    ASSERT_EQ(s.entries().size(), 2u);
    EXPECT_EQ(s.entries()[0].key, "a");
    auto by = s.byCount();
    EXPECT_EQ(by[0].key, "b");
    s.add("a", -8);
    EXPECT_EQ(s.countOf("a"), 0);
    EXPECT_EQ(s.entries().size(), 2u); // Create keeps empty entries
    EXPECT_TRUE(s.erase("a"));
    EXPECT_FALSE(s.erase("a"));
}

TEST(StockSummary, InfiniteSaturates) {
    cs::Summary s;
    s.add("x", cs::kInfinite);
    s.add("x", 5);
    EXPECT_EQ(s.countOf("x"), cs::kInfinite);
    cs::Summary t;
    t.add(s);
    t.add(s);
    EXPECT_EQ(t.countOf("x"), cs::kInfinite);
    auto back = cs::Summary::fromJson(s.toJson());
    EXPECT_EQ(back.countOf("x"), cs::kInfinite);
}

TEST(StockPromises, ExpireFulfilAndClear) {
    int             changes = 0;
    cs::PromiseQueue q;
    q.onChanged = [&] { ++changes; };
    q.add("a", 10);
    q.add("a", 5);
    q.add("b", 3);
    EXPECT_EQ(q.totalPromisedAndRemoveExpired("a", -1), 15);
    for (int i = 0; i < 600; ++i) q.tick();
    q.add("a", 2);
    EXPECT_EQ(q.totalPromisedAndRemoveExpired("a", cs::promiseExpiryTicks(0)), 2);
    q.add("a", 4);
    q.itemEnteredSystem("a", 3); // oldest first: 2 fulfilled, 1 of the next
    EXPECT_EQ(q.totalPromisedAndRemoveExpired("a", -1), 3);
    q.forceClear("a");
    EXPECT_EQ(q.totalPromisedAndRemoveExpired("a", -1), 0);
    EXPECT_EQ(q.totalPromisedAndRemoveExpired("b", -1), 3);
    EXPECT_GT(changes, 4);
    auto back = cs::PromiseQueue::fromJson(q.toJson());
    EXPECT_EQ(back.totalPromisedAndRemoveExpired("b", -1), 3);
}

TEST(StockPromises, ExpiryIntervals) {
    EXPECT_EQ(cs::promiseExpiryTicks(-1), -1);
    EXPECT_EQ(cs::promiseExpiryTicks(0), 600);
    EXPECT_EQ(cs::promiseExpiryTicks(5), 6000);
}

TEST(StockOrders, SplitsAcrossLinksByPower) {
    std::vector<cs::LinkView> links(3);
    links[0].id    = 1;
    links[0].power = 5;
    links[0].available.add("iron", 100);
    links[1].id    = 2;
    links[1].power = 0;
    links[1].available.add("iron", 30);
    links[1].available.add("gold", 4);
    links[2].id = 3;
    links[2].available.add("dirt", 64);
    auto reqs = cs::distributeOrder({{"iron", 50}, {"gold", 4}}, links, [](int) { return 0; });
    ASSERT_EQ(reqs.size(), 3u);
    // lowest redstone power first: link 2 gives 30 iron, link 1 the remaining 20
    EXPECT_EQ(reqs[0].link, 2);
    EXPECT_EQ(reqs[0].count, 30);
    EXPECT_TRUE(reqs[0].context);
    EXPECT_EQ(reqs[1].link, 1);
    EXPECT_EQ(reqs[1].count, 20);
    EXPECT_FALSE(reqs[1].context);
    EXPECT_EQ(reqs[2].link, 2);
    EXPECT_EQ(reqs[2].key, "gold");
    EXPECT_EQ(reqs[2].linkIndex, 0);
    EXPECT_EQ(reqs[1].linkIndex, 1);
    EXPECT_TRUE(reqs[1].finalLink);
    EXPECT_FALSE(reqs[0].finalLink);
}

TEST(StockOrders, SharedInventoryUsesOneLink) {
    std::vector<cs::LinkView> links(2);
    links[0].id        = 1;
    links[0].inventory = "chest";
    links[0].available.add("iron", 10);
    links[1].id        = 2;
    links[1].inventory = "chest";
    links[1].available.add("iron", 10);
    auto reqs = cs::distributeOrder({{"iron", 20}}, links, [](int n) { return n - 1; });
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].link, 2);
    EXPECT_EQ(reqs[0].count, 10);
    EXPECT_FALSE(reqs[0].finalLink); // not fully satisfied
}

TEST(StockOrders, BusyLimits) {
    EXPECT_EQ(cs::busyLimit(cs::RequestType::Player), 50);
    EXPECT_EQ(cs::busyLimit(cs::RequestType::Redstone), 20);
    EXPECT_EQ(cs::busyLimit(cs::RequestType::Restock), 10);
}

TEST(StockGauge, DemandAndRestock) {
    EXPECT_EQ(cs::demandOf(10, true, 64), 10);
    EXPECT_EQ(cs::demandOf(2, false, 64), 128);
    EXPECT_EQ(cs::demandOf(2, false, 16), 32);
    EXPECT_EQ(cs::restockAmount(128, 10, 20, 64, 1000), 98);
    EXPECT_EQ(cs::restockAmount(2000, 0, 0, 64, 10000), 576); // 9 stacks
    EXPECT_EQ(cs::restockAmount(100, 0, 0, 64, 7), 7);
    EXPECT_EQ(cs::restockAmount(10, 5, 20, 64, 100), 0);
}

TEST(StockGauge, PanelSlotsFollowTheFace) {
    using cs::AttachFace;
    using cs::PanelSlot;
    // floor panel facing north: top-left is the north-west quadrant
    auto c = cs::panelCentre(AttachFace::Floor, 2, PanelSlot::TopLeft);
    EXPECT_NEAR(c.x, 0.25, 1e-9);
    EXPECT_NEAR(c.z, 0.25, 1e-9);
    EXPECT_NEAR(c.y, 1.0 / 16, 1e-9);
    EXPECT_EQ(cs::targetedSlot(AttachFace::Floor, 2, {0.8, 0.0, 0.2}), PanelSlot::TopRight);
    EXPECT_EQ(cs::targetedSlot(AttachFace::Floor, 2, {0.2, 0.0, 0.8}), PanelSlot::BottomLeft);
    // wall panel facing south (on the north side of a block): top is up, right is east
    auto w = cs::panelCentre(AttachFace::Wall, 0, PanelSlot::TopRight);
    EXPECT_NEAR(w.y, 0.75, 1e-9);
    EXPECT_NEAR(w.x, 0.75, 1e-9);
    EXPECT_NEAR(w.z, 1.0 / 16, 1e-9);
    EXPECT_EQ(cs::targetedSlot(AttachFace::Wall, 0, {0.8, 0.2, 0.06}), PanelSlot::BottomRight);
    for (int f = 0; f < 4; ++f)
        for (auto face : {AttachFace::Floor, AttachFace::Wall, AttachFace::Ceiling})
            for (int s = 0; s < 4; ++s) {
                auto p = cs::panelCentre(face, f, static_cast<PanelSlot>(s));
                EXPECT_EQ(cs::targetedSlot(face, f, p), static_cast<PanelSlot>(s));
            }
    auto off = cs::supportOffset(AttachFace::Wall, 3); // facing east: support to the west
    EXPECT_EQ(off[0], -1);
    EXPECT_EQ(off[2], 0);
}

TEST(StockGauge, ConnectionIssues) {
    cs::PanelRef a{0, 0, 0, cs::AttachFace::Wall, 0, false, true};
    cs::PanelRef b = a;
    b.x            = 3;
    EXPECT_FALSE(cs::connectionIssue(a, b, false, 0).has_value());
    EXPECT_EQ(*cs::connectionIssue(a, b, true, 0), "create.factory_panel.already_connected");
    EXPECT_EQ(*cs::connectionIssue(a, b, false, 9), "create.factory_panel.cannot_add_more_inputs");
    auto c     = b;
    c.facing2d = 1;
    EXPECT_EQ(*cs::connectionIssue(a, c, false, 0), "create.factory_panel.same_orientation");
    auto d = b;
    d.z    = 1; // wall facing south: z is the normal axis
    EXPECT_EQ(*cs::connectionIssue(a, d, false, 0), "create.factory_panel.same_surface");
    auto e = b;
    e.x    = 16;
    EXPECT_EQ(*cs::connectionIssue(a, e, false, 0), "create.factory_panel.too_far_apart");
    auto f      = b;
    f.restocker = true;
    EXPECT_EQ(*cs::connectionIssue(a, f, false, 0), "create.factory_panel.input_in_restock_mode");
    auto g    = b;
    g.hasItem = false;
    EXPECT_EQ(*cs::connectionIssue(a, g, false, 0), "create.factory_panel.no_item");
}
