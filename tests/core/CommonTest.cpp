#include <gtest/gtest.h>

#include <vector>

#include "core/common/FilterLogic.h"
#include "core/common/Layout.h"
#include "core/common/Protocol.h"
#include "core/common/Recipe.h"
#include "core/common/Transfer.h"
#include "core/common/ValueSettings.h"

using namespace lv::core::common;

// ---- filters -----------------------------------------------------------------------------------

TEST(Filter, AttributeModes) {
    std::vector<AttributeTest> mixed{{true, false}, {false, false}};
    EXPECT_TRUE(combineAttributeTests(mixed, AttributeMode::WhitelistAny));
    EXPECT_FALSE(combineAttributeTests(mixed, AttributeMode::WhitelistAll));
    EXPECT_FALSE(combineAttributeTests(mixed, AttributeMode::Blacklist));

    std::vector<AttributeTest> none{{false, false}, {false, false}};
    EXPECT_FALSE(combineAttributeTests(none, AttributeMode::WhitelistAny));
    EXPECT_FALSE(combineAttributeTests(none, AttributeMode::WhitelistAll));
    EXPECT_TRUE(combineAttributeTests(none, AttributeMode::Blacklist));

    std::vector<AttributeTest> all{{true, false}, {true, false}};
    EXPECT_TRUE(combineAttributeTests(all, AttributeMode::WhitelistAll));
}

TEST(Filter, InvertedAttributes) {
    std::vector<AttributeTest> notEnchanted{{false, true}};
    EXPECT_TRUE(combineAttributeTests(notEnchanted, AttributeMode::WhitelistAny));
    std::vector<AttributeTest> enchanted{{true, true}};
    EXPECT_FALSE(combineAttributeTests(enchanted, AttributeMode::WhitelistAny));
    EXPECT_TRUE(combineAttributeTests(enchanted, AttributeMode::Blacklist));
}

TEST(Filter, ModeNames) {
    for (auto m : {AttributeMode::WhitelistAny, AttributeMode::WhitelistAll, AttributeMode::Blacklist})
        EXPECT_EQ(attributeModeFromName(attributeModeName(m)), m);
    EXPECT_EQ(attributeModeFromName("bogus", AttributeMode::Blacklist), AttributeMode::Blacklist);
}

TEST(Filter, ListCombination) {
    auto second = [](std::size_t i) { return i == 1; };
    auto nothing = [](std::size_t) { return false; };
    EXPECT_TRUE(combineListFilter(3, second, false));
    EXPECT_FALSE(combineListFilter(3, second, true));
    EXPECT_FALSE(combineListFilter(3, nothing, false));
    EXPECT_TRUE(combineListFilter(3, nothing, true));
    EXPECT_EQ(kListFilterSlots, 18);
}

// ---- value settings ----------------------------------------------------------------------------

TEST(ValueBox, SignedRoundTrip) {
    EXPECT_EQ(signedToSettings(-64), (ValueSettings{0, 64}));
    EXPECT_EQ(signedToSettings(16), (ValueSettings{1, 16}));
    EXPECT_EQ(signedFromSettings({0, 64}, 256), -64);
    EXPECT_EQ(signedFromSettings({1, 0}, 256), 1); // zero is not selectable
    EXPECT_EQ(signedFromSettings({1, 999}, 256), 256);
}

TEST(ValueBox, PlainAndOptions) {
    auto b = plainBoard(64, 8, "Value");
    EXPECT_EQ(b.rows.size(), 1u);
    EXPECT_EQ(plainFromSettings({0, 70}, 1, 64), 64);
    EXPECT_EQ(plainFromSettings({0, 0}, 1, 64), 1);
    auto o = optionBoard(3, "Mode");
    EXPECT_EQ(o.maxValue, 2);
    EXPECT_EQ(clampSettings(o, {5, 9}), (ValueSettings{0, 2}));
}

TEST(ValueBox, PointerAndScroll) {
    auto b = signedBoard(256, 32, "cw", "ccw");
    EXPECT_EQ(valueAtFraction(b, 0.0), 0);
    EXPECT_EQ(valueAtFraction(b, 0.5), 128);
    EXPECT_EQ(valueAtFraction(b, 2.0), 256);
    EXPECT_EQ(scrollValue(b, 10, 1, false), 11);
    EXPECT_EQ(scrollValue(b, 10, 1, true), 32);
    EXPECT_EQ(scrollValue(b, 40, -1, true), 32);
    EXPECT_EQ(scrollValue(b, 32, -1, true), 0);
    EXPECT_EQ(scrollValue(b, 250, 3, true), 256);
    EXPECT_EQ(scrollValue(b, 0, -5, false), 0);
}

TEST(ValueBox, HitTest) {
    auto north = faceLocalPoint(2, 8, 8);
    EXPECT_DOUBLE_EQ(north[2], 0.0);
    EXPECT_DOUBLE_EQ(north[0], 0.5);
    auto up = faceLocalPoint(1, 8, 12);
    EXPECT_DOUBLE_EQ(up[1], 1.0);
    EXPECT_DOUBLE_EQ(up[2], 0.25);
    EXPECT_TRUE(testBoxHit({0.5, 0.55, 0.0}, north));
    EXPECT_FALSE(testBoxHit({0.9, 0.9, 0.0}, north));
    auto east = faceLocalPoint(5, 4, 8, 1);
    EXPECT_GT(east[0], 1.0);
    EXPECT_DOUBLE_EQ(east[2], 0.75);
}

// ---- recipes -----------------------------------------------------------------------------------

TEST(Recipe, ParsesCreate6Pressing) {
    auto doc = nlohmann::json::parse(R"({
        "type": "create:pressing",
        "ingredients": [{"tag": "c:ingots/iron"}],
        "results": [{"id": "create:iron_sheet", "count": 1}],
        "processing_time": 150
    })");
    std::string err;
    auto r = parseRecipe(doc, "pressing", "levitate:pressing/iron_ingot", &err);
    ASSERT_TRUE(r) << err;
    ASSERT_EQ(r->ingredients.size(), 1u);
    EXPECT_EQ(r->ingredients[0].tags[0], "c:ingots/iron");
    EXPECT_EQ(r->results[0].id, "levitate:iron_sheet");
    EXPECT_EQ(r->processingTime, 150);
    EXPECT_EQ(r->heat, HeatRequirement::None);
}

TEST(Recipe, ParsesMixingWithFluidsAndHeat) {
    auto doc = nlohmann::json::parse(R"({
        "type": "create:mixing",
        "ingredients": [
            [{"item": "minecraft:copper_ingot"}, {"item": "minecraft:raw_copper"}],
            {"item": "create:zinc_ingot", "count": 2},
            {"type": "fluid_stack", "fluid": "minecraft:lava", "amount": 250},
            {"fluidTag": "c:milk", "amount": 100}
        ],
        "results": [{"item": "create:brass_ingot", "count": 2, "chance": 0.5}, {"fluid": "create:chocolate", "amount": 250}],
        "heatRequirement": "heated",
        "processingTime": 80
    })");
    auto r = parseRecipe(doc, "mixing", "x");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->ingredients.size(), 3u);
    EXPECT_EQ(r->ingredients[0].items.size(), 2u);
    EXPECT_EQ(r->ingredients[1].items[0], "levitate:zinc_ingot");
    ASSERT_EQ(r->fluidIngredients.size(), 2u);
    EXPECT_EQ(r->fluidIngredients[0].fluid, "minecraft:lava");
    EXPECT_EQ(r->fluidIngredients[1].tag, "c:milk");
    EXPECT_EQ(r->fluidResults[0].fluid, "levitate:chocolate");
    EXPECT_FLOAT_EQ(r->results[0].chance, 0.5f);
    EXPECT_EQ(r->heat, HeatRequirement::Heated);
    EXPECT_EQ(r->processingTime, 80);
}

TEST(Recipe, RejectsBadDocuments) {
    std::string err;
    EXPECT_FALSE(parseRecipe(nlohmann::json::array(), "x", "x", &err));
    EXPECT_FALSE(parseRecipe(nlohmann::json::parse(R"({"ingredients": [{"item": "a"}]})"), "x", "x", &err));
    EXPECT_FALSE(parseRecipe(
        nlohmann::json::parse(R"({"ingredients": [{"bogus": 1}], "results": [{"id": "a"}]})"),
        "x",
        "x",
        &err
    ));
    EXPECT_FALSE(parseRecipe(nlohmann::json::parse(R"({"results": [{"id": "a"}], "heat_requirement": "hot"})"), "x", "x", &err));
}

TEST(Recipe, ChanceRolls) {
    ItemResult r{"a", 4, 0.5f};
    std::vector<float> seq{0.1f, 0.9f, 0.4f, 0.6f};
    std::size_t        i = 0;
    EXPECT_EQ(rollResult(r, [&] { return seq[i++]; }), 2);
    EXPECT_EQ(rollResult({"a", 3, 1.0f}, [] { return 0.99f; }), 3);
}

TEST(Recipe, Heat) {
    EXPECT_TRUE(heatSatisfies(HeatRequirement::None, HeatLevel::None));
    EXPECT_FALSE(heatSatisfies(HeatRequirement::Heated, HeatLevel::Smouldering));
    EXPECT_TRUE(heatSatisfies(HeatRequirement::Heated, HeatLevel::Fading));
    EXPECT_TRUE(heatSatisfies(HeatRequirement::Heated, HeatLevel::Seething));
    EXPECT_FALSE(heatSatisfies(HeatRequirement::Superheated, HeatLevel::Kindled));
    EXPECT_TRUE(heatSatisfies(HeatRequirement::Superheated, HeatLevel::Seething));
    EXPECT_EQ(mapId("create:x"), "levitate:x");
    EXPECT_EQ(mapId("minecraft:x"), "minecraft:x");
}

// ---- transfer / keys ---------------------------------------------------------------------------

TEST(Transfer, MergesBeforeFillingEmptySlots) {
    std::vector<SlotView> slots{
        {true, false, 0, 64},
        {false, true, 60, 64},
        {false, false, 10, 64},
        {true, false, 0, 64},
    };
    auto plan = planInsert(slots, 70);
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[0], (std::pair<int, int>{1, 4}));
    EXPECT_EQ(plan[1], (std::pair<int, int>{0, 64}));
    EXPECT_EQ(plan[2], (std::pair<int, int>{3, 2}));
    EXPECT_EQ(plannedTotal(plan), 70);
    std::vector<SlotView> full{{false, false, 1, 64}};
    EXPECT_TRUE(planInsert(full, 5).empty());
}

TEST(Transfer, Keys) {
    EXPECT_EQ(chunkOf(0), 0);
    EXPECT_EQ(chunkOf(15), 0);
    EXPECT_EQ(chunkOf(16), 1);
    EXPECT_EQ(chunkOf(-1), -1);
    EXPECT_EQ(chunkOf(-16), -1);
    EXPECT_EQ(chunkOf(-17), -2);
    EXPECT_EQ(blockDataChunkKey(1, -2, 3), "lv:common:bd:1:-2:3");
    int d, x, z;
    ASSERT_TRUE(parseChunkSuffix("1:-2:3", d, x, z));
    EXPECT_EQ(d, 1);
    EXPECT_EQ(x, -2);
    EXPECT_EQ(z, 3);
    EXPECT_FALSE(parseChunkSuffix("1:2", d, x, z));
    EXPECT_FALSE(parseChunkSuffix("1:2:x", d, x, z));
    int y;
    ASSERT_TRUE(parsePosKey(posKey(-5, 64, 7), x, y, z));
    EXPECT_EQ(x, -5);
    EXPECT_EQ(y, 64);
    EXPECT_EQ(z, 7);
}

// ---- protocol / layout -------------------------------------------------------------------------

TEST(Protocol, Envelope) {
    EXPECT_TRUE(protocolAccepted(kProtocolVersion));
    EXPECT_FALSE(protocolAccepted(kProtocolVersion + 1));
    EXPECT_FALSE(protocolAccepted(0));
    EXPECT_EQ(encodePayload(nullptr), "{}");
    auto back = decodePayload(encodePayload({{"a", 1}}));
    ASSERT_TRUE(back);
    EXPECT_EQ((*back)["a"], 1);
    EXPECT_FALSE(decodePayload("{"));
    EXPECT_TRUE(validChannel("common.hello"));
    EXPECT_FALSE(validChannel("Common.hello"));
    EXPECT_FALSE(validChannel(".x"));
    EXPECT_FALSE(validChannel(""));
}

TEST(Layout, Grid) {
    auto p = centred(320, 240, 100, 50);
    EXPECT_EQ(p, (Rect{110, 95, 100, 50}));
    EXPECT_TRUE(p.contains(110, 95));
    EXPECT_FALSE(p.contains(210, 95));
    EXPECT_EQ(gridCell(0, 0, 9, 18, 10), (Rect{18, 18, 18, 18}));
    EXPECT_EQ(gridIndexAt(0, 0, 9, 18, 18, 19, 19), 10);
    EXPECT_EQ(gridIndexAt(0, 0, 9, 18, 18, 170, 1), -1);
    EXPECT_EQ(gridIndexAt(0, 0, 9, 18, 18, 1, 40), -1);
}
