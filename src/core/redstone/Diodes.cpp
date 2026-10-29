#include "core/redstone/Diodes.h"

#include <algorithm>
#include <cmath>

namespace lv::core::redstone {

int defaultDelay(DiodeKind kind) { return kind == DiodeKind::Timer ? 20 : 2; }

void tickDiode(DiodeKind kind, int delay, bool powered, DiodeState& s) {
    bool atMin = s.state <= 0;
    switch (kind) {
    case DiodeKind::Repeater:
        // count up from the rising edge: the output turns on one tick before the delay is
        // reached and off two ticks later; the counter rests above the delay until the input
        // and the output are both off again
        if (atMin && !powered) return;
        if (s.state > delay + 1) {
            if (!powered && !s.powering) s.state = 0;
            return;
        }
        ++s.state;
        if (s.state == delay - 1 && !s.powering) s.powering = true;
        if (s.state == delay + 1 && s.powering) s.powering = false;
        return;
    case DiodeKind::Extender:
        // on while the input is on; after it goes off, count down from the delay and turn off
        if (atMin && !powered) return;
        if (atMin || powered) {
            s.powering = true;
            s.state    = delay;
            return;
        }
        if (s.state == 1) {
            s.powering = false;
            if (!powered) s.state = 0;
            return;
        }
        if (!powered) --s.state;
        return;
    case DiodeKind::Timer:
        // count while unpowered, restart at the delay; a power input holds it at zero
        if (powered || s.state >= delay - 1) s.state = 0;
        else ++s.state;
        s.powering = !powered && (delay == 2 ? s.state == 0 : s.state <= 1);
        return;
    }
}

float diodeProgress(int state, int delay) {
    int max = std::max(2, delay);
    return static_cast<float>(std::clamp(state, 0, max)) / static_cast<float>(max);
}

// ---- delay settings ----------------------------------------------------------------------------

DelaySettings delayToSettings(int ticks) {
    if (ticks > 60 * 20) return {2, ticks / (60 * 20)};
    if (ticks > 60) return {1, ticks / 20};
    return {0, ticks};
}

int settingsToDelay(DelaySettings s) {
    int multiplier = s.row == 0 ? 1 : s.row == 1 ? 20 : 60 * 20;
    int ticks      = std::max(2, std::max(1, s.value) * multiplier);
    return std::clamp(ticks, kMinDelay, kMaxDelay);
}

std::string formatDelaySettings(DelaySettings s) {
    int value = std::max(1, s.value);
    switch (s.row) {
    case 0:
        return std::to_string(std::max(2, value)) + "t";
    case 1:
        return "0:" + std::string(value < 10 ? "0" : "") + std::to_string(value);
    default:
        return std::to_string(value) + ":00";
    }
}

std::string formatDelay(int ticks) {
    if (ticks < 60) return std::to_string(ticks) + "t";
    if (ticks < 20 * 60) return std::to_string(ticks / 20) + "s";
    return std::to_string(ticks / 20 / 60) + "m";
}

// ---- scheduling --------------------------------------------------------------------------------

bool advanceSchedule(ScheduledTick& t, bool needsTick, int delay) {
    if (t.pending()) {
        if (--t.countdown <= 0) {
            t.countdown = -1;
            return true;
        }
        return false;
    }
    if (needsTick) t.countdown = std::max(1, delay);
    return false;
}

bool diodeInputTick(bool powered, bool input, ScheduledTick& t, int delay) {
    if (powered && !input) return false;
    if (!powered) {
        if (!input) t.countdown = std::max(1, delay); // a short pulse still lasts one delay
        return true;
    }
    return powered;
}

// ---- latches -----------------------------------------------------------------------------------

LatchState poweredLatchTick(LatchState s, bool back, bool side) {
    if (s.powered != back) {
        s.powered = back;
        if (back) s.powering = true;
        else if (s.poweredSide) s.powering = false;
    }
    if (s.poweredSide != side) {
        s.poweredSide = side;
        if (side) s.powering = false;
        else if (s.powered) s.powering = true;
    }
    return s;
}

bool poweredLatchActivate(LatchState& s) {
    if (s.powered != s.poweredSide) return false;
    s.powering = !s.powering;
    return true;
}

ToggleLatchState toggleLatchTick(ToggleLatchState s, bool input, ScheduledTick& t) {
    bool before = s.powered;
    s.powered   = diodeInputTick(s.powered, input, t, kLatchInputDelay);
    if (s.powered && !before) s.powering = !s.powering;
    return s;
}

// ---- threshold switch --------------------------------------------------------------------------

bool thresholdState(bool current, int level, int offBelow, int onAbove) {
    if (current && level <= offBelow) return false;
    if (!current && level >= onAbove) return true;
    return current;
}

int thresholdDisplayLevel(int level, int min, int max) {
    if (level <= 0) return 0;
    if (max <= min) return level > min ? 5 : 0; // Create divides by zero here: +inf / NaN
    float normed = static_cast<float>(level - min) / static_cast<float>(max - min);
    return std::clamp(static_cast<int>(1 + normed * 4), 0, 5);
}

std::pair<int, int> clampThresholds(int offBelow, int onAbove, int min, int max, int step, bool changedUpper) {
    step = std::max(1, step);
    auto round = [step](int v) { return v / step * step; };
    // ranges of the two scroll inputs (Create: on in [min + step, max], off in [min, max - step])
    onAbove  = std::clamp(round(onAbove), min + step, std::max(min + step, max));
    offBelow = std::clamp(round(offBelow), min, std::max(min, max - step));
    if (changedUpper) {
        if (offBelow / step >= onAbove / step) offBelow = std::max(min, (onAbove - step) / step * step);
    } else {
        if (onAbove / step <= offBelow / step) onAbove = (offBelow + step) / step * step;
    }
    return {offBelow, onAbove};
}

} // namespace lv::core::redstone
