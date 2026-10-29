// Building unit: player actions. GameMode is where every player action ends up (network players
// and SimulatedPlayers alike), so the unit hooks it rather than the LeviLamina player events:
// - use on a block: block interactions first (doors, trapdoors, placards, copycats, girder +
//   shaft, copper + axe / honeycomb, slab merging, scaffolding and ladder placement helpers,
//   brackets), then the game's own placement, after which placed blocks are oriented as Create
//   orients them (getStateForPlacement);
// - start / finish breaking: placards give their item back when punched; door halves remember
//   whether a creative player broke them.

#include <cmath>
#include <unordered_map>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/building/Building.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::building {

namespace detail {
void placeDoor(Player& player, GlobalPos const& pos, Vec3 const& hitRel, double hitX, double hitZ);
}

namespace {

constexpr int kPlacementAssistRange = 12; // Create CEquipment.placementAssistRange

std::unordered_map<std::int64_t, std::uint64_t>& lastToggle() {
    static std::unordered_map<std::int64_t, std::uint64_t> m;
    return m;
}
std::uint64_t& now() {
    static std::uint64_t t = 0;
    return t;
}
/// Bedrock repeats a use while the button is held: toggles happen once per 5 ticks.
bool toggleCooldown(Player& player, GlobalPos const& pos) {
    auto key   = player.getOrCreateUniqueID().rawID * 31 + std::hash<std::string>{}(pos.key());
    auto& last = lastToggle()[key];
    if (now() - last < 5 && last != 0) return true;
    last = now();
    return false;
}

Kind kindAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    auto const* i = b ? infoOf(*b) : nullptr;
    return i ? i->kind : Kind::None;
}

Info const* itemInfo(ItemStack const& item) { return item.isNull() ? nullptr : infoOf(item.getTypeName()); }

/// Java SlabBlock.canBeReplaced when clicking the slab itself.
bool slabAccepts(Block const& slab, int face, float hitY) {
    if (stateOf(slab, "levitate:double")) return false;
    bool above = hitY > 0.5f;
    if (!stateOf(slab, "levitate:top")) return face == Up || (above && horizontal(face));
    return face == Down || (!above && horizontal(face));
}

bool mergeSlab(Player& player, GlobalPos const& p) {
    auto const* b = blockAt(p);
    if (!b) return false;
    set(p, *withStates(*b, {{"levitate:double", 1}}));
    consumeHeld(player);
    playSound(p, "dig.copper");
    log().info("building: {} at {} {} {} doubled", b->getTypeName(), p.x, p.y, p.z);
    return true;
}

/// Create's MetalScaffoldingBlockItem: a click on scaffolding places at the end of the column
/// (or 7 blocks sideways when clicking the top).
bool scaffoldHelper(Player& player, Info const& held, GlobalPos const& pos, int face) {
    int dir = face == Up ? horizontalFacing(player) : Up;
    auto* region = regionOf(pos.dim);
    if (!region) return true;
    GlobalPos cur = offset(pos, dir);
    for (int i = 0; i < 7;) {
        if (cur.y < region->getMinHeight() || cur.y >= region->getMaxHeight()) break;
        auto id = idAt(cur);
        if (id != held.id) {
            if (replaceableAt(cur)) {
                auto const* block = game::blockWith(held.id);
                if (!block) return true;
                set(cur, *block);
                consumeHeld(player);
                playSound(cur, "use.scaffolding");
                markDirty(cur);
                log().info("building: {} extended to {} {} {}", held.id, cur.x, cur.y, cur.z);
            }
            return true;
        }
        cur = offset(cur, dir);
        if (horizontal(dir)) ++i;
    }
    return true;
}

bool isLadder(Block const& b) {
    if (auto const* i = infoOf(b)) return i->kind == Kind::Ladder;
    return b.getTypeName() == "minecraft:ladder";
}

/// Create's MetalLadderBlock placement helper: extend the ladder up or down (by the look pitch).
bool ladderHelper(Player& player, Info const& held, GlobalPos const& pos) {
    auto const* clicked = blockAt(pos);
    if (!clicked) return false;
    int dir    = player.getRotation().x < 0 ? Up : Down;
    int facing = hfacingOf(*clicked);
    int count  = 0;
    for (GlobalPos q = offset(pos, dir);; q = offset(q, dir)) {
        auto const* b = blockAt(q);
        if (!b || !isLadder(*b)) break;
        if (++count > 256) break;
    }
    if (count >= kPlacementAssistRange) return false;
    auto target = offset(pos, dir, count + 1);
    if (!replaceableAt(target)) return false;
    // canSurvive at the target: a wall behind or the same ladder above
    auto const* above = blockAt(offset(target, Up));
    bool        held2 = sturdy(offset(target, opposite(facing)), facing)
                 || (above && above->getTypeName() == held.id && hfacingOf(*above) == facing);
    if (!held2) return false;
    auto const* block = game::blockWith(held.id, {{"levitate:facing", facing}});
    if (!block) return false;
    set(target, *block);
    consumeHeld(player);
    playSound(target, "use.ladder");
    log().info("building: {} extended to {} {} {}", held.id, target.x, target.y, target.z);
    return true;
}

bool girderShaft(Player& player, GlobalPos const& pos, int face) {
    auto const* b = blockAt(pos);
    if (!b) return false;
    int axis = (stateOf(*b, "levitate:x") || axisOf(face) == 2) ? 2 : 0;
    auto const* shaft = game::blockWith(kGirderShaftId, {{"levitate:axis", axis}, {"levitate:top", stateOf(*b, "levitate:top")}, {"levitate:bottom", stateOf(*b, "levitate:bottom")}});
    if (!shaft) return false;
    set(pos, *shaft);
    playSound(pos, "hit.netherite", 0.5f, 1.25f);
    consumeHeld(player);
    log().info("building: girder at {} {} {} encased a shaft (axis {})", pos.x, pos.y, pos.z, axis == 0 ? "x" : "z");
    return true;
}

} // namespace

bool onUse(Player& player, ItemStack const& item, GlobalPos const& pos, int face, Vec3 const& hit, bool firstEvent) {
    // a held button repeats the use: only the first event acts (SimulatedPlayers never send a first
    // event, a short per-block cooldown limits their repeats instead)
    if (!firstEvent && !player.isSimulated()) {
        auto k = kindAt(pos);
        if (k == Kind::Door || k == Kind::Trapdoor || k == Kind::Placard) return true;
    }
    bool build = mayBuild(player);
    bool  sneak  = player.isSneaking();
    bool  empty  = item.isNull();
    auto  kind   = kindAt(pos);
    auto const* held = itemInfo(item);
    std::string heldId = empty ? std::string() : item.getTypeName();

    switch (kind) {
    case Kind::Door:
    case Kind::Trapdoor:
        if (sneak && !empty) break;
        if (toggleCooldown(player, pos)) return true;
        return kind == Kind::Door ? useDoor(&player, pos) : useTrapdoor(&player, pos);
    case Kind::Placard:
        if (sneak) break;
        if (toggleCooldown(player, pos)) return true;
        return usePlacard(player, item, pos);
    case Kind::CopycatPanel:
    case Kind::CopycatStep:
        if (sneak || !build) break;
        if (useCopycat(player, item, pos)) return true;
        break;
    case Kind::Girder:
        if (heldId == kShaftId && build) return girderShaft(player, pos, face);
        break;
    case Kind::CopperBlock:
    case Kind::CopperStairs:
    case Kind::CopperSlab:
        if (build && (heldId == "minecraft:honeycomb" || heldId.ends_with("_axe"))) {
            if (toggleCooldown(player, pos)) return true;
            if (tryWaxOrScrape(player, item, pos)) return true;
        }
        if (kind == Kind::CopperSlab && build && held && held->id == idAt(pos)) {
            auto const* b = blockAt(pos);
            if (b && slabAccepts(*b, face, hit.y)) return mergeSlab(player, pos);
        }
        break;
    case Kind::Scaffold:
        if (build && held && held->kind == Kind::Scaffold && held->id == idAt(pos) && !sneak) return scaffoldHelper(player, *held, pos, face);
        break;
    case Kind::Ladder:
        if (build && held && held->kind == Kind::Ladder && !sneak && ladderHelper(player, *held, pos)) return true;
        break;
    default: break;
    }
    if (!build) return false;
    if (useBracket(player, item, pos, face)) return true;
    // a slab placed into the cell of a single slab of the same kind (Java: always merges)
    if (held && held->kind == Kind::CopperSlab && !replaceableAt(pos)) {
        auto next = offset(pos, face);
        auto const* b = blockAt(next);
        if (b && b->getTypeName() == held->id && !stateOf(*b, "levitate:double")) return mergeSlab(player, next);
    }
    return false;
}

void onPlaced(Player& player, GlobalPos const& pos, int face, Vec3 const& hit) {
    auto const* block = blockAt(pos);
    auto const* info  = block ? infoOf(*block) : nullptr;
    if (!info) return;
    float hitY = hit.y;
    auto  refund = [&] {
        clear(pos);
        if (!player.isCreative()) give(player, makeItem(info->id));
    };
    switch (info->kind) {
    case Kind::Ladder: {
        // Java LadderBlock.getStateForPlacement: the clicked side, else the nearest look directions
        std::vector<int> candidates;
        if (horizontal(face)) candidates.push_back(face);
        for (int d : nearestLooking(player))
            if (horizontal(d)) candidates.push_back(opposite(d));
        for (int f : candidates) {
            if (sturdy(offset(pos, opposite(f)), f)) {
                set(pos, *withStates(*block, {{"levitate:facing", f}}));
                return;
            }
        }
        refund();
        return;
    }
    case Kind::Door: detail::placeDoor(player, pos, hit, hit.x, hit.z); return;
    case Kind::Trapdoor: {
        int  facing = horizontal(face) ? face : opposite(horizontalFacing(player));
        bool top    = horizontal(face) ? hitY > 0.5f : face == Down;
        bool power  = hasNeighborSignal(pos);
        set(pos, *withStates(*block, {{"levitate:facing", facing}, {"levitate:top", top}, {"levitate:open", power}, {"levitate:powered", power}}));
        trackRedstone(pos);
        return;
    }
    case Kind::CopperStairs: {
        bool upside = face == Down || (face != Up && hitY > 0.5f);
        set(pos, *withStates(*block, {{"levitate:facing", horizontalFacing(player)}, {"levitate:upside", upside}}));
        updateConnections(pos);
        return;
    }
    case Kind::CopperSlab: {
        bool top = face == Down || (face != Up && hitY > 0.5f);
        set(pos, *withStates(*block, {{"levitate:top", top}}));
        return;
    }
    case Kind::Placard: {
        // FaceAttachedHorizontalDirectionalBlock: the first look direction it can hang on
        for (int d : nearestLooking(player)) {
            int facing = opposite(d);
            if (!canHang(offset(pos, d))) continue;
            set(pos, *withStates(*block, {{"levitate:facing", facing}, {"levitate:powered", 0}}));
            return;
        }
        refund();
        return;
    }
    case Kind::CopycatPanel:
        set(pos, *withStates(*block, {{"levitate:facing", opposite(nearestLooking(player)[0])}}));
        return;
    case Kind::CopycatStep: {
        bool top = face == Down || (face != Up && hitY > 0.5f);
        set(pos, *withStates(*block, {{"levitate:facing", horizontalFacing(player)}, {"levitate:top", top}}));
        return;
    }
    case Kind::Girder: {
        int axis = axisOf(face);
        set(pos, *withStates(*block, {{"levitate:axis", axis}, {"levitate:x", axis == 0}, {"levitate:z", axis == 2}}));
        updateConnections(pos);
        return;
    }
    default: markDirty(pos); return;
    }
}

bool onAttack(Player& player, GlobalPos const& pos) {
    if (kindAt(pos) == Kind::Placard) return attackPlacard(player, pos);
    return false;
}

namespace {

bool isServerPlayer(Player const& p) { return !p.isClientSide(); }

Vec3 relativeHit(Vec3 const& hit, BlockPos const& at) {
    bool rel = hit.x >= -0.01f && hit.x <= 1.01f && hit.y >= -0.01f && hit.y <= 1.01f && hit.z >= -0.01f && hit.z <= 1.01f;
    if (rel) return hit;
    return Vec3{hit.x - at.x, hit.y - at.y, hit.z - at.z};
}

GlobalPos placementPos(GlobalPos const& clicked, int face) {
    if (replaceableAt(clicked)) return clicked;
    return face < 6 ? offset(clicked, face) : clicked;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    BuildingUseItemOnHook,
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
    GlobalPos pos{static_cast<int>(player.getDimensionId()), at.x, at.y, at.z};
    Vec3      rel = relativeHit(hit, at);
    if (onUse(player, item, pos, face, rel, isFirstEvent)) {
        InteractionResult r{};
        r.mSuccess = true;
        r.mSwing   = true;
        player.sendInventory(false); // undo the client's predicted placement
        return r;
    }
    auto const* held = item.isNull() ? nullptr : infoOf(item.getTypeName());
    if (!held) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto target = placementPos(pos, face);
    auto before = idAt(target);
    auto result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    if (before != held->id && idAt(target) == held->id) {
        // Java measures the click from the placed block
        Vec3 placedHit{at.x + rel.x - target.x, at.y + rel.y - target.y, at.z + rel.z - target.z};
        try {
            onPlaced(player, target, face, placedHit);
        } catch (std::exception const& e) {
            log().error("building: placing {} failed: {}", held->id, e.what());
        }
    }
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    BuildingStartDestroyHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$startDestroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face,
    bool&           hasDestroyedBlock
) {
    Player& player = mPlayer;
    if (isServerPlayer(player)) {
        GlobalPos p{static_cast<int>(player.getDimensionId()), pos.x, pos.y, pos.z};
        if (onAttack(player, p)) {
            hasDestroyedBlock = false;
            return false;
        }
    }
    return origin(pos, face, hasDestroyedBlock);
}

LL_TYPE_INSTANCE_HOOK(
    BuildingDestroyHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$destroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face
) {
    Player& player = mPlayer;
    if (isServerPlayer(player)) {
        GlobalPos p{static_cast<int>(player.getDimensionId()), pos.x, pos.y, pos.z};
        // creative players break instantly: punching a placard with an item only takes the item
        if (player.isCreative() && placardItem(p) && onAttack(player, p)) return false;
        notePlayerBreak(p, player.isCreative());
    }
    return origin(pos, face);
}

void installHooks() {
    BuildingUseItemOnHook::hook();
    BuildingStartDestroyHook::hook();
    BuildingDestroyHook::hook();
}

void uninstallHooks() {
    BuildingDestroyHook::unhook();
    BuildingStartDestroyHook::unhook();
    BuildingUseItemOnHook::unhook();
}

namespace detail {
void advanceClock() { ++now(); }
} // namespace detail

} // namespace lv::feature::building
