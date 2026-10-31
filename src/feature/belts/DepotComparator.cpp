// Comparator output of depots and weighted ejectors (U11, through common/Redstone.h, U36):
// Create's SharedDepotBlockMethods.getComparatorInputOverride. f = present stack size (held item
// + processing output buffer) / maxStackSize (a depot: the held stack's max stack size, 64 when
// empty; the ejector's stack size setting,
// 0 = 64); signal = clamp(floor(f * 14) + (f > 0 ? 1 : 0), 0, 15).

#include <algorithm>
#include <cmath>

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "feature/belts/BeltApi.h"
#include "feature/belts/BeltsInternal.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::belts {

namespace {
namespace rs = common::redstone;

std::optional<int> depotSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    auto* d = depotFor(game::globalPos(region, pos));
    if (!d) return 0;
    int present = 0;
    int heldMax = 64; // DepotBehaviour.maxStackSize: the held stack's max stack size, 64 when empty
    if (d->held) {
        if (auto it = d->stacks.find(d->held->id); it != d->stacks.end() && !common::items::isEmpty(it->second.stack)) {
            present += common::items::countOf(it->second.stack);
            heldMax = std::max(1, static_cast<int>(it->second.stack.getMaxStackSize()));
        }
    }
    for (auto const& o : d->outputs) present += common::items::countOf(o);
    // the ejector's getter is its stack size setting (0 = 64)
    int max = d->ejector ? (d->stackSize > 0 ? d->stackSize : 64) : heldMax;
    // Create: clamp(floor(f * 14) + (f > 0 ? 1 : 0), 0, 15) = ComparatorUtil.fractionToRedstoneLevel
    return rs::fractionToRedstoneLevel(static_cast<double>(present) / static_cast<double>(max));
}
} // namespace

class DepotComparatorModule {
public:
    void install() {
        rs::registerAnalog(kDepotId, depotSignal);
        rs::registerAnalog(kEjectorId, depotSignal);
    }
    void uninstall() {
        rs::unregisterAnalog(kEjectorId);
        rs::unregisterAnalog(kDepotId);
    }
};

} // namespace lv::feature::belts

LV_REGISTER_MODULE(lv::feature::belts::DepotComparatorModule);
