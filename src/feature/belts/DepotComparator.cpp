// Comparator output of depots and weighted ejectors (U11, through common/Redstone.h, U36):
// Create's SharedDepotBlockMethods.getComparatorInputOverride. f = present stack size (held item
// + processing output buffer) / maxStackSize (64 for a depot, the ejector's stack size setting,
// 0 = 64); signal = clamp(floor(f * 14) + (f > 0 ? 1 : 0), 0, 15).

#include <algorithm>
#include <cmath>

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
    if (d->held) {
        if (auto it = d->stacks.find(d->held->id); it != d->stacks.end()) present += common::items::countOf(it->second.stack);
    }
    for (auto const& o : d->outputs) present += common::items::countOf(o);
    int   max = d->ejector && d->stackSize > 0 ? d->stackSize : 64;
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
