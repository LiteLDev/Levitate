#pragma once

// Air currents and fan processing (extension of the U3 contracts added by U13 `bulk`).
//
// Other units integrate with encased fans through this header without including U13's code:
//   - chutes (U12): a fan below a chute facing up pushes items up, a fan above facing down pulls
//     them (Create's ChuteBlockEntity.updatePush / updatePull): ask `airflow::sourceAt` for the
//     block next to the chute;
//   - belts / depots (U11) may run Create's fan processing on their own items (Create's
//     TransportedItemStackHandlerBehaviour): `airflow::typeAt` names the processing air at a block,
//     `canProcess` / `process` / `processingTime` apply Create's FanProcessing rules. (U13 already
//     processes the slots of `levitate:belt`, `levitate:depot` and `levitate:weighted_ejector`
//     through the item capability when those blocks are in a fan's current.)
//
// Server thread only. Every function returns "nothing" until U13 is installed.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/GlobalPos.h"

class BlockPos;
class BlockSource;
class ItemStack;

namespace lv::feature::common::airflow {

/// An air current source (Create's IAirCurrentSource: the encased fan).
struct Source {
    core::GlobalPos pos;
    int             facing      = 0;    ///< origin side, Bedrock face index (0 down .. 5 east)
    float           speed       = 0;    ///< RPM (0: no current)
    bool            pushing     = true; ///< air flows out of the facing side (else into it)
    float           maxDistance = 0;    ///< reach of the current in blocks (0 when stopped / blocked)
};

/// The source at `pos` (nullopt when the block there is not an air current source).
[[nodiscard]] std::optional<Source> sourceAt(core::GlobalPos const& pos);
/// Create's FanProcessingType at a block: "splashing", "smoking", "blasting", "haunting" or "".
[[nodiscard]] std::string typeAt(BlockSource& region, BlockPos const& pos);
/// Whether air of `type` processes `stack` (Create's FanProcessingType.canProcess).
[[nodiscard]] bool canProcess(ItemStack const& stack, std::string const& type);
/// The outputs (nullopt: nothing happens; empty: the stack burns up).
[[nodiscard]] std::optional<std::vector<ItemStack>> process(ItemStack const& stack, std::string const& type);
/// Ticks a stack of `count` items needs (fanProcessingTime per started 16 items, + 1).
[[nodiscard]] int processingTime(int count);

/// Implementation hooks (set by U13 when it installs).
struct Provider {
    std::function<std::optional<Source>(core::GlobalPos const&)>                       sourceAt;
    std::function<std::string(BlockSource&, BlockPos const&)>                          typeAt;
    std::function<bool(ItemStack const&, std::string const&)>                          canProcess;
    std::function<std::optional<std::vector<ItemStack>>(ItemStack const&, std::string const&)> process;
    std::function<int(int)>                                                            processingTime;
};
void setProvider(Provider provider);

} // namespace lv::feature::common::airflow
