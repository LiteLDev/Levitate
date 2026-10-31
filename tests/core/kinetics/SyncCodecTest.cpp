#include <gtest/gtest.h>

#include "core/kinetics/SyncCodec.h"

using namespace lv::core::kinetics::sync;

TEST(KineticsSync, RoundTrip) {
    Batch b;
    b.dim     = 1;
    b.reset   = true;
    b.cx      = -2;
    b.cz      = 7;
    b.entries = {
        Entry{-20, 64, 113, 16.f, 4, false},
        Entry{-19, 64, 113, -0.125f, -1, false},
        Entry{-18, -60, 120, 0, -1, true},
    };
    auto text = encode(b);
    EXPECT_EQ(text, "1;R;-2;7|-20,64,113,16,4;-19,64,113,-0.125,-1;-18,-60,120,-,-1");
    auto back = decode(text);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, b);
}

TEST(KineticsSync, OverstressedFlag) {
    Batch b{0, false, 0, 0, {Entry{1, 2, 3, 0, 2, false, true}, Entry{4, 5, 6, 8, -1, false, false}}};
    auto  text = encode(b);
    EXPECT_EQ(text, "0;D;0;0|1,2,3,0,2,o;4,5,6,8,-1");
    auto back = decode(text);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, b);
    EXPECT_FALSE(decode("0;D;0;0|1,2,3,0,2,x"));
}

TEST(KineticsSync, EmptyBatch) {
    Batch b{0, true, 0, 0, {}};
    auto  back = decode(encode(b));
    ASSERT_TRUE(back);
    EXPECT_TRUE(back->reset);
    EXPECT_TRUE(back->entries.empty());
}

TEST(KineticsSync, RejectsGarbage) {
    EXPECT_FALSE(decode(""));
    EXPECT_FALSE(decode("hello"));
    EXPECT_FALSE(decode("0;X;0;0|"));
    EXPECT_FALSE(decode("0;D;0;0|1,2,3"));
    EXPECT_FALSE(decode("0;D;0;0|1,2,3,fast,-1"));
    EXPECT_FALSE(decode("0;D;0;0|1,2,3,4,9"));
}

TEST(KineticsSync, GroupsByChunk) {
    std::vector<Entry> entries = {
        Entry{0, 0, 0, 1},
        Entry{15, 0, 15, 2},
        Entry{16, 0, 0, 3},
        Entry{-1, 0, 0, 4},
        Entry{-16, 0, -17, 5},
    };
    auto batches = byChunk(0, entries, true);
    ASSERT_EQ(batches.size(), 4u);
    EXPECT_TRUE(batches[0].reset);
    EXPECT_FALSE(batches[1].reset);
    EXPECT_EQ(batches[0].cx, -1);
    EXPECT_EQ(batches[0].cz, -2);
    EXPECT_EQ(chunkOf(-1), -1);
    EXPECT_EQ(chunkOf(-16), -1);
    EXPECT_EQ(chunkOf(-17), -2);
    EXPECT_EQ(chunkOf(15), 0);
    auto empty = byChunk(2, {}, true);
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_TRUE(empty[0].reset);
    EXPECT_TRUE(byChunk(2, {}, false).empty());
}
