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
//
// Comparator output (U36): a unit registers Create's getAnalogOutputSignal for its block ids and
// vanilla comparators read it (one ComparatorCapacitor hook here gives the comparator's circuit
// component its rear analog input):
//
//     rs::registerAnalog("levitate:fluid_tank", [](BlockSource& r, BlockPos const& p, Block const& b)
//         -> std::optional<int> { return level0to15; });   // nullopt: no comparator output now
//
// As in Java (ComparatorBlock.getInputSignal) a comparator reads the block right behind it, or,
// when that is a solid conductor, the block behind that one. The function runs on the server
// thread whenever the comparator's circuit component is evaluated: keep it cheap.
// Powering wire from custom blocks (redstone links, ...) stays with the units.

#include <functional>
#include <optional>
#include <string>

#include "core/GlobalPos.h"

class Block;
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
/// (U49) The block at `pos` is a conducting block the circuit powers (Bedrock's powered-block
/// component), not a source: its strength may come from any of its neighbours, including the block
/// asking (Bedrock's producers power the solid blocks around them, Java's weak signals don't).
[[nodiscard]] bool isPoweredBlock(BlockSource& region, BlockPos const& pos);

using WatchId = int;
using WatchFn = std::function<void(GlobalPos const& pos, int power)>;
/// Calls `fn` whenever the power reaching `pos` changes (first call: on the next poll, with the
/// current power, even if 0). Returns an id for `unwatch`. Watches are dropped when the world closes.
WatchId watch(GlobalPos const& pos, WatchFn fn);
void    unwatch(WatchId id);

// ---- custom signal sources (U36) ---------------------------------------------------------------

/// The signal a custom block at `at` sends into its neighbour on side `towards` (FacingID from
/// `at`), Create's getSignal. Custom blocks are not part of the game's circuit graph: units whose
/// blocks emit redstone (links, latches, pulse repeaters, ... of U23) register them here so that
/// `power` / `signalFrom` see them like vanilla sources.
using EmitterFn = std::function<int(BlockSource& region, BlockPos const& at, Block const& block, int towards)>;
/// Registers a signal source block id (replaces an earlier one for the same id).
void registerEmitter(std::string const& blockId, EmitterFn fn);
void unregisterEmitter(std::string const& blockId);

// ---- comparator output (U36) -------------------------------------------------------------------

/// Create's getAnalogOutputSignal for a block id: 0..15, or nullopt for "no output right now".
using AnalogFn = std::function<std::optional<int>(BlockSource& region, BlockPos const& pos, Block const& block)>;
/// Registers the comparator output of `blockId` (replaces an earlier one for the same id).
void registerAnalog(std::string const& blockId, AnalogFn fn);
void unregisterAnalog(std::string const& blockId);
/// The registered comparator output of the block at `pos` (nullopt: not registered / not loaded).
[[nodiscard]] std::optional<int> analogOutput(BlockSource& region, BlockPos const& pos);
/// What a vanilla comparator at `comparatorPos` reads from a registered block (behind it, or
/// behind the solid block behind it); nullopt when it doesn't read one.
[[nodiscard]] std::optional<int> comparatorInput(BlockSource& region, BlockPos const& comparatorPos);
/// The input side (FacingID) of a vanilla comparator block, nullopt if `block` is none.
[[nodiscard]] std::optional<int> comparatorRear(Block const& block);
/// Create's ComparatorUtil.fractionToRedstoneLevel: 0 when empty, else 1 + floor(fraction * 14).
[[nodiscard]] int fractionToRedstoneLevel(double fraction);

} // namespace lv::feature::common::redstone
