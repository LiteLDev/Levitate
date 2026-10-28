// Tracks unit: game hooks for using / breaking with the track item.
//
// GameMode is where every player action ends up (network players and SimulatedPlayers alike),
// so the track item is handled here rather than through the LeviLamina player events.

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::tracks {

namespace {

bool isServerPlayer(Player const& p) { return !p.isClientSide(); }

/// Where a plain right-click would put a block.
ct::BlockPos3 placementPos(BlockSource& region, BlockPos const& clicked, uchar face) {
    if (game::replaceable(region, clicked)) return toCore(clicked);
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    ct::BlockPos3 p = toCore(clicked);
    if (face < 6) p = p.offset(dx[face], dy[face], dz[face]);
    return p;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TracksUseItemOnHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItemOn,
    InteractionResult,
    ItemStack&      item,
    BlockPos const& at,
    uchar           face,
    Vec3 const&     hit,
    HandSlot        handSlot,
    Block const*    targetBlock,
    bool            isFirstEvent
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player) || item.isNull() || item.getTypeName() != kTrackId) {
        return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    }
    auto&        region  = player.getDimensionBlockSource();
    Block const* clicked = targetBlock ? targetBlock : &region.getBlock(at);
    if (onTrackItemUse(player, at, face, clicked)) {
        InteractionResult r{};
        r.mSuccess = true;
        r.mSwing   = true;
        player.sendInventory(false); // undo the client's predicted use
        return r;
    }
    // a plain placement: remember how the player looked for the shape (Create's getStateForPlacement)
    PlaceIntent intent;
    Vec3        look = player.getViewVector();
    intent.look      = {look.x, look.y, look.z};
    intent.yaw       = player.getRotation().y;
    intent.uuid      = player.getUuid().asString();
    state().placeIntents[{int(player.getDimensionId()), placementPos(region, at, face)}] = intent;
    return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
}

LL_TYPE_INSTANCE_HOOK(
    TracksUseItemHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItem,
    bool,
    ItemStack& item,
    HandSlot   handSlot
) {
    Player& player = mPlayer;
    if (isServerPlayer(player) && !item.isNull() && item.getTypeName() == kTrackId && player.isSneaking()
        && state().selections.contains(player.getUuid().asString())) {
        clearSelection(player); // Create: sneak-use in the air clears the selection
        return true;
    }
    return origin(item, handSlot);
}

LL_TYPE_INSTANCE_HOOK(
    TracksDestroyBlockHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$destroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face
) {
    Player& player = mPlayer;
    if (isServerPlayer(player)) {
        auto key = std::pair{int(player.getDimensionId()), toCore(pos)};
        state().playerBroke.insert(key);
        if (player.isCreative()) state().noDrops.insert(key);
    }
    return origin(pos, face);
}

void installHooks() {
    TracksUseItemOnHook::hook();
    TracksUseItemHook::hook();
    TracksDestroyBlockHook::hook();
}

void uninstallHooks() {
    TracksDestroyBlockHook::unhook();
    TracksUseItemHook::unhook();
    TracksUseItemOnHook::unhook();
}

} // namespace lv::feature::tracks

