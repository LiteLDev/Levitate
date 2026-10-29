// U3's BlockData travels with contraption blocks: every unit's per-block document (kinetic
// records, filters, inventories, settings, chassis ranges, ...) is taken from the world when a
// block is assembled and put back where the block lands, so units need no carrier of their own
// for data they keep in BlockData.
//
// Owners that cache their part elsewhere write it back first (blockdata::onWriteBack, run by
// take()); owners with caches adopt it at the new position (blockdata::onPut, run by put()).
// A unit that still registers its own carrier for BlockData keys gets what this carrier left:
// whichever capture runs first takes the document, and restore merges into what is there.

#include "feature/common/BlockData.h"
#include "feature/contraption/Contraption.h"

namespace lv::feature::contraption {

namespace {

namespace bd = common::blockdata;

struct BlockDataCarrier {
    BlockDataCarrier() {
        registerDataCarrier({
            "blockdata",
            [](BlockSource&, core::GlobalPos const& pos) -> std::optional<nlohmann::json> { return bd::take(pos); },
            [](BlockSource&, core::GlobalPos const& pos, nlohmann::json const& carried) {
                if (!carried.is_object()) return;
                // other carriers may have restored keys already: theirs stay, ours fill the rest
                nlohmann::json doc = carried;
                if (auto const* there = bd::get(pos)) {
                    for (auto const& [k, v] : there->items()) doc[k] = v;
                }
                bd::put(pos, std::move(doc));
            },
            // units register how their direction-dependent keys turn (blockdata::onRotate)
            [](nlohmann::json& carried, cc::Axis axis, int quarterTurns) { bd::rotate(carried, static_cast<int>(axis), quarterTurns); },
        });
    }
};
BlockDataCarrier const gBlockDataCarrier;

} // namespace

} // namespace lv::feature::contraption
