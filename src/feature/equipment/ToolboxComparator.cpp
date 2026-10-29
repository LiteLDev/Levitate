// Comparator output of toolboxes (U31, through common/Redstone.h, U36): Create's
// ToolboxBlock.getAnalogOutputSignal = ItemHelper.calcRedstoneFromBlockEntity over the toolbox
// inventory's 32 slots: floor(fill * 14) + (any item ? 1 : 0). The lectern controller gives 15.

#include <cmath>

#include "mc/world/level/BlockSource.h"

#include "core/equipment/Toolbox.h"
#include "feature/common/Redstone.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::feature::equipment {

namespace {
namespace rs = common::redstone;

std::optional<int> toolboxSignal(BlockSource& region, BlockPos const& pos, Block const&) {
    auto inv = core::equipment::ToolboxInv::fromJson(toolboxInventory(game::globalPos(region, pos)));
    bool any = false;
    for (auto const& s : inv.slots) any = any || !s.empty();
    return static_cast<int>(std::floor(inv.fillLevel() * 14.f)) + (any ? 1 : 0);
}
} // namespace

class ToolboxComparatorModule {
public:
    void install() {
        for (auto c : kColours) rs::registerAnalog(toolboxId(c), toolboxSignal);
        // Create's LecternControllerBlock.getAnalogOutputSignal: always 15
        rs::registerAnalog(std::string(kLecternController), [](BlockSource&, BlockPos const&, Block const&) -> std::optional<int> {
            return 15;
        });
    }
    void uninstall() {
        rs::unregisterAnalog(std::string(kLecternController));
        for (auto c : kColours) rs::unregisterAnalog(toolboxId(c));
    }
};

} // namespace lv::feature::equipment

LV_REGISTER_MODULE(lv::feature::equipment::ToolboxComparatorModule);
