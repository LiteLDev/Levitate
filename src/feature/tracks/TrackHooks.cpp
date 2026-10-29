// Tracks unit: using / breaking with the track item, through the shared interaction dispatch
// (feature/common/Interactions.h; GameMode is where every player action ends up, network players
// and SimulatedPlayers alike).

#include <vector>

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Interactions.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::tracks {

namespace ia = common::interactions;

namespace {

/// Where a plain right-click would put a block.
ct::BlockPos3 placementPos(BlockSource& region, BlockPos const& clicked, int face) {
    if (game::replaceable(region, clicked)) return toCore(clicked);
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    ct::BlockPos3 p = toCore(clicked);
    if (face >= 0 && face < 6) p = p.offset(dx[face], dy[face], dz[face]);
    return p;
}

std::vector<ia::HandlerId>& handlers() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

} // namespace

void installHooks() {
    auto& ids = handlers();
    ids.push_back(ia::onUseOn({.item = std::string(kTrackId)}, [](ia::UseOnContext const& c) {
        Player&  player = c.player;
        BlockPos at     = game::blockPos(c.pos);
        if (onTrackItemUse(player, at, c.face, &c.block)) return ia::Result::Consume;
        // a plain placement: remember how the player looked for the shape (Create's getStateForPlacement)
        PlaceIntent intent;
        Vec3        look = player.getViewVector();
        intent.look      = {look.x, look.y, look.z};
        intent.yaw       = player.getRotation().y;
        intent.uuid      = player.getUuid().asString();
        state().placeIntents[{c.pos.dim, placementPos(player.getDimensionBlockSource(), at, c.face)}] = intent;
        return ia::Result::Pass;
    }));
    ids.push_back(ia::onUse(std::string(kTrackId), [](ia::UseContext const& c) {
        if (c.sneaking && state().selections.contains(c.player.getUuid().asString())) {
            clearSelection(c.player); // Create: sneak-use in the air clears the selection
            return ia::Result::Consume;
        }
        return ia::Result::Pass;
    }));
    // every block a player breaks (the track block handler needs to know who broke it)
    ids.push_back(ia::onDestroy({}, [](ia::DestroyContext const& c) {
        auto key = std::pair{c.pos.dim, toCore(game::blockPos(c.pos))};
        state().playerBroke.insert(key);
        if (c.player.isCreative()) state().noDrops.insert(key);
        return ia::Result::Pass;
    }, ia::priority::Highest + 1)); // observes only: before anything that could keep the block
}

void uninstallHooks() {
    for (auto id : handlers()) ia::remove(id);
    handlers().clear();
}

} // namespace lv::feature::tracks
