// Player interactions of the assembly unit, through the shared dispatch (common/Interactions.h):
// right-clicks on deployers, arm point selection, item application and the placement of our
// blocks (network players, SimulatedPlayers and the deployer player alike).

#include <algorithm>
#include <map>
#include <vector>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/common/Interactions.h"
#include "game/Blocks.h"

namespace lv::feature::assembly {

namespace ia = common::interactions;

namespace {

/// Bedrock repeats use-on packets while the button is held: one action per click and block.
bool repeated(Player& player, BlockPos const& pos) {
    static std::map<std::int64_t, std::pair<BlockPos, std::uint64_t>> last;
    auto  key  = player.getOrCreateUniqueID().rawID;
    auto& e    = last[key];
    bool  skip = e.first == pos && detail::currentTick() - e.second < 5;
    e          = {pos, detail::currentTick()};
    return skip;
}

} // namespace

namespace {

ia::Result onUseOn(ia::UseOnContext const& c) {
    if (c.face > 5) return ia::Result::Pass;
    Player&            player  = c.player;
    auto&              region  = player.getDimensionBlockSource();
    Block const&       clicked = c.block;
    std::string const& heldId  = c.itemId;
    BlockPos           at{c.pos.x, c.pos.y, c.pos.z};
    Face               f    = static_cast<Face>(c.face);
    bool               fake = fakeplayer::isFake(player);
    // the dispatch passes the hook's (mutable) held stack as const: these functions take from it
    auto& item = const_cast<ItemStack&>(c.item);

    // the arm item selects interaction points instead of placing
    if (heldId == kArmId && arm::isPoint(region, at)) {
        if (repeated(player, at)) return ia::Result::Consume; // the held button repeats: one toggle per click
        if (arm::onSelect(player, at)) return ia::Result::Consume;
    }

    // deployers: swap items on the front face, the wrench toggles the mode
    if (c.blockId == kDeployerId && (!c.sneaking || c.item.isNull())
        && (f == static_cast<Face>(detail::intState(clicked, "levitate:facing").value_or(1)))) {
        if (repeated(player, at)) return ia::Result::Consume;
        if (deployer::onUse(player, at, f, item)) return ia::Result::Consume;
    }

    // item application recipes (Create's ManualApplicationRecipe)
    if (!c.item.isNull() && !c.sneaking) {
        if (application::tryApply(player, region, at, item, fake)) return ia::Result::Consume;
    }
    return ia::Result::Pass;
}

/// Our blocks: orient them once placed.
void onPlaced(ia::PlaceContext const& c) {
    if (c.itemId != c.blockId) return;
    auto&    region = c.player.getDimensionBlockSource();
    BlockPos target{c.pos.x, c.pos.y, c.pos.z};
    Face     f = static_cast<Face>(std::clamp(c.face, 0, 5));
    if (c.blockId == kDeployerId) deployer::onPlaced(region, target, &c.player, f);
    else arm::onPlaced(region, target, &c.player, f);
}

/// Left-click with the arm on a selected point deselects it instead of mining.
ia::Result onArmHit(ia::DestroyContext const& c) {
    return arm::onDeselect(c.player, BlockPos{c.pos.x, c.pos.y, c.pos.z}) ? ia::Result::Deny : ia::Result::Pass;
}

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

} // namespace

void installHooks() {
    auto& ids = handlerIds();
    ids.push_back(ia::onUseOn({}, onUseOn));
    ids.push_back(ia::onPlace({.block = std::string(kDeployerId)}, onPlaced));
    ids.push_back(ia::onPlace({.block = std::string(kArmId)}, onPlaced));
    ids.push_back(ia::onStartDestroy({.item = std::string(kArmId)}, onArmHit));
    ids.push_back(ia::onDestroy({.item = std::string(kArmId)}, onArmHit));
}

void uninstallHooks() {
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
}

} // namespace lv::feature::assembly
