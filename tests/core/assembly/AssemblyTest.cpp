#include <gtest/gtest.h>

#include <cmath>

#include "core/assembly/Arm.h"
#include "core/assembly/Deployer.h"
#include "core/assembly/Sequenced.h"

using namespace lv::core::assembly;
using lv::core::kinetics::Axis;
using lv::core::kinetics::Face;

namespace {
nlohmann::json mechanismRecipe() {
    return nlohmann::json::parse(R"({
      "type": "create:sequenced_assembly",
      "ingredient": {"item": "create:golden_sheet"},
      "transitional_item": {"id": "create:incomplete_precision_mechanism"},
      "loops": 5,
      "sequence": [
        {"type": "create:deploying", "ingredients": [{"item": "create:incomplete_precision_mechanism"}, {"item": "create:cogwheel"}],
         "results": [{"id": "create:incomplete_precision_mechanism"}]},
        {"type": "create:deploying", "ingredients": [{"item": "create:incomplete_precision_mechanism"}, {"item": "create:large_cogwheel"}],
         "results": [{"id": "create:incomplete_precision_mechanism"}]},
        {"type": "create:deploying", "ingredients": [{"item": "create:incomplete_precision_mechanism"}, {"tag": "c:nuggets/iron"}],
         "results": [{"id": "create:incomplete_precision_mechanism"}]}
      ],
      "results": [{"id": "create:precision_mechanism", "chance": 120.0}, {"id": "create:golden_sheet", "chance": 8.0},
                  {"id": "create:andesite_alloy", "chance": 8.0}, {"id": "create:cogwheel", "chance": 5.0}]
    })");
}
} // namespace

TEST(Sequenced, ParsesCreateFormat) {
    std::string err;
    auto        r = parseSequenced(mechanismRecipe(), "levitate:sequenced_assembly/precision_mechanism", &err);
    ASSERT_TRUE(r) << err;
    EXPECT_EQ(r->transitionalItem, "levitate:incomplete_precision_mechanism");
    EXPECT_EQ(r->loops, 5);
    ASSERT_EQ(r->sequence.size(), 3u);
    EXPECT_EQ(r->sequence[0].type, "deploying");
    EXPECT_EQ(r->sequence[0].recipe.ingredients[1].items[0], "levitate:cogwheel");
    EXPECT_EQ(r->sequence[2].recipe.ingredients[1].tags[0], "c:nuggets/iron");
    EXPECT_EQ(r->totalSteps(), 15);
    ASSERT_EQ(r->results.size(), 4u);
    EXPECT_FLOAT_EQ(r->results[0].chance, 120.f);
    EXPECT_EQ(r->ingredient.items[0], "levitate:golden_sheet");
}

TEST(Sequenced, ParsesTrackRecipeWithInlineTags) {
    auto doc = nlohmann::json::parse(R"({
      "type": "levitate:sequenced_assembly",
      "ingredient": { "tag": "levitate:sleepers" },
      "tags": { "levitate:sleepers": ["minecraft:stone_slab"] },
      "transitional_item": { "item": "levitate:incomplete_track" },
      "loops": 1,
      "sequence": [
        { "type": "levitate:deploying",
          "ingredients": [{ "item": "levitate:incomplete_track" }, [{ "item": "minecraft:iron_nugget" }, { "item": "levitate:zinc_nugget" }]],
          "results": [{ "item": "levitate:incomplete_track" }] },
        { "type": "levitate:pressing", "ingredients": [{ "item": "levitate:incomplete_track" }], "results": [{ "item": "levitate:incomplete_track" }] }
      ],
      "results": [{ "item": "levitate:track", "count": 1 }]
    })");
    auto r = parseSequenced(doc, "t");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->ingredient.tags[0], "levitate:sleepers");
    EXPECT_EQ(r->inlineTags.at("levitate:sleepers")[0], "minecraft:stone_slab");
    EXPECT_EQ(r->sequence[0].recipe.ingredients[1].items.size(), 2u);
    EXPECT_EQ(r->sequence[1].type, "pressing");
}

TEST(Sequenced, RejectsBrokenRecipes) {
    std::string err;
    EXPECT_FALSE(parseSequenced(nlohmann::json::object(), "x", &err));
    auto doc = mechanismRecipe();
    doc["sequence"][0]["type"] = "create:mixing";
    EXPECT_FALSE(parseSequenced(doc, "x", &err));
    EXPECT_NE(err.find("mixing"), std::string::npos);
    doc = mechanismRecipe();
    doc["sequence"][0]["ingredients"].erase(1);
    EXPECT_FALSE(parseSequenced(doc, "x", &err));
}

TEST(Sequenced, StepsAndProgress) {
    auto r = *parseSequenced(mechanismRecipe(), "x");
    // 3 steps x 5 loops: step 14 (the 15th) completes
    for (int s = 0; s < 14; ++s) EXPECT_FALSE(completesWith(r, s)) << s;
    EXPECT_TRUE(completesWith(r, 14));
    EXPECT_FLOAT_EQ(progressAt(r, 3), 0.2f);
    EXPECT_EQ(r.stepAt(4).recipe.ingredients[1].items[0], "levitate:large_cogwheel");
}

TEST(Sequenced, WeightedPick) {
    auto r = *parseSequenced(mechanismRecipe(), "x");
    // total weight 141: 0..120 mechanism, then 8, 8, 5
    EXPECT_EQ(pickWeighted(r.results, 0.0f), 0);
    EXPECT_EQ(pickWeighted(r.results, 119.9f / 141.f), 0);
    EXPECT_EQ(pickWeighted(r.results, 121.f / 141.f), 1);
    EXPECT_EQ(pickWeighted(r.results, 130.f / 141.f), 2);
    EXPECT_EQ(pickWeighted(r.results, 0.9999f), 3);
    auto shares = resultShares(r.results);
    EXPECT_NEAR(shares[0], 120.0 / 141.0, 1e-5);
    EXPECT_EQ(pickWeighted({}, 0.5f), -1);
}

TEST(Sequenced, TooltipLines) {
    auto r     = *parseSequenced(mechanismRecipe(), "x");
    auto lines = tooltipLines(r, 13, {"Deploy Cogwheel", "Deploy Large Cogwheel", "Deploy Iron Nugget"});
    ASSERT_EQ(lines.size(), 4u); // header, progress, next + one more (two steps remain)
    EXPECT_EQ(lines[1].args[0], "13");
    EXPECT_EQ(lines[1].args[1], "15");
    EXPECT_EQ(lines[2].key, "create.recipe.assembly.next");
    EXPECT_EQ(lines[2].args[0], "Deploy Large Cogwheel");
    EXPECT_EQ(lines[3].args[0], "Deploy Iron Nugget");
}

TEST(Deployer, TimerSpeed) {
    EXPECT_EQ(deployerTimerSpeed(0), 0);
    EXPECT_EQ(deployerTimerSpeed(1), 8);
    EXPECT_EQ(deployerTimerSpeed(-16), 32);
    EXPECT_EQ(deployerTimerSpeed(256), 512);
}

TEST(Deployer, ShaftAxis) {
    EXPECT_EQ(deployerShaftAxis(Face::East, true), Axis::Y);
    EXPECT_EQ(deployerShaftAxis(Face::East, false), Axis::Z);
    EXPECT_EQ(deployerShaftAxis(Face::Up, true), Axis::X);
    EXPECT_EQ(deployerShaftAxis(Face::Down, false), Axis::Z);
    EXPECT_EQ(deployerShaftAxis(Face::North, true), Axis::X);
    EXPECT_EQ(deployerShaftAxis(Face::South, false), Axis::Y);
    for (Face f : lv::core::kinetics::kFaces) {
        for (Axis a : lv::core::kinetics::kAxes) {
            if (a == lv::core::kinetics::axisOf(f)) continue;
            EXPECT_EQ(deployerShaftAxis(f, deployerAlongFirstFor(f, a)), a);
        }
    }
}

TEST(Deployer, Placement) {
    PlacementNeighbours none;
    // horizontal: the shaft stays horizontal by default
    EXPECT_EQ(deployerShaftAxis(Face::North, deployerPlacementAlongFirst(Face::North, false, none)), Axis::X);
    EXPECT_EQ(deployerShaftAxis(Face::East, deployerPlacementAlongFirst(Face::East, false, none)), Axis::Z);
    PlacementNeighbours above;
    above.shaftTowards[static_cast<int>(Face::Up)] = true;
    EXPECT_EQ(deployerShaftAxis(Face::North, deployerPlacementAlongFirst(Face::North, false, above)), Axis::Y);
    // vertical: the look axis, unless one horizontal shaft axis is offered
    EXPECT_EQ(deployerShaftAxis(Face::Down, deployerPlacementAlongFirst(Face::Down, true, none)), Axis::X);
    EXPECT_EQ(deployerShaftAxis(Face::Down, deployerPlacementAlongFirst(Face::Down, false, none)), Axis::Z);
    PlacementNeighbours side;
    side.shaftTowards[static_cast<int>(Face::West)] = true;
    EXPECT_EQ(deployerShaftAxis(Face::Down, deployerPlacementAlongFirst(Face::Down, false, side)), Axis::X);
}

TEST(Deployer, HandOffset) {
    EXPECT_FLOAT_EQ(deployerHandOffset(DeployerState::Waiting, 0, 32, 0, 0.5f, HandPose::Pointing), 0.f);
    EXPECT_FLOAT_EQ(deployerHandOffset(DeployerState::Expanding, 0, 32, 0, 0.5f, HandPose::Pointing), 0.5f);
    EXPECT_FLOAT_EQ(deployerHandOffset(DeployerState::Expanding, 500, 32, 0, 0.5f, HandPose::Holding), 0.5f * (0.5f + 0.25f));
    EXPECT_FLOAT_EQ(deployerHandOffset(DeployerState::Retracting, 1000, 32, 0, 0.5f, HandPose::Punching), 0.5f + 3 / 16.f);
    EXPECT_FLOAT_EQ(deployerReach(0.f), 0.5f);
    EXPECT_FLOAT_EQ(deployerReach(2.f), 1.25f);
    EXPECT_EQ(deployerHandPose(DeployerMode::Punch, true), HandPose::Punching);
    EXPECT_EQ(deployerHandPose(DeployerMode::Use, false), HandPose::Pointing);
}

TEST(Arm, ScanRanges) {
    // prefer first: always from 0
    auto s = armScanRange(ArmSelectionMode::PreferFirst, 2, 4);
    EXPECT_EQ(s.begin, 0);
    EXPECT_EQ(s.end, 4);
    // round robin: after the last one
    s = armScanRange(ArmSelectionMode::RoundRobin, 1, 4);
    EXPECT_EQ(s.begin, 2);
    EXPECT_EQ(s.end, 4);
    // forced: only the next one
    s = armScanRange(ArmSelectionMode::ForcedRoundRobin, 1, 4);
    EXPECT_EQ(s.begin, 2);
    EXPECT_EQ(s.end, 3);
    s = armScanRange(ArmSelectionMode::ForcedRoundRobin, -1, 4);
    EXPECT_EQ(s.begin, 0);
    EXPECT_EQ(s.end, 1);
    EXPECT_EQ(armLastIndexAfter(ArmSelectionMode::RoundRobin, 1, false, 4), -1);
    EXPECT_EQ(armLastIndexAfter(ArmSelectionMode::ForcedRoundRobin, 1, false, 4), 1);
    EXPECT_EQ(armLastIndexAfter(ArmSelectionMode::ForcedRoundRobin, 3, true, 4), -1);
}

TEST(Arm, ProgressAndRange) {
    EXPECT_FLOAT_EQ(armProgressPerTick(64), 64 / 1024.f);
    EXPECT_FLOAT_EQ(armProgressPerTick(-512), 256 / 1024.f);
    EXPECT_TRUE(armInRange(4, 0, 0, 5));
    EXPECT_TRUE(armInRange(3, 3, 1, 5));
    EXPECT_FALSE(armInRange(5, 0, 0, 5));
    EXPECT_FALSE(armInRange(3, 4, 0, 5));
}

TEST(Arm, AnglesReachTheTarget) {
    // a point straight in front (south) on the floor two blocks away
    auto a = armAnglesFor({0, 0, 0}, {0.5, 1.0, 2.5}, Face::Down, false);
    EXPECT_NEAR(std::fmod(a.base + 360.f, 360.f), 180.f, 0.5f) << a.base;
    EXPECT_GT(a.lower, 0.f);
    EXPECT_GT(a.upper, 0.f);
    auto rest = armRestAngles();
    auto mid  = armPoseAt(rest, a, true, 0.25f);
    EXPECT_FLOAT_EQ(mid.lower, rest.lower); // folding back first (already resting)
    auto done = armPoseAt(rest, a, true, 1.f);
    EXPECT_FLOAT_EQ(done.lower, a.lower);
    EXPECT_FLOAT_EQ(done.head, a.head);
}
