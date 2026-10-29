// Comparator output of packagers and package ports (U25, through common/Redstone.h, U36):
// - packager / repackager: Create's PackagerBlock.getAnalogOutputSignal = 15 while it holds a
//   package (or is animating one), else 0;
// - frogport / postboxes: Create's PackagePortBlock.getAnalogOutputSignal =
//   ItemHelper.calcRedstoneFromInventory of the port's inventory.

#include <algorithm>
#include <cmath>

#include "mc/world/level/BlockSource.h"

#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/packages/Packages.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::packages {

namespace {
namespace rs = common::redstone;

std::optional<int> packagerSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    auto const& s = packager(game::globalPos(region, pos));
    return common::items::isEmpty(s.held) && s.animationTicks == 0 ? 0 : 15;
}

/// Create's ItemHelper.calcRedstoneFromInventory (slot limit 64).
std::optional<int> portSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    auto const& s = port(game::globalPos(region, pos));
    if (s.inventory.empty()) return 0;
    float f    = 0;
    int   used = 0;
    for (auto const& st : s.inventory) {
        if (common::items::isEmpty(st)) continue;
        int max = std::clamp(static_cast<int>(st.getMaxStackSize()), 1, 64);
        f += static_cast<float>(common::items::countOf(st)) / static_cast<float>(max);
        ++used;
    }
    f /= static_cast<float>(s.inventory.size());
    return static_cast<int>(std::floor(f * 14.f)) + (used > 0 ? 1 : 0);
}
} // namespace

class PackagesComparatorModule {
public:
    void install() {
        rs::registerAnalog(kPackagerId, packagerSignal);
        rs::registerAnalog(kRepackagerId, packagerSignal);
        rs::registerAnalog(kFrogportId, portSignal);
        for (auto const& id : postboxIds()) rs::registerAnalog(id, portSignal);
    }
    void uninstall() {
        for (auto const& id : postboxIds()) rs::unregisterAnalog(id);
        rs::unregisterAnalog(kFrogportId);
        rs::unregisterAnalog(kRepackagerId);
        rs::unregisterAnalog(kPackagerId);
    }
};

} // namespace lv::feature::packages

LV_REGISTER_MODULE(lv::feature::packages::PackagesComparatorModule);
