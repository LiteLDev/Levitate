#pragma once

// Redstone helpers of the redstone unit (U23, SPEC §3.6.23) that other units may use:
// reading the vanilla redstone signal around a block (custom blocks get no redstone neighbour
// callbacks), Create's funnel → smart observer notification and the threshold switch's
// "observable" blocks (Create's ThresholdSwitchObservable). Server thread only.
//
// Link networks are in RedstoneLink.h.

#include <functional>
#include <string>

class BlockPos;
class BlockSource;
class ItemStack;

namespace lv::feature::redstone {

/// Faces are Bedrock FacingIDs: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.
///
/// The strength of the signal the neighbour of `pos` on `face` sends into `pos` (Java's
/// `level.getSignal(pos.relative(face), face)`): wires, levers, buttons, torches, repeaters and
/// comparators pointing at `pos`, powered blocks, daylight sensors, Create's redstone blocks, ...
/// Read from the game's own redstone circuit (the values vanilla redstone uses).
[[nodiscard]] int signalFrom(BlockSource& region, BlockPos const& pos, int face);
/// The strongest signal from any side (Java's getBestNeighborSignal).
[[nodiscard]] int bestNeighborSignal(BlockSource& region, BlockPos const& pos);

namespace observer {
/// A funnel at `funnelPos` moved `stack` (Create's SmartObserverBlock.onFunnelTransfer): smart
/// observers looking at the funnel whose filter accepts the stack emit a 4-tick pulse.
void onFunnelTransfer(BlockSource& region, BlockPos const& funnelPos, ItemStack const& stack);
} // namespace observer

namespace threshold {
/// A block the threshold switch can read besides inventories and tanks (Create's
/// ThresholdSwitchObservable, e.g. a rope pulley's height).
struct Observable {
    std::function<int(BlockSource&, BlockPos const&)> min;
    std::function<int(BlockSource&, BlockPos const&)> max;
    std::function<int(BlockSource&, BlockPos const&)> current;
    /// The text of a value on the threshold screen (default: the number).
    std::function<std::string(int value)> format;
};
void registerObservable(std::string const& blockId, Observable observable);
} // namespace threshold

} // namespace lv::feature::redstone
