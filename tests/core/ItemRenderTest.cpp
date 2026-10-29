#include <gtest/gtest.h>

#include "core/itemrender/DisplayItem.h"

using namespace lv::core::itemrender;

TEST(ItemRender, LerpAngleTakesTheShortWay) {
    EXPECT_FLOAT_EQ(lerpAngle(350.f, 10.f, .5f), 360.f);
    EXPECT_FLOAT_EQ(lerpAngle(10.f, 350.f, .5f), 0.f);
    EXPECT_FLOAT_EQ(lerpAngle(0.f, 90.f, .5f), 45.f);
    EXPECT_FLOAT_EQ(lerpAngle(-170.f, 170.f, .5f), -180.f);
}

TEST(ItemRender, LerpTransform) {
    Transform a{0, 1, 2, 0, 0, 0, 1};
    Transform b{2, 3, 4, 90, -90, 180, .5f};
    auto      m = lerp(a, b, .5f);
    EXPECT_DOUBLE_EQ(m.x, 1);
    EXPECT_DOUBLE_EQ(m.y, 2);
    EXPECT_DOUBLE_EQ(m.z, 3);
    EXPECT_FLOAT_EQ(m.yaw, 45);
    EXPECT_FLOAT_EQ(m.pitch, -45);
    EXPECT_FLOAT_EQ(m.scale, .75f);
    EXPECT_TRUE(nearlySame(lerp(a, b, 2.f), b)); // clamped
    EXPECT_TRUE(nearlySame(lerp(a, b, -1.f), a));
}

TEST(ItemRender, Progress) {
    EXPECT_FLOAT_EQ(progress(10, 10, 0), 1.f);
    EXPECT_FLOAT_EQ(progress(10, 10, 4), 0.f);
    EXPECT_FLOAT_EQ(progress(12, 10, 4), .5f);
    EXPECT_FLOAT_EQ(progress(20, 10, 4), 1.f);
    EXPECT_FLOAT_EQ(progress(5, 10, 4), 0.f);
}

TEST(ItemRender, PileCopiesAsCreate) {
    EXPECT_EQ(pileCopies(1), 0);
    EXPECT_EQ(pileCopies(3), 0);
    EXPECT_EQ(pileCopies(4), 1);
    EXPECT_EQ(pileCopies(15), 1);
    EXPECT_EQ(pileCopies(16), 2);
    EXPECT_EQ(pileCopies(64), 3);
    EXPECT_EQ(pileOffset(1), pileOffset(5));
}

TEST(ItemRender, EntryRoundTrip) {
    Entry e;
    e.owner = {1, -2, 3};
    e.slot  = (std::int64_t{1} << 40) + 7;
    e.item  = Json{{"id", "minecraft:stone"}, {"count", 3}};
    e.t     = Transform{1.5, -60.25, 3.75, 90, -90, 10, .5f};
    e.style = Style::Lying;
    e.spin  = 3;
    e.pile  = true;
    e.lerp  = 2;
    e.hide  = 123456789012;
    auto d  = decode(encode(e, true));
    ASSERT_TRUE(d);
    EXPECT_EQ(d->owner, e.owner);
    EXPECT_EQ(d->slot, e.slot);
    EXPECT_EQ(d->item, e.item);
    EXPECT_TRUE(nearlySame(d->t, e.t));
    EXPECT_EQ(d->style, Style::Lying);
    EXPECT_FLOAT_EQ(d->spin, 3);
    EXPECT_TRUE(d->pile);
    EXPECT_EQ(d->lerp, 2);
    EXPECT_EQ(d->hide, e.hide);

    auto moved = decode(encode(e, false));
    ASSERT_TRUE(moved);
    EXPECT_TRUE(moved->item.is_null());

    Entry plain;
    auto  j = encode(plain, true);
    EXPECT_FALSE(j.contains("st"));
    EXPECT_FALSE(j.contains("h"));
    auto p = decode(j);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->style, Style::Facing);
    EXPECT_FLOAT_EQ(p->t.scale, 1);
}

TEST(ItemRender, DecodeRejectsBrokenEntries) {
    EXPECT_FALSE(decode(Json()));
    EXPECT_FALSE(decode(Json{{"o", {1, 2}}, {"s", 0}, {"p", {0, 0, 0}}}));
    EXPECT_FALSE(decode(Json{{"o", {1, 2, 3}}, {"p", {0, 0, 0}}}));
    EXPECT_FALSE(decode(Json{{"o", {1, 2, 3}}, {"s", 0}, {"p", {0, "a", 0}}}));
    EXPECT_FALSE(decode(Json{{"o", {1, 2, 3}}, {"s", 0}, {"p", {0, 0, 0}}, {"r", {0}}}));
    auto e = decode(Json{{"o", {1, 2, 3}}, {"s", 0}, {"p", {0, 0, 0}}, {"t", 100000}});
    ASSERT_TRUE(e);
    EXPECT_EQ(e->lerp, 200);
}

TEST(ItemRender, Keys) {
    auto k = decodeKey(encodeKey({4, 5, -6}, 99));
    ASSERT_TRUE(k);
    EXPECT_EQ(k->first, (std::array<int, 3>{4, 5, -6}));
    EXPECT_EQ(k->second, 99);
    EXPECT_FALSE(decodeKey(Json::array({1, 2, 3})));
    EXPECT_FALSE(decodeKey(Json::array({1, 2, 3, "x"})));
}
