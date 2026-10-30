#pragma once

// Create 6.0.10's RoseQuartzLampBlock and DeskBellBlock as pure rules (U23 / U49, SPEC §3.6.23).
// The world adapter (src/feature/redstone/BellLamp.cpp) reads the world through the callbacks.

#include <array>
#include <functional>
#include <vector>

namespace lv::core::redstone {

// ---- rose quartz lamp ----------------------------------------------------------------------------

struct LampState {
    bool powered  = false; ///< the input seen last (POWERED)
    bool powering = false; ///< lit, emits redstone (POWERING)
    bool activate = false; ///< turns on at its scheduled tick (ACTIVATE)
    friend bool operator==(LampState const&, LampState const&) = default;
};

/// A neighbour change of a lamp whose input is now `input` (RoseQuartzLampBlock.neighborChanged).
enum class LampChange {
    None,     ///< nothing to do
    Unpower,  ///< only POWERED goes off (the lamp stays lit)
    Activate, ///< the cluster goes dark and this lamp lights up (POWERED, POWERING, ACTIVATE)
};
[[nodiscard]] LampChange lampNeighbourChanged(LampState const& s, bool input);

/// The scheduled tick of a lamp: POWERING becomes ACTIVATE, ACTIVATE clears.
[[nodiscard]] LampState lampTick(LampState s);

/// Lamps within 16 blocks (Manhattan) connected to `start` through lamps (Create's
/// forEachInCluster: `start` itself is not listed).
using LampPos = std::array<int, 3>;
[[nodiscard]] std::vector<LampPos> lampCluster(LampPos start, std::function<bool(LampPos const&)> const& isLamp);
/// Manhattan limit of a cluster.
inline constexpr int kLampClusterRange = 16;

/// What a comparator reads from a lamp: 15 when it is lit, one less for every lamp further along
/// `step` up to the first lit one, 0 when the column ends first (getDistanceToPowered).
/// `lampAt(i)`: the i-th block of the column (0 = the lamp next to the comparator): -1 not a lamp,
/// 0 an unlit lamp, 1 a lit lamp.
[[nodiscard]] int lampDistanceToPowered(std::function<int(int i)> const& lampAt);

// ---- desk bell -----------------------------------------------------------------------------------

/// Ticks the bell stays powered after a ring (DeskBellBlockEntity.blockStateTimer).
inline constexpr int kDeskBellPulse = 20;
/// The bell animation (client): starts at 1 and falls linearly by this per tick.
inline constexpr float kDeskBellAnimationStep = .05f;
/// Plunger offset (blocks, downwards) and bell tilt (degrees about x, z) at animation value `p`
/// and the random phase `offset` (DeskBellRenderer).
struct BellPose {
    float plunger = 0;
    float tiltX = 0, tiltZ = 0;
};
[[nodiscard]] BellPose deskBellPose(float p, float offset);

} // namespace lv::core::redstone
