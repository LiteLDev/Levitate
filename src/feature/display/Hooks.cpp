// Display unit (U24): game hooks. GameMode is where every player action ends up (network players
// and SimulatedPlayers alike), so the display blocks and items are handled here: right-clicks on
// links / boards / nixie tubes / clipboards, the link and clipboard items, placements (states the
// BP can't compute) and the clipboard's left-click paste.

#include <map>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Items.h"
#include "feature/display/Internal.h"
#include "game/Blocks.h"

namespace lv::feature::display {

namespace items = common::items;

namespace {

bool isServerPlayer(Player const& p) { return !p.isClientSide(); }

InteractionResult handled(Player& player) {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    player.sendInventory(false); // undo the client's predicted use
    return r;
}

/// Bedrock repeats use-on packets while the button is held: one action per click.
bool repeated(Player& player, GlobalPos const& pos) {
    static std::map<std::int64_t, std::pair<GlobalPos, std::uint64_t>> last;
    auto  now = gameTime();
    auto& l   = last[player.getOrCreateUniqueID().rawID];
    bool  rep = l.first == pos && now - l.second < 5;
    l         = {pos, now};
    return rep;
}

BlockPos placementPos(BlockSource& region, BlockPos const& clicked, uchar face) {
    if (game::replaceable(region, clicked) || face > 5) return clicked;
    return BlockPos{clicked.x + kDx[face], clicked.y + kDy[face], clicked.z + kDz[face]};
}

/// Our handling of a right-click on a block; true = consumed (no vanilla use / placement).
bool onUseOn(Player& player, ItemStack const& held, BlockSource& region, BlockPos const& at, uchar face, Vec3 const& hit) {
    auto  gp     = game::globalPos(region, at);
    auto  heldId = items::idOf(held);
    auto  id     = idAt(region, gp);
    bool  sneak  = player.isSneaking();
    bool  rep    = repeated(player, gp);

    // the clipboard's settings handler runs before the block (Create's RightClickBlock event)
    if (heldId == kClipboardId && !sneak) {
        if (rep) return true;
        if (clipboard::useOnBlock(player, region, gp, face, hit)) return true;
    }
    if (id == kLinkId && !sneak && heldId != kWrenchId) {
        if (!rep) {
            auto pid = player.getOrCreateUniqueID().rawID;
            defer([pid, gp] {
                if (auto* p = playerById(pid)) link::openScreen(*p, gp);
            });
        }
        return true;
    }
    if (board::isBoard(id)) {
        if (!sneak && heldId == kBoardId) {
            if (auto target = board::placementHelper(player, region, gp, face, hit)) {
                // Create's placement helper: extend the board instead of placing against it
                auto const* fresh = game::blockWith(kBoardId);
                if (fresh && game::setBlock(region, *target, *fresh)) {
                    auto tp = game::globalPos(region, *target);
                    board::placed(player, region, tp, board::structureOf(region, gp).value_or(board::Structure{}).facing);
                    if (!player.isCreative()) {
                        player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
                    }
                    playSound(region, tp, "dig.stone");
                }
                return true;
            }
        }
        if (!sneak) {
            if (rep) return heldId.empty() || dyeOf(heldId) || heldId == "minecraft:glow_ink_sac";
            if (board::use(player, region, gp, face, hit, held)) return true;
        }
    }
    if (nixie::isNixie(id) && !sneak) {
        if (rep) return heldId.empty() || dyeOf(heldId);
        if (nixie::use(player, region, gp, held)) return true;
    }
    if (id == kClipboardId && heldId != kClipboardId) {
        if (rep) return true;
        return clipboard::useBlock(player, region, gp);
    }
    if (heldId == kClipboardId && !sneak) {
        // Create's ClipboardBlockItem.useOn: without sneaking it opens the clipboard (sneak places it)
        if (!rep) clipboard::useInAir(player);
        return true;
    }
    if (heldId == kLinkId) {
        if (rep) return true;
        return link::useItem(player, region, at, face);
    }
    return false;
}

/// After the game placed a block from one of our items.
void afterPlace(Player& player, ItemStack const& before, BlockSource& region, BlockPos const& placedAt, uchar face, std::optional<int> clickedBoardFacing) {
    auto gp = game::globalPos(region, placedAt);
    auto id = idAt(region, gp);
    auto heldId = items::idOf(before);
    if (id.empty() || id != heldId) return;
    if (id == kLinkId) link::placed(player, gp);
    else if (id == kBoardId) board::placed(player, region, gp, clickedBoardFacing);
    else if (nixie::isNixie(id)) nixie::placed(player, region, gp, face);
    else if (id == kClipboardId) clipboard::placed(player, region, gp, face, before);
}

bool ourItem(std::string const& id) { return id == kLinkId || id == kBoardId || id == kClipboardId || nixie::isNixie(id); }

} // namespace

LL_TYPE_INSTANCE_HOOK(
    DisplayUseItemOnHook,
    ll::memory::HookPriority::High,
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
    if (!isServerPlayer(player)) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto& region = player.getDimensionBlockSource();
    if (onUseOn(player, item, region, at, face, hit)) return handled(player);
    auto heldId = items::idOf(item);
    if (!ourItem(heldId)) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    // a placement: let the game place it, then set the states the pack can't compute
    BlockPos           target = placementPos(region, at, face);
    std::optional<int> boardFacing;
    if (heldId == kBoardId) {
        if (auto s = board::structureOf(region, game::globalPos(region, at))) boardFacing = s->facing;
    }
    ItemStack before = item;
    auto      result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    afterPlace(player, before, region, target, face, boardFacing);
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    DisplayUseItemHook,
    ll::memory::HookPriority::High,
    GameMode,
    &GameMode::$useItem,
    bool,
    ItemStack& item,
    HandSlot   handSlot
) {
    Player& player = mPlayer;
    if (isServerPlayer(player) && items::idOf(item) == kClipboardId && !player.isSneaking()) {
        clipboard::useInAir(player);
        return true;
    }
    return origin(item, handSlot);
}

LL_TYPE_INSTANCE_HOOK(
    DisplayStartDestroyHook,
    ll::memory::HookPriority::High,
    GameMode,
    &GameMode::$startDestroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face,
    bool&           hasDestroyedBlock
) {
    Player& player = mPlayer;
    if (isServerPlayer(player) && items::idOf(player.getSelectedItem()) == kClipboardId) {
        auto& region = player.getDimensionBlockSource();
        if (clipboard::paste(player, region, game::globalPos(region, pos))) {
            hasDestroyedBlock = false;
            return false;
        }
    }
    return origin(pos, face, hasDestroyedBlock);
}

LL_TYPE_INSTANCE_HOOK(
    DisplayDestroyBlockHook,
    ll::memory::HookPriority::High,
    GameMode,
    &GameMode::$destroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player)) return origin(pos, face);
    auto& region = player.getDimensionBlockSource();
    auto  gp     = game::globalPos(region, pos);
    if (items::idOf(player.getSelectedItem()) == kClipboardId && player.isCreative()) {
        // creative left-clicks destroy at once: paste first (Create cancels the break)
        if (clipboard::paste(player, region, gp)) return false;
    }
    auto id = idAt(region, gp);
    if (id == kClipboardId) clipboard::noteBreak(player, gp);
    bool ok = origin(pos, face);
    if (ok) {
        if (board::isBoard(id)) board::removed(region, gp);
        else if (nixie::isNixie(id)) nixie::removed(gp);
    }
    return ok;
}

void installHooks() {
    DisplayUseItemOnHook::hook();
    DisplayUseItemHook::hook();
    DisplayStartDestroyHook::hook();
    DisplayDestroyBlockHook::hook();
}

void uninstallHooks() {
    DisplayDestroyBlockHook::unhook();
    DisplayStartDestroyHook::unhook();
    DisplayUseItemHook::unhook();
    DisplayUseItemOnHook::unhook();
}

} // namespace lv::feature::display
