// Belts unit: game hooks. Right-clicks on belts / depots / ejectors and with the belt connector /
// ejector item go through the shared interaction dispatch (common/Interactions.h); the ItemActor
// hooks keep the display items (items riding belts, held on depots, in flight) from being picked
// up, merging or despawning.

#include <vector>

#include "ll/api/memory/Hook.h"

#include "mc/world/Container.h"
#include "mc/world/actor/Hopper.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Explosion.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/belts/BeltsInternal.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::belts {

namespace ia = common::interactions;

namespace {

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

ia::Result handledResult(Player& player) {
    player.sendInventory(false); // undo the client's predicted use
    return ia::Result::Consume;
}

ia::Result onUseOn(ia::UseOnContext const& c) {
    Player&            player = c.player;
    auto&              region = player.getDimensionBlockSource();
    std::string const& id     = c.blockId;
    std::string const& heldId = c.itemId;
    bool               ours   = id == kBeltId || isDepotId(id) || heldId == kBeltConnectorId || heldId == kEjectorId;
    if (!ours) return ia::Result::Pass;

    BlockPos  at{c.pos.x, c.pos.y, c.pos.z};
    auto      pos  = c.pos;
    int       face = c.face;
    cb::Vec3d h{c.hit[0], c.hit[1], c.hit[2]};
    try {
        // value boxes (the ejector's stack size) are handled by the value box module
        if (id == kEjectorId && !c.sneaking && (heldId.empty() || heldId == kWrenchId)
            && common::valuebox::boxAt(c.block, face, {h.x, h.y, h.z})) {
            return ia::Result::Pass;
        }
        bool handled = false;
        bool mine    = heldId == kBeltConnectorId || id == kBeltId || isDepotId(id) || (heldId == kEjectorId && c.sneaking);
        // a repeat of a click we handle: swallow it (a second depot click would take the item back)
        if (mine && !c.sneaking && debounced(player, pos)) return handledResult(player);
        if (mine && c.sneaking && (heldId == kBeltConnectorId || heldId == kEjectorId) && debounced(player, pos))
            return handledResult(player);
        if (heldId == kBeltConnectorId) handled = onConnectorUse(player, pos, face, h);
        else if (id == kBeltId) handled = onBeltUse(player, pos, face, h);
        else if (isDepotId(id)) handled = onDepotUse(player, pos, face, h);
        if (!handled && heldId == kEjectorId) {
            if (c.sneaking) handled = onEjectorItemUse(player, pos);
            else state().placeIntents[placementPos(region, at, face)] = {uuidOf(player), game::playerFacing(player)};
        }
        if (handled) return handledResult(player);
    } catch (std::exception const& e) {
        log().error("belts: interaction failed: {}", e.what());
    }
    return ia::Result::Pass;
}

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

} // namespace

bool isDisplayActor(Actor const& actor) {
    return state().displays.contains(actor.getOrCreateUniqueID().rawID) || actor.hasTag(kDisplayTag);
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

// ... and hoppers don't suck them in
LL_TYPE_INSTANCE_HOOK(
    BeltsDisplayHopperHook,
    ll::memory::HookPriority::Normal,
    Hopper,
    &Hopper::_tryMoveInItem,
    bool,
    BlockSource&                                                  region,
    Container&                                                    container,
    std::optional<std::reference_wrapper<ItemActor>> const&       itemActor,
    ItemStack&                                                    item,
    int                                                           slot,
    int                                                           face,
    int                                                           itemCount
) {
    if (itemActor && isDisplayActor(itemActor->get())) return false;
    return origin(region, container, itemActor, item, slot, face, itemCount);
}

// explosions: remember which positions were blown up, so the belt segments among them drop their
// loot (Create's explosion drops; players and commands are told apart in BeltEdit.cpp)
LL_TYPE_INSTANCE_HOOK(BeltsExplosionHook, ll::memory::HookPriority::Normal, Explosion, &Explosion::explode, bool, ::IRandom& random) {
    // belt segments that disappear while the explosion runs were blown up
    auto&       s      = state();
    std::size_t before = s.removals.size();
    bool        result = origin(random);
    for (std::size_t i = before; i < s.removals.size(); ++i) s.exploded[s.removals[i].pos] = mRadius;
    return result;
}

void installHooks() {
    auto& ids = handlerIds();
    ids.push_back(ia::onUseOn({}, onUseOn));
    ids.push_back(ia::onUse(std::string(kBeltConnectorId), [](ia::UseContext const& c) {
        if (c.onBlock || !c.sneaking) return ia::Result::Pass;
        clearConnectorSelection(c.player); // Create: sneak-use resets the first pulley
        return ia::Result::Consume;
    }));
    ids.push_back(ia::onDestroy({.block = std::string(kBeltId)}, [](ia::DestroyContext const& c) {
        state().playerBroke[c.pos] = c.player.isCreative();
        return ia::Result::Pass;
    }));
    BeltsDisplayTouchHook::hook();
    BeltsDisplayTickHook::hook();
    BeltsDisplayHopperHook::hook();
    BeltsExplosionHook::hook();
    installShapes();
}

void uninstallHooks() {
    uninstallShapes();
    BeltsExplosionHook::unhook();
    BeltsDisplayHopperHook::unhook();
    BeltsDisplayTickHook::unhook();
    BeltsDisplayTouchHook::unhook();
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
}

} // namespace lv::feature::belts
