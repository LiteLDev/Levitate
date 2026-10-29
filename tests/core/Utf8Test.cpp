#include <gtest/gtest.h>

#include "core/common/Utf8.h"

using lv::core::utf8Prefix;

TEST(Utf8, PrefixCountsCharactersNotBytes) {
    EXPECT_EQ(utf8Prefix("Mill", 2), "Mi");
    EXPECT_EQ(utf8Prefix("Mill", 10), "Mill");
    EXPECT_EQ(utf8Prefix("", 3), "");
    // 磨坊 = two 3-byte characters
    EXPECT_EQ(utf8Prefix("\xE7\xA3\xA8\xE5\x9D\x8A", 1), "\xE7\xA3\xA8");
    EXPECT_EQ(utf8Prefix("a\xF0\x9F\x98\x80" "b", 2), "a\xF0\x9F\x98\x80");
}

TEST(Utf8, PrefixNeverSplitsACharacter) {
    // a truncated 3-byte sequence at the end is dropped
    EXPECT_EQ(utf8Prefix("ab\xE7\xA3", 5), "ab");
    // a stray continuation byte counts as one character
    EXPECT_EQ(utf8Prefix("\x80" "ab", 2), "\x80" "a");
}
