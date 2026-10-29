// Comparator output of the redstone requester (U26, through common/Redstone.h, U36): Create's
// RedstoneRequesterBlock.getAnalogOutputSignal = 15 when the last request succeeded, else 0
// (`success` is cleared on every power change and set when an order was sent).

#include "mc/world/level/BlockSource.h"

#include "feature/common/Redstone.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::stock {

namespace {
namespace rs = common::redstone;

std::optional<int> requesterSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    auto const* d = dataOf(game::globalPos(region, pos));
    return d && d->value("success", false) ? 15 : 0;
}
} // namespace

class RequesterComparatorModule {
public:
    void install() { rs::registerAnalog(kRedstoneRequester, requesterSignal); }
    void uninstall() { rs::unregisterAnalog(kRedstoneRequester); }
};

} // namespace lv::feature::stock

LV_REGISTER_MODULE(lv::feature::stock::RequesterComparatorModule);
