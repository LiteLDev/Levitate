#pragma once

// Boiler maths (U18, SPEC §3.6.18): Create 6.0.10's BoilerData, BoilerHeaters and the steam
// engine / powered shaft numbers, independent of the game.
//
// A fluid tank becomes a boiler when steam engines or whistles are attached to it. Its level
// (0..18) is the smallest of three limits: the size (blocks / 4), the water supply
// (mB per tick / 10) and the heat under it (1 per kindled burner, 2 per seething burner).

#include <array>
#include <utility>
#include <vector>

#include "core/common/Recipe.h"

namespace lv::core::steam {

inline constexpr int   kMaxBoilerLevel          = 18;
inline constexpr int   kWaterSupplyPerLevel     = 10; ///< mB per tick per level
inline constexpr int   kSampleRate              = 5;  ///< ticks between water supply samples
inline constexpr int   kSupplySamples           = 10; ///< samples kept (Create's supplyOverTime)
inline constexpr float kPassiveEngineEfficiency = 1.f / 8.f;

/// Create's BoilerHeaters.getActiveHeat for a heat level: -1 no heat, 0 passive heat
/// (smouldering burner, fire, lava, magma, lit campfires), 1 fading / kindled, 2 seething.
[[nodiscard]] int heatScore(common::HeatLevel level);

/// Colours of the goggle lines (Minecraft ChatFormatting).
enum class Colour { DarkGreen, Green, DarkRed, DarkGray, Gray, Aqua, Blue, Gold };

/// The display limits computed by Create's `calcMinMaxForSize`.
struct BoilerDisplay {
    int maxHeatForSize  = 0;
    int maxHeatForWater = 0;
    int heat            = 0; ///< passive ? 1 : activeHeat
    int minValue        = 0;
    int maxValue        = 0;
    int level           = 0; ///< min(activeHeat, maxHeatForWater, maxHeatForSize)
    bool passive        = false;
};

/// Create's BoilerData (server side).
struct Boiler {
    // pooled water supply
    int                               gatheredSupply = 0;
    std::array<float, kSupplySamples> supplyOverTime{};
    int                               ticksUntilNextSample = 0;
    int                               currentIndex         = 0;
    // heat score
    bool needsHeatLevelUpdate = true;
    bool passiveHeat          = false;
    int  activeHeat           = 0;

    float waterSupply      = 0; ///< mB per tick
    int   attachedEngines  = 0;
    int   attachedWhistles = 0;

    [[nodiscard]] bool isActive() const { return attachedEngines > 0 || attachedWhistles > 0; }
    [[nodiscard]] int  maxHeatForSize(int boilerSize) const;
    [[nodiscard]] int  maxHeatForWater() const;
    [[nodiscard]] bool isPassive(int boilerSize) const;
    /// Share of a full engine each attached engine gets (0..1).
    [[nodiscard]] float engineEfficiency(int boilerSize) const;
    /// The boiler level: min(activeHeat, water limit, size limit).
    [[nodiscard]] int actualHeat(int boilerSize) const;
    [[nodiscard]] BoilerDisplay display(int boilerSize) const;
    /// Whether a whistle on this boiler sounds when powered (Create: active with any heat).
    [[nodiscard]] bool hasSteam() const { return isActive() && (passiveHeat || activeHeat > 0); }

    /// Create's `updateTemperature`: `scores` are the `heatScore`s of the blocks under the boiler.
    /// Returns true when the heat changed.
    bool updateTemperature(std::vector<int> const& scores);
    /// Create's `evaluate` result. Returns true when the counts changed.
    bool setAttachments(int engines, int whistles);
    /// Water accepted by the boiler (BoilerFluidHandler.fill, executed).
    void addSupply(int mb) { gatheredSupply += mb; }
    /// Create's server tick (after the heat update): samples the water supply every
    /// `kSampleRate` ticks. `capacity` is the tank capacity in mB (0: nothing happens).
    /// Returns true when a sample was taken (Create sends the block entity to clients).
    bool tick(int capacity, bool creative);
    /// Create's `clear` (the tank stopped being a multiblock controller).
    void clear();
};

/// Create's `totalSU` on the boiler goggles: efficiency · 16 · max(level, engines) · capacity.
[[nodiscard]] double boilerCapacityShown(Boiler const& boiler, int boilerSize, double engineCapacity);

/// The coloured bar of a goggle line (Create's barComponent): runs of '|' per colour.
[[nodiscard]] std::vector<std::pair<Colour, int>> boilerBars(BoilerDisplay const& d, int level);

// ---- steam engine / powered shaft ---------------------------------------------------------------

/// Create's PoweredShaftBlockEntity.getSpeedModifier: 1 + (efficiency >= 1 ? 3 : min(2, floor(4e))).
[[nodiscard]] int speedModifier(float efficiency);
/// Generated speed of a powered shaft (RPM): direction · 16 · speedModifier when the combined
/// capacity (efficiency · engineCapacity) is positive, else 0.
[[nodiscard]] float poweredShaftSpeed(int direction, float efficiency, double engineCapacity);
/// Stress capacity per RPM of a powered shaft: efficiency · engineCapacity / speedModifier
/// (so the shaft provides efficiency · engineCapacity · 16 SU at its speed).
[[nodiscard]] double poweredShaftCapacity(float efficiency, double engineCapacity);

/// Axes and directions as Create's (Direction order down, up, north, south, west, east).
enum class Dir : int { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };
enum class Axis : int { X = 0, Y = 1, Z = 2 };
[[nodiscard]] Axis axisOf(Dir d);
[[nodiscard]] bool positive(Dir d);
[[nodiscard]] Dir  opposite(Dir d);

/// Create's SteamEngineBlockEntity.tick: the direction (+1 / -1) an engine drives its shaft.
/// `facing` is the engine's facing (away from the tank) or, for floor / ceiling engines, its
/// horizontal facing; `counterClockwise` is the engine's value box; `efficiency` 0 always gives 1.
[[nodiscard]] int conveyedDirection(float efficiency, Axis shaftAxis, Dir facing, bool counterClockwise);

} // namespace lv::core::steam
