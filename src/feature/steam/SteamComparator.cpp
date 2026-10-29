// Comparator output of fluid tanks (U18, through common/Redstone.h, U36): Create's
// FluidTankBlock.getAnalogOutputSignal = ComparatorUtil.fractionToRedstoneLevel of the controller's
// fill state (the whole multiblock; a boiler is a tank structure). The creative fluid tank is a
// FluidTankBlock too: 15 while it holds a fluid (always full), 0 when empty.

#include "mc/world/level/BlockSource.h"

#include "feature/common/Redstone.h"
#include "feature/steam/Steam.h"
#include "feature/steam/SteamInternal.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::steam {

namespace {
namespace rs = common::redstone;

std::optional<int> tankSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    return comparatorLevel(game::globalPos(region, pos));
}
} // namespace

class SteamComparatorModule {
public:
    void install() {
        rs::registerAnalog(kTank, tankSignal);
        rs::registerAnalog(kCreativeTank, tankSignal);
    }
    void uninstall() {
        rs::unregisterAnalog(kCreativeTank);
        rs::unregisterAnalog(kTank);
    }
};

} // namespace lv::feature::steam

LV_REGISTER_MODULE(lv::feature::steam::SteamComparatorModule);
