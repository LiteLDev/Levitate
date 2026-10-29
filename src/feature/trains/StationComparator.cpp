// Comparator output of train stations (U27, through common/Redstone.h, U36): Create's
// StationBlock.getAnalogOutputSignal = 15 while a train is present at the station, else 0.

#include "mc/world/level/BlockSource.h"

#include "feature/common/Redstone.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::trains {

namespace {
namespace rs = common::redstone;

std::optional<int> stationSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    auto* s = stationAt(game::globalPos(region, pos));
    return s && trainAt(*s) ? 15 : 0;
}
} // namespace

class StationComparatorModule {
public:
    void install() { rs::registerAnalog(std::string(kStation), stationSignal); }
    void uninstall() { rs::unregisterAnalog(std::string(kStation)); }
};

} // namespace lv::feature::trains

LV_REGISTER_MODULE(lv::feature::trains::StationComparatorModule);
