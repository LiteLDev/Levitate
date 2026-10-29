// Game hooks and the module of the equipment unit. Item uses, block uses, breaking and attacks go
// through the shared dispatch (common/Interactions.h; network players and SimulatedPlayers
// alike); the pick range, releasing a used item and item durability are hooked here.

#include <cmath>
#include <map>
#include <vector>

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
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
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

/// Bedrock repeats use packets while the button is held: one action per click (4 ticks).
bool repeated(Player& player, BlockPos const& at, bool isFirstEvent) {
    // (the client may also send the same click twice in one tick, both marked as first events)
    // the second packet may even arrive a tick later: 4 ticks per block and player
    static std::map<std::int64_t, std::pair<BlockPos, std::uint64_t>> last;
    auto  uid  = uidOf(player);
    auto  it   = last.find(uid);
    auto  now  = currentTick() + 1; // 0 = never
    bool  skip = it != last.end() && now - it->second.second < 4 && (!isFirstEvent || it->second.first == at);
    if (!skip) last[uid] = {at, now};
    return skip;
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

// ---- interactions (common/Interactions.h) --------------------------------------------------------

namespace ia = common::interactions;

namespace {

/// Whether a block click concerns the unit (its blocks, its items).
bool oursOn(std::string const& tid, std::string const& held) {
    return isToolbox(tid) || isBacktank(tid) || tid == kLecternController || (tid == "minecraft:lectern" && held == kLinkedController)
        || held == kWand || held == kWorldshaper || held == kPotatoCannon || held == kLinkedController || held == kTreeFertilizer || isPaper(held);
}

/// Grip held when the current attack started (the attack may use it up).
thread_local bool attackWithGrip = false;

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

void installInteractions() {
    auto& ids = handlerIds();
    ids.push_back(ia::onUseOn({}, [](ia::UseOnContext const& c) {
        auto&       player = c.player;
        auto&       item   = ia::mutableItem(c.item);
        std::string held   = items::isEmpty(item) ? std::string() : items::idOf(item);
        auto const& tid    = c.blockId;
        BlockPos    at     = game::blockPos(c.pos);
        // sneaking with a block item against our blocks places the block (Create's sneak pass-through)
        if (oursOn(tid, held) && !(c.sneaking && (isToolbox(tid) || isBacktank(tid)) && game::blockWith(held))) {
            if (repeated(player, at, c.firstEvent)) return ia::Result::Consume;
            try {
                if (useOn(player, item, at, static_cast<uchar>(c.face), c.block)) {
                    player.sendInventory(false);
                    return ia::Result::Consume;
                }
            } catch (std::exception const& e) {
                log().error("equipment: use on block failed: {}", e.what());
            }
        }
        return ia::Result::Pass;
    }));
    // placement: the item's data (toolbox contents, backtank air), mirrored placement, the grip
    ids.push_back(ia::onPlace({}, [](ia::PlaceContext const& c) {
        try {
            auto const& held = c.itemId;
            if (c.itemBefore && isToolbox(c.blockId) && c.blockId == held) {
                toolboxPlaced(c.player, c.pos, items::getData(*c.itemBefore, kToolboxData));
            }
            if (c.itemBefore && isBacktank(c.blockId) && c.blockId == held) backtankPlaced(c.player, c.pos, airOf(*c.itemBefore), {});
            mirrorPlacement(c.player, c.pos);
            extendoAction(c.player);
        } catch (std::exception const& e) {
            log().error("equipment: placement handling failed: {}", e.what());
        }
    }));
    ids.push_back(ia::onUse({}, [](ia::UseContext const& c) {
        auto& player = c.player;
        if (items::isEmpty(c.item) || c.onBlock) return ia::Result::Pass; // block clicks are the use-on's
        auto&       item = ia::mutableItem(c.item);
        std::string held = items::idOf(item);
        if (held == kWand || held == kWorldshaper || held == kPotatoCannon || held == kLinkedController || isPaper(held)) {
            try {
                if (useInAir(player, item)) {
                    // sand paper keeps the vanilla use (the client shows the "eating" animation)
                    if (isPaper(held)) return ia::Result::Pass;
                    return ia::Result::Consume;
                }
            } catch (std::exception const& e) {
                log().error("equipment: use failed: {}", e.what());
            }
        }
        return ia::Result::Pass;
    }));
    ids.push_back(ia::onStartDestroy({}, [](ia::DestroyContext const& c) {
        auto& player = c.player;
        if (c.itemId == kWorldshaper) {
            ItemStack shaper = player.getSelectedItem();
            if (shaperSelect(player, shaper, c.pos)) return ia::Result::Deny;
        }
        if (isToolbox(c.blockId) && attackToolbox(player, c.pos)) return ia::Result::Deny;
        return ia::Result::Pass;
    }));
    ids.push_back(ia::onDestroy({}, [](ia::DestroyContext const& c) {
        auto& player = c.player;
        if (c.itemId == kWorldshaper) {
            ItemStack shaper = player.getSelectedItem();
            if (shaperSelect(player, shaper, c.pos)) return ia::Result::Deny;
        }
        if (isToolbox(c.blockId)) {
            if (attackToolbox(player, c.pos)) return ia::Result::Deny;
            noteToolboxBreak(c.pos);
        }
        if (isBacktank(c.blockId)) noteBacktankBreak(c.pos, player.isCreative());
        return ia::Result::Pass;
    }));
    ids.push_back(ia::onDestroyed({}, [](ia::DestroyContext const& c, bool destroyed) {
        if (isBacktank(c.blockId)) clearBacktankBreak(c.pos); // used by the erase listener inside the break (or not at all)
        if (destroyed && idAt(c.pos) != c.blockId) {
            try {
                mirrorBreak(c.player, c.pos, c.blockId);
                extendoAction(c.player);
            } catch (std::exception const& e) {
                log().error("equipment: break handling failed: {}", e.what());
            }
        }
    }));
    ids.push_back(ia::onAttack({}, [](ia::EntityContext const& c) {
        attackWithGrip = extendoReach(c.player) > 0;
        return ia::Result::Pass;
    }));
    ids.push_back(ia::onAttacked({}, [](ia::EntityContext const& c, bool hit) {
        bool grip      = attackWithGrip;
        attackWithGrip = false;
        if (!hit || !grip) return;
        // Create: +2 knockback while holding a grip, and the hit uses the grip
        auto&       player = c.player;
        auto&       entity = c.entity;
        auto const& look   = player.getViewVector(1.0f);
        double      h      = std::sqrt(double(look.x) * look.x + double(look.z) * look.z);
        if (h > 1e-6 && entity.isAlive() && !entity.isPlayer()) {
            auto const& v = entity.getPosDelta();
            setMotion(entity, v.x + look.x / h * 0.8, v.y + 0.1, v.z + look.z / h * 0.8);
        }
        extendoAction(player);
    }));
}

void uninstallInteractions() {
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
}

} // namespace

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
        installInteractions();
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
        uninstallInteractions();
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
