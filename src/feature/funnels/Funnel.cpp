// Andesite and brass funnels (Create's FunnelBlock / FunnelBlockEntity, SPEC §3.6 U12).
//
// A funnel is attached to the block behind it (opposite of `levitate:facing`). Extracting
// funnels pull items out of that inventory every `defaultExtractionTimer` ticks and drop them in
// front; inserting funnels put item entities touching their opening (and items handed to an
// upward funnel by belts / chutes / players) into it. Redstone power pauses both. Brass funnels
// filter and extract up to a configurable amount; andesite funnels move single items.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::funnels::detail {

namespace items = common::items;

namespace {

struct Runtime {
    int          cooldown     = 0;
    std::int64_t lastObserved = 0; ///< item entity that may still block the output
    int          flapTicks    = 0;
};
std::unordered_map<std::string, Runtime>& runtimes() {
    static std::unordered_map<std::string, Runtime> m;
    return m;
}
Runtime& runtime(GlobalPos const& pos) { return runtimes()[pos.key()]; }

bool isBrass(Block const& block) { return block.getTypeName() == kBrassFunnel; }

int facingOf(Block const& block) { return std::clamp(state(block, kFacing, cf::kUp), 0, 5); }

/// Create's FunnelFilterSlotPositioning point for the SOUTH side, turned to `side`.
std::array<double, 3> turned(int side, double x, double y, double z) {
    switch (side) {
    case cf::kNorth: return {1 - x, y, 1 - z};
    case cf::kEast: return {z, y, 1 - x};
    case cf::kWest: return {1 - z, y, x};
    default: return {x, y, z};
    }
}

std::optional<std::array<double, 3>> filterCentre(Block const& block, int face) {
    int facing = facingOf(block);
    if (cf::isHorizontal(facing)) {
        if (face != facing) return std::nullopt;
        return turned(facing, 8 / 16.0, 12.2 / 16.0, 8.55 / 16.0);
    }
    if (!cf::isHorizontal(face)) return std::nullopt;
    return turned(face, 8 / 16.0, (facing == cf::kDown ? 14 : 2) / 16.0, 15.5 / 16.0);
}

/// The "Extracted Amount" box: Create edits the count by holding the filter slot; here it is a
/// separate box next to it (SPEC §8).
std::optional<std::array<double, 3>> amountCentre(Block const& block, int face) {
    if (!state(block, kExtracting)) return std::nullopt;
    int facing = facingOf(block);
    if (cf::isHorizontal(facing)) {
        if (face != facing) return std::nullopt;
        return turned(facing, 8 / 16.0, 5 / 16.0, 8.55 / 16.0);
    }
    if (!cf::isHorizontal(face)) return std::nullopt;
    return turned(face, 8 / 16.0, (facing == cf::kDown ? 9 : 7) / 16.0, 15.5 / 16.0);
}

bool passesFilter(Block const& block, GlobalPos const& pos, ItemStack const& stack) {
    return !isBrass(block) || common::filterslot::matches(pos, stack, "filter");
}

void flap(BlockSource& region, GlobalPos const& pos, Block const& block) {
    if (!cf::isHorizontal(facingOf(block))) return;
    auto& rt     = runtime(pos);
    rt.flapTicks = 4;
    setStates(region, toBlockPos(pos), {{kFlap, 1}});
    // Create's funnel flap sound: an item frame turn and a muffled wool break
    playSound(region, toBlockPos(pos), "block.itemframe.rotate_item", .125f, 1.5f);
    playSound(region, toBlockPos(pos), "dig.cloth", .0425f, .75f);
}

/// The funnel in front of an inventory: the block it is attached to is behind it.
BlockPos attachedPos(GlobalPos const& pos, int facing) { return offset(toBlockPos(pos), cf::oppositeFace(facing)); }

/// Create's FunnelBlockEntity.getEntityOverflowScanningArea.
std::pair<Vec3, Vec3> scanArea(GlobalPos const& pos, int facing) {
    Vec3 min{float(pos.x), float(pos.y), float(pos.z)};
    Vec3 max{pos.x + 1.f, pos.y + 1.f, pos.z + 1.f};
    if (facing != cf::kUp) min.y -= 1.f;
    return {min, max};
}

bool observedStillBlocking(BlockSource& region, Runtime& rt, GlobalPos const& pos, int facing) {
    if (rt.lastObserved == 0) return false;
    auto level = ll::service::getLevel();
    if (!level) return false;
    ActorUniqueID id;
    id.rawID     = rt.lastObserved;
    auto* entity = level->fetchEntity(id, false);
    if (entity && entity->isAlive()) {
        auto [min, max] = scanArea(pos, facing);
        auto const& p   = entity->getPosition();
        if (p.x >= min.x - .25f && p.x <= max.x + .25f && p.y >= min.y - .25f && p.y <= max.y + .25f && p.z >= min.z - .25f
            && p.z <= max.z + .25f)
            return true;
    }
    rt.lastObserved = 0;
    (void)region;
    return false;
}

/// Create's FunnelBlockEntity.activateExtractor.
void activateExtractor(BlockSource& region, GlobalPos const& pos, Block const& block) {
    auto& rt     = runtime(pos);
    int   facing = facingOf(block);
    if (observedStillBlocking(region, rt, pos, facing)) return;

    bool                brass  = isBrass(block);
    cf::AmountSetting   amount = brass ? amountOf(pos) : cf::AmountSetting{1, true};
    auto                inv    = attachedPos(pos, facing);
    auto                filter = [&](ItemStack const& s) { return passesFilter(block, pos, s); };
    auto stack = extractCounted(region, inv, facing, filter, amount.count, !amount.upTo, true);
    if (items::isEmpty(stack)) return;

    // only scan for blocking entities when there is something to extract
    auto [min, max] = scanArea(pos, facing);
    auto blocking   = itemsIn(region, min, max);
    if (!blocking.empty()) {
        rt.lastObserved = blocking.front()->getOrCreateUniqueID().rawID;
        return;
    }
    stack = extractCounted(region, inv, facing, filter, amount.count, !amount.upTo, false);
    if (items::isEmpty(stack)) return;
    flap(region, pos, block);

    bool  vertical = !cf::isHorizontal(facing);
    bool  up       = facing == cf::kUp;
    auto  n        = cf::faceOffset(facing);
    float scale    = vertical ? (up ? .15f : .5f) : .25f;
    Vec3  out{pos.x + .5f + n[0] * scale, pos.y + .5f + n[1] * scale, pos.z + .5f + n[2] * scale};
    if (!vertical) out.y -= .45f;
    Vec3 motion{0, up ? 4 / 16.f : 0.f, 0};
    if (auto* item = spawnItem(region, stack, out, motion)) rt.lastObserved = item->getOrCreateUniqueID().rawID;
    rt.cooldown = std::max(1, config().defaultExtractionTimer);
}

/// Create's FunnelBlock.entityInside for item entities at the funnel's opening.
void collect(BlockSource& region, GlobalPos const& pos, Block const& block) {
    int  facing = facingOf(block);
    Vec3 min{pos.x - .05f, pos.y - .05f, pos.z - .05f};
    Vec3 max{pos.x + 1.05f, pos.y + 1.05f, pos.z + 1.05f};
    auto n = cf::faceOffset(facing);
    for (auto* entity : itemsIn(region, min, max)) {
        auto& stack = entity->item();
        if (items::isEmpty(stack)) continue;
        Vec3  open{pos.x + .5f - n[0] * .25f, pos.y + .5f - n[1] * .25f, pos.z + .5f - n[2] * .25f};
        auto  p    = entity->getPosition();
        float diff = n[0] != 0 ? p.x - open.x : (n[1] != 0 ? p.y - open.y : p.z - open.z);
        bool  positive = cf::isPositive(facing);
        if ((diff < 0) == positive) continue;
        auto rest = funnelInsert(region, pos, stack, false);
        if (items::isEmpty(rest)) {
            entity->remove();
        } else if (items::countOf(rest) < items::countOf(stack)) {
            entity->item() = rest;
        }
    }
}

common::items::Handler capability() {
    // Create's DirectBeltInputBehaviour of an upward, inserting funnel: belts, chutes and
    // other machines hand items to it and it puts them into the inventory below.
    common::items::Handler h;
    h.slots   = [](common::Access const&) { return 0; };
    h.getSlot = [](common::Access const&, int) { return items::empty(); };
    h.insert  = [](common::Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        auto const& block = at.region.getBlock(at.pos);
        if (facingOf(block) != cf::kUp || state(block, kExtracting) || state(block, kPowered)) return stack;
        return funnelInsert(at.region, game::globalPos(at.region, at.pos), stack, simulate);
    };
    h.extract = [](common::Access const&, common::items::ItemPredicate const&, int, bool) { return items::empty(); };
    return h;
}

} // namespace

ItemStack funnelInsert(BlockSource& region, GlobalPos const& pos, ItemStack const& stack, bool simulate) {
    auto const* block = loadedBlock(region, toBlockPos(pos));
    if (!block || !isFunnel(kindOf(block->getTypeName())) || items::isEmpty(stack)) return stack;
    if (!passesFilter(*block, pos, stack)) return stack;
    int  facing = facingOf(*block);
    auto rest   = items::insert(region, attachedPos(pos, facing), facing, stack, simulate);
    if (!simulate && items::countOf(rest) != items::countOf(stack)) flap(region, pos, *block);
    return rest;
}

void tickFunnel(BlockSource& region, GlobalPos const& pos, Block const& blockIn, std::uint64_t tick) {
    auto&        rt    = runtime(pos);
    auto         bp    = toBlockPos(pos);
    Block const* block = &blockIn;
    // redstone (Create schedules a tick on neighbour changes; polled here, SPEC §8)
    if ((tick + static_cast<std::uint64_t>(pos.x + pos.z)) % 2 == 0) {
        bool p = powered(region, bp);
        if (p != (state(*block, kPowered) != 0)) {
            setStates(region, bp, {{kPowered, p ? 1 : 0}});
            block = loadedBlock(region, bp);
            if (!block) return;
            log().info("funnels: funnel at {} {}", pos.key(), p ? "powered" : "unpowered");
        }
    }
    if (rt.flapTicks > 0 && --rt.flapTicks == 0) {
        setStates(region, bp, {{kFlap, 0}});
        block = loadedBlock(region, bp);
        if (!block) return;
    }
    if (state(*block, kPowered)) {
        rt.cooldown = 0; // redstone resets the extraction cooldown
        return;
    }
    if (!state(*block, kExtracting)) {
        collect(region, pos, *block);
        return;
    }
    if (rt.cooldown > 0) {
        --rt.cooldown;
        return;
    }
    activateExtractor(region, pos, *block);
}

bool useFunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, int face, ItemStack& held) {
    int facing = facingOf(block);
    if (face != facing) return false;
    if (state(block, kExtracting) || state(block, kPowered)) return false;
    if (!items::isEmpty(held) && items::idOf(held) == kMechanicalArm) return false;
    if (items::isEmpty(held)) return true;
    ItemStack toInsert = held;
    auto      rest     = funnelInsert(region, pos, toInsert, false);
    if (items::countOf(rest) != items::countOf(toInsert)) {
        held = rest; // the stack in the hand that used the funnel
        player.sendInventory(false);
        log().info("funnels: {} inserted {} x{} through the funnel at {}", player.getRealName(), items::idOf(toInsert),
            items::countOf(toInsert) - items::countOf(rest), pos.key());
    }
    return true;
}

bool wrenchFunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block) {
    int next = state(block, kExtracting) ? 0 : 1;
    setStates(region, toBlockPos(pos), {{kExtracting, next}});
    playSound(region, toBlockPos(pos), "random.click", .5f, 1.2f);
    log().info("funnels: funnel at {} set to {} by {}", pos.key(), next ? "extract" : "insert", player.getRealName());
    return true;
}

void placeFunnel(Player* player, BlockSource& region, GlobalPos const& pos, bool sneaking, Vec3 const& look) {
    (void)player;
    // Create: facing towards the player along the nearest looking directions, the first one that
    // isn't attached to another funnel; extracting unless sneaking
    std::array<int, 3> axes{0, 1, 2};
    double             comp[3]{look.x, look.y, look.z};
    std::stable_sort(axes.begin(), axes.end(), [&](int a, int b) { return std::abs(comp[a]) > std::abs(comp[b]); });
    auto dirOf = [&](int axis) {
        bool pos = comp[axis] >= 0;
        if (axis == 0) return pos ? cf::kEast : cf::kWest;
        if (axis == 1) return pos ? cf::kUp : cf::kDown;
        return pos ? cf::kSouth : cf::kNorth;
    };
    std::array<int, 6> nearest{dirOf(axes[0]), dirOf(axes[1]), dirOf(axes[2]), cf::oppositeFace(dirOf(axes[2])),
        cf::oppositeFace(dirOf(axes[1])), cf::oppositeFace(dirOf(axes[0]))};
    int facing = cf::oppositeFace(nearest[0]);
    for (int dir : nearest) {
        int f = cf::oppositeFace(dir);
        if (!isFunnel(kindAt(region, attachedPos(pos, f)))) {
            facing = f;
            break;
        }
    }
    auto bp = toBlockPos(pos);
    setStates(region, bp, {{kFacing, facing}, {kExtracting, sneaking ? 0 : 1}, {kPowered, powered(region, bp) ? 1 : 0}});
}

void forgetFunnel(GlobalPos const& pos) { runtimes().erase(pos.key()); }

void installFunnels() {
    for (char const* id : {kAndesiteFunnel, kBrassFunnel}) common::items::registerBlock(id, capability());
    common::filterslot::registerSlot(kBrassFunnel, {"filter", filterCentre, 0.5, nullptr});
    registerAmountBox(kBrassFunnel, amountCentre);
}

} // namespace lv::feature::funnels::detail
