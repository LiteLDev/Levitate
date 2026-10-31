// Andesite and brass funnels (Create's FunnelBlock / BeltFunnelBlock / FunnelBlockEntity, SPEC
// §3.6.12).
//
// A funnel is attached to the block behind it (opposite of `levitate:facing`). Extracting
// funnels pull items out of that inventory every `defaultExtractionTimer` ticks and drop them in
// front; inserting funnels put item entities touching their opening (and items handed to an
// upward funnel by belts / chutes / players) into it. Redstone power pauses both. Brass funnels
// filter and extract up to a configurable amount; andesite funnels move single items.
//
// Belt funnels (Create's BeltFunnelBlock) are funnels with a `levitate:shape` (1 retracted,
// 2 extended, 3 pushing, 4 pulling): a horizontal funnel on top of a belt / depot / ejector. A
// pushing one (or a retracted / extended one at the end of a belt moving away from it) puts items
// from its inventory onto the belt; a pulling one (or one the belt moves towards) takes the belt
// items that pass under it (belts::registerAboveInteractor, Create's BeltFunnelInteractionHandler).

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/level/block/Block.h"

#include "feature/advancements/Advancements.h"
#include "feature/belts/BeltApi.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/contraption/Contraption.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/funnels/FunnelsInternal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
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
/// A new world (the client loads several in one process) starts without cached state.
struct Lifecycle {
    Lifecycle() {
        game::db::onWorldClosed([] { runtimes().clear(); });
    }
} const gLifecycle;

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
    int shape  = beltShapeOf(block);
    if (shape == kBeltExtended) {
        // the extended belt funnel shows its slot on top (Create: isSideActive UP)
        if (face != cf::kUp || !cf::isHorizontal(facing)) return std::nullopt;
        return turned(facing, 8 / 16.0, 11.3 / 16.0, 12.5 / 16.0);
    }
    if (cf::isHorizontal(facing)) {
        if (face != facing) return std::nullopt;
        // belt funnels sit 3 px lower, on the belt
        return turned(facing, 8 / 16.0, (shape != kNotOnBelt ? 9.2 : 12.2) / 16.0, 8.55 / 16.0);
    }
    if (!cf::isHorizontal(face)) return std::nullopt;
    return turned(face, 8 / 16.0, (facing == cf::kDown ? 14 : 2) / 16.0, 15.5 / 16.0);
}

/// The "Extracted Amount" box: Create edits the count by holding the filter slot; here it is a
/// separate box next to it (SPEC §8). Create shows the count on extracting funnels and on belt
/// funnels (supportsAmountOnFilter).
std::optional<std::array<double, 3>> amountCentre(Block const& block, int face) {
    int shape = beltShapeOf(block);
    if (shape == kNotOnBelt && !state(block, kExtracting)) return std::nullopt;
    int facing = facingOf(block);
    if (shape == kBeltExtended) {
        if (face != cf::kUp || !cf::isHorizontal(facing)) return std::nullopt;
        return turned(facing, 8 / 16.0, 11.3 / 16.0, 6.5 / 16.0);
    }
    if (cf::isHorizontal(facing)) {
        if (face != facing) return std::nullopt;
        return turned(facing, 8 / 16.0, (shape != kNotOnBelt ? 3 : 5) / 16.0, 8.55 / 16.0);
    }
    if (!cf::isHorizontal(face)) return std::nullopt;
    return turned(face, 8 / 16.0, (facing == cf::kDown ? 9 : 7) / 16.0, 15.5 / 16.0);
}

bool passesFilter(Block const& block, GlobalPos const& pos, ItemStack const& stack) {
    return !isBrass(block) || common::filterslot::matches(pos, stack, "filter");
}

/// Create's FunnelBlockEntity.flap: the flap state for 4 ticks (every client sees it), the flap
/// sound, and a flap event for the client mod (which swings the flap smoothly, Create's
/// FunnelRenderer). `inward` = items went into the funnel.
void flap(BlockSource& region, GlobalPos const& pos, Block const& block, bool inward) {
    if (!cf::isHorizontal(facingOf(block))) return;
    auto& rt     = runtime(pos);
    rt.flapTicks = 4;
    setStates(region, toBlockPos(pos), {{kFlap, 1}});
    // Create's funnel flap sound: an item frame turn and a muffled wool break
    playSound(region, toBlockPos(pos), "block.itemframe.rotate_item", .125f, 1.5f);
    playSound(region, toBlockPos(pos), "dig.cloth", .0425f, .75f);
    if (auto level = ll::service::getLevel()) {
        net::Json msg{{"dim", pos.dim}, {"pos", {pos.x, pos.y, pos.z}}, {"inward", inward}};
        level->forEachPlayer([&](Player& p) {
            if (static_cast<int>(p.getDimensionId()) != pos.dim || !net::isCompanion(p)) return true;
            auto const& at = p.getPosition();
            float       dx = at.x - pos.x, dy = at.y - pos.y, dz = at.z - pos.z;
            if (dx * dx + dy * dy + dz * dz > 96.f * 96.f) return true;
            net::send(p, "funnels.flap", msg);
            return true;
        });
    }
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
    flap(region, pos, block, false);

    bool  vertical = !cf::isHorizontal(facing);
    bool  up       = facing == cf::kUp;
    auto  n        = cf::faceOffset(facing);
    float scale    = vertical ? (up ? .15f : .5f) : .25f;
    Vec3  out{pos.x + .5f + n[0] * scale, pos.y + .5f + n[1] * scale, pos.z + .5f + n[2] * scale};
    if (!vertical) out.y -= .45f;
    Vec3 motion{0, up ? 4 / 16.f : 0.f, 0};
    if (auto* item = spawnItem(region, stack, out, motion)) rt.lastObserved = item->getOrCreateUniqueID().rawID;
    rt.cooldown = std::max(1, config().defaultExtractionTimer);
    advancements::awardOwner(pos, "funnel"); // Create's onTransfer
}

/// Create's FunnelBlockEntity.activateExtractingBeltFunnel: items from the inventory onto the
/// belt / depot below, inserted from the funnel's side.
void activateExtractingBeltFunnel(BlockSource& region, GlobalPos const& pos, Block const& block) {
    auto& rt     = runtime(pos);
    int   facing = facingOf(block);
    auto  below  = offset(pos, cf::kDown);
    if (!belts::canInsertDirect(below, facing)) return;
    // Create's isOccupied: a probe stack the input refuses
    belts::TransportedItem probe;
    bool                   brass  = isBrass(block);
    cf::AmountSetting      amount = brass ? amountOf(pos) : cf::AmountSetting{1, true};
    auto                   accept = [&](ItemStack const& s) {
        if (!passesFilter(block, pos, s)) return false;
        probe.stack = s;
        return items::isEmpty(belts::insertDirect(below, probe, facing, true));
    };
    auto stack = extractCounted(region, attachedPos(pos, facing), facing, accept, amount.count, !amount.upTo, false);
    if (items::isEmpty(stack)) return;
    flap(region, pos, block, false);
    belts::TransportedItem t;
    t.stack   = stack;
    auto rest = belts::insertDirect(below, t, facing, false);
    if (!items::isEmpty(rest)) {
        // the input changed its mind between the probe and the insertion: back into the inventory
        auto back = items::insert(region, attachedPos(pos, facing), facing, rest, false);
        if (!items::isEmpty(back)) spawnItem(region, back, Vec3{pos.x + .5f, pos.y + .5f, pos.z + .5f}, Vec3{0, 0, 0});
    }
    rt.cooldown = std::max(1, config().defaultExtractionTimer);
    advancements::awardOwner(pos, "funnel"); // Create's onTransfer
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

/// The horizontal FacingID of a belt's `levitate:facing` (0 south, 1 west, 2 north, 3 east).
int beltFace(int beltFacing) {
    static constexpr int kFaces[] = {cf::kSouth, cf::kWest, cf::kNorth, cf::kEast};
    return kFaces[beltFacing & 3];
}

enum class Mode { Invalid, Paused, Collect, Extract, PushingToBelt, TakingFromBelt };

/// Create's FunnelBlockEntity.determineCurrentMode.
Mode modeOf(GlobalPos const& pos, Block const& block) {
    if (state(block, kPowered)) return Mode::Paused;
    int shape = beltShapeOf(block);
    if (shape == kNotOnBelt) return state(block, kExtracting) ? Mode::Extract : Mode::Collect;
    if (shape == kBeltPulling) return Mode::TakingFromBelt;
    if (shape == kBeltPushing) return Mode::PushingToBelt;
    auto belt = belts::beltAt(offset(pos, cf::kDown));
    if (!belt) return Mode::Invalid;
    return belt->movementFace == facingOf(block) ? Mode::PushingToBelt : Mode::TakingFromBelt;
}

/// Create's BeltFunnelInteractionHandler.checkForFunnels for one segment (`c.segment`), and
/// DirectBeltInputBehaviour.tryExportingToBeltFunnel for a depot / ejector below.
belts::PassResult onBeltPass(belts::PassContext& c) {
    auto* region = regionOf(c.above.dim);
    if (!region) return belts::PassResult::Pass;
    auto const* block = loadedBlock(*region, toBlockPos(c.above));
    if (!block || !isFunnel(kindOf(block->getTypeName()))) return belts::PassResult::Pass;
    int shape  = beltShapeOf(*block);
    int facing = facingOf(*block);
    if (shape == kNotOnBelt || !cf::isHorizontal(facing)) return belts::PassResult::Pass;

    if (c.depot) {
        if (shape != kBeltPulling || state(*block, kPowered) || items::isEmpty(c.item.stack)) return belts::PassResult::Pass;
        // an ejector keeps funnels from pulling against its launch direction (canFunnelsPullFrom)
        if (auto const* below = loadedBlock(*region, toBlockPos(c.segment)); below && below->getTypeName() == belts::kEjectorId) {
            if (cf::oppositeFace(facing) == beltFace(state(*below, kFacing))) return belts::PassResult::Pass;
        }
        c.item.stack = funnelInsert(*region, c.above, c.item.stack, false);
        return belts::PassResult::Pass;
    }

    int  movement = c.movementFace;
    bool blocking = facing == cf::oppositeFace(movement);
    if (facing == movement || shape == kBeltPushing) return belts::PassResult::Pass;
    float entry = float(c.segmentIndex) + .5f;
    if (shape == kBeltExtended) entry += .499f * (c.movementPositive ? -1.f : 1.f);
    bool crossed = c.movementPositive ? c.nextOffset > entry : c.nextOffset < entry;
    if (!crossed) return belts::PassResult::Pass;
    if (blocking) c.item.beltPosition = entry;
    auto const stop = blocking ? belts::PassResult::Stop : belts::PassResult::Pass;
    if (state(*block, kPowered) || items::isEmpty(c.item.stack)) return stop;
    if (!passesFilter(*block, c.above, c.item.stack)) return stop;

    // Create's getAmountToExtract / getModeToExtract (a belt funnel supports the amount)
    bool              brass  = isBrass(*block);
    cf::AmountSetting amount = brass ? amountOf(c.above) : cf::AmountSetting{1, true};
    ItemStack         toInsert = c.item.stack;
    int               have     = items::countOf(toInsert);
    if (!amount.upTo) {
        if (amount.count > have) return stop;
        toInsert = items::withCount(toInsert, std::min(amount.count, have));
        if (!items::isEmpty(items::insert(*region, attachedPos(c.above, facing), facing, toInsert, true))) return stop;
    }
    auto remainder = items::insert(*region, attachedPos(c.above, facing), facing, toInsert, false);
    if (items::countOf(remainder) == items::countOf(toInsert) && items::sameItemSameData(remainder, toInsert)) return stop;
    int notFilled = have - items::countOf(toInsert);
    if (!items::isEmpty(remainder)) remainder = items::withCount(remainder, items::countOf(remainder) + notFilled);
    else if (notFilled > 0) remainder = items::withCount(c.item.stack, notFilled);
    flap(*region, c.above, *block, true);
    advancements::awardOwner(c.above, "funnel"); // Create's onTransfer
    c.item.stack = items::isEmpty(remainder) ? items::empty() : remainder;
    return stop;
}

} // namespace

int beltShapeOf(Block const& block) { return std::clamp(state(block, kShape, kNotOnBelt), 0, 4); }

bool onValidBelt(BlockSource& region, GlobalPos const& pos) {
    (void)region;
    return belts::isTransporter(offset(pos, cf::kDown));
}

int beltShapeFor(BlockSource& region, GlobalPos const& pos, int facing, bool extracting) {
    (void)region;
    int  preferred = extracting ? kBeltPushing : kBeltPulling;
    auto belt      = belts::beltAt(offset(pos, cf::kDown));
    if (!belt) return preferred;
    return cf::axisOf(beltFace(belt->facing)) != cf::axisOf(facing) ? preferred : kBeltRetracted;
}

void updateFunnelShape(BlockSource& region, GlobalPos const& pos) {
    auto        bp    = toBlockPos(pos);
    auto const* block = loadedBlock(region, bp);
    if (!block || !isFunnel(kindOf(block->getTypeName()))) return;
    int facing = facingOf(*block);
    if (!cf::isHorizontal(facing)) return;
    int  shape = beltShapeOf(*block);
    bool valid = onValidBelt(region, pos);
    if (shape == kNotOnBelt) {
        // Create's FunnelBlock.updateShape: a funnel that gets a belt below becomes a belt funnel
        if (!valid) return;
        int next = beltShapeFor(region, pos, facing, state(*block, kExtracting) != 0);
        setStates(region, bp, {{kShape, next}});
        log().info("funnels: funnel at {} is now on a belt (shape {})", pos.key(), next);
        return;
    }
    if (!valid) {
        // Create's BeltFunnelBlock.updateShape: back to a plain funnel
        setStates(region, bp, {{kShape, kNotOnBelt}, {kExtracting, shape == kBeltPushing ? 1 : 0}});
        log().info("funnels: funnel at {} left the belt", pos.key());
        return;
    }
    int updated = beltShapeFor(region, pos, facing, shape == kBeltPushing);
    if (updated == shape) return;
    // don't revert wrenched states
    if (updated == kBeltPushing && shape == kBeltPulling) return;
    if (updated == kBeltRetracted && shape == kBeltExtended) return;
    setStates(region, bp, {{kShape, updated}});
}

ItemStack funnelInsert(BlockSource& region, GlobalPos const& pos, ItemStack const& stack, bool simulate) {
    auto const* block = loadedBlock(region, toBlockPos(pos));
    if (!block || !isFunnel(kindOf(block->getTypeName())) || items::isEmpty(stack)) return stack;
    if (!passesFilter(*block, pos, stack)) return stack;
    int  facing = facingOf(*block);
    auto rest   = items::insert(region, attachedPos(pos, facing), facing, stack, simulate);
    if (!simulate && items::countOf(rest) != items::countOf(stack)) {
        flap(region, pos, *block, true);
        advancements::awardOwner(pos, "funnel"); // Create's onTransfer
    }
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
    Mode mode = modeOf(pos, *block);
    if (beltShapeOf(*block) != kNotOnBelt && (mode == Mode::PushingToBelt || mode == Mode::TakingFromBelt)) {
        // the back plate shows the direction of a belt funnel (retracted / extended ones turn
        // with the belt)
        int extracting = mode == Mode::PushingToBelt ? 1 : 0;
        if (state(*block, kExtracting) != extracting) {
            setStates(region, bp, {{kExtracting, extracting}});
            block = loadedBlock(region, bp);
            if (!block) return;
        }
    }
    switch (mode) {
    case Mode::Paused: rt.cooldown = 0; return; // redstone resets the extraction cooldown
    case Mode::Invalid:
    case Mode::TakingFromBelt: return;          // the belt hands its items over (onBeltPass)
    case Mode::Collect: collect(region, pos, *block); return;
    default: break;
    }
    if (rt.cooldown > 0) {
        --rt.cooldown;
        return;
    }
    if (mode == Mode::PushingToBelt) activateExtractingBeltFunnel(region, pos, *block);
    else activateExtractor(region, pos, *block);
}

bool useFunnel(Player& player, BlockSource& region, GlobalPos const& pos, Block const& block, int face, ItemStack& held) {
    int facing = facingOf(block);
    if (face != facing) return false;
    // Create's canInsertIntoFunnel: plain inserting funnels only (belt funnels belong to the belt)
    if (beltShapeOf(block) != kNotOnBelt) return false;
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
    int shape = beltShapeOf(block);
    if (shape != kNotOnBelt) {
        // Create's BeltFunnelBlock.onWrenched: pulling <-> pushing, extended <-> retracted
        int next = shape;
        if (shape == kBeltPulling) next = kBeltPushing;
        else if (shape == kBeltPushing) next = kBeltPulling;
        else if (shape == kBeltExtended) next = kBeltRetracted;
        else if (shape == kBeltRetracted) {
            auto belt = belts::beltAt(offset(pos, cf::kDown));
            next      = belt && belt->slope != 0 ? kBeltRetracted : kBeltExtended;
        }
        if (next == shape) return true;
        std::vector<std::pair<std::string_view, int>> states{{kShape, next}};
        if (next == kBeltPushing || next == kBeltPulling) states.emplace_back(kExtracting, next == kBeltPushing ? 1 : 0);
        setStates(region, toBlockPos(pos), states);
        playSound(region, toBlockPos(pos), "random.click", .5f, 1.2f);
        static constexpr char const* kNames[] = {"", "retracted", "extended", "pushing", "pulling"};
        log().info("funnels: belt funnel at {} set to {} by {}", pos.key(), kNames[next], player.getRealName());
        // Create's BeltFunnelBlock.onWrenched: two extended belt funnels facing each other
        if (next == kBeltExtended) {
            int         facing = facingOf(block);
            auto const* other  = loadedBlock(region, toBlockPos(offset(pos, facing)));
            if (other && isFunnel(kindOf(other->getTypeName())) && beltShapeOf(*other) == kBeltExtended
                && facingOf(*other) == cf::oppositeFace(facing))
                advancements::award(player, "belt_funnel_kiss");
        }
        return true;
    }
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
    // Create's FunnelBlock.getStateForPlacement: a horizontal funnel on a belt is a belt funnel
    int shape = kNotOnBelt;
    if (cf::isHorizontal(facing) && onValidBelt(region, pos)) shape = beltShapeFor(region, pos, facing, !sneaking);
    setStates(
        region,
        bp,
        {{kFacing, facing}, {kExtracting, sneaking ? 0 : 1}, {kPowered, powered(region, bp) ? 1 : 0}, {kShape, shape}}
    );
}

void forgetFunnel(GlobalPos const& pos) { runtimes().erase(pos.key()); }

// ---- on contraptions (Create's FunnelMovementBehaviour) ------------------------------------------

/// The carried BlockData part `key` of a funnel on a contraption (null when absent).
Json const* carriedPart(contraption::MovementContext& ctx, char const* key) {
    auto const* carried = ctx.contraption.blockData(ctx.localPos);
    if (!carried || !carried->contains("common:blockdata")) return nullptr;
    auto const& doc = (*carried)["common:blockdata"];
    if (!doc.is_object() || !doc.contains(key)) return nullptr;
    return &doc[key];
}

ItemStack carriedFilter(contraption::MovementContext& ctx) {
    auto const* f = carriedPart(ctx, "filter");
    return f && f->is_object() && f->contains("filter") ? items::fromJson((*f)["filter"]) : items::empty();
}

void funnelExtract(contraption::MovementContext& ctx, GlobalPos const& pos, bool brass, int facing) {
    auto* region = regionOf(pos.dim);
    auto  level  = ll::service::getLevel();
    if (!region || !level) return;
    core::contraption::Vec3d entityPos = ctx.position;
    if (facing != cf::kDown) entityPos.y -= .5;
    auto const& visited = region->getBlock(toBlockPos(pos));
    if (!visited.isAir()) {
        AABB shape = visited.getBlockType().getCollisionShape(visited, *region, toBlockPos(pos), nullptr);
        if (shape.max.x - shape.min.x > 1e-4f && shape.max.y - shape.min.y > 1e-4f && shape.max.z - shape.min.z > 1e-4f) return;
    }
    BlockPos cell{int(std::floor(entityPos.x)), int(std::floor(entityPos.y)), int(std::floor(entityPos.z))};
    AABB     box{Vec3{float(cell.x), float(cell.y), float(cell.z)}, Vec3{cell.x + 1.f, cell.y + 1.f, cell.z + 1.f}};
    if (!region->getEntities(box, 0.f, ActorType::ItemEntity).empty()) return;
    ItemStack filter = brass ? carriedFilter(ctx) : items::empty();
    int       count  = 1;
    bool      upTo   = false;
    if (brass) {
        cf::AmountSetting amount;
        if (auto const* d = carriedPart(ctx, "funnels"); d && d->is_object()) {
            amount.count = std::clamp(d->value("count", 64), 1, 64);
            amount.upTo  = d->value("upTo", true);
        }
        count = amount.count;
        upTo  = amount.upTo;
    }
    auto accept = [&](ItemStack const& s) { return items::isEmpty(filter) || common::filter::matches(filter, s); };
    auto& storage = ctx.contraption.storage();
    if (!upTo) {
        // ExtractionCountMode.EXACTLY: all or nothing
        auto probe = storage.extractItem(accept, count, true);
        if (items::countOf(probe) < count) return;
    }
    auto stack = storage.extractItem(accept, count, false);
    if (items::isEmpty(stack)) return;
    auto* item = level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{float(entityPos.x), float(entityPos.y), float(entityPos.z)}, 0);
    if (!item) return;
    item->mPickupDelay = 5;
    item->lerpMotion(Vec3{0, 0, 0});
    playSound(*region, toBlockPos(pos), "random.pop", 1 / 16.f, .1f);
}

void funnelCollect(contraption::MovementContext& ctx, GlobalPos const& pos, bool brass) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    ItemStack filter = brass ? carriedFilter(ctx) : items::empty();
    AABB      box{Vec3{float(pos.x), float(pos.y), float(pos.z)}, Vec3{pos.x + 1.f, pos.y + 1.f, pos.z + 1.f}};
    for (auto* a : region->getEntities(box, 0.f, ActorType::ItemEntity)) {
        if (!a || !a->isAlive()) continue;
        auto& entityItem = static_cast<ItemActor*>(a)->item();
        if (items::isEmpty(entityItem)) continue;
        if (!items::isEmpty(filter) && !common::filter::matches(filter, entityItem)) continue;
        auto rest = ctx.contraption.storage().insertItem(entityItem, false);
        if (items::countOf(rest) == items::countOf(entityItem)) continue;
        if (items::isEmpty(rest)) a->remove();
        else entityItem.set(items::countOf(rest));
    }
}

contraption::MovementBehaviour funnelBehaviour(bool brass) {
    contraption::MovementBehaviour b;
    // the active area: 0.65 out of the front (0.15 for extracting funnels not facing up); a new
    // block there is visited (MovementBehaviour#visitNewPosition through getActiveAreaOffset)
    b.tick = [brass](contraption::MovementContext& ctx) {
        auto intState   = [&](char const* key, std::int64_t fallback) {
            auto it = ctx.states.find(key);
            auto const* v = it == ctx.states.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
            return v ? *v : fallback;
        };
        int  facing     = static_cast<int>(std::clamp<std::int64_t>(intState(kFacing, 1), 0, 5));
        bool extracting = intState(kExtracting, 0) != 0;
        double k        = facing != cf::kUp && extracting ? .15 : .65;
        auto   o        = core::contraption::offset(static_cast<core::contraption::Dir>(facing));
        auto   dir      = ctx.contraption.transform().rotateVector(core::contraption::Vec3d::of(o));
        auto   area     = ctx.position + dir * k;
        GlobalPos at{ctx.worldPos.dim, int(std::floor(area.x)), int(std::floor(area.y)), int(std::floor(area.z))};
        auto      last = ctx.data.value("FunnelVisited", Json());
        Json      now  = Json::array({at.x, at.y, at.z});
        if (last == now) return;
        ctx.data["FunnelVisited"] = now;
        if (extracting) funnelExtract(ctx, at, brass, facing);
        else funnelCollect(ctx, at, brass);
    };
    b.stopMoving = [](contraption::MovementContext& ctx) { ctx.data.erase("FunnelVisited"); };
    return b;
}

void installFunnels() {
    contraption::registerActor(kAndesiteFunnel, funnelBehaviour(false));
    contraption::registerActor(kBrassFunnel, funnelBehaviour(true));
    for (char const* id : {kAndesiteFunnel, kBrassFunnel}) {
        common::items::registerBlock(id, capability());
        belts::registerAboveInteractor(id, onBeltPass);
        // Create's DirectBeltInputBehaviour of an upward, inserting funnel (supportsDirectBeltInput)
        belts::registerDirectInput(
            id,
            belts::DirectInput{
                .canInsertFromSide =
                    [](GlobalPos const& p, int) {
                        auto* region = regionOf(p.dim);
                        auto const* b = region ? loadedBlock(*region, toBlockPos(p)) : nullptr;
                        return b && facingOf(*b) == cf::kUp && !state(*b, kExtracting) && beltShapeOf(*b) == kNotOnBelt;
                    },
                .insert =
                    [](GlobalPos const& p, belts::TransportedItem const& t, int, bool simulate) {
                        auto* region = regionOf(p.dim);
                        auto const* b = region ? loadedBlock(*region, toBlockPos(p)) : nullptr;
                        if (!b || state(*b, kPowered)) return t.stack;
                        return funnelInsert(*region, p, t.stack, simulate);
                    },
            }
        );
    }
    // Create: holding the filter slot opens its count (the "Extracted Amount" board) on funnels that
    // support an amount (extracting / belt funnels); the separate amount box stays for vanilla
    // players (SPEC §8)
    common::filterslot::SlotSpec slot{"filter", filterCentre, 0.5, nullptr, nullptr};
    slot.onHold = [](Player& p, GlobalPos const& pos) {
        auto* region = regionOf(pos.dim);
        auto const* b = region ? loadedBlock(*region, toBlockPos(pos)) : nullptr;
        if (!b || (beltShapeOf(*b) == kNotOnBelt && !state(*b, kExtracting))) return; // no count to edit
        common::valuebox::openEditor(p, pos, "amount");
    };
    common::filterslot::registerSlot(kBrassFunnel, std::move(slot));
    registerAmountBox(kBrassFunnel, amountCentre);
}

} // namespace lv::feature::funnels::detail
