#include <gtest/gtest.h>

#include "core/schematics/Nbt.h"
#include "core/schematics/SafeData.h"

using namespace lv::core::schematics;
using nbt::Compound;
using nbt::Tag;
using Json = nlohmann::json;

namespace {

Tag blockEntity(std::string const& id) {
    Compound c;
    c.set("id", Tag(id));
    c.set("x", Tag(std::int32_t(1)));
    c.set("Items", Tag(std::string("secret")));
    c.set("Command", Tag(std::string("/op everyone")));
    c.set("FrontText", Tag(std::string("hello")));
    return Tag(std::move(c));
}

} // namespace

TEST(SchematicsSafeData, OnlySignsAndBannersKeepVanillaBlockEntityData) {
    EXPECT_FALSE(safeBlockEntity(blockEntity("Chest")).has_value());
    EXPECT_FALSE(safeBlockEntity(blockEntity("CommandBlock")).has_value());
    EXPECT_FALSE(safeBlockEntity(blockEntity("MobSpawner")).has_value());
    EXPECT_FALSE(safeBlockEntity(blockEntity("StructureBlock")).has_value());
    auto sign = safeBlockEntity(blockEntity("Sign"));
    ASSERT_TRUE(sign.has_value());
    ASSERT_NE(sign->asCompound(), nullptr);
    EXPECT_NE(sign->asCompound()->find("FrontText"), nullptr);
    EXPECT_EQ(sign->asCompound()->find("Items"), nullptr);
    EXPECT_EQ(sign->asCompound()->find("Command"), nullptr);
    EXPECT_TRUE(safeBlockEntity(blockEntity("HangingSign")).has_value());
    auto banner = safeBlockEntity(blockEntity("Banner"));
    ASSERT_TRUE(banner.has_value());
    EXPECT_NE(banner->asCompound()->find("Items"), nullptr); // banners are printed whole
    EXPECT_FALSE(safeBlockEntity(Tag(std::string("not a compound"))).has_value());
}

TEST(SchematicsSafeData, SafeNbtBlocksKeepTheirDocumentButRuntimeKeys) {
    Json doc{{"redstone", {{"first", "diamond"}}}, {"kinetics", {{"speed", 16}}}, {"advancements", "owner"}, {"filter", {{"filter", 1}}}};
    EXPECT_TRUE(isSafeNbtBlock("levitate:redstone_link"));
    auto safe = safeBlockData("levitate:redstone_link", doc);
    EXPECT_TRUE(safe.contains("redstone"));
    EXPECT_TRUE(safe.contains("filter"));
    EXPECT_FALSE(safe.contains("kinetics"));
    EXPECT_FALSE(safe.contains("advancements"));
}

TEST(SchematicsSafeData, OtherBlocksKeepOnlyFiltersAndSettings) {
    Json doc{{"depot", {{"items", {1, 2, 3}}}}, {"filter", {{"filter", "gold"}}}};
    EXPECT_FALSE(isSafeNbtBlock("levitate:depot"));
    auto safe = safeBlockData("levitate:depot", doc);
    EXPECT_EQ(safe, (Json{{"filter", {{"filter", "gold"}}}}));
    EXPECT_TRUE(safeBlockData("levitate:depot", Json{{"depot", 1}}).is_null());
    // the threshold switch: its thresholds (PartialSafeNBT), not its measurement
    Json sw{{"redstone", {{"onAbove", 100}, {"offBelow", 10}, {"inverted", true}, {"inStacks", false}, {"current", 50}, {"state", true}}}};
    auto s = safeBlockData("levitate:stockpile_switch", sw);
    EXPECT_EQ(s["redstone"], (Json{{"onAbove", 100}, {"offBelow", 10}, {"inverted", true}, {"inStacks", false}}));
}

TEST(SchematicsSafeData, OperatorOnlyEntities) {
    EXPECT_TRUE(onlyOpCanSetNbt("minecraft:command_block_minecart"));
    EXPECT_TRUE(onlyOpCanSetNbt("minecraft:falling_block"));
    EXPECT_TRUE(onlyOpCanSetNbt("minecraft:npc"));
    EXPECT_FALSE(onlyOpCanSetNbt("minecraft:armor_stand"));
    Compound c;
    c.set("identifier", Tag(std::string("minecraft:armor_stand")));
    EXPECT_EQ(entityIdOf(Tag(std::move(c))), "minecraft:armor_stand");
    EXPECT_EQ(entityIdOf(Tag(std::int32_t(3))), "");
}
