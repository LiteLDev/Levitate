#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/ponder/BlockModel.h"
#include "core/ponder/View.h"

using namespace lv::core::ponder;
using Json = nlohmann::json;

TEST(PonderModel, Molang) {
    std::map<std::string, int> st{{"levitate:facing", 2}, {"levitate:rotor", 0}};
    auto                       is = [&](std::string const& e) { return evalMolang(e, st).value_or(-1.f); };
    EXPECT_EQ(is("q.block_state('levitate:facing') == 2"), 1.f);
    EXPECT_EQ(is("query.block_state('levitate:facing') != 2"), 0.f);
    EXPECT_EQ(is("q.block_state('levitate:facing') == 2 && q.block_state('levitate:rotor') == 0"), 1.f);
    EXPECT_EQ(is("(q.block_state('levitate:facing') == 1 || q.block_state('levitate:facing') == 2) && !(q.block_state('levitate:rotor') == 1)"), 1.f);
    EXPECT_EQ(is("q.block_state('levitate:facing') >= 3"), 0.f);
    EXPECT_EQ(is("q.block_state('levitate:facing') <= 3 || 0 == 1"), 1.f);
    EXPECT_EQ(is("True"), 1.f);
    EXPECT_EQ(is("false"), 0.f);
    EXPECT_EQ(is("q.block_state('missing') == 0"), 1.f);
    EXPECT_EQ(evalMolang("q.block_state('minecraft:cardinal_direction') == 'north'", st, {{"minecraft:cardinal_direction", "north"}}).value_or(-1), 1.f);
    EXPECT_FALSE(evalMolang("q.block_state('x' == ", st).has_value());
    EXPECT_FALSE(evalMolang("1 == 1 garbage", st).has_value());
}

TEST(PonderModel, FullBlockShading) {
    ResolvedBlock b;
    b.materials["*"] = "stone";
    auto quads       = buildQuads(b, fullBlock(), {});
    ASSERT_EQ(quads.size(), 6u);
    int ups = 0, downs = 0;
    for (auto const& q : quads) {
        EXPECT_EQ(q.texture, "stone");
        float y = q.pos[0][1] + q.pos[1][1] + q.pos[2][1] + q.pos[3][1];
        if (y == 4.f) {
            ++ups;
            EXPECT_FLOAT_EQ(q.shade, 1.f);
        } else if (y == 0.f) {
            ++downs;
            EXPECT_FLOAT_EQ(q.shade, 0.5f);
        } else {
            EXPECT_TRUE(q.shade == 0.8f || q.shade == 0.6f);
        }
        for (auto const& p : q.pos)
            for (float c : p) EXPECT_TRUE(c >= -1e-5f && c <= 1.f + 1e-5f);
    }
    EXPECT_EQ(ups, 1);
    EXPECT_EQ(downs, 1);
}

TEST(PonderModel, ResolvesPermutations) {
    auto file = parseBlockFile(Json::parse(R"({"minecraft:block": {
        "description": {"identifier": "levitate:test", "states": {"levitate:facing": [0, 1, 2, 3, 4, 5], "levitate:rotor": [0, 1]}},
        "components": {
            "minecraft:geometry": {"identifier": "geometry.test", "bone_visibility": {"rotor": "q.block_state('levitate:rotor') == 0"}},
            "minecraft:material_instances": {"*": {"texture": "body"}, "casing": "side_alias", "side_alias": {"texture": "casing_tex"}}
        },
        "permutations": [
            {"condition": "q.block_state('levitate:facing') == 2", "components": {"minecraft:transformation": {"rotation": [-90, 0, 0]}}},
            {"condition": "q.block_state('levitate:facing') == 5", "components": {"minecraft:transformation": {"rotation": [0, 0, -90]}}}
        ]}})"));
    ASSERT_TRUE(file);
    EXPECT_EQ(file->defaults["levitate:facing"], 0);
    auto r = resolve(*file, {{"levitate:facing", 2}});
    EXPECT_EQ(r.geometry, "geometry.test");
    EXPECT_EQ(r.rotation, (V3{-90, 0, 0}));
    EXPECT_EQ(r.materials["casing"], "casing_tex");
    EXPECT_EQ(resolve(*file, {{"levitate:facing", 1}}).rotation, (V3{0, 0, 0}));

    // a thin pole along +y: facing north (rotation x -90) points it to -z, east (z -90) to +x
    auto geo = parseGeometries(Json::parse(R"({"minecraft:geometry": [{"description": {"identifier": "geometry.test"},
        "bones": [{"name": "body", "cubes": [{"origin": [-1, 8, -1], "size": [2, 8, 2], "uv": [0, 0]}]},
                  {"name": "rotor", "cubes": [{"origin": [-8, 0, -8], "size": [16, 1, 16], "uv": [0, 0]}]}]}]})"));
    ASSERT_TRUE(geo.contains("geometry.test"));
    auto tip = [](std::vector<Quad> const& quads, int axis, bool maxSide) {
        float best = maxSide ? -1e9f : 1e9f;
        for (auto const& q : quads)
            for (auto const& p : q.pos) best = maxSide ? std::max(best, p[static_cast<std::size_t>(axis)]) : std::min(best, p[static_cast<std::size_t>(axis)]);
        return best;
    };
    auto north = buildQuads(r, geo["geometry.test"], {{"levitate:facing", 2}, {"levitate:rotor", 1}});
    EXPECT_EQ(north.size(), 6u); // the rotor bone is hidden (rotor = 1)
    EXPECT_NEAR(tip(north, 2, false), 0.f, 1e-4);  // reaches the north face
    auto east = buildQuads(resolve(*file, {{"levitate:facing", 5}}), geo["geometry.test"], {{"levitate:facing", 5}, {"levitate:rotor", 0}});
    EXPECT_EQ(east.size(), 12u);
    EXPECT_NEAR(tip(buildQuads(resolve(*file, {{"levitate:facing", 5}}), geo["geometry.test"], {{"levitate:rotor", 1}}), 0, true), 1.f, 1e-4);
    for (auto const& q : north) EXPECT_EQ(q.texture, "body");
}

TEST(PonderModel, ShippedBlocksResolve) {
    // every shipped block file resolves to a geometry that exists (or the full block)
    namespace fs = std::filesystem;
    fs::path root;
    for (fs::path c = fs::current_path(); !c.empty(); c = c.parent_path()) {
        if (fs::exists(c / "assets" / "pack" / "bp" / "blocks")) {
            root = c / "assets" / "pack";
            break;
        }
        if (c == c.root_path()) break;
    }
    ASSERT_FALSE(root.empty()) << "run the tests inside the repository";
    std::map<std::string, Geometry> geos;
    for (auto const& e : fs::directory_iterator(root / "rp" / "models" / "blocks")) {
        std::ifstream in(e.path());
        auto          j = Json::parse(in, nullptr, false, true);
        geos.merge(parseGeometries(j));
    }
    int blocks = 0;
    for (auto const& e : fs::directory_iterator(root / "bp" / "blocks")) {
        std::ifstream in(e.path());
        auto          file = parseBlockFile(Json::parse(in, nullptr, false));
        ASSERT_TRUE(file) << e.path().string();
        ++blocks;
        auto r = resolve(*file, {});
        EXPECT_TRUE(r.geometry == "minecraft:geometry.full_block" || geos.contains(r.geometry)) << file->id << " -> " << r.geometry;
        for (auto const& [cond, comps] : file->permutations) EXPECT_TRUE(evalMolang(cond, file->defaults).has_value()) << file->id << ": " << cond;
    }
    EXPECT_GT(blocks, 20);
}

TEST(PonderView, ProjectionMatchesMatrix) {
    Scene s;
    s.sizeX = 5, s.sizeZ = 5, s.height = 5;
    auto v  = View::fit(s, 200, 120, 300, 200);
    auto c  = v.project(v.center);
    EXPECT_FLOAT_EQ(c.x, 200);
    EXPECT_FLOAT_EQ(c.y, 120);
    auto m = v.matrix(1.f);
    for (Vec3f p : {Vec3f{0, 0, 0}, Vec3f{5, 1, 0}, Vec3f{2.5f, 3, 4}}) {
        auto  pr = v.project(p);
        float x  = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        float y  = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        float z  = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        EXPECT_NEAR(x, pr.x, 1e-3);
        EXPECT_NEAR(y, pr.y, 1e-3);
        EXPECT_NEAR(z, pr.depth, 1e-3);
    }
    // seen from the south-west and above: the south-west corner is nearest, the top above the bottom
    EXPECT_GT(v.project({0, 0, 5}).depth, v.project({5, 0, 0}).depth);
    EXPECT_LT(v.project({2.5f, 3, 2.5f}).y, v.project({2.5f, 0, 2.5f}).y);
    v.drag(10, 1000);
    EXPECT_FLOAT_EQ(v.pitch, 90.f);
}
