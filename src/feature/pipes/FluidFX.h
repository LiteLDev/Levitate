#pragma once

// Fluid particles (Create's FluidFX, PipeConnection.spawnParticles and the spout's particles) for
// every client, mod or not: the server spawns the resource pack particles
//   levitate:fluid_particle  Create's FLUID_PARTICLE (a bit of the fluid, falling and landing)
//   levitate:fluid_drip      Create's dripping particles (water / lava drips and FLUID_DRIP)
// tinted with the fluid's colour, for the players within `range` blocks of the particle (Create
// spawns them on the client within MAX_PARTICLE_RENDER_DISTANCE = 20 blocks of the camera).
//
// Used by the pipes unit (rim drips, splashes, the stream of open ends) and the fluids unit
// (spout). Server thread only.

#include <cstdint>
#include <string>

#include "core/GlobalPos.h"
#include "core/pipes/Types.h"

namespace lv::feature::pipes::fx {

struct V3 {
    double x = 0, y = 0, z = 0;
};

/// PipeConnection.MAX_PARTICLE_RENDER_DISTANCE.
inline constexpr double kParticleRange = 20;
/// PipeConnection.RIM_RADIUS.
inline constexpr double kRimRadius = 1 / 4. + 1 / 64.;
/// PipeConnection.IDLE_PARTICLE_SPAWN_CHANCE.
inline constexpr double kIdleParticleChance = 1 / 1000.;

enum class Kind : std::uint8_t { Fluid, Drip };

/// One particle at `at` moving `motionPerTick` (Java blocks per tick) for the players near it.
/// Returns how many players it was sent to.
int spawn(int dim, V3 at, V3 motionPerTick, std::string const& fluid, Kind kind, double range = kParticleRange);

/// Create's FluidFX.spawnRimParticles with the fluid's dripping particle (PipeConnection's rim drips).
void rimParticles(core::GlobalPos const& pos, core::pipes::Face side, int amount, std::string const& fluid);
/// Create's FluidFX.spawnPouringLiquid: the stream of an open end (out of the pipe, or sucked in).
void pouringLiquid(core::GlobalPos const& pos, core::pipes::Face side, int amount, std::string const& fluid, bool inbound);

/// Uniform random number in [0, 1) (shared generator of the effects).
[[nodiscard]] double random01();

/// Particles spawned since the world was loaded, per kind (selftests).
struct Stats {
    std::uint64_t fluid = 0, drip = 0, sent = 0;
};
[[nodiscard]] Stats const& stats();
void                       resetStats();

} // namespace lv::feature::pipes::fx
