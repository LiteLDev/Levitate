// Per-block counters behind some advancements (U49): Create keeps them in the block entity
// (MechanicalPressBlockEntity#tracksCreated, SpoutBlockEntity#created*), Levitate in the block's
// BlockData under the key "advancements" (carried by contraptions with the rest of the document).

#include <algorithm>

#include "feature/advancements/Advancements.h"
#include "feature/common/BlockData.h"

namespace lv::feature::advancements {

namespace bd = common::blockdata;

namespace {
constexpr char kKey[] = "advancements";

int counter(GlobalPos const& pos, char const* name) {
    auto const* d = bd::get(pos);
    if (!d) return 0;
    auto it = d->find(kKey);
    return it != d->end() && it->is_object() ? it->value(name, 0) : 0;
}

void setCounter(GlobalPos const& pos, char const* name, int value) {
    auto& d = bd::edit(pos)[kKey];
    if (!d.is_object()) d = bd::Json::object();
    d[name] = value;
}
} // namespace

void onItemPressed(GlobalPos const& pos, std::string_view resultId, int count) {
    awardOwner(pos, "mechanical_press");
    if (resultId != "levitate:track" || count <= 0) return;
    int made = counter(pos, "tracks") + count;
    if (made >= 1000) {
        awardOwner(pos, "track_crafting_factory");
        made = 0;
    }
    setCounter(pos, "tracks", made);
}

void onMovingDeployerUsed(std::string_view contraptionKind, nlohmann::json const& deployerDoc, std::string_view usedItemId, bool placed) {
    if (!placed || (contraptionKind != "cart_assembler" && contraptionKind != "train")) return;
    // IBaseRailBlockExtension (vanilla rails) or ITrackBlock
    static constexpr std::string_view kRails[] = {"minecraft:rail", "minecraft:golden_rail", "minecraft:detector_rail",
                                                  "minecraft:activator_rail", "levitate:track"};
    if (std::ranges::find(kRails, usedItemId) == std::end(kRails)) return;
    awardCarriedOwner(deployerDoc, "self_deploying");
}

void onSpoutFilled(GlobalPos const& pos, std::string_view resultId) {
    awardOwner(pos, "spout");
    // Create's trackFoods: only while the spout knows its owner
    if (ownerOf(pos).empty()) return;
    int foods = counter(pos, "foods");
    if (resultId == "levitate:chocolate_glazed_berries") foods |= 1;
    else if (resultId == "levitate:honeyed_apple") foods |= 2;
    else if (resultId == "levitate:sweet_roll") foods |= 4;
    if (foods != counter(pos, "foods")) setCounter(pos, "foods", foods);
    if (foods == 7) awardOwner(pos, "foods");
}

} // namespace lv::feature::advancements
