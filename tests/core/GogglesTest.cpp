#include <gtest/gtest.h>

#include "core/goggles/Rotation.h"
#include "core/goggles/Tooltip.h"

using namespace lv::core::goggles;

namespace {
constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;
constexpr int kX = 0, kY = 1, kZ = 2;
} // namespace

// ---- rotation (IWrenchable.getRotatedBlockState) -------------------------------------------------

TEST(GogglesRotation, Clockwise) {
    EXPECT_EQ(clockwise(kNorth, kY), kEast);
    EXPECT_EQ(clockwise(kEast, kY), kSouth);
    EXPECT_EQ(clockwise(kWest, kY), kNorth);
    EXPECT_EQ(clockwise(kUp, kY), kUp); // on the axis: unchanged
    EXPECT_EQ(clockwise(kDown, kX), kSouth);
    EXPECT_EQ(clockwise(kSouth, kX), kUp);
    EXPECT_EQ(clockwise(kUp, kX), kNorth);
    EXPECT_EQ(clockwise(kNorth, kX), kDown);
    EXPECT_EQ(clockwise(kDown, kZ), kWest);
    EXPECT_EQ(clockwise(kWest, kZ), kUp);
    EXPECT_EQ(clockwise(kUp, kZ), kEast);
    EXPECT_EQ(clockwise(kEast, kZ), kDown);
}

TEST(GogglesRotation, AxisCyclesAroundClickedFace) {
    // shaft along x clicked on top: east turns to south -> z
    EXPECT_EQ(rotatedStates({{kAxis, kX}}, kUp).at(kAxis), kZ);
    // along z clicked on top: south -> west -> x
    EXPECT_EQ(rotatedStates({{kAxis, kZ}}, kUp).at(kAxis), kX);
    // along y clicked on top: unchanged
    EXPECT_EQ(rotatedStates({{kAxis, kY}}, kDown).at(kAxis), kY);
    // along x clicked on the north face (z axis): east -> down -> y
    EXPECT_EQ(rotatedStates({{kAxis, kX}}, kNorth).at(kAxis), kY);
    // along y clicked on the east face (x axis): up -> north -> z
    EXPECT_EQ(rotatedStates({{kAxis, kY}}, kEast).at(kAxis), kZ);
}

TEST(GogglesRotation, FacingTurnsAboutTheClickedAxis) {
    EXPECT_EQ(rotatedStates({{kFacing, kNorth}}, kUp).at(kFacing), kEast);
    EXPECT_EQ(rotatedStates({{kFacing, kUp}}, kUp).at(kFacing), kUp); // facing along the clicked axis
    EXPECT_EQ(rotatedStates({{kFacing, kUp}}, kEast).at(kFacing), kNorth);
    EXPECT_EQ(rotatedStates({{kVanillaFacing, kDown}}, kSouth).at(kVanillaFacing), kWest);
    // other states are kept
    auto r = rotatedStates({{kFacing, kNorth}, {"levitate:powered", 1}}, kUp);
    EXPECT_EQ(r.at("levitate:powered"), 1);
}

TEST(GogglesRotation, AxisAlongFirstCoordinate) {
    // facing on the clicked axis: only the flag toggles
    auto a = rotatedStates({{kFacing, kUp}, {kAxisAlongFirst, 0}}, kUp);
    EXPECT_EQ(a.at(kFacing), kUp);
    EXPECT_EQ(a.at(kAxisAlongFirst), 1);
    // horizontal turn on top also toggles it
    auto b = rotatedStates({{kFacing, kNorth}, {kAxisAlongFirst, 1}}, kUp);
    EXPECT_EQ(b.at(kFacing), kEast);
    EXPECT_EQ(b.at(kAxisAlongFirst), 0);
    // turn about a horizontal axis keeps it
    auto c = rotatedStates({{kFacing, kNorth}, {kAxisAlongFirst, 1}}, kEast);
    EXPECT_EQ(c.at(kFacing), kDown);
    EXPECT_EQ(c.at(kAxisAlongFirst), 1);
}

TEST(GogglesRotation, HorizontalStatesOnlyTurnOnTopAndBottom) {
    EXPECT_EQ(rotatedStates({{kHorizontalFacing, 2}}, kUp).at(kHorizontalFacing), 3);   // north -> east
    EXPECT_EQ(rotatedStates({{kHorizontalFacing, 3}}, kDown).at(kHorizontalFacing), 0); // east -> south
    EXPECT_EQ(rotatedStates({{kHorizontalFacing, 2}}, kNorth).at(kHorizontalFacing), 2); // side: no change
    EXPECT_EQ(rotatedStates({{kHorizontalAxis, kX}}, kUp).at(kHorizontalAxis), kZ);
    EXPECT_EQ(rotatedStates({{kHorizontalAxis, kZ}}, kUp).at(kHorizontalAxis), kX);
    // a horizontal axis block that also has an axis state turns that on the sides
    auto r = rotatedStates({{kHorizontalAxis, kX}, {kAxis, kX}}, kNorth);
    EXPECT_EQ(r.at(kHorizontalAxis), kX);
    EXPECT_EQ(r.at(kAxis), kY);
}

TEST(GogglesRotation, NothingToRotate) {
    States s{{"levitate:powered", 0}};
    EXPECT_EQ(rotatedStates(s, kUp), s);
    EXPECT_EQ(rotatedStates({{kAxis, kX}}, 9).at(kAxis), kX);
}

// ---- text helpers --------------------------------------------------------------------------------

TEST(GogglesTooltip, NumberFormat) {
    EXPECT_EQ(formatNumber(0), "0");
    EXPECT_EQ(formatNumber(-0.000001), "0");
    EXPECT_EQ(formatNumber(16384), "16,384");
    EXPECT_EQ(formatNumber(1234.5), "1,234.5");
    EXPECT_EQ(formatNumber(2.125), "2.13");
    EXPECT_EQ(formatNumber(0.25), "0.25");
    EXPECT_EQ(formatNumber(-64), "-64");
    EXPECT_EQ(formatNumber(1000000), "1,000,000");
}

TEST(GogglesTooltip, ProgressBarAndIndent) {
    EXPECT_EQ(progressBar(3, 1), " █▒▒ ");
    EXPECT_EQ(progressBar(3, 3), " ███ ");
    EXPECT_EQ(forGoggles("Kinetic Stats:"), "    Kinetic Stats:");
    EXPECT_EQ(forGoggles("x", 1), "     x");
}

TEST(GogglesTooltip, Width) {
    EXPECT_EQ(textWidth("ab"), 12);
    EXPECT_EQ(textWidth("§7ab"), 12);
    EXPECT_EQ(textWidth("_a_"), 6);
    EXPECT_EQ(textWidth("i l"), 2 + 4 + 3);
    EXPECT_EQ(textWidth("应力"), 18);
    EXPECT_EQ(stripFormatting("§7a§fb"), "ab");
}

TEST(GogglesTooltip, CutTextHighlights) {
    auto lines = cutText("It is _overstressed_ now.", "§7", "§f");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "§7§7It is §foverstressed§7 now.");
}

TEST(GogglesTooltip, CutTextWrapsAndCarriesHighlight) {
    std::string text = "aaaa _bbbb cccc_ dddd";
    auto        lines = cutText(text, "§7", "§f", 0, 60); // words of 28 px: two fit on a line
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "§7§7aaaa §fbbbb ");
    EXPECT_EQ(lines[1], "§7§fcccc§7 dddd");
}

TEST(GogglesTooltip, CutTextCjkAndIndent) {
    auto lines = cutText("显然此_应力_网络", "§7", "§f", 1, 40); // 9 px per character
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "§7 §7显然此§f应");
    EXPECT_EQ(lines[1], "§7 §f力§7网络");
}

TEST(GogglesTooltip, CutTextLongWordKeepsItsLine) {
    auto lines = cutText("abcdefghij klm", "§7", "§f", 0, 30);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "§7§7abcdefghij ");
    EXPECT_EQ(lines[1], "§7§7klm");
}

TEST(GogglesTooltip, ItemDescription) {
    DescriptionText t;
    t.summary    = {"Multi-purpose tool."};
    t.behaviours = {{"When used on Blocks", "_Rotates components_."}};
    auto plain   = describeItem(t, false, false);
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_EQ(plain[0], "§8Hold [§7Shift§8] for Summary");

    auto shift = describeItem(t, true, false);
    ASSERT_EQ(shift.size(), 6u);
    EXPECT_EQ(shift[0], "§8Hold [§fShift§8] for Summary");
    EXPECT_EQ(shift[1], "");
    EXPECT_EQ(shift[2], "§p§pMulti-purpose tool.");
    EXPECT_EQ(shift[3], "");
    EXPECT_EQ(shift[4], "§7When used on Blocks");
    EXPECT_EQ(shift[5], "§p §eRotates components§p.");

    // no controls: Ctrl shows the plain lines
    EXPECT_EQ(describeItem(t, false, true), plain);
    // no summary: nothing
    EXPECT_TRUE(describeItem(DescriptionText{}, true, false).empty());
}

TEST(GogglesTooltip, ItemDescriptionWithControls) {
    DescriptionText t;
    t.summary = {"S"};
    t.actions = {{"R-Click", "Does it"}};
    auto plain = describeItem(t, false, false);
    ASSERT_EQ(plain.size(), 2u);
    EXPECT_EQ(plain[1], "§8Hold [§7Ctrl§8] for Controls");
    auto ctrl = describeItem(t, false, true);
    ASSERT_EQ(ctrl.size(), 5u);
    EXPECT_EQ(ctrl[1], "§8Hold [§fCtrl§8] for Controls");
    EXPECT_EQ(ctrl[2], "");
    EXPECT_EQ(ctrl[3], "§7R-Click");
}
