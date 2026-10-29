#pragma once

// Additions to the track contract for units that react to trains (added by U27 trains; the
// wave-1 header Tracks.h is unchanged). Signals and track observers (U28) and display sources
// (U24) use it instead of the trains unit's internals.
//
//   tracks::trains::onPointEntered([](uint64_t train, EdgePoint const& p) { ... });  // a train reaches an edge point
//   tracks::trains::onPointLeft(...);                                              // its last carriage passed it
//   tracks::trains::setPassCheck([](uint64_t train, EdgePoint const& p) { return !red(p); });
//
// An edge point is "occupied" by a train while it lies between the train's front and back
// points. The pass check is asked for points ahead of a moving train that face it (the point's
// `facingNode2` side): `false` makes the train brake and wait in front of the point (Create's
// navigation waiting for a signal; manual trains stop too). Stations are never asked.
//
// Server thread only. Implemented in src/feature/trains/.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/GlobalPos.h"
#include "core/tracks/Graph.h"

namespace lv::feature::tracks::trains {

struct TrainInfo {
    std::uint64_t            id = 0;
    std::string              name;
    int                      dim = 0;
    std::string              graph;       ///< graph id of the front point ("" when derailed)
    double                   speed = 0;   ///< blocks per second (negative: backwards)
    bool                     derailed = false;
    std::vector<std::string> points;      ///< edge points the train occupies
    std::string              station;     ///< station it waits at ("" when travelling)
    std::string              destination; ///< station it travels to ("" when none)
    std::string              title;       ///< schedule title (Create's display "train status")
};

struct StationInfo {
    core::GlobalPos                                pos;
    std::string                                    name;
    std::string                                    presentTrain;
    std::vector<std::pair<std::string, int>>       arrivals; ///< (train name, ticks; 0 = present, -1 unknown)
};

/// Every live train.
[[nodiscard]] std::vector<TrainInfo> all();
/// Every train station with its predicted arrivals (Create's station summary display source).
[[nodiscard]] std::vector<StationInfo> stations();

void onPointEntered(std::function<void(std::uint64_t train, core::tracks::EdgePoint const& point)> fn);
void onPointLeft(std::function<void(std::uint64_t train, core::tracks::EdgePoint const& point)> fn);
/// One check (the last registration wins). Without it every point may be passed.
void setPassCheck(std::function<bool(std::uint64_t train, core::tracks::EdgePoint const& point)> fn);

} // namespace lv::feature::tracks::trains
