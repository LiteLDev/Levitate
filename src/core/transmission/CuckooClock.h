#pragma once

// Pure rules of the cuckoo clock (U6 / U49, SPEC §3.6.6): Create 6.0.10's CuckooClockBlockEntity
// (time of day, the noon / dusk animations, the mysterious clock's surprise, its sounds) and
// CuckooClockRenderer (hands, doors and the figure). No game types: the world adapter
// (src/feature/transmission/Clock.cpp) and the client renderer call these.

#include <cstdint>
#include <optional>
#include <vector>

namespace lv::core::transmission {

/// Create's CuckooClockBlockEntity.Animation (state values of `levitate:cuckoo` use 1 / 2 for the
/// pig / creeper figure; the surprise shows the creeper).
enum class CuckooAnimation : int { None = 0, Pig = 1, Creeper = 2, Surprise = 3 };

struct ClockTime {
    int hours   = 0; ///< 0..23
    int minutes = 0; ///< 0..59
};

/// Create's time of day: in a natural dimension the day time, elsewhere 24 times faster (the
/// hands spin). Day time 0 is 6:00.
[[nodiscard]] ClockTime clockTime(std::int64_t dayTime, bool natural);

/// The animation a clock with no animation running starts at `t` (None: none). Create: the pig
/// at noon (12:00..12:04), the creeper at dusk (18:32..18:35).
[[nodiscard]] CuckooAnimation cuckooTrigger(ClockTime t);
/// Create's startAnimation: a mysterious clock turns every animation into the surprise.
[[nodiscard]] CuckooAnimation cuckooStarted(CuckooAnimation requested, bool mysterious);

/// The animation runs while its progress is at most this (Create: `value > 100` ends it).
inline constexpr int kCuckooAnimationEnd = 100;
/// The mysterious clock explodes when its progress reaches this (Create: 50).
inline constexpr int kCuckooSurpriseTick = 50;
/// Explosion power of the surprise (Create: 3, no fire, breaks blocks).
inline constexpr float kCuckooSurprisePower = 3.f;

/// Hand target angles in degrees, clockwise from 12 (Create's moveHands).
[[nodiscard]] float hourHandAngle(int hours);
[[nodiscard]] float minuteHandAngle(int minutes);
/// One step of Create's LerpedFloat.angular chasing `target` with Chaser.EXP at `speed` (0.2).
[[nodiscard]] float chaseAngle(float current, float target, float speed = .2f);

/// The static hand positions of the block model (vanilla clients): hour 0..11, minute 0..11 (5
/// minute steps).
[[nodiscard]] int hourHandState(ClockTime t);
[[nodiscard]] int minuteHandState(ClockTime t);

/// Door angle in degrees (0 closed, 135 open) at animation progress `value` (CuckooClockRenderer,
/// including its overshoot while the surprise flaps the doors).
[[nodiscard]] float cuckooDoorAngle(CuckooAnimation type, float value);
/// How far the figure slides out (blocks, Create's `-(angle / 135) / 2 + 10 / 16`; 10 / 16 is in).
[[nodiscard]] float cuckooFigureOffset(float doorAngle);
/// The `levitate:cuckoo` state of the static model at `value`: 0 doors closed, 1 open with the
/// pig out, 2 open with the creeper out (doors at least half open).
[[nodiscard]] int cuckooModelState(CuckooAnimation type, float value);

/// A sound of the clock (Bedrock sound names of Create's vanilla sounds).
struct ClockSound {
    char const* name   = "";
    float       volume = 1;
    float       pitch  = 1;
    friend bool operator==(ClockSound const& a, ClockSound const& b);
};
/// The idle tick (note block hat): every 16 game ticks in a natural dimension (pitch 2 on
/// multiples of 32, else 1.5), every 3 ticks elsewhere (2 on multiples of 6).
[[nodiscard]] std::optional<ClockSound> cuckooTickSound(std::uint64_t gameTick, bool natural);
/// The sounds at animation progress `value` (the value before this tick's increment): chimes,
/// door creaks, the figure's voice, the surprise's fuse.
[[nodiscard]] std::vector<ClockSound> cuckooAnimationSounds(CuckooAnimation type, int value);
/// The surprise smokes from progress 31 on.
[[nodiscard]] bool cuckooSmokes(CuckooAnimation type, int value);

} // namespace lv::core::transmission
