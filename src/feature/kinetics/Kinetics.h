#pragma once

// Kinetic network contract (U2). Every unit with rotating blocks uses this header.
//
// Registering a block (in a .cpp file, at static init or in the unit's `install()`):
//
//     #include "feature/kinetics/Kinetics.h"
//     namespace k = lv::kinetics;
//     LV_REGISTER_KINETIC_BLOCK("levitate:shaft", k::specs::shaft());
//     LV_REGISTER_KINETIC_BLOCK("levitate:creative_motor", [] {
//         auto s = k::specs::generatorShaft(16384, "levitate:axis");
//         return s;
//     }());
//
// The network follows the world by itself: placing, breaking or changing the listed states of a
// registered block (any cause: players, commands, pistons, explosions, other units) re-propagates
// within the tick. Generators set their speed with `setGeneratedSpeed`; machines read
// `speedAt` / `isOverstressed` or subscribe with `onSpeedChanged`.
//
// Everything here runs on the server thread (integrated server on the client). Speeds are RPM
// (signed: the sign is the direction), stress is SU (see docs/SPEC.md §1.3, §3.1).

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/GlobalPos.h"
#include "core/kinetics/Solver.h"
#include "core/kinetics/Spec.h"
#include "mod/Registry.h"

namespace lv::feature::kinetics {

using core::GlobalPos;
using core::kinetics::Axis;
using core::kinetics::BlockState;
using core::kinetics::CogType;
using core::kinetics::Face;
using core::kinetics::KineticSpec;
using core::kinetics::NetworkInfo;
using core::kinetics::NodeInfo;
using core::kinetics::NodeView;
using core::kinetics::Offset;
using core::kinetics::RenderView;
using core::kinetics::RotorPart;
using core::kinetics::SpeedLevel;
using core::kinetics::StressLevel;
namespace specs = core::kinetics::specs;

// ---- registry --------------------------------------------------------------------------------------------
/// Declares `id` ("levitate:<path>") a kinetic block. Later registrations replace earlier ones.
void registerBlock(std::string id, KineticSpec spec);
/// The spec of `id`, nullptr if it is not a kinetic block. Pointers stay valid for the process.
[[nodiscard]] KineticSpec const* specOf(std::string_view id);
/// Every registered id (sorted).
[[nodiscard]] std::vector<std::string> registeredIds();

// ---- queries ------------------------------------------------------------------------------------------------
/// Effective speed at `pos` (0 while overstressed or when there is no kinetic block).
[[nodiscard]] float speedAt(GlobalPos const& pos);
/// Speed ignoring overstress (Create's `getTheoreticalSpeed`).
[[nodiscard]] float theoreticalSpeedAt(GlobalPos const& pos);
/// Full state of the kinetic block at `pos`.
[[nodiscard]] std::optional<NodeInfo> nodeAt(GlobalPos const& pos);
/// The network of the block at `pos` (stress, capacity, overstressed, size).
[[nodiscard]] std::optional<NetworkInfo> networkAt(GlobalPos const& pos);
/// Members of the network the block at `pos` belongs to (sorted; empty if none).
[[nodiscard]] std::vector<GlobalPos> networkMembers(GlobalPos const& pos);
/// The block's network is overstressed (Create: everything in it stops).
[[nodiscard]] bool isOverstressed(GlobalPos const& pos);
/// Create's speed levels with the configured thresholds (mediumSpeed / fastSpeed).
[[nodiscard]] SpeedLevel speedLevelOf(float speed);
/// SU per RPM a block id uses / provides according to the stress table (config `kinetics.json`).
[[nodiscard]] double stressImpactOf(std::string_view id);
[[nodiscard]] double stressCapacityOf(std::string_view id);

// ---- changes --------------------------------------------------------------------------------------------------
/// Speed (RPM, signed) a generator at `pos` produces; Create's `updateGeneratedRotation`.
void setGeneratedSpeed(GlobalPos const& pos, float rpm);
/// Per-block stress overrides for blocks whose impact / capacity depends on their state
/// (nullopt: back to the stress table).
void setStressImpact(GlobalPos const& pos, std::optional<double> suPerRpm);
void setStressCapacity(GlobalPos const& pos, std::optional<double> suPerRpm);
/// Re-reads the block at `pos` from the world now (normally automatic within the tick).
void refresh(GlobalPos const& pos);

// ---- listeners --------------------------------------------------------------------------------------------------
struct SpeedChange {
    GlobalPos                pos;
    std::string              blockId;
    float                    prevSpeed = 0, speed = 0; ///< effective speeds
    float                    prevTheoretical = 0, theoretical = 0;
    std::optional<GlobalPos> source;
    bool                     added = false, removed = false;
};
using ListenerId = int;
/// Called once per tick (server thread) with the speed changes of blocks with id `blockId`
/// (empty: every kinetic block). Returns an id for `removeListener`.
ListenerId onSpeedChanged(std::string blockId, std::function<void(SpeedChange const&)> fn);
void       removeListener(ListenerId id);

/// A kinetic block broke because of a speed conflict, a too high speed, a ratio loop or
/// flickering (already removed from the network; the world block is broken within the tick).
struct BreakEvent {
    GlobalPos   pos;
    std::string blockId;
    std::string reason;
};
ListenerId onBreak(std::function<void(BreakEvent const&)> fn);

namespace detail {
struct BlockRegistrar {
    BlockRegistrar(std::string id, KineticSpec spec) { registerBlock(std::move(id), std::move(spec)); }
};
} // namespace detail

} // namespace lv::feature::kinetics

namespace lv {
/// Short alias used by the other units: `lv::kinetics::registerBlock(...)`.
namespace kinetics = feature::kinetics;
} // namespace lv

/// Registers a kinetic block at static init: `LV_REGISTER_KINETIC_BLOCK("levitate:shaft", spec);`
#define LV_REGISTER_KINETIC_BLOCK(id, ...)                                                                            \
    static ::lv::feature::kinetics::detail::BlockRegistrar LV_UNIQUE(lv_kinetic_block_)(id, __VA_ARGS__)
