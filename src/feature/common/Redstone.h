#pragma once

// Redstone input for custom blocks (U32, SPEC §3.2).
//
// Custom (data-driven) blocks get no neighbour updates and are not part of the game's redstone
// circuit, so every unit with a redstone-controlled block used to poll its neighbours itself.
// This header reads the power reaching a position from the game's circuit graph (conducting
// blocks, wire, levers, buttons, plates, redstone blocks, torches, repeaters, comparators,
// observers, ...) and polls watched positions centrally:
//
//     namespace rs = lv::feature::common::redstone;
//     int  p  = rs::power(region, pos);      // 0..15, the strongest signal into the block
//     auto id = rs::watch(gpos, [](core::GlobalPos const& at, int power) { ... });   // on changes
//     rs::unwatch(id);
//
// Server thread only. Watches are polled every `kPollTicks` ticks (and once right after
// `watch`), only while their chunk is loaded; the callback runs when the power level changes.
// Redstone *output* of custom blocks (comparator signals, powering wire) is not here: U12's
// ComparatorCapacitor hook (FunnelsExt.h) covers comparators.

#include <functional>

#include "core/GlobalPos.h"

class BlockPos;
class BlockSource;

namespace lv::feature::common::redstone {

using core::GlobalPos;

inline constexpr int kPollTicks = 2;

/// The strongest redstone signal (0..15) that reaches the block at `pos` from its six neighbours,
/// as a vanilla mechanism (piston, lamp, dispenser) would see it.
[[nodiscard]] int power(BlockSource& region, BlockPos const& pos);
/// `power(...) > 0`.
[[nodiscard]] bool powered(BlockSource& region, BlockPos const& pos);
/// The signal a single neighbour at `from` sends into `to` (adjacent positions), 0..15.
[[nodiscard]] int signalFrom(BlockSource& region, BlockPos const& from, BlockPos const& to);

using WatchId = int;
using WatchFn = std::function<void(GlobalPos const& pos, int power)>;
/// Calls `fn` whenever the power reaching `pos` changes (first call: on the next poll, with the
/// current power, even if 0). Returns an id for `unwatch`. Watches are dropped when the world closes.
WatchId watch(GlobalPos const& pos, WatchFn fn);
void    unwatch(WatchId id);

} // namespace lv::feature::common::redstone
