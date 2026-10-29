#pragma once

// Train signals and track observers (U28): contract for trains (U27) and other units.
// Implemented in src/feature/signals/. SPEC §3.6 "Signals and observers (U28)" documents it.
//
// Signals split the track graph into signal sections (Create's signal edge groups). Trains report
// the track they cover; sections, signal states and observers follow. Before a train enters a
// section it asks for the way ahead (Create's Navigation signal scan with chain signalling):
//
//     namespace sig = lv::feature::tracks::signals;
//     // every tick the train moved, after it was assembled and after `tracks::onGraphChanged`:
//     sig::setTrainExtent(trainId, trailingBogeyLocation, leadingBogeyLocation);
//     // every tick while driving (front = the leading point, travelling towards front.node2):
//     auto way = sig::checkAhead(trainId, front, path.edges, brakingDistance + 4);
//     if (!way.clear) brakeToStopWithin(way.stopDistance);   // stop before the signal
//     // when the train stops navigating (schedule cancelled, manual control):
//     sig::releaseReservations(trainId);
//     // disassembled / destroyed:
//     sig::removeTrain(trainId);
//     // path finding (tracks::PathQuery::edgePenalty), Create's red signal penalties:
//     q.edgePenalty = [&](auto const& g, auto const& e) { return sig::pathPenalty(trainId, g, e); };
//
// Track observers need the train's cargo for their filter (Create: items in the carriages'
// inventories, fluids in their tanks). Trains provide it once:
//
//     sig::setCargoProvider([](std::string const& trainId, sig::ItemTest const& item, sig::FluidTest const& fluid) {
//         return anyCarriageItem(trainId, item) || anyCarriageFluid(trainId, fluid);
//     });
//
// Everything runs on the server thread while a world is loaded. Train ids are any stable string
// (Create: the train UUID). Graph types: core/tracks/Graph.h; signal types: core/signals/Signals.h.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/GlobalPos.h"
#include "core/signals/Signals.h"
#include "core/tracks/Graph.h"
#include "feature/common/Capabilities.h"

class ItemStack;

namespace lv::feature::tracks::signals {

using core::signals::Authority;
using core::signals::Crossing;
using core::signals::GroupId;
using core::signals::kNoGroup;
using core::signals::SignalState;
using core::signals::SignalType;
using core::signals::Span;

inline constexpr std::string_view kSignalId   = "levitate:track_signal";
inline constexpr std::string_view kObserverId = "levitate:track_observer";
/// Edge point types (core::tracks::EdgePoint::type) of signal boundaries and observers.
inline constexpr std::string_view kSignalPointType   = "levitate:signal";
inline constexpr std::string_view kObserverPointType = "levitate:observer";

/// Block states of `levitate:track_signal` (Create's SignalBlock TYPE / POWERED + the lamp).
inline constexpr std::string_view kTypeState    = "levitate:signal_type";  ///< SignalType
inline constexpr std::string_view kLampState    = "levitate:signal_state"; ///< lampValue(SignalState)
inline constexpr std::string_view kPoweredState = "levitate:powered";      ///< redstone (signal: input, observer: output)

/// Value of `kLampState` for a state: 0 off (Invalid, the default of a fresh block), 1 red,
/// 2 yellow, 3 green.
[[nodiscard]] constexpr int lampValue(SignalState s) { return s == SignalState::Invalid ? 0 : int(s) + 1; }

/// Create's Train.Penalties used by signals in path finding.
inline constexpr double kRedSignalPenalty         = 25;
inline constexpr double kRedstoneRedSignalPenalty = 400;

// ---- reported by trains -----------------------------------------------------------------------

/// The track a train covers now (every stretch from its last to its first wheel), replacing
/// what it reported before. A section is occupied while any part of a train is on it; observers
/// see the trains over them. Entering a section releases the train's reservation of it.
void setTrainSpans(std::string const& trainId, std::vector<Span> const& spans);
/// Convenience: the spans from `trailing` to `leading` (both travelling the same way, the
/// leading point ahead), found along the graph within `maxLength` blocks. False (nothing
/// changed) when no such way exists.
bool setTrainExtent(
    std::string const&                trainId,
    core::tracks::GraphLocation const& trailing,
    core::tracks::GraphLocation const& leading,
    double                            maxLength = 1024
);
/// The train is gone: its sections are free, its reservations released.
void removeTrain(std::string const& trainId);

// ---- asked by trains --------------------------------------------------------------------------

/// May the train go on? Checks the signals on `path` (edges after `front`'s edge, as
/// core::tracks::PathResult::edges) within `lookahead` blocks, reserves the sections it may
/// enter and tells where to stop otherwise (Create's entry / cross signal rules, SPEC §3.6).
[[nodiscard]] Authority checkAhead(
    std::string const&                     trainId,
    core::tracks::GraphLocation const&     front,
    std::vector<core::tracks::PathStep> const& path,
    double                                 lookahead
);
/// Drops the sections reserved for the train (it no longer follows the path it asked for).
void releaseReservations(std::string const& trainId);
/// Signal boundaries on the way (in order, with the side entered and the distance).
[[nodiscard]] std::vector<Crossing> signalsAhead(
    core::tracks::GraphLocation const&         front,
    std::vector<core::tracks::PathStep> const& path,
    double                                     maxDistance
);
/// Extra path cost for entering `edge` (Create: red signal 25, redstone-forced red signal 400).
[[nodiscard]] double pathPenalty(std::string const& trainId, core::tracks::Graph const& graph, core::tracks::Edge const& edge);

// ---- queries ----------------------------------------------------------------------------------

/// The signal section at a graph location (kNoGroup on track without signals).
[[nodiscard]] GroupId sectionAt(core::tracks::GraphLocation const& loc);
/// Trains in a section.
[[nodiscard]] std::vector<std::string> trainsIn(GroupId section);
/// The state shown by the signal block at `pos` (Invalid when it isn't bound to a track).
[[nodiscard]] SignalState signalState(core::GlobalPos const& pos);
/// Whether the track observer at `pos` is powered (a matching train passes).
[[nodiscard]] bool observerPowered(core::GlobalPos const& pos);

// ---- provided by trains -----------------------------------------------------------------------

using ItemTest      = std::function<bool(ItemStack const&)>;
using FluidTest     = std::function<bool(common::fluids::FluidStack const&)>;
/// Whether any item / fluid the train carries passes the tests.
using CargoProvider = std::function<bool(std::string const& trainId, ItemTest const& item, FluidTest const& fluid)>;
/// Without a provider, observers with a filter never trigger (empty filters always do).
void setCargoProvider(CargoProvider provider);

} // namespace lv::feature::tracks::signals
