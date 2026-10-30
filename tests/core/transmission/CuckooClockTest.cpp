#include <gtest/gtest.h>

#include "core/transmission/CuckooClock.h"

using namespace lv::core::transmission;

TEST(CuckooClock, TimeOfDay) {
    auto t = clockTime(0, true);
    EXPECT_EQ(t.hours, 6);
    EXPECT_EQ(t.minutes, 0);
    t = clockTime(6000, true); // noon
    EXPECT_EQ(t.hours, 12);
    EXPECT_EQ(t.minutes, 0);
    t = clockTime(12550, true); // 18:33
    EXPECT_EQ(t.hours, 18);
    EXPECT_EQ(t.minutes, 33);
    t = clockTime(24000 * 3 + 18000, true); // midnight of day 4
    EXPECT_EQ(t.hours, 0);
    // elsewhere the clock runs 24 times faster
    t = clockTime(250, false); // 6000 scaled
    EXPECT_EQ(t.hours, 12);
}

TEST(CuckooClock, Triggers) {
    EXPECT_EQ(cuckooTrigger({12, 0}), CuckooAnimation::Pig);
    EXPECT_EQ(cuckooTrigger({12, 4}), CuckooAnimation::Pig);
    EXPECT_EQ(cuckooTrigger({12, 5}), CuckooAnimation::None);
    EXPECT_EQ(cuckooTrigger({18, 31}), CuckooAnimation::None);
    EXPECT_EQ(cuckooTrigger({18, 32}), CuckooAnimation::Creeper);
    EXPECT_EQ(cuckooTrigger({18, 35}), CuckooAnimation::Creeper);
    EXPECT_EQ(cuckooTrigger({18, 36}), CuckooAnimation::None);
    EXPECT_EQ(cuckooTrigger({0, 0}), CuckooAnimation::None);
    EXPECT_EQ(cuckooStarted(CuckooAnimation::Pig, true), CuckooAnimation::Surprise);
    EXPECT_EQ(cuckooStarted(CuckooAnimation::Creeper, false), CuckooAnimation::Creeper);
    EXPECT_EQ(cuckooStarted(CuckooAnimation::None, true), CuckooAnimation::None);
}

TEST(CuckooClock, Hands) {
    EXPECT_EQ(hourHandAngle(15), 90.f);
    EXPECT_EQ(hourHandAngle(0), 0.f);
    EXPECT_EQ(minuteHandAngle(30), 180.f);
    EXPECT_EQ(hourHandState({15, 59}), 3);
    EXPECT_EQ(minuteHandState({15, 59}), 11);
    EXPECT_EQ(minuteHandState({15, 4}), 0);
    // the shortest way round, a fifth of the way per tick
    EXPECT_NEAR(chaseAngle(0, 90), 18.f, 1e-4);
    EXPECT_NEAR(chaseAngle(350, 10), 354.f, 1e-4);
    EXPECT_NEAR(chaseAngle(10, 350), 6.f, 1e-4);
    float a = 0;
    for (int i = 0; i < 200; ++i) a = chaseAngle(a, 120);
    EXPECT_EQ(a, 120.f);
}

TEST(CuckooClock, DoorsOpenThreeTimesForThePig) {
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Pig, 0), 0.f);
    EXPECT_NEAR(cuckooDoorAngle(CuckooAnimation::Pig, 27.5f), 67.5f, 1e-4); // opening
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Pig, 30), 135.f);
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Pig, 34), 135.f);
    EXPECT_NEAR(cuckooDoorAngle(CuckooAnimation::Pig, 37.5f), 67.5f, 1e-4); // closing
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Pig, 40), 0.f);
    EXPECT_NEAR(cuckooDoorAngle(CuckooAnimation::Pig, 41), 27.f, 1e-4); // the second opening
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Pig, 45), 135.f);
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Pig, 60), 135.f);
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Pig, 90), 0.f);
    EXPECT_EQ(cuckooModelState(CuckooAnimation::Pig, 30), 1);
    EXPECT_EQ(cuckooModelState(CuckooAnimation::Creeper, 45), 2);
    EXPECT_EQ(cuckooModelState(CuckooAnimation::Creeper, 20), 0);
    EXPECT_EQ(cuckooFigureOffset(0), 10.f / 16);
    EXPECT_NEAR(cuckooFigureOffset(135), 10.f / 16 - .5f, 1e-6);
}

TEST(CuckooClock, SurpriseFlapsTheDoors) {
    // step 3: open at every phase 30, 33, ..., 60
    EXPECT_EQ(cuckooDoorAngle(CuckooAnimation::Surprise, 33), 135.f);
    EXPECT_NEAR(cuckooDoorAngle(CuckooAnimation::Surprise, 32), 108.f, 1e-4);
    EXPECT_EQ(cuckooModelState(CuckooAnimation::Surprise, 33), 2);
}

TEST(CuckooClock, Sounds) {
    EXPECT_EQ(cuckooTickSound(32, true), (ClockSound{"note.hat", 1.f / 16, 2.f}));
    EXPECT_EQ(cuckooTickSound(48, true), (ClockSound{"note.hat", 1.f / 16, 1.5f}));
    EXPECT_FALSE(cuckooTickSound(47, true).has_value());
    EXPECT_EQ(cuckooTickSound(9, false), (ClockSound{"note.hat", 1.f / 16, 1.5f}));
    EXPECT_EQ(cuckooTickSound(12, false), (ClockSound{"note.hat", 1.f / 16, 2.f}));

    auto s = cuckooAnimationSounds(CuckooAnimation::Pig, 1);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0], (ClockSound{"note.chime", 2.f, .5f}));
    s = cuckooAnimationSounds(CuckooAnimation::Pig, 30);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_STREQ(s[0].name, "mob.pig.say");
    s = cuckooAnimationSounds(CuckooAnimation::Creeper, 25);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_STREQ(s[0].name, "random.chestopen");
    s = cuckooAnimationSounds(CuckooAnimation::Creeper, 50);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_STREQ(s[0].name, "random.chestclosed");
    s = cuckooAnimationSounds(CuckooAnimation::Surprise, 40);
    bool fuse = false;
    for (auto const& x : s) fuse |= std::string_view(x.name) == "random.fuse";
    EXPECT_TRUE(fuse);
    EXPECT_TRUE(cuckooSmokes(CuckooAnimation::Surprise, 31));
    EXPECT_FALSE(cuckooSmokes(CuckooAnimation::Surprise, 30));
    EXPECT_FALSE(cuckooSmokes(CuckooAnimation::Creeper, 40));
}
