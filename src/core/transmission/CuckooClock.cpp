#include "core/transmission/CuckooClock.h"

#include <cmath>
#include <string_view>

namespace lv::core::transmission {

bool operator==(ClockSound const& a, ClockSound const& b) {
    return std::string_view(a.name) == std::string_view(b.name) && a.volume == b.volume && a.pitch == b.pitch;
}

ClockTime clockTime(std::int64_t dayTime, bool natural) {
    std::int64_t scaled = dayTime * (natural ? 1 : 24);
    int          t      = static_cast<int>(scaled % 24000);
    if (t < 0) t += 24000;
    return ClockTime{(t / 1000 + 6) % 24, (t % 1000) * 60 / 1000};
}

CuckooAnimation cuckooTrigger(ClockTime t) {
    if (t.hours == 12 && t.minutes < 5) return CuckooAnimation::Pig;
    if (t.hours == 18 && t.minutes < 36 && t.minutes > 31) return CuckooAnimation::Creeper;
    return CuckooAnimation::None;
}

CuckooAnimation cuckooStarted(CuckooAnimation requested, bool mysterious) {
    if (requested != CuckooAnimation::None && mysterious) return CuckooAnimation::Surprise;
    return requested;
}

float hourHandAngle(int hours) { return static_cast<float>(360 / 12 * (((hours % 12) + 12) % 12)); }
float minuteHandAngle(int minutes) { return static_cast<float>(360 / 60 * minutes); }

float chaseAngle(float current, float target, float speed) {
    // AngleHelper.getShortestAngleDiff, then Chaser.EXP; snaps when close (LerpedFloat)
    float diff = std::fmod(target - current, 360.f);
    if (diff < -180.f) diff += 360.f;
    if (diff >= 180.f) diff -= 360.f;
    float next = current + diff * speed;
    if (std::fabs(diff * (1 - speed)) < 1.f / 512) next = current + diff;
    return next;
}

int hourHandState(ClockTime t) { return ((t.hours % 12) + 12) % 12; }
int minuteHandState(ClockTime t) { return (t.minutes / 5) % 12; }

namespace {
float lerp(float delta, float start, float end) { return start + delta * (end - start); }
} // namespace

float cuckooDoorAngle(CuckooAnimation type, float value) {
    if (type == CuckooAnimation::None) return 0;
    float angle = 0;
    int   step  = type == CuckooAnimation::Surprise ? 3 : 15;
    for (int phase = 30; phase <= 60; phase += step) {
        float local = value - static_cast<float>(phase);
        if (local < static_cast<float>(-step / 3)) continue;
        if (local < 0) angle = lerp((value - static_cast<float>(phase - 5)) / 5.f, 0, 135);
        else if (local < static_cast<float>(step / 3)) angle = 135;
        else if (local < static_cast<float>(2 * step / 3)) angle = lerp((value - static_cast<float>(phase + 5)) / 5.f, 135, 0);
    }
    return angle;
}

float cuckooFigureOffset(float doorAngle) { return -(doorAngle / 135.f) * 1.f / 2.f + 10.f / 16.f; }

int cuckooModelState(CuckooAnimation type, float value) {
    if (type == CuckooAnimation::None) return 0;
    if (cuckooDoorAngle(type, value) < 67.5f) return 0;
    return type == CuckooAnimation::Pig ? 1 : 2;
}

std::optional<ClockSound> cuckooTickSound(std::uint64_t gameTick, bool natural) {
    if (natural) {
        if (gameTick % 32 == 0) return ClockSound{"note.hat", 1.f / 16, 2.f};
        if (gameTick % 16 == 0) return ClockSound{"note.hat", 1.f / 16, 1.5f};
        return std::nullopt;
    }
    if (gameTick % 6 == 0) return ClockSound{"note.hat", 1.f / 16, 2.f};
    if (gameTick % 3 == 0) return ClockSound{"note.hat", 1.f / 16, 1.5f};
    return std::nullopt;
}

std::vector<ClockSound> cuckooAnimationSounds(CuckooAnimation type, int value) {
    std::vector<ClockSound> out;
    if (type == CuckooAnimation::None) return out;
    bool surprise = type == CuckooAnimation::Surprise;
    if (value == 1) out.push_back({"note.chime", 2.f, .5f});
    if (value == 21) out.push_back({"note.chime", 2.f, 0.793701f});
    if (value == 40 && surprise) out.push_back({"random.fuse", 1.f, 1.f});
    int step = surprise ? 3 : 15;
    for (int phase = 30; phase <= 60; phase += step) {
        if (value == phase - step / 3) out.push_back({"random.chestopen", 1.f / 16, 2.f});
        if (value == phase) {
            if (type == CuckooAnimation::Pig) out.push_back({"mob.pig.say", 1.f / 4, 1.f});
            else out.push_back({"mob.creeper.say", 1.f / 4, 3.f});
        }
        if (value == phase + step / 3) out.push_back({"random.chestclosed", 1.f / 16, 2.f});
    }
    return out;
}

bool cuckooSmokes(CuckooAnimation type, int value) { return type == CuckooAnimation::Surprise && value > 30; }

} // namespace lv::core::transmission
