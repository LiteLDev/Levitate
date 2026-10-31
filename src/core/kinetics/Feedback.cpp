#include "core/kinetics/Feedback.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace lv::core::kinetics::feedback {

float overstressEffectAfter(float initial, int ticks) {
    float v = initial;
    for (int i = 0; i < ticks && v != 0; ++i) {
        v -= v * .1f;
        if (std::fabs(v) < 1.f / 128.f) v = 0;
    }
    return v;
}

std::array<float, 3> overstressTint(float effect) {
    if (effect == 0) return {1.f, 1.f, 1.f};
    float                w      = std::min(std::fabs(effect), 1.f);
    std::array<float, 3> target = effect > 0 ? std::array<float, 3>{1.f, 0.f, 0.f} : std::array<float, 3>{0.f, 1.f, 127.f / 255.f};
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i) out[i] = 1.f + (target[i] - 1.f) * w;
    return out;
}

float chaseExp(float current, float target, float speed) {
    if (std::fabs(current - target) < 1e-5f) return target; // Mth.equal
    return current + (target - current) * speed;
}

unsigned speedLevelColour(SpeedLevel level) {
    switch (level) {
    case SpeedLevel::Slow:
        return 0x22FF22;
    case SpeedLevel::Medium:
        return 0x0084FF;
    case SpeedLevel::Fast:
        return 0xFF55FF;
    default:
        return 0x000000;
    }
}

int speedLevelParticleSpeed(SpeedLevel level) {
    switch (level) {
    case SpeedLevel::Slow:
        return 10;
    case SpeedLevel::Medium:
        return 20;
    case SpeedLevel::Fast:
        return 30;
    default:
        return 0;
    }
}

unsigned fadeColour(unsigned rgb) {
    auto mix = [](unsigned c) { return static_cast<unsigned>(std::lround(c + (255.0 - c) * .5)); };
    unsigned r = mix((rgb >> 16) & 0xFF), g = mix((rgb >> 8) & 0xFF), b = mix(rgb & 0xFF);
    return (r << 16) | (g << 8) | b;
}

bool queuesIndicator(bool generator, bool hasSource, float prevSpeed, float speed, float mediumSpeed, float fastSpeed) {
    if (!generator || hasSource || prevSpeed == speed) return false;
    return speedLevel(prevSpeed, mediumSpeed, fastSpeed) != speedLevel(speed, mediumSpeed, fastSpeed);
}

std::optional<Indicator> indicatorFor(float speed, std::optional<Axis> axis, CogType cog, float mediumSpeed, float fastSpeed) {
    if (speed == 0 || !axis) return std::nullopt;
    auto      level = speedLevel(speed, mediumSpeed, fastSpeed);
    Indicator out;
    out.colour  = speedLevelColour(level);
    out.fade    = fadeColour(out.colour);
    out.speed   = speedLevelParticleSpeed(level) * (speed > 0 ? 1 : -1);
    bool large  = cog == CogType::Large;
    out.radius1 = large ? 1.f : .75f;
    out.radius2 = large ? 1.125f : .65f;
    out.axis    = *axis;
    return out;
}

float ambientPitch(float speed) { return std::clamp(std::abs(speed) / 256.f + .45f, .85f, 1.f); }

PitchGroup pitchGroupOf(float pitch) {
    if (pitch < .70f) return PitchGroup::VeryLow;
    if (pitch < .90f) return PitchGroup::Low;
    if (pitch < 1.10f) return PitchGroup::Normal;
    if (pitch < 1.30f) return PitchGroup::High;
    return PitchGroup::VeryHigh;
}

float relativeVolume(Ambience group) { return group == Ambience::Cog ? 1.5f : .025f; }

float scapeVolume(int count, double distance, float volumeCap) {
    double multiplier = 2.0 + (0.0 - 2.0) * (distance / kMaxAmbientDistance); // Mth.lerp(d / 16, 2, 0)
    double argMax     = kSoundVolumeArgMax;
    double v          = std::clamp(count / (argMax * 10.0), .025, std::max(.025, static_cast<double>(volumeCap)));
    return static_cast<float>(v * multiplier);
}

bool inAmbientRange(GlobalPos const& source, GlobalPos const& listener) {
    if (source.dim != listener.dim) return false;
    double dx = source.x - listener.x, dy = source.y - listener.y, dz = source.z - listener.z;
    return dx * dx + dy * dy + dz * dz < static_cast<double>(kMaxAmbientDistance) * kMaxAmbientDistance;
}

std::vector<Scape> groupSources(std::vector<Source> const& sources) {
    std::map<std::pair<Ambience, PitchGroup>, Scape> scapes;
    for (auto const& s : sources) {
        auto  key        = std::make_pair(s.group, pitchGroupOf(s.pitch));
        auto [it, fresh] = scapes.try_emplace(key);
        auto& sc         = it->second;
        if (fresh) {
            sc.group      = s.group;
            sc.pitchGroup = key.second;
            sc.pitch      = s.pitch;
        }
        sc.x += s.pos.x + .5;
        sc.y += s.pos.y + .5;
        sc.z += s.pos.z + .5;
        ++sc.count;
    }
    std::vector<Scape> out;
    for (auto& [k, sc] : scapes) {
        sc.x /= sc.count;
        sc.y /= sc.count;
        sc.z /= sc.count;
        out.push_back(sc);
    }
    return out;
}

} // namespace lv::core::kinetics::feedback
