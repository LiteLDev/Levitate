// Comparator output of the basin and the blaze burner (U14, through common/Redstone.h, U36).
//
// - Basin: Create's BasinBlock.getAnalogOutputSignal = ItemHelper.calcRedstoneFromBlockEntity on
//   the basin's item capability (input and output inventories, 18 slots).
// - Blaze burner: Create's BlazeBurnerBlock.getAnalogOutputSignal = max(0, heat ordinal - 1)
//   (the empty burner is a blaze burner block with heat NONE: 0); the lit burner (Create's
//   LitBlazeBurnerBlock) gives 1, 2 with a soul flame.

#include <algorithm>

#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/Redstone.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::basin {

namespace {
namespace rs = common::redstone;

std::optional<int> basinSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    auto* b = basinops::get(region, game::globalPos(region, pos));
    if (!b) return 0;
    std::vector<int> counts, maxStacks;
    counts.reserve(cb::kSlotCount * 2);
    maxStacks.reserve(cb::kSlotCount * 2);
    for (auto const* inv : {&b->in, &b->out}) {
        for (auto const& st : *inv) {
            bool empty = st.isNull() || st.mCount == 0;
            counts.push_back(empty ? 0 : st.mCount);
            maxStacks.push_back(empty ? 64 : std::min(64, maxStackOf(st)));
        }
    }
    return cb::redstoneFromInventory(counts, maxStacks);
}

std::optional<int> burnerSignal(BlockSource&, BlockPos const&, Block const& block) {
    return cb::burnerComparator(burners::heatOfBlock(block));
}

/// Create's LitBlazeBurnerBlock.getAnalogOutputSignal: 2 for a soul flame, else 1.
std::optional<int> litBurnerSignal(BlockSource&, BlockPos const&, Block const& block) {
    return game::intState(block, kFlameState).value_or(0) == 1 ? 2 : 1;
}
} // namespace

class BasinComparatorModule {
public:
    void install() {
        rs::registerAnalog(kBasin, basinSignal);
        rs::registerAnalog(kBurner, burnerSignal);
        rs::registerAnalog(kEmptyBurner, burnerSignal);
        rs::registerAnalog(kLitBurner, litBurnerSignal);
    }
    void uninstall() {
        rs::unregisterAnalog(kLitBurner);
        rs::unregisterAnalog(kEmptyBurner);
        rs::unregisterAnalog(kBurner);
        rs::unregisterAnalog(kBasin);
    }
};

} // namespace lv::feature::basin

LV_REGISTER_MODULE(lv::feature::basin::BasinComparatorModule);
