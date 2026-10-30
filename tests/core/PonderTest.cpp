#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>

#include "core/ponder/Scene.h"

using namespace lv::core::ponder;
namespace fs = std::filesystem;

namespace {

Json minimal() {
    return Json::parse(R"({
        "id": "test_scene",
        "size": [5, 5],
        "blocks": [
            {"area": [[0, 1, 2], [4, 1, 2]], "id": "levitate:shaft", "states": {"levitate:axis": 0}},
            {"at": [2, 2, 2], "id": "levitate:cogwheel"}
        ],
        "timeline": [
            {"show": "base", "from": "up"},
            {"idle": 10},
            {"show": [[0, 1, 2], [4, 1, 2]], "as": "row"},
            {"idle": 20},
            {"rotate": [[0, 1, 2], [4, 1, 2]], "rpm": 16},
            {"text": 1, "at": [2.5, 1.5, 2.5], "ticks": 40, "keyframe": true, "color": "green"},
            {"input": [2.5, 2, 2.5], "action": "right", "item": "levitate:wrench", "ticks": 30},
            {"idle": 50},
            {"move": "row", "by": [0, 1, 0], "ticks": 20},
            {"set": [2, 2, 2], "id": "minecraft:stone"},
            {"text": 2, "ticks": 30},
            {"idle": 30}
        ]
    })");
}

fs::path repoRoot() {
    // tests/core/PonderTest.cpp -> repo
    fs::path p = fs::path(__FILE__).parent_path().parent_path().parent_path();
    if (fs::exists(p / "assets" / "ponder")) return p;
    for (fs::path c = fs::current_path(); !c.empty() && c != c.root_path(); c = c.parent_path()) {
        if (fs::exists(c / "assets" / "ponder")) return c;
    }
    return p;
}

Json load(fs::path const& p) {
    std::ifstream in(p);
    return Json::parse(in, nullptr, false);
}

std::map<std::string, std::string> langOf(std::string const& locale) {
    std::map<std::string, std::string> out;
    for (auto const& e : fs::directory_iterator(repoRoot() / "assets" / "lang" / locale)) {
        if (e.path().extension() != ".json") continue;
        auto j = load(e.path());
        for (auto const& [k, v] : j.items()) out[k] = v.is_string() ? v.get<std::string>() : "";
    }
    return out;
}

} // namespace

TEST(Ponder, ParsesAMinimalScene) {
    auto r = parseScene(minimal());
    ASSERT_TRUE(r.scene) << (r.errors.empty() ? "" : r.errors[0]);
    auto const& s = *r.scene;
    EXPECT_EQ(s.blocks.size(), 6u);
    EXPECT_EQ(s.length(), 110);
    EXPECT_EQ(s.keyframes(), std::vector<int>({30}));
    EXPECT_EQ(s.textNumbers(), std::vector<int>({1, 2}));
    EXPECT_EQ(s.textKey(2), "create.ponder.test_scene.text_2");
    EXPECT_EQ(s.headerKey(), "create.ponder.test_scene.header");
    auto ids = s.blockIds();
    EXPECT_EQ(ids, std::vector<std::string>({"levitate:cogwheel", "levitate:shaft", "minecraft:stone"}));
    // the input keeps its held item
    bool found = false;
    for (auto const& in : s.timeline)
        if (in.op == Op::Input) found = in.item == "levitate:wrench";
    EXPECT_TRUE(found);
}

TEST(Ponder, RejectsBadScenes) {
    auto bad = [](auto mutate) {
        auto j = minimal();
        mutate(j);
        return parseScene(j);
    };
    EXPECT_FALSE(bad([](Json& j) { j["timeline"].push_back({{"fly", 1}}); }).scene);
    EXPECT_FALSE(bad([](Json& j) { j["timeline"].push_back({{"show", Json::array({9, 1, 1})}}); }).scene); // outside
    EXPECT_FALSE(bad([](Json& j) { j["timeline"].push_back({{"move", "nope"}, {"by", {1, 0, 0}}}); }).scene);
    EXPECT_FALSE(bad([](Json& j) { j["blocks"].push_back({{"at", {1, 1, 1}}, {"id", "shaft"}}); }).scene); // namespace
    EXPECT_FALSE(bad([](Json& j) { j["timeline"].push_back({{"text", 3}, {"colour", "red"}}); }).scene); // unknown key
    EXPECT_FALSE(bad([](Json& j) { j["unknown"] = 1; }).scene);
    EXPECT_FALSE(bad([](Json& j) { j["timeline"] = Json::array({Json{{"show", "base"}}}); }).scene); // no length
    EXPECT_FALSE(bad([](Json& j) { j["blocks"][0]["states"] = {{"levitate:axis", 1.5}}; }).scene);
}

TEST(Ponder, EntitiesAndParticles) {
    auto j = minimal();
    j["timeline"] = Json::parse(R"([
        {"show": "base"},
        {"entity": "minecraft:minecart", "at": [0.5, 1, 2.5], "to": [4.5, 1, 2.5], "ticks": 40, "yaw": 90, "yaw_to": 180},
        {"particles": "flame", "at": [2.5, 1.5, 2.5], "ticks": 20, "rate": 0.5, "spread": [0.1, 0, 0.1], "motion": [0, 0.05, 0]},
        {"idle": 50}
    ])");
    auto r = parseScene(j);
    ASSERT_TRUE(r.scene) << (r.errors.empty() ? "" : r.errors[0]);
    auto st = evaluate(*r.scene, 10);
    ASSERT_EQ(st.entities.size(), 1u);
    EXPECT_EQ(st.entities[0].id, "minecraft:minecart");
    EXPECT_EQ(st.entities[0].instance, 1);
    EXPECT_FLOAT_EQ(st.entities[0].pos.x, 1.5f); // a quarter of the way
    EXPECT_FLOAT_EQ(st.entities[0].yaw, 112.5f);
    ASSERT_EQ(st.particles.size(), 1u);
    EXPECT_EQ(st.particles[0].type, "flame");
    EXPECT_FLOAT_EQ(st.particles[0].rate, 0.5f);
    EXPECT_FLOAT_EQ(st.particles[0].age, 10.f);
    EXPECT_FLOAT_EQ(st.particles[0].motion.y, 0.05f);
    EXPECT_FLOAT_EQ(st.particles[0].spread.y, 0.f);
    auto later = evaluate(*r.scene, 30);
    EXPECT_EQ(later.entities.size(), 1u);
    EXPECT_TRUE(later.particles.empty()); // the emitter ran for 20 ticks
    EXPECT_TRUE(evaluate(*r.scene, 45).entities.empty());

    auto bad = [&](Json in) {
        auto k        = minimal();
        k["timeline"] = Json::array({Json{{"show", "base"}}, in, Json{{"idle", 10}}});
        return parseScene(k).scene.has_value();
    };
    EXPECT_FALSE(bad({{"particles", "fireworks"}, {"at", {1, 1, 1}}}));
    EXPECT_FALSE(bad({{"particles", "flame"}, {"at", {1, 1, 1}}, {"rate", 0}}));
    EXPECT_FALSE(bad({{"entity", "pig"}, {"at", {1, 1, 1}}}));
    EXPECT_FALSE(bad({{"entity", "minecraft:pig"}}));
    EXPECT_FALSE(bad({{"entity", "minecraft:pig"}, {"at", {1, 1, 1}}, {"yaw", "north"}}));
    EXPECT_TRUE(bad({{"entity", "minecraft:pig"}, {"at", {1, 1, 1}}}));
}

TEST(Ponder, EvaluatesShowFadeAndRotation) {
    auto s = *parseScene(minimal()).scene;
    auto at0 = evaluate(s, 0);
    EXPECT_TRUE(at0.blocks.empty()); // the base starts fading in: alpha 0
    auto at10 = evaluate(s, 10);
    std::size_t base = 0, shafts = 0;
    for (auto const& b : at10.blocks) {
        if (b.base) ++base;
        if (b.block.id == "levitate:shaft") ++shafts;
    }
    EXPECT_EQ(base, 25u - 0u); // the floor below the shafts is still floor (y = 0)
    EXPECT_EQ(shafts, 0u);     // shown at 10, alpha 0
    auto at40 = evaluate(s, 40);
    float angle = 0;
    for (auto const& b : at40.blocks) {
        if (b.block.id == "levitate:shaft") {
            EXPECT_FLOAT_EQ(b.alpha, 1.f);
            EXPECT_FLOAT_EQ(b.rpm, 16.f);
            angle = b.angle;
        }
    }
    EXPECT_NEAR(angle, 16 * 0.3f * 10, 1e-3); // 10 ticks at 16 RPM
    EXPECT_EQ(at40.texts.size(), 1u);
    EXPECT_EQ(at40.texts[0].text, 1);
    EXPECT_EQ(at40.texts[0].color, Palette::Green);
    EXPECT_EQ(at40.inputs.size(), 1u);
    EXPECT_EQ(at40.inputs[0].item, "levitate:wrench");
    // the cogwheel was never shown
    for (auto const& b : at40.blocks) EXPECT_NE(b.block.id, "levitate:cogwheel");
}

TEST(Ponder, EvaluatesSectionsAndSet) {
    auto s   = *parseScene(minimal()).scene;
    auto end = evaluate(s, 109);
    bool stone = false;
    for (auto const& b : end.blocks) {
        if (b.block.id == "levitate:shaft") EXPECT_NEAR(b.offset.y, 1.f, 1e-4);
        if (b.block.id == "minecraft:stone") stone = true;
    }
    EXPECT_TRUE(stone);
    auto mid = evaluate(s, 90);
    for (auto const& b : mid.blocks)
        if (b.block.id == "levitate:shaft") EXPECT_NEAR(b.offset.y, 0.5f, 1e-4);
    ASSERT_EQ(end.texts.size(), 1u);
    EXPECT_FALSE(end.texts[0].hasPoint);
}

TEST(Ponder, ShowHideSetEdgeCases) {
    auto j = Json::parse(R"({
        "id": "edge", "size": [5, 5],
        "blocks": [{"area": [[0, 1, 0], [4, 1, 0]], "id": "levitate:shaft"},
                   {"at": [2, 2, 2], "id": "levitate:steam_engine", "states": {"minecraft:block_face": "north"}}],
        "timeline": [
            {"show": [[0, 1, 0], [4, 1, 0]]}, {"idle": 20},
            {"rotate": [[0, 1, 0], [4, 1, 0]], "rpm": 32},
            {"show": {"union": [[[0, 1, 0], [4, 1, 0]], [2, 2, 2]]}}, {"idle": 5},
            {"hide": [[0, 1, 0], [1, 1, 0]]}, {"idle": 20},
            {"hide": [[0, 1, 0], [4, 1, 0]]}, {"idle": 5},
            {"set": [4, 1, 0], "id": "levitate:cogwheel"}, {"idle": 5}
        ]})");
    auto r = parseScene(j);
    ASSERT_TRUE(r.scene) << (r.errors.empty() ? "" : r.errors[0]);
    auto const& s = *r.scene;
    EXPECT_EQ(s.blocks.at({2, 2, 2}).strings.at("minecraft:block_face"), "north");
    // shown again while visible: stays fully visible
    for (auto const& b : evaluate(s, 22).blocks)
        if (b.pos == (Vec3i{3, 1, 0})) EXPECT_FLOAT_EQ(b.alpha, 1.f);
    // a second hide does not bring the first hidden blocks back
    for (auto const& b : evaluate(s, 47).blocks) EXPECT_FALSE(b.pos == (Vec3i{0, 1, 0})) << "hidden block came back";
    // a placed block starts still
    bool cog = false;
    for (auto const& b : evaluate(s, 54).blocks) {
        if (b.block.id == "levitate:cogwheel") {
            cog = true;
            EXPECT_FLOAT_EQ(b.rpm, 0.f);
        }
    }
    EXPECT_TRUE(cog);
}

TEST(Ponder, RejectsHugeAreasCheaply) {
    auto j = minimal();
    j["blocks"].push_back({{"area", Json::array({Json::array({0, 1, 0}), Json::array({100000, 100, 100000})})}, {"id", "minecraft:stone"}});
    EXPECT_FALSE(parseScene(j).scene); // returns at once instead of expanding the box
}

TEST(Ponder, HoldProgress) {
    float p     = 0;
    int   ticks = 0;
    while (p < 1 && ticks < 100) {
        p = holdProgressStep(p, true);
        ++ticks;
    }
    EXPECT_FLOAT_EQ(p, 1.f);
    EXPECT_GE(ticks, 8);
    EXPECT_LE(ticks, 14);
    EXPECT_FLOAT_EQ(holdProgressStep(0.5f, false), 0.45f);
    EXPECT_FLOAT_EQ(holdProgressStep(0.01f, false), 0.f);
}

TEST(Ponder, WrapsText) {
    auto width = [](std::string const& s) { return static_cast<float>(utf8Boundaries(s).size() - 1) * 6; };
    auto lines = wrapText("one two three four five", 60, width);
    ASSERT_GE(lines.size(), 2u);
    for (auto const& l : lines) EXPECT_LE(width(l), 60.f) << l;
    EXPECT_EQ(lines[0], "one two");
    auto cjk = wrapText("传动杆可以把旋转力传递到很远的地方", 36, width);
    EXPECT_GE(cjk.size(), 3u);
    for (auto const& l : cjk) EXPECT_LE(width(l), 36.f);
    EXPECT_EQ(wrapText("a\nb", 100, width), std::vector<std::string>({"a", "b"}));
}

TEST(Ponder, NormalisesIds) {
    EXPECT_EQ(normaliseId("shaft"), "levitate:shaft");
    EXPECT_EQ(normaliseId("minecraft:chest"), "minecraft:chest");
}

// ---- the shipped data ------------------------------------------------------------------------

TEST(PonderAssets, EverySceneParsesAndHasText) {
    auto root = repoRoot();
    ASSERT_TRUE(fs::exists(root / "assets" / "ponder" / "index.json")) << root.string();
    Library lib;
    auto    indexErrors = lib.setIndex(load(root / "assets" / "ponder" / "index.json"));
    EXPECT_TRUE(indexErrors.empty()) << indexErrors[0];

    auto en = langOf("en_US");
    auto zh = langOf("zh_CN");
    std::set<std::string> sceneKeys;
    int                   files = 0;
    for (auto const& e : fs::directory_iterator(root / "assets" / "ponder" / "scenes")) {
        ++files;
        auto j = load(e.path());
        ASSERT_FALSE(j.is_discarded()) << e.path().string() << ": not JSON";
        auto r = parseScene(j);
        for (auto const& err : r.errors) ADD_FAILURE() << e.path().filename().string() << ": " << err;
        if (!r.scene) continue;
        auto const& s = *r.scene;
        EXPECT_EQ(e.path().stem().string(), s.id) << "file name must be the scene id";
        std::vector<std::string> keys{s.headerKey()};
        auto                     nums = s.textNumbers();
        for (std::size_t i = 0; i < nums.size(); ++i) {
            EXPECT_EQ(nums[i], static_cast<int>(i) + 1) << s.id << ": text numbers must be 1..n";
            keys.push_back(s.textKey(nums[i]));
        }
        for (auto const& k : keys) {
            EXPECT_FALSE(en[k].empty()) << "en_US misses " << k;
            EXPECT_FALSE(zh[k].empty()) << "zh_CN misses " << k;
            sceneKeys.insert(k);
        }
        lib.add(s);
    }
    EXPECT_GE(files, 167);
    auto missing = lib.missingScenes();
    for (auto const& m : missing) ADD_FAILURE() << "index scene without a file: " << m;
    // no orphan scene text
    for (auto const* lang : {&en, &zh}) {
        for (auto const& [k, v] : *lang) {
            if (k.rfind("create.ponder.", 0) != 0) continue;
            if (k.find(".header") == std::string::npos && k.find(".text_") == std::string::npos) continue;
            EXPECT_TRUE(sceneKeys.contains(k)) << "unused scene text " << k;
        }
    }
    // every scene is reachable from a component
    std::set<std::string> reachable;
    for (auto const& c : lib.allComponents())
        for (auto const& s : c.scenes) reachable.insert(s);
    for (auto const& [id, s] : lib.scenes()) EXPECT_TRUE(reachable.contains(id)) << id << " is in no component";
}

TEST(PonderAssets, IndexCoversCreatesPonderLessons) {
    Library lib;
    lib.setIndex(load(repoRoot() / "assets" / "ponder" / "index.json"));
    // a sample of Create 6.0.10's components and their lessons, in Create's order
    auto expectScenes = [&](std::string const& comp, std::vector<std::string> scenes) {
        auto const* c = lib.component(comp);
        ASSERT_NE(c, nullptr) << comp;
        EXPECT_EQ(c->scenes, scenes) << comp;
    };
    expectScenes("levitate:cogwheel", {"cogwheel", "cog_speedup", "cogwheel_casing"});
    expectScenes("levitate:large_cogwheel", {"cog_speedup", "large_cogwheel", "cogwheel_casing"});
    expectScenes("levitate:basin", {"basin", "mechanical_mixer", "mechanical_press_compacting"});
    expectScenes("levitate:brass_funnel", {"brass_funnel", "funnel_intro", "funnel_direction", "funnel_compat", "funnel_redstone", "funnel_transfer"});
    expectScenes("levitate:deployer", {"deployer", "deployer_modes", "deployer_processing", "deployer_redstone", "deployer_contraption"});
    expectScenes("levitate:track_station", {"train_assembly", "auto_schedule"});
    expectScenes("levitate:creative_fluid_tank", {"creative_fluid_tank", "fluid_tank_sizes"});
    EXPECT_GE(lib.allComponents().size(), 150u);
    std::set<std::string> tagIds;
    for (auto const& t : lib.tags()) tagIds.insert(t.id);
    for (auto const* t : {"kinetic_relays", "kinetic_sources", "kinetic_appliances", "fluids", "logistics", "high_logistics", "redstone", "decoration",
                          "creative", "movement_anchor", "contraption_actor", "contraption_assembly", "windmill_sails", "arm_targets",
                          "train_related", "display_sources", "display_targets", "threshold_switch_targets"}) {
        EXPECT_TRUE(tagIds.contains(t)) << t;
    }
}

TEST(PonderAssets, TagsHaveTitles) {
    Library lib;
    lib.setIndex(load(repoRoot() / "assets" / "ponder" / "index.json"));
    auto en = langOf("en_US");
    auto zh = langOf("zh_CN");
    for (auto const& t : lib.tags()) {
        for (auto const& k : {"create.ponder.tag." + t.id, "create.ponder.tag." + t.id + ".description"}) {
            EXPECT_FALSE(en[k].empty()) << "en_US misses " << k;
            EXPECT_FALSE(zh[k].empty()) << "zh_CN misses " << k;
        }
    }
    for (auto const* k : {"ponder.ui.hold_to_ponder", "ponder.ui.pondering", "ponder.ui.identify_mode", "ponder.ui.index_title",
                          "ponder.ui.associated", "ponder.ui.next", "ponder.ui.previous", "ponder.ui.replay", "ponder.ui.identify",
                          "ponder.ui.slow_text", "ponder.ui.close", "ponder.ui.think_back", "ponder.ui.welcome", "ponder.ui.categories",
                          "ponder.ui.index_description", "ponder.ui.subject", "ponder.ui.next_up", "ponder.ui.exit"}) {
        EXPECT_FALSE(en[k].empty()) << "en_US misses " << k;
        EXPECT_FALSE(zh[k].empty()) << "zh_CN misses " << k;
    }
}
