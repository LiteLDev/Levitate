// Creative crate (Create's CreativeCrateBlockEntity / BottomlessItemHandler, SPEC §3.6.12):
// an endless supply of the item in its filter slot; whatever is inserted is voided.

#include <algorithm>

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"

namespace lv::feature::funnels::detail {

namespace items = common::items;

namespace {

ItemStack supplied(common::Access const& at) {
    return common::filterslot::get(game::globalPos(at.region, at.pos), "filter");
}

common::items::Handler capability() {
    common::items::Handler h;
    h.slots   = [](common::Access const&) { return 2; };
    h.getSlot = [](common::Access const& at, int slot) -> ItemStack {
        auto s = supplied(at);
        if (slot != 0 || items::isEmpty(s)) return items::empty();
        return items::withCount(s, std::max(1, static_cast<int>(s.getMaxStackSize())));
    };
    h.insert  = [](common::Access const&, ItemStack const&, bool) { return items::empty(); };
    h.extract = [](common::Access const& at, common::items::ItemPredicate const& filter, int amount, bool) -> ItemStack {
        auto s = supplied(at);
        if (items::isEmpty(s) || amount <= 0 || !filter(s)) return items::empty();
        return items::withCount(s, std::min(amount, std::max(1, static_cast<int>(s.getMaxStackSize()))));
    };
    return h;
}

} // namespace

void installCrate() {
    common::items::registerBlock(kCreativeCrate, capability());
    common::filterslot::registerSlot(
        kCreativeCrate,
        {"filter",
         [](Block const&, int face) -> std::optional<std::array<double, 3>> {
             if (face != cf::kUp) return std::nullopt;
             return std::array<double, 3>{.5, 1., .5};
         },
         0.5,
         nullptr}
    );
}

} // namespace lv::feature::funnels::detail
