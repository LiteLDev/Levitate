#pragma once

// Redstone components of Create 6.0.10 as pure state machines (U23, SPEC §3.6.23):
// pulse repeater / extender / timer (Create's brass diodes), powered latch, powered toggle latch,
// the delay value settings and the vanilla diode tick scheduling they build on.
//
// No game types: the world adapter (src/feature/redstone/) reads the inputs, calls these once per
// game tick and writes the resulting block states.

#include <string>
#include <utility>

namespace lv::core::redstone {

// ---- brass diodes ------------------------------------------------------------------------------

enum class DiodeKind { Repeater, Extender, Timer };

/// Delay limits of the brass diodes (Create: 2 ticks .. 60 minutes).
inline constexpr int kMinDelay = 2;
inline constexpr int kMaxDelay = 60 * 20 * 60;
/// Ticks between an input change and the diode's `powered` state following it (Create: 2).
inline constexpr int kBrassDiodeInputDelay = 2;
/// The same for both latches (Create: 1).
inline constexpr int kLatchInputDelay = 1;

/// Default delay of a freshly placed diode (repeater / extender: 2 ticks, timer: 20 ticks).
[[nodiscard]] int defaultDelay(DiodeKind kind);

/// The block entity part of a brass diode: a counter and the `powering` block state.
struct DiodeState {
    int  state    = 0;
    bool powering = false;
};

/// One block entity tick: `powered` is the diode's input state (after the input delay),
/// `delay` its configured delay. Updates `s` like Create's PulseRepeater/PulseExtender/
/// PulseTimer block entities.
void tickDiode(DiodeKind kind, int delay, bool powered, DiodeState& s);

/// The strength a brass diode emits from its front.
[[nodiscard]] constexpr int diodeOutput(bool powering, bool inverted) { return powering != inverted ? 15 : 0; }

/// Progress of the counter for the client display (0..1).
[[nodiscard]] float diodeProgress(int state, int delay);

// ---- delay value settings (Create's BrassDiodeScrollValueBehaviour) ---------------------------

/// The value settings board: rows ticks / seconds / minutes, columns 0..60, milestones every 10.
inline constexpr int kDelayBoardMax       = 60;
inline constexpr int kDelayBoardMilestone = 10;

struct DelaySettings {
    int  row   = 0;
    int  value = 0;
    friend bool operator==(DelaySettings const&, DelaySettings const&) = default;
};
/// The board position showing a delay.
[[nodiscard]] DelaySettings delayToSettings(int ticks);
/// The delay picked on the board (at least 2 ticks, at most 60 minutes).
[[nodiscard]] int settingsToDelay(DelaySettings s);
/// Label of a board position: "5t", "0:07", "3:00".
[[nodiscard]] std::string formatDelaySettings(DelaySettings s);
/// Short label of a delay ("5t", "3s", "2m").
[[nodiscard]] std::string formatDelay(int ticks);

// ---- vanilla diode tick scheduling ------------------------------------------------------------

/// Vanilla DiodeBlock behaviour the Create diodes inherit: when the input differs from the
/// `powered` state a block tick is scheduled `delay` ticks later; the tick switches `powered`
/// on (and schedules another tick if the input is already off again) or off.
struct ScheduledTick {
    int countdown = -1; ///< ticks until the scheduled tick runs, -1 = none
    [[nodiscard]] bool pending() const { return countdown >= 0; }
};

/// Called once per game tick with the current input. Returns true when the scheduled tick runs
/// this tick (the caller then applies the block's tick logic, e.g. `diodeInputTick`).
/// `needsTick` is the block's own "state differs from the input" test (checkTickOnNeighbor).
bool advanceSchedule(ScheduledTick& t, bool needsTick, int delay);

/// DiodeBlock.tick for the `powered` state: returns the new `powered` and reschedules `t`
/// when a short pulse must still be switched off.
[[nodiscard]] bool diodeInputTick(bool powered, bool input, ScheduledTick& t, int delay);

// ---- latches ----------------------------------------------------------------------------------

/// Powered latch (Create's PoweredLatchBlock): `powered` = back input, `poweredSide` = side
/// input (reset), `powering` = output.
struct LatchState {
    bool powered     = false;
    bool poweredSide = false;
    bool powering    = false;
    friend bool operator==(LatchState const&, LatchState const&) = default;
};

/// Whether the powered latch must schedule a tick (back or side input changed).
[[nodiscard]] constexpr bool poweredLatchNeedsTick(LatchState s, bool back, bool side) {
    return s.powered != back || s.poweredSide != side;
}
/// The powered latch's block tick: back sets, side resets.
[[nodiscard]] LatchState poweredLatchTick(LatchState s, bool back, bool side);
/// Right-click on a powered latch: toggles the output only when both inputs agree.
/// Returns false (nothing happens, the click passes) otherwise.
bool poweredLatchActivate(LatchState& s);

/// Powered toggle latch (Create's ToggleLatchBlock): each rising edge of the back input
/// (after the latch delay) toggles `powering`.
struct ToggleLatchState {
    bool powered  = false;
    bool powering = false;
    friend bool operator==(ToggleLatchState const&, ToggleLatchState const&) = default;
};
/// The toggle latch's block tick (vanilla DiodeBlock.tick followed by the toggle).
[[nodiscard]] ToggleLatchState toggleLatchTick(ToggleLatchState s, bool input, ScheduledTick& t);

// ---- threshold switch -------------------------------------------------------------------------

/// Create's ThresholdSwitchBlockEntity hysteresis: on at or above `onAbove`, off at or below
/// `offBelow`, otherwise unchanged.
[[nodiscard]] bool thresholdState(bool current, int level, int offBelow, int onAbove);
/// The `level` block state (0..5) shown on the switch.
[[nodiscard]] int thresholdDisplayLevel(int level, int min, int max);
/// Value step of the threshold screen: 1 item, 64 items (stacks) or 1000 mB (buckets).
[[nodiscard]] constexpr int thresholdStep(bool fluid, bool stacks) { return fluid ? 1000 : stacks ? 64 : 1; }
/// Create's ThresholdSwitchScreen keeps `onAbove > offBelow` (in steps) and both inside the
/// target's range; returns the clamped pair (offBelow, onAbove) for a pair edited on the screen.
/// `changedUpper` tells which input the player moved last (the other one is pushed).
[[nodiscard]] std::pair<int, int>
clampThresholds(int offBelow, int onAbove, int min, int max, int step, bool changedUpper);

// ---- analog lever -----------------------------------------------------------------------------

/// Ticks after the last change before an analog lever updates its neighbours (Create: 15).
inline constexpr int kAnalogLeverDelay = 15;
/// Next lever state after a click (sneaking: down), clamped to 0..15.
[[nodiscard]] constexpr int analogLeverNext(int state, bool back) {
    int s = state + (back ? -1 : 1);
    return s < 0 ? 0 : s > 15 ? 15 : s;
}
/// Pitch of the click sound (Create: .25 + (state + 5) / 15 * .5).
[[nodiscard]] constexpr float analogLeverPitch(int state) { return .25f + ((state + 5) / 15.f) * .5f; }

} // namespace lv::core::redstone
