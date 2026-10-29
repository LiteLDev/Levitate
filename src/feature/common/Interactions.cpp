// Shared interaction dispatch (see Interactions.h): one hook per GameMode action.

#include "feature/common/Interactions.h"

#include <algorithm>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::common::interactions {

namespace {

template <class Fn>
struct Entry {
    HandlerId id;
    int       priority;
    Match     match;
    Fn        fn;
};

struct Registry {
    std::vector<Entry<UseOnHandler>>   useOn;
    std::vector<Entry<UseHandler>>     use;
    std::vector<Entry<DestroyHandler>> destroy;
    std::vector<Entry<PlaceHandler>>   place;
    std::vector<Entry<DestroyHandler>> startDestroy;
    std::vector<Entry<EntityHandler>>  interact;
    std::vector<Entry<EntityHandler>>  attack;
    std::vector<std::function<void()>> deferred;
    HandlerId                          next    = 0;
    int                                inUseOn = 0; ///< depth of the use-on hook (UseContext::onBlock)
};
Registry& registry() {
    static Registry r;
    return r;
}

template <class Fn>
HandlerId add(std::vector<Entry<Fn>>& list, Match match, Fn fn, int priority) {
    auto id = ++registry().next;
    list.push_back({id, priority, std::move(match), std::move(fn)});
    // higher priority first; equal priorities keep the registration order
    std::stable_sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.priority > b.priority; });
    return id;
}

bool matches(Match const& m, std::string const& blockId, std::string const& itemId) {
    if (!m.block.empty() && m.block != blockId) return false;
    if (m.emptyHand) return itemId.empty();
    return m.item.empty() || m.item == itemId;
}

template <class Fn, class Ctx>
Result run(std::vector<Entry<Fn>> const& list, std::string const& blockId, std::string const& itemId, Ctx const& ctx, char const* what) {
    auto snapshot = list; // handlers may register or remove handlers
    for (auto const& e : snapshot) {
        if (!matches(e.match, blockId, itemId)) continue;
        try {
            auto r = e.fn(ctx);
            if (r != Result::Pass) return r;
        } catch (std::exception const& ex) {
            log().error("interactions: {} handler failed: {}", what, ex.what());
        }
    }
    return Result::Pass;
}

std::string idOf(ItemStack const& item) { return item.isNull() ? std::string{} : item.getTypeName(); }

/// Block-local hit position (the game passes it absolute or relative depending on the source).
Vec localHit(BlockPos const& pos, Vec3 const& click) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(click.x) && in(click.y) && in(click.z)) return {click.x, click.y, click.z};
    return {click.x - pos.x, click.y - pos.y, click.z - pos.z};
}

Vec vecOf(Vec3 const& v) { return {v.x, v.y, v.z}; }

} // namespace

/// A player of the server level. `isClientSide()` alone is not enough: the client's own
/// LocalPlayer goes through GameMode too (prediction) and was seen passing it, which ran handlers
/// twice in local worlds and on the client of a remote server.
bool isServerPlayer(Player& p) {
    if (p.isClientSide()) return false;
    auto level = ll::service::getLevel();
    return level && &p.getLevel() == level.as_ptr();
}

HandlerId onUseOn(Match match, UseOnHandler fn, int prio) { return add(registry().useOn, std::move(match), std::move(fn), prio); }

HandlerId onUse(std::string itemId, UseHandler fn, int prio) {
    return add(registry().use, Match{{}, std::move(itemId), false}, std::move(fn), prio);
}

HandlerId onDestroy(Match match, DestroyHandler fn, int prio) {
    return add(registry().destroy, std::move(match), std::move(fn), prio);
}

HandlerId onWrench(std::string blockId, UseOnHandler fn, int prio) {
    return onUseOn(Match{std::move(blockId), kWrenchId, false}, std::move(fn), prio);
}

HandlerId onPlace(Match match, PlaceHandler fn, int prio) { return add(registry().place, std::move(match), std::move(fn), prio); }

HandlerId onStartDestroy(Match match, DestroyHandler fn, int prio) {
    return add(registry().startDestroy, std::move(match), std::move(fn), prio);
}

HandlerId onInteract(Match match, EntityHandler fn, int prio) {
    return add(registry().interact, std::move(match), std::move(fn), prio);
}

HandlerId onAttack(Match match, EntityHandler fn, int prio) { return add(registry().attack, std::move(match), std::move(fn), prio); }

void nextTick(std::function<void()> fn) { registry().deferred.push_back(std::move(fn)); }

void remove(HandlerId id) {
    auto& r    = registry();
    auto  gone = [id](auto const& e) { return e.id == id; };
    std::erase_if(r.useOn, gone);
    std::erase_if(r.use, gone);
    std::erase_if(r.destroy, gone);
    std::erase_if(r.place, gone);
    std::erase_if(r.startDestroy, gone);
    std::erase_if(r.interact, gone);
    std::erase_if(r.attack, gone);
}

Result dispatchUseOn(UseOnContext const& ctx) { return run(registry().useOn, ctx.blockId, ctx.itemId, ctx, "use-on"); }
Result dispatchUse(UseContext const& ctx) { return run(registry().use, std::string{}, ctx.itemId, ctx, "use"); }
Result dispatchDestroy(DestroyContext const& ctx) { return run(registry().destroy, ctx.blockId, ctx.itemId, ctx, "destroy"); }
Result dispatchStartDestroy(DestroyContext const& ctx) {
    return run(registry().startDestroy, ctx.blockId, ctx.itemId, ctx, "start-destroy");
}
Result dispatchInteract(EntityContext const& ctx) { return run(registry().interact, ctx.entityId, ctx.itemId, ctx, "interact"); }
Result dispatchAttack(EntityContext const& ctx) { return run(registry().attack, ctx.entityId, ctx.itemId, ctx, "attack"); }

void dispatchPlace(PlaceContext const& ctx) {
    auto snapshot = registry().place;
    for (auto const& e : snapshot) {
        if (!matches(e.match, ctx.blockId, ctx.itemId)) continue;
        try {
            e.fn(ctx);
        } catch (std::exception const& ex) {
            log().error("interactions: place handler failed: {}", ex.what());
        }
    }
}

namespace {
void runDeferred() {
    auto& r = registry();
    if (r.deferred.empty()) return;
    auto list = std::move(r.deferred);
    r.deferred.clear();
    for (auto& fn : list) {
        try {
            fn();
        } catch (std::exception const& ex) {
            log().error("interactions: deferred action failed: {}", ex.what());
        }
    }
}

BlockPos stepOf(BlockPos const& p, int face) {
    static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    if (face < 0 || face > 5) return p;
    return {p.x + dx[face], p.y + dy[face], p.z + dz[face]};
}

struct UseOnDepth {
    UseOnDepth() { ++registry().inUseOn; }
    ~UseOnDepth() { --registry().inUseOn; }
    UseOnDepth(UseOnDepth const&)            = delete;
    UseOnDepth& operator=(UseOnDepth const&) = delete;
};
} // namespace

// ---- hooks -------------------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    InteractionsUseItemOnHook,
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
    if (!isServerPlayer(player)) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    UseOnDepth   depth;
    auto&        region  = player.getDimensionBlockSource();
    Block const& block   = targetBlock ? *targetBlock : region.getBlock(at);
    std::string  blockId = block.getTypeName();
    std::string  itemId  = idOf(item);
    UseOnContext ctx{
        player,
        item,
        itemId,
        block,
        blockId,
        game::globalPos(region, at),
        static_cast<int>(face),
        localHit(at, hit),
        player.isSneaking(),
        isFirstEvent,
    };
    auto r = registry().useOn.empty() ? Result::Pass : dispatchUseOn(ctx);
    if (r == Result::Pass) {
        if (registry().place.empty() || item.isNull()) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
        // a placement: the block lands in the clicked block when that is replaceable
        BlockPos     dest   = game::replaceable(region, at) ? at : stepOf(at, face);
        std::string  before = region.getBlock(dest).getTypeName();
        auto         result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
        Block const& now    = region.getBlock(dest);
        std::string  after  = now.getTypeName();
        if (after != before && !now.isAir()) {
            PlaceContext pc{player, itemId, now, after, game::globalPos(region, dest), ctx.pos, ctx.face, ctx.hit, ctx.sneaking};
            dispatchPlace(pc);
        }
        return result;
    }
    if (isFirstEvent) player.sendInventory(false); // undo the client's predicted use (once per click)
    InteractionResult result{};
    if (r == Result::Consume) {
        result.mSuccess = true;
        result.mSwing   = true;
    }
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    InteractionsUseItemHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::baseUseItem, // in the air, and after a block that did not take a use-on
    bool,
    ItemStack const& item,
    HandSlot         handSlot
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player) || registry().use.empty()) return origin(item, handSlot);
    std::string itemId = idOf(item);
    auto        r      = dispatchUse(UseContext{player, item, itemId, player.isSneaking(), registry().inUseOn > 0});
    if (r == Result::Pass) return origin(item, handSlot);
    player.sendInventory(false);
    return r == Result::Consume;
}

LL_TYPE_INSTANCE_HOOK(
    InteractionsDestroyBlockHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$destroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player) || registry().destroy.empty()) return origin(pos, face);
    auto&        region  = player.getDimensionBlockSource();
    Block const& block   = region.getBlock(pos);
    std::string  blockId = block.getTypeName();
    auto const&  item    = player.getSelectedItem();
    std::string  itemId  = idOf(item);
    DestroyContext ctx{player, item, itemId, block, blockId, game::globalPos(region, pos), static_cast<int>(face), player.isSneaking()};
    if (dispatchDestroy(ctx) == Result::Pass) return origin(pos, face);
    return false;
}

LL_TYPE_INSTANCE_HOOK(
    InteractionsStartDestroyHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$startDestroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face,
    bool&           hasDestroyedBlock
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player) || registry().startDestroy.empty()) return origin(pos, face, hasDestroyedBlock);
    auto&          region  = player.getDimensionBlockSource();
    Block const&   block   = region.getBlock(pos);
    std::string    blockId = block.getTypeName();
    auto const&    item    = player.getSelectedItem();
    std::string    itemId  = idOf(item);
    DestroyContext ctx{player, item, itemId, block, blockId, game::globalPos(region, pos), static_cast<int>(face), player.isSneaking()};
    if (dispatchStartDestroy(ctx) == Result::Pass) return origin(pos, face, hasDestroyedBlock);
    hasDestroyedBlock = false;
    return false;
}

LL_TYPE_INSTANCE_HOOK(
    InteractionsInteractHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$interact,
    bool,
    Actor&      entity,
    Vec3 const& location,
    HandSlot    handSlot
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player) || registry().interact.empty()) return origin(entity, location, handSlot);
    auto const&   item     = player.getSelectedItem();
    std::string   itemId   = idOf(item);
    std::string   entityId = entity.getTypeName();
    EntityContext ctx{player, item, itemId, entity, entityId, vecOf(location), player.isSneaking()};
    auto          r = dispatchInteract(ctx);
    if (r == Result::Pass) return origin(entity, location, handSlot);
    player.sendInventory(false);
    return r == Result::Consume;
}

LL_TYPE_INSTANCE_HOOK(
    InteractionsAttackHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$attack,
    bool,
    Actor&      entity,
    Vec3 const& hitPosition
) {
    Player& player = mPlayer;
    if (!isServerPlayer(player) || registry().attack.empty()) return origin(entity, hitPosition);
    auto const&   item     = player.getSelectedItem();
    std::string   itemId   = idOf(item);
    std::string   entityId = entity.getTypeName();
    EntityContext ctx{player, item, itemId, entity, entityId, vecOf(hitPosition), player.isSneaking()};
    auto          r = dispatchAttack(ctx);
    if (r == Result::Pass) return origin(entity, hitPosition);
    return r == Result::Consume;
}

class InteractionsModule {
public:
    void install() {
        InteractionsUseItemOnHook::hook();
        InteractionsUseItemHook::hook();
        InteractionsDestroyBlockHook::hook();
        InteractionsStartDestroyHook::hook();
        InteractionsInteractHook::hook();
        InteractionsAttackHook::hook();
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runDeferred(); });
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldClosed([] { registry().deferred.clear(); });
        }
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mTick);
        InteractionsAttackHook::unhook();
        InteractionsInteractHook::unhook();
        InteractionsStartDestroyHook::unhook();
        InteractionsDestroyBlockHook::unhook();
        InteractionsUseItemHook::unhook();
        InteractionsUseItemOnHook::unhook();
    }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace lv::feature::common::interactions

LV_REGISTER_MODULE_ORDERED(lv::feature::common::interactions::InteractionsModule, lv::ModuleOrder::Services);
