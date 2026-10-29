#pragma once

// Display sources and targets (U24, SPEC §2.20 / §3.6 "Display"): the contract other units use to
// feed Display Links (Create's DisplaySource / DisplayTarget registries). Server thread only.
//
// A Display Link sits on a *source* block (the block it is attached to) and sends text to a
// *target* block (selected with the link item before placing it). Which sources a block offers
// and how a block shows text are registered here, per block id:
//
//     #include "feature/display/Display.h"
//     namespace dsp = lv::feature::display;
//     dsp::registerSource("levitate:track_station", dsp::Source{
//         .id   = "station_summary",                       // Create's source id
//         .name = "create.display_source.station_summary", .nameFallback = "Train Station Summary",
//         .text = [](dsp::Context const& ctx, dsp::TargetStats const& stats) -> dsp::Lines { ... },
//     });
//
// Built-in (this unit): sources for vanilla blocks (command block scoreboard, respawn anchor
// deaths, enchanting table, target block), Create's blocks of every unit by id (speedometer,
// stressometer, cuckoo clock, smart observer, threshold switch, seats, belts / depots / ejector,
// tunnels, nixie tubes) and the targets display board, nixie tube, sign and lectern. Units that
// own a block whose source needs their internal state (boilers, stations, track observers,
// elevator contacts, factory gauges, packages) register the source themselves; a later
// registration with the same (block, source id) replaces the earlier one.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"

class BlockSource;
class Block;
class Player;

namespace lv::feature::display {

using Json = nlohmann::json;
using core::GlobalPos;
using Lines = std::vector<std::string>;

/// Size of what a target can show (Create's DisplayTargetStats).
struct TargetStats {
    int         maxRows    = 1;
    int         maxColumns = 1;
    std::string targetId; ///< "display_board", "nixie_tube", "sign", "lectern"
};

/// What a source / target call sees (Create's DisplayLinkContext).
struct Context {
    BlockSource& region;
    GlobalPos    link;   ///< the display link
    GlobalPos    source; ///< the block the link is attached to
    GlobalPos    target; ///< the targeted block
    Json&        config; ///< the source's options (persisted in the link's BlockData, editable)
    std::uint64_t gameTime = 0; ///< the level's current tick
};

/// A configuration option shown in the link's screen (Create's initConfigurationWidgets).
struct Option {
    std::string              key;          ///< key in `Context::config`
    std::string              label;        ///< translation key
    std::string              labelFallback;
    std::vector<std::string> choices;      ///< translation keys; empty = free text
    std::vector<std::string> choiceFallbacks;
    Json                     defaultValue = 0; ///< index for choices, string for text
};

struct Source {
    std::string id;           ///< Create's source id ("kinetic_speed", "count_items", ...)
    std::string name;         ///< translation key ("create.display_source.kinetic_speed")
    std::string nameFallback; ///< English
    /// The lines to send (one entry per line). Empty = an empty line (Create's EMPTY).
    std::function<Lines(Context&, TargetStats const&)> text;
    std::vector<Option> options;
    /// Create's getPassiveRefreshTicks (default 100).
    int  refreshTicks = 100;
    /// Create's shouldPassiveReset: whether the passive timer refreshes at all.
    bool passiveRefresh = true;
    /// Whether the source writes several lines (Create's single line vs. multi-line sources:
    /// the screen then says "Start writing at:").
    bool multiLine = false;
    /// Create's onSignalReset (the link stopped being powered), e.g. the stopwatch restarts.
    std::function<void(Context&)> onSignalReset;
    /// Called once when the source is selected in the link (initial config).
    std::function<void(Context&)> onSelected;
    /// Display boards: text split into columns (Create's provideFlapDisplayText / layouts:
    /// value lists show their value right-aligned in its own column). Optional: each returned
    /// line is a list of sections; nullopt = plain `text`.
    std::function<std::optional<std::vector<Lines>>(Context&, TargetStats const&)> boardText;
};

struct Target {
    std::string id; ///< "display_board", ...
    std::function<TargetStats(BlockSource&, GlobalPos const& target)> stats;
    /// Writes `lines` starting at `line`.
    std::function<void(BlockSource&, GlobalPos const& target, int line, Lines const& lines, Context& ctx)> accept;
    /// Optional column-aware version for display boards.
    std::function<void(BlockSource&, GlobalPos const& target, int line, std::vector<Lines> const& sections, Context& ctx)>
        acceptSections;
    /// The line labels for the link's screen ("Line 1", "Page 1", "Single Line").
    std::function<std::string(int line, Player const& viewer)> lineName;
};

/// Registers a source offered by blocks with id `blockId` (several sources per block).
void registerSource(std::string const& blockId, Source source);
/// Generic source for any block for which `applies` is true (vanilla block entities).
void registerSourceProvider(std::function<bool(BlockSource&, GlobalPos const&, Block const&)> applies, Source source);
/// Registers a target for blocks with id `blockId` (one per block).
void registerTarget(std::string const& blockId, Target target);
void registerTargetProvider(std::function<bool(BlockSource&, GlobalPos const&, Block const&)> applies, Target target);

/// The sources the block at `pos` offers (Create's DisplaySource.getAll), in registration order.
[[nodiscard]] std::vector<Source const*> sourcesAt(BlockSource& region, GlobalPos const& pos);
[[nodiscard]] Target const*              targetAt(BlockSource& region, GlobalPos const& pos);

/// Create's DisplayLinkBlock.notifyGatherers: links attached to `source` refresh now (a nixie
/// tube changed, a station's train arrived, ...).
void notifyGatherers(GlobalPos const& source);
/// Create's sendToGatherers for item throughput: a tunnel / funnel at `source` passed `count`
/// items (the item_throughput and accumulate_items sources count them).
void itemsPassed(GlobalPos const& source, int count);

/// Formats a value as Create's value lists do ("1.2k", "3m" when shortened).
[[nodiscard]] std::string formatValue(long long value, bool shortened);
/// A Create progress bar ("█" filled, "▒" empty) `width` characters wide for `level` 0..1.
[[nodiscard]] std::string progressBar(double level, int width);

} // namespace lv::feature::display
