#include "feature/common/Hoppers.h"

#include <cmath>
#include <set>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Hopper.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "mod/Module.h"

namespace lv::feature::common::hoppers {

namespace {

std::set<std::string, std::less<>>& excluded() {
    // Create: funnels and blaze burners have no item capability; the portable storage interface
    // moves items to and from its hoppers itself (feature/actors/Psi.cpp)
    static std::set<std::string, std::less<>> s{
        "levitate:andesite_funnel",
        "levitate:brass_funnel",
        "levitate:blaze_burner",
        "levitate:portable_storage_interface",
    };
    return s;
}

/// Bedrock FacingID offsets (0 down, 1 up, 2 north, 3 south, 4 west, 5 east).
BlockPos offset(BlockPos p, int face) {
    switch (face) {
    case 0: --p.y; break;
    case 1: ++p.y; break;
    case 2: --p.z; break;
    case 3: ++p.z; break;
    case 4: --p.x; break;
    case 5: ++p.x; break;
    default: break;
    }
    return p;
}

BlockPos blockAt(Vec3 const& v) {
    return {static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y)), static_cast<int>(std::floor(v.z))};
}

/// The item handler hoppers use at `pos`, if the block is one of ours.
std::optional<items::Handler> handlerAt(Access const& at) {
    if (!at.region.hasChunksAt(at.pos, 0, false)) return std::nullopt;
    if (!reaches(at.region.getBlock(at.pos).getTypeName())) return std::nullopt;
    return items::find(at);
}

/// NeoForge's VanillaInventoryCodeHooks.insertHook: one item of the first hopper slot the target
/// accepts. nullopt: not ours, the vanilla code runs.
std::optional<bool> push(BlockSource& region, Container& from, Vec3 const& hopper, int facing) {
    if (facing < 0 || facing > 5) return std::nullopt;
    Access at{region, offset(blockAt(hopper), facing), facing ^ 1};
    auto   h = handlerAt(at);
    if (!h) return std::nullopt;
    if (!h->insert) return false;
    for (int i = 0; i < from.getContainerSize(); ++i) {
        ItemStack const& item = from.getItem(i);
        if (items::isEmpty(item)) continue;
        auto one = items::withCount(item, 1);
        if (!items::isEmpty(h->insert(at, one, true))) continue;
        if (!items::isEmpty(h->insert(at, one, false))) continue;
        from.removeItem(i, 1);
        return true;
    }
    return false;
}

/// NeoForge's VanillaInventoryCodeHooks.extractHook: one item of the first slot of the block above
/// that the hopper has room for. nullopt: not ours, the vanilla code runs.
std::optional<bool> pull(BlockSource& region, Container& to, Vec3 const& hopper) {
    Access at{region, offset(blockAt(hopper), 1), 0};
    auto   h = handlerAt(at);
    if (!h) return std::nullopt;
    if (!h->slots || !h->getSlot || !h->extract) return false;
    int slots = h->slots(at);
    for (int i = 0; i < slots; ++i) {
        auto kind = h->getSlot(at, i);
        if (items::isEmpty(kind)) continue;
        if (!to.hasRoomForItem(items::withCount(kind, 1))) continue;
        auto same = [kind](ItemStack const& s) { return items::sameItemSameData(s, kind); };
        if (items::isEmpty(h->extract(at, same, 1, true))) continue;
        auto got = h->extract(at, same, 1, false);
        if (items::isEmpty(got)) continue;
        if (!to.addItem(got)) {
            if (h->insert) (void)h->insert(at, got, false); // no room after all: put it back
            continue;
        }
        return true;
    }
    return false;
}

} // namespace

void exclude(std::string blockId) { excluded().insert(std::move(blockId)); }

bool reaches(std::string_view blockId) { return blockId.starts_with("levitate:") && !excluded().contains(blockId); }

LL_TYPE_INSTANCE_HOOK(
    HopperPushOutHook,
    ll::memory::HookPriority::Normal,
    Hopper,
    &Hopper::_pushOutItems,
    bool,
    BlockSource&  region,
    Container&    fromContainer,
    Vec3 const&   position,
    int           attachedFace
) {
    if (!region.getLevel().isClientSide())
        if (auto r = push(region, fromContainer, position, attachedFace)) return *r;
    return origin(region, fromContainer, position, attachedFace);
}

LL_TYPE_INSTANCE_HOOK(
    HopperPullAboveHook,
    ll::memory::HookPriority::Normal,
    Hopper,
    &Hopper::_tryPullInItemsFromAboveContainer,
    bool,
    BlockSource& region,
    Container&   toContainer,
    Vec3 const&  pos
) {
    if (!region.getLevel().isClientSide())
        if (auto r = pull(region, toContainer, pos)) return *r;
    return origin(region, toContainer, pos);
}

class HoppersModule {
public:
    void install() {
        HopperPushOutHook::hook();
        HopperPullAboveHook::hook();
    }
    void uninstall() {
        HopperPullAboveHook::unhook();
        HopperPushOutHook::unhook();
    }
};

} // namespace lv::feature::common::hoppers

LV_REGISTER_MODULE_ORDERED(lv::feature::common::hoppers::HoppersModule, lv::ModuleOrder::Services);
