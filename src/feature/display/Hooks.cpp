// Display unit (U24): player interactions through the shared dispatch (common/Interactions.h;
// network players and SimulatedPlayers alike): right-clicks on links / boards / nixie tubes /
// clipboards, the link and clipboard items, placements (states the BP can't compute) and the
// clipboard's left-click paste.

#include <map>
#include <optional>
#include <vector>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/display/Internal.h"
#include "game/Blocks.h"

namespace lv::feature::display {

namespace items = common::items;

namespace {

/// Bedrock repeats use-on packets while the button is held: one action per click.
bool repeated(Player& player, GlobalPos const& pos) {
    static std::map<std::int64_t, std::pair<GlobalPos, std::uint64_t>> last;
    auto  now = gameTime();
    auto& l   = last[player.getOrCreateUniqueID().rawID];
    bool  rep = l.first == pos && now - l.second < 5;
    l         = {pos, now};
    return rep;
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

namespace ia = common::interactions;

namespace {

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

/// Facing of the clicked board, noted before a board placement (per player).
std::map<std::int64_t, std::optional<int>>& pendingBoardFacing() {
    static std::map<std::int64_t, std::optional<int>> m;
    return m;
}

Vec3 hitVec(ia::Vec const& h) { return Vec3{float(h[0]), float(h[1]), float(h[2])}; }

} // namespace

void installHooks() {
    auto& ids = handlerIds();
    // right-clicks on our blocks / with our items (Create's use / useOn)
    ids.push_back(ia::onUseOn(
        {},
        [](ia::UseOnContext const& c) {
            auto& player = c.player;
            auto& region = player.getDimensionBlockSource();
            auto  at     = game::blockPos(c.pos);
            if (onUseOn(player, c.item, region, at, static_cast<uchar>(c.face), hitVec(c.hit))) {
                player.sendInventory(false); // undo the client's predicted use
                return ia::Result::Consume;
            }
            // a board placement: remember the clicked board's facing for the placed one
            if (c.itemId == kBoardId) {
                std::optional<int> facing;
                if (auto st = board::structureOf(region, c.pos)) facing = st->facing;
                pendingBoardFacing()[player.getOrCreateUniqueID().rawID] = facing;
            }
            return ia::Result::Pass;
        },
        ia::priority::High
    ));
    // after the game placed a block from one of our items: the states the pack can't compute
    ids.push_back(ia::onPlace({}, [](ia::PlaceContext const& c) {
        std::optional<int> boardFacing;
        if (c.itemId == kBoardId) {
            auto& pending = pendingBoardFacing();
            if (auto it = pending.find(c.player.getOrCreateUniqueID().rawID); it != pending.end()) {
                boardFacing = it->second;
                pending.erase(it);
            }
        }
        if (!ourItem(c.itemId) || c.blockId != c.itemId) return;
        ItemStack before = c.itemBefore ? *c.itemBefore : ItemStack{};
        afterPlace(c.player, before, c.player.getDimensionBlockSource(), game::blockPos(c.pos), static_cast<uchar>(c.face), boardFacing);
    }));
    // the clipboard in the air opens it (sneaking: vanilla)
    ids.push_back(ia::onUse(
        kClipboardId,
        [](ia::UseContext const& c) {
            if (c.sneaking || c.onBlock) return ia::Result::Pass; // a block click was the use-on's
            clipboard::useInAir(c.player);
            return ia::Result::Consume;
        },
        ia::priority::High
    ));
    // left-click with the clipboard pastes its settings
    ids.push_back(ia::onStartDestroy(
        {.item = kClipboardId},
        [](ia::DestroyContext const& c) {
            auto& region = c.player.getDimensionBlockSource();
            return clipboard::paste(c.player, region, c.pos) ? ia::Result::Consume : ia::Result::Pass;
        },
        ia::priority::High
    ));
    ids.push_back(ia::onDestroy(
        {},
        [](ia::DestroyContext const& c) {
            auto& region = c.player.getDimensionBlockSource();
            if (c.itemId == kClipboardId && c.player.isCreative()) {
                // creative left-clicks destroy at once: paste first (Create cancels the break)
                if (clipboard::paste(c.player, region, c.pos)) return ia::Result::Consume;
            }
            if (c.blockId == kClipboardId) clipboard::noteBreak(c.player, c.pos);
            return ia::Result::Pass;
        },
        ia::priority::High
    ));
    ids.push_back(ia::onDestroyed({}, [](ia::DestroyContext const& c, bool destroyed) {
        if (!destroyed) return;
        if (board::isBoard(c.blockId)) board::removed(c.player.getDimensionBlockSource(), c.pos);
        else if (nixie::isNixie(c.blockId)) nixie::removed(c.pos);
    }));
}

void uninstallHooks() {
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
    pendingBoardFacing().clear();
}

} // namespace lv::feature::display
