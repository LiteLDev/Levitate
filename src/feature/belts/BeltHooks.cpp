// Belts unit: game hooks. GameMode is where every player action ends up (network players and
// SimulatedPlayers alike), so right-clicks on belts / depots / ejectors and with the belt
// connector / ejector item are handled here; the ItemActor hooks keep the display items (items
// riding belts, held on depots, in flight) from being picked up, merging or despawning.

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/belts/BeltsInternal.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::belts {

namespace {

cb::Vec3d localHit(BlockPos const& pos, Vec3 const& click) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(click.x) && in(click.y) && in(click.z)) return {click.x, click.y, click.z};
    return {click.x - pos.x, click.y - pos.y, click.z - pos.z};
}

GlobalPos placementPos(BlockSource& region, BlockPos const& clicked, int face) {
    auto g = game::globalPos(region, clicked);
    if (game::replaceable(region, clicked)) return g;
    return offset(g, cb::faceNormal(face));
}

/// Bedrock sends a use packet per click and repeats while the button is held: one action per
/// player and block every few ticks.
bool debounced(Player& player, GlobalPos const& pos) {
    auto& s   = state();
    auto  key = uuidOf(player) + "@" + pos.key();
    auto  it  = s.lastUse.find(key);
    bool  hit = it != s.lastUse.end() && s.tick - it->second < 5;
    s.lastUse[key] = s.tick;
    if (s.lastUse.size() > 256) {
        for (auto i = s.lastUse.begin(); i != s.lastUse.end();) i = s.tick - i->second > 20 ? s.lastUse.erase(i) : std::next(i);
    }
    return hit;
}

InteractionResult handledResult(Player& player) {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    player.sendInventory(false); // undo the client's predicted use
    return r;
}

} // namespace

bool isDisplayActor(Actor const& actor) {
    return state().displays.contains(actor.getOrCreateUniqueID().rawID) || actor.hasTag(kDisplayTag);
}

LL_TYPE_INSTANCE_HOOK(
    BeltsUseItemOnHook,
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
    if (player.isClientSide()) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto&        region  = player.getDimensionBlockSource();
    Block const* clicked = targetBlock ? targetBlock : &region.getBlock(at);
    auto const&  id      = clicked->getTypeName();
    auto         heldId  = item.isNull() ? std::string() : item.getTypeName();
    bool         ours    = id == kBeltId || isDepotId(id) || heldId == kBeltConnectorId || heldId == kEjectorId;
    if (!ours) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);

    auto pos = game::globalPos(region, at);
    auto h   = localHit(at, hit);
    try {
        // value boxes (the ejector's stack size) are handled by the value box module
        if (id == kEjectorId && !player.isSneaking() && (item.isNull() || heldId == kWrenchId)
            && common::valuebox::boxAt(*clicked, face, {h.x, h.y, h.z})) {
            return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
        }
        bool handled = false;
        bool mine    = heldId == kBeltConnectorId || id == kBeltId || isDepotId(id) || (heldId == kEjectorId && player.isSneaking());
        // a repeat of a click we handle: swallow it (a second depot click would take the item back)
        if (mine && !player.isSneaking() && debounced(player, pos)) return handledResult(player);
        if (mine && player.isSneaking() && (heldId == kBeltConnectorId || heldId == kEjectorId) && debounced(player, pos))
            return handledResult(player);
        if (heldId == kBeltConnectorId) handled = onConnectorUse(player, pos, face, h);
        else if (id == kBeltId) handled = onBeltUse(player, pos, face, h);
        else if (isDepotId(id)) handled = onDepotUse(player, pos, face, h);
        if (!handled && heldId == kEjectorId) {
            if (player.isSneaking()) handled = onEjectorItemUse(player, pos);
            else state().placeIntents[placementPos(region, at, face)] = {uuidOf(player), game::playerFacing(player)};
        }
        if (handled) return handledResult(player);
    } catch (std::exception const& e) {
        log().error("belts: interaction failed: {}", e.what());
    }
    return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
}

LL_TYPE_INSTANCE_HOOK(
    BeltsUseItemHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItem,
    bool,
    ItemStack& item,
    HandSlot   handSlot
) {
    Player& player = mPlayer;
    if (!player.isClientSide() && !item.isNull() && item.getTypeName() == kBeltConnectorId && player.isSneaking()) {
        clearConnectorSelection(player); // Create: sneak-use resets the first pulley
        return true;
    }
    return origin(item, handSlot);
}

LL_TYPE_INSTANCE_HOOK(
    BeltsDestroyBlockHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$destroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face
) {
    Player& player = mPlayer;
    if (!player.isClientSide()) {
        auto& region = player.getDimensionBlockSource();
        if (game::blockIdAt(region, pos) == kBeltId) state().playerBroke[game::globalPos(region, pos)] = player.isCreative();
    }
    return origin(pos, face);
}

// display items are never picked up
LL_TYPE_INSTANCE_HOOK(BeltsDisplayTouchHook, ll::memory::HookPriority::Normal, ItemActor, &ItemActor::$playerTouch, void, Player& player) {
    if (!isClientSide() && isDisplayActor(*this)) return;
    origin(player);
}

// ... and don't age, merge or move by themselves; orphans from a saved world vanish
LL_TYPE_INSTANCE_HOOK(BeltsDisplayTickHook, ll::memory::HookPriority::Normal, ItemActor, &ItemActor::postNormalTick, void) {
    if (!isClientSide() && isDisplayActor(*this)) {
        if (!state().displays.contains(getOrCreateUniqueID().rawID)) remove();
        return;
    }
    origin();
}

void installHooks() {
    BeltsUseItemOnHook::hook();
    BeltsUseItemHook::hook();
    BeltsDestroyBlockHook::hook();
    BeltsDisplayTouchHook::hook();
    BeltsDisplayTickHook::hook();
}

void uninstallHooks() {
    BeltsDisplayTickHook::unhook();
    BeltsDisplayTouchHook::unhook();
    BeltsDestroyBlockHook::unhook();
    BeltsUseItemHook::unhook();
    BeltsUseItemOnHook::unhook();
}

} // namespace lv::feature::belts
