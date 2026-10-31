#include <gtest/gtest.h>

#include "core/kinetics/Feedback.h"

using namespace lv::core::kinetics;
using namespace lv::core::kinetics::feedback;

TEST(KineticFeedback, OverstressEffectDecaysAndTints) {
    EXPECT_FLOAT_EQ(overstressEffectAfter(1.f, 0), 1.f);
    EXPECT_FLOAT_EQ(overstressEffectAfter(1.f, 1), .9f);
    EXPECT_NEAR(overstressEffectAfter(-1.f, 2), -.81f, 1e-6);
    EXPECT_EQ(overstressEffectAfter(1.f, 60), 0.f); // below 1/128 it stops
    auto white = overstressTint(0);
    EXPECT_EQ(white, (std::array<float, 3>{1.f, 1.f, 1.f}));
    auto red = overstressTint(1);
    EXPECT_EQ(red, (std::array<float, 3>{1.f, 0.f, 0.f}));
    auto half = overstressTint(-.5f);
    EXPECT_FLOAT_EQ(half[0], .5f);
    EXPECT_FLOAT_EQ(half[1], 1.f);
    EXPECT_NEAR(half[2], .5f + .5f * 127.f / 255.f, 1e-6);
}

TEST(KineticFeedback, FlywheelChase) {
    EXPECT_FLOAT_EQ(chaseExp(0, 64, kFlywheelChase), 1.f);
    EXPECT_FLOAT_EQ(chaseExp(64, 0, kFlywheelChase), 63.f);
    EXPECT_EQ(chaseExp(32, 32, kFlywheelChase), 32.f);
}

TEST(KineticFeedback, SpeedLevelColoursAndParticleSpeeds) {
    EXPECT_EQ(speedLevelColour(SpeedLevel::Slow), 0x22FF22u);
    EXPECT_EQ(speedLevelColour(SpeedLevel::Medium), 0x0084FFu);
    EXPECT_EQ(speedLevelColour(SpeedLevel::Fast), 0xFF55FFu);
    EXPECT_EQ(speedLevelParticleSpeed(SpeedLevel::None), 0);
    EXPECT_EQ(speedLevelParticleSpeed(SpeedLevel::Slow), 10);
    EXPECT_EQ(speedLevelParticleSpeed(SpeedLevel::Medium), 20);
    EXPECT_EQ(speedLevelParticleSpeed(SpeedLevel::Fast), 30);
    EXPECT_EQ(fadeColour(0x000000), 0x808080u);
    EXPECT_EQ(fadeColour(0xFFFFFF), 0xFFFFFFu);
    EXPECT_EQ(fadeColour(0x22FF22), 0x91FF91u);
}

TEST(KineticFeedback, IndicatorsQueuedOnGeneratorSpeedLevelChanges) {
    EXPECT_TRUE(queuesIndicator(true, false, 0, 16, 30, 100));    // none -> slow
    EXPECT_TRUE(queuesIndicator(true, false, 16, 64, 30, 100));   // slow -> medium
    EXPECT_TRUE(queuesIndicator(true, false, 64, 0, 30, 100));    // medium -> none
    EXPECT_FALSE(queuesIndicator(true, false, 16, 24, 30, 100));  // same level
    EXPECT_FALSE(queuesIndicator(true, false, 16, -16, 30, 100)); // direction only
    EXPECT_FALSE(queuesIndicator(true, true, 0, 64, 30, 100));    // driven by another source
    EXPECT_FALSE(queuesIndicator(false, false, 0, 64, 30, 100));  // not a generator
}

TEST(KineticFeedback, IndicatorShape) {
    EXPECT_FALSE(indicatorFor(0, Axis::X, CogType::None, 30, 100));
    EXPECT_FALSE(indicatorFor(16, std::nullopt, CogType::None, 30, 100));
    auto slow = indicatorFor(-16, Axis::Z, CogType::None, 30, 100);
    ASSERT_TRUE(slow);
    EXPECT_EQ(slow->speed, -10);
    EXPECT_EQ(slow->colour, 0x22FF22u);
    EXPECT_FLOAT_EQ(slow->radius1, .75f);
    EXPECT_FLOAT_EQ(slow->radius2, .65f);
    EXPECT_EQ(slow->count, 20);
    EXPECT_EQ(slow->lifeSpan, 10);
    auto large = indicatorFor(128, Axis::Y, CogType::Large, 30, 100);
    ASSERT_TRUE(large);
    EXPECT_EQ(large->speed, 30);
    EXPECT_FLOAT_EQ(large->radius1, 1.f);
    EXPECT_FLOAT_EQ(large->radius2, 1.125f);
}

TEST(KineticFeedback, AmbientPitchAndGroups) {
    EXPECT_FLOAT_EQ(ambientPitch(16), .85f);   // 16/256 + .45 = .5125 -> clamped
    EXPECT_FLOAT_EQ(ambientPitch(-128), .95f); // .5 + .45
    EXPECT_FLOAT_EQ(ambientPitch(256), 1.f);
    EXPECT_EQ(pitchGroupOf(.85f), PitchGroup::Low);
    EXPECT_EQ(pitchGroupOf(.95f), PitchGroup::Normal);
    EXPECT_EQ(pitchGroupOf(.5f), PitchGroup::VeryLow);
    EXPECT_EQ(pitchGroupOf(1.2f), PitchGroup::High);
    EXPECT_EQ(pitchGroupOf(1.3f), PitchGroup::VeryHigh);
    EXPECT_FLOAT_EQ(relativeVolume(Ambience::Cog), 1.5f);
    EXPECT_FLOAT_EQ(relativeVolume(Ambience::Kinetic), .025f);
}

TEST(KineticFeedback, ScapeVolume) {
    EXPECT_FLOAT_EQ(scapeVolume(1, 0, .1f), .05f);  // floor .025, twice at distance 0
    EXPECT_FLOAT_EQ(scapeVolume(15, 8, .1f), .1f);  // 15 / 150 = .1, times 1 at half range
    EXPECT_FLOAT_EQ(scapeVolume(150, 8, .1f), .1f); // capped
    EXPECT_FLOAT_EQ(scapeVolume(30, 16, .1f), 0.f); // silent at the range
    EXPECT_TRUE(inAmbientRange({0, 15, 0, 0}, {0, 0, 0, 0}));
    EXPECT_FALSE(inAmbientRange({0, 16, 0, 0}, {0, 0, 0, 0}));
    EXPECT_FALSE(inAmbientRange({1, 1, 0, 0}, {0, 0, 0, 0}));
}

TEST(KineticFeedback, GroupsSourcesByAmbienceAndPitchGroup) {
    std::vector<Source> s{
        {Ambience::Kinetic, {0, 0, 0, 0}, .85f},
        {Ambience::Kinetic, {0, 2, 0, 0}, .88f},
        {Ambience::Kinetic, {0, 4, 0, 0}, .95f},
        {Ambience::Cog,     {0, 2, 0, 0}, .88f},
    };
    auto g = groupSources(s);
    ASSERT_EQ(g.size(), 3u);
    EXPECT_EQ(g[0].group, Ambience::Kinetic);
    EXPECT_EQ(g[0].pitchGroup, PitchGroup::Low);
    EXPECT_EQ(g[0].count, 2);
    EXPECT_FLOAT_EQ(g[0].pitch, .85f);
    EXPECT_DOUBLE_EQ(g[0].x, 1.5);
    EXPECT_EQ(g[1].pitchGroup, PitchGroup::Normal);
    EXPECT_EQ(g[1].count, 1);
    EXPECT_EQ(g[2].group, Ambience::Cog);
    EXPECT_DOUBLE_EQ(g[2].x, 2.5);
}
