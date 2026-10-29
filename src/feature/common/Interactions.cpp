// Shared interaction dispatch (see Interactions.h): one hook per GameMode action.

#include "feature/common/Interactions.h"

#include <algorithm>
#include <vector>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "game/Blocks.h"
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
    HandlerId                          next = 0;
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

/// A player of the server level. `isClientSide()` alone is not enough: the client's own
/// LocalPlayer goes through GameMode too (prediction) and was seen passing it, which ran handlers
/// twice in local worlds and on the client of a remote server.
bool isServerPlayer(Player& p) {
    if (p.isClientSide()) return false;
    auto level = ll::service::getLevel();
    return level && &p.getLevel() == level.as_ptr();
}

} // namespace

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

void remove(HandlerId id) {
    auto& r = registry();
    std::erase_if(r.useOn, [id](auto const& e) { return e.id == id; });
    std::erase_if(r.use, [id](auto const& e) { return e.id == id; });
    std::erase_if(r.destroy, [id](auto const& e) { return e.id == id; });
}

Result dispatchUseOn(UseOnContext const& ctx) { return run(registry().useOn, ctx.blockId, ctx.itemId, ctx, "use-on"); }
Result dispatchUse(UseContext const& ctx) { return run(registry().use, std::string{}, ctx.itemId, ctx, "use"); }
Result dispatchDestroy(DestroyContext const& ctx) { return run(registry().destroy, ctx.blockId, ctx.itemId, ctx, "destroy"); }

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
    if (!isServerPlayer(player) || registry().useOn.empty()) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
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
    auto r = dispatchUseOn(ctx);
    if (r == Result::Pass) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
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
    if (!isServerPlayer(player) || registry().use.empty() || item.isNull()) return origin(item, handSlot);
    std::string itemId = idOf(item);
    auto        r      = dispatchUse(UseContext{player, item, itemId, player.isSneaking()});
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

class InteractionsModule {
public:
    void install() {
        InteractionsUseItemOnHook::hook();
        InteractionsUseItemHook::hook();
        InteractionsDestroyBlockHook::hook();
    }
    void uninstall() {
        InteractionsDestroyBlockHook::unhook();
        InteractionsUseItemHook::unhook();
        InteractionsUseItemOnHook::unhook();
    }
};

} // namespace lv::feature::common::interactions

LV_REGISTER_MODULE_ORDERED(lv::feature::common::interactions::InteractionsModule, lv::ModuleOrder::Services);
