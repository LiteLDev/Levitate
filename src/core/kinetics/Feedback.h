#pragma once

// Kinetic feedback rules (U37): Create's rotation indicator particles (KineticEffectHandler,
// RotationIndicatorParticle) and ambient kinetic soundscapes (KineticBlockEntity.tickAudio,
// SoundScapes, SoundScape). Pure maths; the server adapter is feature/kinetics/Feedback.cpp.
// See docs/SPEC.md §3.1 "Kinetic feedback".

#include <cstdint>
#include <optional>
#include <vector>

#include "core/kinetics/Solver.h"
#include "core/kinetics/Spec.h"

namespace lv::core::kinetics::feedback {

// ---- rotation indicators ---------------------------------------------------------------------------
/// Ticks between a generator's speed level change and its particles (queueRotationIndicators).
inline constexpr int kIndicatorDelay = 2;
/// Particles per burst (ServerLevel.sendParticles count).
inline constexpr int kIndicatorCount = 20;
/// Base particle life span in ticks (the particle adds 0..31-span random ticks).
inline constexpr int kIndicatorLifeSpan = 10;
/// Players within this distance of the block receive the burst (sendParticles).
inline constexpr double kIndicatorRange = 32;

/// Create's SpeedLevel colours (RGB): slow 0x22FF22, medium 0x0084FF, fast 0xFF55FF, none 0.
[[nodiscard]] unsigned speedLevelColour(SpeedLevel level);
/// Create's SpeedLevel particle speeds (degrees per tick): 10 / 20 / 30, none 0.
[[nodiscard]] int speedLevelParticleSpeed(SpeedLevel level);
/// The fade colour of the particle: the colour mixed half way with white (Color.mixColors).
[[nodiscard]] unsigned fadeColour(unsigned rgb);

/// GeneratingKineticBlockEntity.updateGeneratedRotation: a generator without a source whose speed
/// level changes queues the indicators.
[[nodiscard]] bool queuesIndicator(bool generator, bool hasSource, float prevSpeed, float speed, float mediumSpeed, float fastSpeed);

struct Indicator {
    unsigned colour    = 0; ///< RGB
    unsigned fade      = 0; ///< RGB
    int      speed     = 0; ///< signed degrees per tick
    float    radius1   = 0; ///< initial radius (blocks)
    float    radius2   = 0; ///< target radius
    int      lifeSpan  = kIndicatorLifeSpan;
    Axis     axis      = Axis::Y;
    int      count     = kIndicatorCount;
};
/// KineticEffectHandler.spawnRotationIndicators for the block's current (effective) speed: nothing
/// at speed 0 or without a rotation axis. Radii: KineticBlock .75 -> .65, large cogs 1 -> 1.125.
[[nodiscard]] std::optional<Indicator> indicatorFor(float speed, std::optional<Axis> axis, CogType cog, float mediumSpeed, float fastSpeed);

// ---- soundscapes ------------------------------------------------------------------------------------
enum class Ambience : std::uint8_t { Kinetic, Cog };
enum class PitchGroup : std::uint8_t { VeryLow, Low, Normal, High, VeryHigh };

/// SoundScapes.MAX_AMBIENT_SOURCE_DISTANCE: sources farther (block positions) are not heard.
inline constexpr int kMaxAmbientDistance = 16;
/// SoundScapes.SOUND_VOLUME_ARG_MAX.
inline constexpr int kSoundVolumeArgMax = 15;
/// Ticks between two plays of the 0.5 s ambient samples (Levitate: Create's sound loops).
inline constexpr int kAmbientInterval = 10;

/// KineticBlockEntity.tickAudio: clamp(|speed| / 256 + .45, .85, 1).
[[nodiscard]] float ambientPitch(float speed);
/// SoundScapes.PitchGroup.of.
[[nodiscard]] PitchGroup pitchGroupOf(float pitch);
/// The volume of a sound in the soundscape's continuous sound (kinetic .025, cogs 1.5).
[[nodiscard]] float relativeVolume(Ambience group);
/// SoundScape.getVolume: clamp(count / (argMax * 10), .025, cap) * lerp(distance / 16, 2, 0).
[[nodiscard]] float scapeVolume(int count, double distance, float volumeCap);
/// Create's outOfRange: block positions closer than 16 (squared distance < 256).
[[nodiscard]] bool inAmbientRange(GlobalPos const& source, GlobalPos const& listener);

struct Source {
    Ambience  group = Ambience::Kinetic;
    GlobalPos pos;
    float     pitch = 1;
};
struct Scape {
    Ambience   group      = Ambience::Kinetic;
    PitchGroup pitchGroup = PitchGroup::Normal;
    float      pitch      = 1;     ///< of the first source (SoundScape keeps its creation pitch)
    double     x = 0, y = 0, z = 0; ///< mean of the block centres (getMeanPos)
    int        count = 0;
};
/// SoundScapes.addSound + SoundScape: groups the sources by (group, pitch group), in the order
/// kinetic before cogs, then pitch group; a scape's pitch is its first source's.
[[nodiscard]] std::vector<Scape> groupSources(std::vector<Source> const& sources);

} // namespace lv::core::kinetics::feedback
