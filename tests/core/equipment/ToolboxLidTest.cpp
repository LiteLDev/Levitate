#include <gtest/gtest.h>

#include "core/equipment/ToolboxLid.h"

using namespace lv::core::equipment;

TEST(ToolboxLid, LidLinearDrawersExponential) {
    ToolboxLid a;
    a.tick(true);
    EXPECT_FLOAT_EQ(a.lid, .2f);
    EXPECT_FLOAT_EQ(a.drawers, .2f);
    a.tick(true);
    EXPECT_FLOAT_EQ(a.lid, .4f);
    EXPECT_FLOAT_EQ(a.drawers, .36f); // .2 + .8 * .2
    for (int i = 0; i < 3; ++i) a.tick(true);
    EXPECT_FLOAT_EQ(a.lid, 1.f); // open after 5 ticks
    EXPECT_FALSE(a.settled(true)); // the drawers are still sliding
    for (int i = 0; i < 40; ++i) a.tick(true);
    EXPECT_TRUE(a.settled(true));
    // closing
    a.tick(false);
    EXPECT_FLOAT_EQ(a.lid, .8f);
    EXPECT_FLOAT_EQ(a.drawers, .8f);
    for (int i = 0; i < 4; ++i) a.tick(false);
    EXPECT_FLOAT_EQ(a.lid, 0.f);
    for (int i = 0; i < 40; ++i) a.tick(false);
    EXPECT_TRUE(a.settled(false));
}

TEST(ToolboxLid, RendererValues) {
    EXPECT_FLOAT_EQ(lidDegrees(1.f), 135.f);
    EXPECT_FLOAT_EQ(lidDegrees(.5f), 67.5f);
    EXPECT_FLOAT_EQ(drawerOut(1.f, false), .35f);
    EXPECT_FLOAT_EQ(drawerOut(1.f, true), .175f);
    EXPECT_FLOAT_EQ(drawerOut(0.f, false), 0.f);
}

TEST(ToolboxLid, DyeingMatchesOneToolboxAndOneDye) {
    EXPECT_EQ(dyeingToolboxSlot({"", "levitate:brown_toolbox", "", "minecraft:red_dye"}), 1);
    EXPECT_EQ(dyeingToolboxSlot({"minecraft:lime_dye", "", "", "", "levitate:light_blue_toolbox"}), 4);
    EXPECT_FALSE(dyeingToolboxSlot({"levitate:brown_toolbox"}));                                   // no dye
    EXPECT_FALSE(dyeingToolboxSlot({"minecraft:red_dye"}));                                         // no toolbox
    EXPECT_FALSE(dyeingToolboxSlot({"levitate:brown_toolbox", "minecraft:red_dye", "minecraft:red_dye"})); // two dyes
    EXPECT_FALSE(dyeingToolboxSlot({"levitate:brown_toolbox", "levitate:red_toolbox", "minecraft:red_dye"}));
    EXPECT_FALSE(dyeingToolboxSlot({"levitate:brown_toolbox", "minecraft:red_dye", "minecraft:stick"})); // anything else
    EXPECT_FALSE(dyeingToolboxSlot({"levitate:_toolbox", "minecraft:red_dye"}));
}
