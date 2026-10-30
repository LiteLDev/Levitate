// Item hatch (U49, SPEC §3.6.12): Create's ItemHatchBlock / ItemHatchBlockEntity / ItemHatchHandler /
// HatchFilterSlot. Placed on the side of a container; a right-click puts the held item (sneaking:
// every inventory item outside the hotbar) into that container, through its filter.

#include <map>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/ActorsExt.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

// U15's deployer player (Create's FakePlayer), which Create's hatch ignores.
namespace lv::feature::assembly::fakeplayer {
bool isFake(Player const& player);
}

namespace lv::feature::funnels {

namespace items = common::items;
namespace ia    = common::interactions;

inline constexpr char kItemHatch[] = "levitate:item_hatch";
inline constexpr char kOpen[]      = "levitate:open";

namespace {

using namespace detail;

struct State {
    std::map<GlobalPos, std::uint64_t> closeAt; ///< scheduled ticks (Create's scheduleTick(pos, 10))
    std::uint64_t                      tick = 0;
    std::map<std::int64_t, std::pair<GlobalPos, std::uint64_t>> lastUse;
};
State& st() {
    static State s;
    return s;
}

bool fitsInContainers(ItemStack const& s) {
    // Item#canFitInsideContainerItems: shulker boxes don't (packages always do)
    return !items::idOf(s).ends_with("shulker_box");
}

/// ItemHatchBlock#useItemOn.
ia::Result use(ia::UseOnContext const& c) {
    auto& player = c.player;
    if (assembly::fakeplayer::isFake(player)) return ia::Result::Consume;
    // Bedrock repeats the use while the button is held: one deposit per click
    auto uid = player.getOrCreateUniqueID().rawID;
    if (auto it = st().lastUse.find(uid); it != st().lastUse.end() && it->second.first == c.pos && st().tick - it->second.second < 5 && !c.firstEvent)
        return ia::Result::Consume;
    st().lastUse[uid] = {c.pos, st().tick};

    auto& region = player.getDimensionBlockSource();
    int   facing = state(c.block, kFacing, 3);
    if (facing < 2 || facing > 5) return ia::Result::Deny;
    BlockPos target = offset(toBlockPos(c.pos), facing);
    if (!items::has(region, target, -1)) return ia::Result::Deny; // no inventory there: FAIL

    bool depositHeld = !c.sneaking;
    if (!depositHeld && c.itemId == kWrench) return ia::Result::Pass; // the wrench's own sneak-click
    auto filter    = common::filterslot::get(c.pos);
    bool hasFilter = !items::isEmpty(filter);

    auto&                  inv      = player.getInventory();
    int                    selected = player.getSelectedItemSlot();
    std::vector<ItemStack> failed;
    bool                   any = false;
    for (int i = 0; i < 36 && i < inv.getContainerSize(); ++i) {
        bool hotbar = i < 9;
        if (hotbar != depositHeld) continue;
        if (depositHeld && i != selected) continue;
        ItemStack item = inv.getItem(i);
        if (items::isEmpty(item) || !fitsInContainers(item)) continue;
        if (hasFilter && !common::filterslot::matches(c.pos, item)) continue;
        int  count = items::countOf(item);
        auto rest  = items::insert(region, target, -1, item, true);
        int  fit   = count - (items::isEmpty(rest) ? 0 : items::countOf(rest));
        if (fit <= 0) continue;
        inv.setItem(i, fit >= count ? items::empty() : items::withCount(item, count - fit));
        auto left = items::insert(region, target, -1, items::withCount(item, fit), false);
        any       = true;
        // the remainder might not be empty in item handler edge cases
        if (!items::isEmpty(left)) failed.push_back(left);
    }
    for (auto const& f : failed) giveOrDrop(player, f);
    player.sendInventory(false);
    if (!any) return ia::Result::Consume;

    // AllSoundEvents.ITEM_HATCH: a barrel opening and a netherite block placed
    playSound(region, toBlockPos(c.pos), "block.barrel.open", .25f, 1.4f);
    playSound(region, toBlockPos(c.pos), "use.netherite", .75f, 1.15f);
    setStates(region, toBlockPos(c.pos), {{kOpen, 1}});
    st().closeAt[c.pos] = st().tick + 10;
    auto key = depositHeld ? "create.item_hatch.deposit_item" : "create.item_hatch.deposit_inventory";
    contraption::actors_ext::actionBar(player, trFor(player, key, depositHeld ? "Held item deposited" : "Inventory items deposited"));
    log().info("funnels: {} deposited {} through the item hatch at {}", player.getRealName(), depositHeld ? "the held item" : "the inventory", c.pos.key());
    return ia::Result::Consume;
}

/// ItemHatchBlock#getStateForPlacement: only against the side of a block, facing into it.
void placed(ia::PlaceContext const& c) {
    auto& region = c.player.getDimensionBlockSource();
    if (c.face < 2 || c.face > 5) {
        game::clearBlock(region, toBlockPos(c.pos));
        if (!c.player.isCreative())
            if (auto item = items::make(kItemHatch, 1)) giveOrDrop(c.player, *item);
        return;
    }
    setStates(region, toBlockPos(c.pos), {{kFacing, c.face ^ 1}, {kOpen, 0}});
    log().info("funnels: levitate:item_hatch placed at {} by {}", c.pos.key(), c.player.getRealName());
}

void tick() {
    auto& s = st();
    ++s.tick;
    for (auto it = s.closeAt.begin(); it != s.closeAt.end();) {
        if (it->second > s.tick) {
            ++it;
            continue;
        }
        // ItemHatchBlock#tick: the hatch closes again
        if (auto* region = regionOf(it->first.dim)) {
            auto bp = toBlockPos(it->first);
            if (kItemHatch == idAt(*region, bp)) setStates(*region, bp, {{kOpen, 0}});
        }
        it = s.closeAt.erase(it);
    }
    if (s.lastUse.size() > 256) s.lastUse.clear();
}


class ItemHatchModule {
public:
    void install() {
        static bool once = false;
        if (!once) {
            once = true;
            // HatchFilterSlot: on the slanted door, in front (the face away from the container)
            common::filterslot::SlotSpec slot;
            slot.id     = "filter";
            slot.scale  = 0.5 * 0.965;
            slot.centre = [](Block const& b, int face) -> std::optional<core::common::Vec> {
                int facing = game::intState(b, kFacing).value_or(3);
                if (facing < 2 || facing > 5 || face != (facing ^ 1)) return std::nullopt;
                return core::common::Vec{0.5, 0.2, 0.5};
            };
            common::filterslot::registerSlot(kItemHatch, std::move(slot));
            // ItemHatchBlockEntity's filtering behaviour drops a filter item with the block
            common::blockdata::onErase([](GlobalPos const& pos, Json const& doc, std::string const& previous) {
                if (previous != kItemHatch) return;
                std::vector<ItemStack> drops;
                if (auto f = doc.find("filter"); f != doc.end() && f->is_object())
                    for (auto const& [k, v] : f->items())
                        if (auto s = items::fromJson(v); common::filter::isFilterItem(s)) drops.push_back(s);
                dropLater(pos, std::move(drops));
            });
            game::db::onWorldClosed([] { st() = State{}; });
        }
        mIds.push_back(ia::onUseOn({.block = kItemHatch}, use));
        mIds.push_back(ia::onPlace({.block = kItemHatch}, placed));
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            try {
                tick();
            } catch (std::exception const& e) {
                log().error("funnels: item hatch tick failed: {}", e.what());
            }
        });
    }

    void uninstall() {
        for (auto id : mIds) ia::remove(id);
        mIds.clear();
        ll::event::EventBus::getInstance().removeListener(mTick);
    }

private:
    std::vector<ia::HandlerId> mIds;
    ll::event::ListenerPtr     mTick;
};

} // namespace

} // namespace lv::feature::funnels

LV_REGISTER_MODULE(lv::feature::funnels::ItemHatchModule);
