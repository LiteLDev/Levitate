// Game hooks and the module of the equipment unit. GameMode is where every player action ends
// up (network players and SimulatedPlayers alike), so item uses, block uses, breaking, attacks and
// the pick range are handled here.

#include <map>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Commands.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::equipment {

namespace items = common::items;

namespace {

InteractionResult handled() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

/// Bedrock repeats use packets while the button is held: one action per click (4 ticks).
bool repeated(Player& player, bool isFirstEvent) {
    static std::map<std::int64_t, std::uint64_t> last;
    auto  uid  = uidOf(player);
    auto& t    = last[uid];
    bool  skip = !isFirstEvent && currentTick() - t < 4;
    if (!skip) t = currentTick();
    return skip;
}

GlobalPos placementPos(BlockSource& region, BlockPos const& at, uchar face) {
    if (game::replaceable(region, at)) return game::globalPos(region, at);
    static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    int f = face < 6 ? face : 1;
    return game::globalPos(region, BlockPos{at.x + dx[f], at.y + dy[f], at.z + dz[f]});
}

/// Block interactions of the unit; true when handled.
bool useOn(Player& player, ItemStack& item, BlockPos const& at, uchar face, Block const& target) {
    GlobalPos   p    = game::globalPos(player.getDimensionBlockSource(), at);
    std::string tid  = target.getTypeName();
    std::string held = items::isEmpty(item) ? std::string() : items::idOf(item);

    if (isToolbox(tid)) return useToolbox(player, item, p);
    if (isBacktank(tid) && useBacktankBlock(player, item, p)) return true;
    if (tid == kLecternController) return lecternUse(player, item, p);
    if (tid == "minecraft:lectern" && held == kLinkedController && lecternUse(player, item, p)) return true;

    if (held == kWand) return wandUseOn(player, item, p, face);
    if (held == kWorldshaper) return shaperUse(player, item);
    if (held == kPotatoCannon) return firePotatoCannon(player);
    if (held == kLinkedController) return controllerUseOn(player, item, p);
    if (held == kTreeFertilizer) return useTreeFertilizer(player, item, p);
    if (isPaper(held)) {
        if (sandBlock(player, item, p)) return true;
        return startSanding(player, true);
    }
    return false;
}

bool useInAir(Player& player, ItemStack& item) {
    std::string held = items::idOf(item);
    if (held == kWand) return wandUse(player, item);
    if (held == kWorldshaper) return shaperUse(player, item);
    if (held == kPotatoCannon) return firePotatoCannon(player);
    if (held == kLinkedController) return controllerUse(player, item);
    if (isPaper(held)) return startSanding(player, true);
    return false;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    EquipmentUseItemOnHook,
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
    auto&        region = player.getDimensionBlockSource();
    Block const& target = targetBlock ? *targetBlock : region.getBlock(at);
    std::string  held   = items::isEmpty(item) ? std::string() : items::idOf(item);
    std::string  tid    = target.getTypeName();
    bool ours = isToolbox(tid) || isBacktank(tid) || tid == kLecternController || (tid == "minecraft:lectern" && held == kLinkedController)
             || held == kWand || held == kWorldshaper || held == kPotatoCannon || held == kLinkedController || held == kTreeFertilizer || isPaper(held);
    // sneaking with a block item against our blocks places the block (Create's sneak pass-through)
    if (ours && !(player.isSneaking() && (isToolbox(tid) || isBacktank(tid)) && game::blockWith(held))) {
        if (repeated(player, isFirstEvent)) {
            InteractionResult r{};
            r.mSuccess = true;
            return r;
        }
        try {
            if (useOn(player, item, at, face, target)) {
                player.sendInventory(false);
                return handled();
            }
        } catch (std::exception const& e) {
            log().error("equipment: use on block failed: {}", e.what());
        }
    }
    // placement: remember the item's data (toolbox contents, backtank air), mirror it, count the grip
    GlobalPos   p      = placementPos(region, at, face);
    std::string before = idAt(p);
    Json        toolboxData = isToolbox(held) ? items::getData(item, kToolboxData) : Json();
    int         air         = isBacktank(held) ? airOf(item) : 0;
    auto        result      = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    std::string after       = idAt(p);
    if (after != before && after != "minecraft:air" && !after.empty()) {
        try {
            if (isToolbox(after) && after == held) toolboxPlaced(player, p, toolboxData);
            if (isBacktank(after) && after == held) backtankPlaced(player, p, air, {});
            mirrorPlacement(player, p);
            extendoAction(player);
        } catch (std::exception const& e) {
            log().error("equipment: placement handling failed: {}", e.what());
        }
    }
    return result;
}

LL_TYPE_INSTANCE_HOOK(EquipmentUseItemHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$useItem, bool, ItemStack& item, HandSlot handSlot) {
    Player& player = mPlayer;
    if (player.isClientSide() || items::isEmpty(item)) return origin(item, handSlot);
    std::string held = items::idOf(item);
    if (held == kWand || held == kWorldshaper || held == kPotatoCannon || held == kLinkedController || isPaper(held)) {
        try {
            if (useInAir(player, item)) {
                // sand paper keeps the vanilla use (the client shows the "eating" animation)
                if (isPaper(held)) return origin(item, handSlot);
                return true;
            }
        } catch (std::exception const& e) {
            log().error("equipment: use failed: {}", e.what());
        }
    }
    return origin(item, handSlot);
}

LL_TYPE_INSTANCE_HOOK(EquipmentStartDestroyHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$startDestroyBlock, bool, BlockPos const& pos,
                      uchar face, bool& hasDestroyedBlock) {
    Player& player = mPlayer;
    if (player.isClientSide()) return origin(pos, face, hasDestroyedBlock);
    GlobalPos   p    = game::globalPos(player.getDimensionBlockSource(), pos);
    std::string id   = idAt(p);
    std::string held = items::idOf(player.getSelectedItem());
    if (held == kWorldshaper) {
        ItemStack shaper = player.getSelectedItem();
        if (shaperSelect(player, shaper, p)) return false;
    }
    if (isToolbox(id) && attackToolbox(player, p)) {
        hasDestroyedBlock = false;
        return false;
    }
    return origin(pos, face, hasDestroyedBlock);
}

LL_TYPE_INSTANCE_HOOK(EquipmentDestroyBlockHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$destroyBlock, bool, BlockPos const& pos, uchar face) {
    Player& player = mPlayer;
    if (player.isClientSide()) return origin(pos, face);
    GlobalPos   p    = game::globalPos(player.getDimensionBlockSource(), pos);
    std::string id   = idAt(p);
    std::string held = items::idOf(player.getSelectedItem());
    if (held == kWorldshaper) {
        ItemStack shaper = player.getSelectedItem();
        if (shaperSelect(player, shaper, p)) return false;
    }
    if (isToolbox(id)) {
        if (attackToolbox(player, p)) return false;
        noteToolboxBreak(p, player.isCreative());
    }
    if (isBacktank(id)) noteBacktankBreak(p, player.isCreative());
    bool ok = origin(pos, face);
    if (ok && idAt(p) != id) {
        try {
            mirrorBreak(player, p, id);
            extendoAction(player);
        } catch (std::exception const& e) {
            log().error("equipment: break handling failed: {}", e.what());
        }
    }
    return ok;
}

LL_TYPE_INSTANCE_HOOK(EquipmentAttackHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$attack, bool, Actor& entity, Vec3 const& hitPos) {
    Player& player = mPlayer;
    if (player.isClientSide()) return origin(entity, hitPos);
    bool grip = extendoReach(player) > 0;
    bool ok   = origin(entity, hitPos);
    if (ok && grip) {
        // Create: +2 knockback while holding a grip, and the hit uses the grip
        auto const& look = player.getViewVector(1.0f);
        double      h    = std::sqrt(double(look.x) * look.x + double(look.z) * look.z);
        if (h > 1e-6 && entity.isAlive() && !entity.isPlayer()) {
            auto const& v = entity.getPosDelta();
            setMotion(entity, v.x + look.x / h * 0.8, v.y + 0.1, v.z + look.z / h * 0.8);
        }
        extendoAction(player);
    }
    return ok;
}

LL_TYPE_INSTANCE_HOOK(EquipmentPickRangeHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$getPickRange, float, InputMode const& mode) {
    float base = origin(mode);
    return base + extendoReach(mPlayer);
}

LL_TYPE_INSTANCE_HOOK(EquipmentReleaseHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$releaseUsingItem, void) {
    Player& player = mPlayer;
    if (!player.isClientSide()) cancelSanding(player);
    origin();
}

// backtanks are unbreakable (their durability is the air bar)
LL_TYPE_INSTANCE_HOOK(EquipmentHurtAndBreakHook, ll::memory::HookPriority::Normal, ItemStackBase, &ItemStackBase::hurtAndBreak, bool, int delta, Actor* owner) {
    if (!isNull() && isBacktank(getTypeName())) return false;
    return origin(delta, owner);
}

namespace {

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload().text("toolbox").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = commands::playerOf(origin);
        if (!player) return output.error("Only players can use toolboxes.");
        openToolboxForm(*player);
        output.success("");
    });
});

class EquipmentModule {
public:
    void install() {
        installToolboxes();
        installBacktanks();
        installWand();
        installShaper();
        installControllers();
        // Create's sandpaper_polishing recipes (U3 loader)
        common::recipes::loadType("sandpaper_polishing");
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldClosed([] { reset(); });
        }
        EquipmentUseItemOnHook::hook();
        EquipmentUseItemHook::hook();
        EquipmentStartDestroyHook::hook();
        EquipmentDestroyBlockHook::hook();
        EquipmentAttackHook::hook();
        EquipmentPickRangeHook::hook();
        EquipmentReleaseHook::hook();
        EquipmentHurtAndBreakHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); }));
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        uninstallBacktanks();
        uninstallControllers();
        EquipmentHurtAndBreakHook::unhook();
        EquipmentReleaseHook::unhook();
        EquipmentPickRangeHook::unhook();
        EquipmentAttackHook::unhook();
        EquipmentDestroyBlockHook::unhook();
        EquipmentStartDestroyHook::unhook();
        EquipmentUseItemHook::unhook();
        EquipmentUseItemOnHook::unhook();
    }

private:
    static void reset() {
        clearDeferred();
        resetToolboxes();
        resetBacktanks();
        resetProjectiles();
        resetControllers();
    }

    static void tick() {
        try {
            runDeferred();
            tickToolboxes();
            tickBacktanks();
            tickDivingGear();
            tickProjectiles();
            tickSanding();
            tickWands();
            tickControllers();
        } catch (std::exception const& e) {
            log().error("equipment: tick failed: {}", e.what());
        }
    }

    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace

} // namespace lv::feature::equipment

LV_REGISTER_MODULE(lv::feature::equipment::EquipmentModule);
