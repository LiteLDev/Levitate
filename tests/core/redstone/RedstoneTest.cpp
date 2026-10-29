#include <gtest/gtest.h>

#include <algorithm>
#include <tuple>
#include <vector>

#include "core/redstone/Diodes.h"
#include "core/redstone/LinkNetwork.h"

using namespace lv::core::redstone;

namespace {
/// Runs a diode for `ticks` ticks with the given input sequence (last value repeats) and
/// returns the output after each tick.
std::vector<bool> run(DiodeKind kind, int delay, std::vector<bool> input, int ticks) {
    DiodeState        s;
    std::vector<bool> out;
    for (int t = 0; t < ticks; ++t) {
        bool in = input.empty() ? false : input[std::min<std::size_t>(t, input.size() - 1)];
        tickDiode(kind, delay, in, s);
        out.push_back(s.powering);
    }
    return out;
}
int countOn(std::vector<bool> const& v) {
    int n = 0;
    for (bool b : v) n += b;
    return n;
}
} // namespace

TEST(RedstoneDiodes, RepeaterEmitsOneShortPulseAfterTheDelay) {
    // input on for good: one pulse, two ticks long, starting at delay - 1
    auto out = run(DiodeKind::Repeater, 10, {true}, 40);
    EXPECT_EQ(countOn(out), 2);
    EXPECT_FALSE(out[7]);
    EXPECT_TRUE(out[8]);  // state 9 = delay - 1
    EXPECT_TRUE(out[9]);
    EXPECT_FALSE(out[10]); // state 11 = delay + 1
}

TEST(RedstoneDiodes, RepeaterRearmsOnlyAfterInputAndOutputAreOff) {
    std::vector<bool> in(30, true);
    in.resize(60, false);
    in.resize(90, true);
    auto out = run(DiodeKind::Repeater, 4, in, 90);
    EXPECT_EQ(countOn(out), 4); // one pulse per rising edge
}

TEST(RedstoneDiodes, ExtenderHoldsTheOutputAfterTheInputDrops) {
    std::vector<bool> in(3, true);
    in.resize(40, false);
    auto out = run(DiodeKind::Extender, 10, in, 40);
    EXPECT_TRUE(out[0]);
    // three powered ticks, then the counter runs down from 10 to 1
    int lastOn = -1;
    for (int i = 0; i < 40; ++i)
        if (out[i]) lastOn = i;
    EXPECT_EQ(lastOn, 2 + 9);
    EXPECT_EQ(countOn(out), 12);
}

TEST(RedstoneDiodes, TimerPulsesEveryDelayUnlessPowered) {
    auto out = run(DiodeKind::Timer, 20, {false}, 100);
    // two-tick pulses (state 0 and 1) once per 20-tick cycle
    EXPECT_EQ(countOn(out), 10);
    auto off = run(DiodeKind::Timer, 20, {true}, 100);
    EXPECT_EQ(countOn(off), 0);
    auto fast = run(DiodeKind::Timer, 2, {false}, 10);
    EXPECT_EQ(countOn(fast), 5); // delay 2: on every other tick
}

TEST(RedstoneDiodes, DefaultsAndOutput) {
    EXPECT_EQ(defaultDelay(DiodeKind::Repeater), 2);
    EXPECT_EQ(defaultDelay(DiodeKind::Extender), 2);
    EXPECT_EQ(defaultDelay(DiodeKind::Timer), 20);
    EXPECT_EQ(diodeOutput(true, false), 15);
    EXPECT_EQ(diodeOutput(false, true), 15);
    EXPECT_EQ(diodeOutput(true, true), 0);
    EXPECT_FLOAT_EQ(diodeProgress(5, 10), 0.5f);
}

TEST(RedstoneDiodes, DelaySettingsRoundTrip) {
    EXPECT_EQ(delayToSettings(2), (DelaySettings{0, 2}));
    EXPECT_EQ(delayToSettings(60), (DelaySettings{0, 60}));
    EXPECT_EQ(delayToSettings(61), (DelaySettings{1, 3}));
    EXPECT_EQ(delayToSettings(1200), (DelaySettings{1, 60}));
    EXPECT_EQ(delayToSettings(1201), (DelaySettings{2, 1}));
    EXPECT_EQ(settingsToDelay({0, 0}), 2);
    EXPECT_EQ(settingsToDelay({0, 1}), 2);
    EXPECT_EQ(settingsToDelay({0, 37}), 37);
    EXPECT_EQ(settingsToDelay({1, 0}), 20);
    EXPECT_EQ(settingsToDelay({1, 5}), 100);
    EXPECT_EQ(settingsToDelay({2, 60}), kMaxDelay);
    EXPECT_EQ(formatDelaySettings({0, 1}), "2t");
    EXPECT_EQ(formatDelaySettings({1, 7}), "0:07");
    EXPECT_EQ(formatDelaySettings({1, 42}), "0:42");
    EXPECT_EQ(formatDelaySettings({2, 3}), "3:00");
    EXPECT_EQ(formatDelay(59), "59t");
    EXPECT_EQ(formatDelay(100), "5s");
    EXPECT_EQ(formatDelay(3600), "3m");
}

TEST(RedstoneDiodes, ScheduledInputFollowsWithDelay) {
    ScheduledTick t;
    bool          powered = false;
    std::vector<bool> trace;
    // input on for one tick only: powered goes on after 2 ticks and stays for another 2
    for (int i = 0; i < 10; ++i) {
        bool input = i == 0;
        if (advanceSchedule(t, powered != input, kBrassDiodeInputDelay)) powered = diodeInputTick(powered, input, t, kBrassDiodeInputDelay);
        trace.push_back(powered);
    }
    EXPECT_FALSE(trace[1]);
    EXPECT_TRUE(trace[2]);
    EXPECT_TRUE(trace[3]);
    EXPECT_FALSE(trace[4]);
    EXPECT_FALSE(trace[9]);
}

TEST(RedstoneLatches, PoweredLatchSetsAndResets) {
    LatchState s;
    s = poweredLatchTick(s, true, false);
    EXPECT_TRUE(s.powering);
    s = poweredLatchTick(s, false, false);
    EXPECT_TRUE(s.powering); // latched
    s = poweredLatchTick(s, false, true);
    EXPECT_FALSE(s.powering);
    s = poweredLatchTick(s, false, false);
    EXPECT_FALSE(s.powering);
    // both inputs: back wins while side is held, side release turns it back on
    s = poweredLatchTick(s, true, true);
    EXPECT_FALSE(s.powering);
    s = poweredLatchTick(s, true, false);
    EXPECT_TRUE(s.powering);
}

TEST(RedstoneLatches, PoweredLatchClickOnlyWhenInputsAgree) {
    LatchState s;
    EXPECT_TRUE(poweredLatchActivate(s));
    EXPECT_TRUE(s.powering);
    s.powered = true;
    EXPECT_FALSE(poweredLatchActivate(s));
    EXPECT_TRUE(s.powering);
}

TEST(RedstoneLatches, ToggleLatchTogglesOnRisingEdges) {
    ToggleLatchState s;
    ScheduledTick    t;
    auto step = [&](bool input) {
        if (advanceSchedule(t, s.powered != input, kLatchInputDelay)) s = toggleLatchTick(s, input, t);
    };
    for (int i = 0; i < 3; ++i) step(true);
    EXPECT_TRUE(s.powering);
    for (int i = 0; i < 3; ++i) step(false);
    EXPECT_TRUE(s.powering);
    for (int i = 0; i < 3; ++i) step(true);
    EXPECT_FALSE(s.powering);
}

TEST(RedstoneThreshold, Hysteresis) {
    EXPECT_TRUE(thresholdState(false, 128, 64, 128));
    EXPECT_FALSE(thresholdState(false, 127, 64, 128));
    EXPECT_TRUE(thresholdState(true, 65, 64, 128));
    EXPECT_FALSE(thresholdState(true, 64, 64, 128));
    EXPECT_EQ(thresholdDisplayLevel(0, 0, 100), 0);
    EXPECT_EQ(thresholdDisplayLevel(1, 0, 100), 1);
    EXPECT_EQ(thresholdDisplayLevel(50, 0, 100), 3);
    EXPECT_EQ(thresholdDisplayLevel(100, 0, 100), 5);
    EXPECT_EQ(thresholdStep(true, false), 1000);
    EXPECT_EQ(thresholdStep(false, true), 64);
}

TEST(RedstoneThreshold, ClampKeepsUpperAboveLower) {
    auto [off, on] = clampThresholds(100, 50, 0, 1728, 1, true);
    EXPECT_EQ(on, 50);
    EXPECT_EQ(off, 49);
    std::tie(off, on) = clampThresholds(100, 50, 0, 1728, 1, false);
    EXPECT_EQ(off, 100);
    EXPECT_EQ(on, 101);
    std::tie(off, on) = clampThresholds(5000, 9000, 0, 8000, 1000, true);
    EXPECT_EQ(on, 8000);
    EXPECT_EQ(off, 5000);
}

TEST(RedstoneLever, StepsAndPitch) {
    EXPECT_EQ(analogLeverNext(0, true), 0);
    EXPECT_EQ(analogLeverNext(15, false), 15);
    EXPECT_EQ(analogLeverNext(7, false), 8);
    EXPECT_FLOAT_EQ(analogLeverPitch(10), 0.75f);
}

TEST(RedstoneLinks, TransmitterPowersReceiversInRange) {
    LinkNetwork net;
    LinkKey     key{"minecraft:diamond", "minecraft:gold_ingot"};
    auto [rx, d0] = net.add({0, 0, 0, 0, key, true, 0});
    ASSERT_EQ(d0.size(), 1u);
    EXPECT_EQ(d0[0], (Delivery{rx, 0}));
    auto [tx, d1] = net.add({0, 10, 0, 0, key, false, 7});
    ASSERT_EQ(d1.size(), 1u);
    EXPECT_EQ(d1[0], (Delivery{rx, 7}));
    auto d2 = net.setTransmit(tx, 15);
    ASSERT_EQ(d2.size(), 1u);
    EXPECT_EQ(d2[0].power, 15);
    auto d3 = net.remove(tx);
    ASSERT_EQ(d3.size(), 1u);
    EXPECT_EQ(d3[0], (Delivery{rx, 0}));
}

TEST(RedstoneLinks, FrequenciesAndDimensionsSeparateNetworks) {
    LinkNetwork net;
    auto [rx, a] = net.add({0, 0, 0, 0, {"a", ""}, true, 0});
    auto [tx, b] = net.add({0, 1, 0, 0, {"b", ""}, false, 15});
    EXPECT_TRUE(b.empty());
    auto c = net.setKey(tx, {"a", ""});
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0], (Delivery{rx, 15}));
    auto [other, d] = net.add({1, 0, 0, 0, {"a", ""}, true, 0});
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].power, 0); // another dimension
    EXPECT_TRUE(net.anyPower({"a", ""}));
    EXPECT_FALSE(net.anyPower({"b", ""}));
}

TEST(RedstoneLinks, RangeIsMeasuredFromTheChangedNode) {
    LinkNetwork net;
    net.setRange(10);
    LinkKey key{"x", "y"};
    auto [near, a] = net.add({0, 9, 0, 0, key, true, 0});
    auto [far, b]  = net.add({0, 25, 0, 0, key, true, 0});
    auto [tx, c]   = net.add({0, 0, 0, 0, key, false, 15});
    ASSERT_EQ(c.size(), 1u); // only the receiver within 10 blocks of the transmitter
    EXPECT_EQ(c[0], (Delivery{near, 15}));
    EXPECT_FALSE(LinkNetwork::withinRange({0, 0, 0, 0}, {0, 10, 0, 0}, 10)); // strictly closer
    EXPECT_EQ(net.powerAt(0, key, 5, 0, 0), 15);
    EXPECT_EQ(net.powerAt(0, key, 30, 0, 0), 0);
    (void)far;
    (void)a;
    (void)b;
}

TEST(RedstoneLinks, SwitchingModeKeepsFrequencies) {
    LinkNetwork net;
    LinkKey     key{"x", ""};
    auto [rx, a] = net.add({0, 0, 0, 0, key, true, 0});
    auto [n, b]  = net.add({0, 2, 0, 0, key, true, 0});
    auto d       = net.setListening(n, false, 9);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0], (Delivery{rx, 9}));
    EXPECT_EQ(net.node(n)->key, key);
}
