#include <gtest/gtest.h>

#include "nlohmann/json.hpp"

#include "core/kinetics/RotorModel.h"

using namespace lv::core::kinetics;

TEST(RotorModel, ParsesCubesAndUvs) {
    auto j = nlohmann::json::parse(R"({
      "format_version": "1.16.0",
      "minecraft:geometry": [{
        "description": {"identifier": "geometry.test", "texture_width": 16, "texture_height": 16},
        "bones": [
          {"name": "rotor", "cubes": [
            {"origin": [-2, 0, -2], "size": [4, 16, 4],
             "uv": {"north": {"uv": [0, 0], "uv_size": [4, 16]}, "up": {"uv": [4, 0], "uv_size": [4, 4]}}}
          ]},
          {"name": "casing", "cubes": [{"origin": [-8, 0, -8], "size": [16, 16, 16], "uv": [0, 0]}]}
        ]
      }]
    })");
    auto models = parseGeometry(j);
    ASSERT_TRUE(models);
    auto const& m = models->at("geometry.test");
    ASSERT_EQ(m.bones.at("rotor").size(), 2u);   // only faces with a UV
    ASSERT_EQ(m.bones.at("casing").size(), 6u);  // box UV gives all six
    for (auto const& q : m.bones.at("rotor")) {
        for (auto const& p : q.pos) {
            EXPECT_NEAR(std::abs(p[0] - .5f), .125f, 1e-6);
            EXPECT_NEAR(std::abs(p[2] - .5f), .125f, 1e-6);
            EXPECT_TRUE(p[1] == 0.f || p[1] == 1.f);
        }
    }
    auto const& up = m.bones.at("rotor")[1]; // faces in name order: north, up
    EXPECT_FLOAT_EQ(up.uv[0][0], .25f);
    EXPECT_FLOAT_EQ(up.uv[2][0], .5f);
    EXPECT_FLOAT_EQ(up.uv[2][1], .25f);
}

TEST(RotorModel, RotatesCubesAboutTheirPivot) {
    auto j = nlohmann::json::parse(R"({
      "minecraft:geometry": [{
        "description": {"identifier": "geometry.tooth"},
        "bones": [{"name": "rotor", "cubes": [
          {"origin": [-1, 6, 6], "size": [2, 4, 3], "pivot": [0, 8, 0], "rotation": [0, 90, 0],
           "uv": {"up": {"uv": [0, 0], "uv_size": [2, 3]}}}
        ]}]
      }]
    })");
    auto models = parseGeometry(j);
    ASSERT_TRUE(models);
    auto const& q = models->at("geometry.tooth").bones.at("rotor").at(0);
    // the tooth moved from +z to the x axis: z stays near the centre line
    for (auto const& p : q.pos) {
        EXPECT_NEAR(p[2], .5f, 1.f / 16 + 1e-5);
        EXPECT_GT(std::abs(p[0] - .5f), 5.f / 16);
    }
}

TEST(RotorModel, RejectsOtherFiles) {
    EXPECT_FALSE(parseGeometry(nlohmann::json::parse(R"({"format_version": "1.16.0"})")));
    EXPECT_FALSE(parseGeometry(nlohmann::json::array()));
}
