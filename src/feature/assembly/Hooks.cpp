// Game hooks of the assembly unit. GameMode is where every player action ends up (network players,
// SimulatedPlayers and the deployer player alike), so right-clicks on deployers, arm point
// selection, item application and the placement of our blocks are handled here.

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

#include "feature/assembly/AssemblyInternal.h"
#include "game/Blocks.h"

namespace lv::feature::assembly {

namespace {

InteractionResult handled() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

/// Bedrock repeats use-on packets while the button is held: one action per click and block.
bool repeated(Player& player, BlockPos const& pos) {
    static std::map<std::int64_t, std::pair<BlockPos, std::uint64_t>> last;
    auto  key  = player.getOrCreateUniqueID().rawID;
    auto& e    = last[key];
    bool  skip = e.first == pos && detail::currentTick() - e.second < 5;
    e          = {pos, detail::currentTick()};
    return skip;
}

BlockPos placementPos(BlockSource& region, BlockPos const& clicked, Face face) {
    if (game::replaceable(region, clicked)) return clicked;
    return detail::offsetPos(clicked, face);
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    AssemblyUseItemOnHook,
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
    if (player.isClientSide() || face > 5) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto&       region  = player.getDimensionBlockSource();
    Block const& clicked = targetBlock ? *targetBlock : region.getBlock(at);
    std::string heldId   = item.isNull() ? "" : item.getTypeName();
    Face        f        = static_cast<Face>(face);
    bool        fake     = fakeplayer::isFake(player);

    // the arm item selects interaction points instead of placing
    if (heldId == kArmId && arm::isPoint(region, at)) {
        if (repeated(player, at)) return handled(); // the held button repeats: one toggle per click
        if (arm::onSelect(player, at)) return handled();
    }

    // deployers: swap items on the front face, the wrench toggles the mode
    if (clicked.getTypeName() == kDeployerId && (!player.isSneaking() || item.isNull())
        && (f == static_cast<Face>(detail::intState(clicked, "levitate:facing").value_or(1)))) {
        if (repeated(player, at)) return handled();
        if (deployer::onUse(player, at, f, item)) return handled();
    }

    // item application recipes (Create's ManualApplicationRecipe)
    if (!item.isNull() && !player.isSneaking()) {
        if (application::tryApply(player, region, at, item, fake)) return handled();
    }

    // our blocks: orient them once placed
    if (heldId == kDeployerId || heldId == kArmId) {
        BlockPos target = placementPos(region, at, f);
        auto     result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
        if (game::blockIdAt(region, target) == heldId) {
            if (heldId == kDeployerId) deployer::onPlaced(region, target, &player, f);
            else arm::onPlaced(region, target, &player, f);
        }
        return result;
    }
    return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
}

LL_TYPE_INSTANCE_HOOK(
    AssemblyStartDestroyHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$startDestroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face,
    bool&           hasDestroyedBlock
) {
    Player& player = mPlayer;
    if (!player.isClientSide()) {
        auto const& held = player.getSelectedItem();
        if (!held.isNull() && held.getTypeName() == kArmId && arm::onDeselect(player, pos)) {
            hasDestroyedBlock = false;
            return false;
        }
    }
    return origin(pos, face, hasDestroyedBlock);
}

LL_TYPE_INSTANCE_HOOK(
    AssemblyDestroyHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$destroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face
) {
    Player& player = mPlayer;
    if (!player.isClientSide()) {
        auto const& held = player.getSelectedItem();
        if (!held.isNull() && held.getTypeName() == kArmId && arm::onDeselect(player, pos)) return false;
    }
    return origin(pos, face);
}

void installHooks() {
    AssemblyUseItemOnHook::hook();
    AssemblyStartDestroyHook::hook();
    AssemblyDestroyHook::hook();
}

void uninstallHooks() {
    AssemblyDestroyHook::unhook();
    AssemblyStartDestroyHook::unhook();
    AssemblyUseItemOnHook::unhook();
}

} // namespace lv::feature::assembly
