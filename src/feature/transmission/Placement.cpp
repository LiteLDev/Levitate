// Placing and using the transmission blocks (U6): Create's getStateForPlacement of every block,
// the placement helpers (shaft on shaft, cog on cog, large cog on a speed controller), encasing
// shafts and cogwheels with a casing, and opening the sequenced gearshift screen.
//
// Everything goes through GameMode::$useItemOn, where every player action ends up (network players
// and SimulatedPlayers alike). A plain placement is done by the game; the state Create would have
// chosen (computed from the world *before* the placement) is applied right after, in the same
// call, so the kinetic network only ever sees the final state.

#include <unordered_map>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::transmission {

namespace items = feature::common::items;
using States    = std::vector<std::pair<std::string_view, int>>;

namespace {

int axisInt(Axis a) { return static_cast<int>(a); }

/// The block an item places ("" when the item is not one of ours).
std::string_view blockOfItem(std::string_view item) {
    if (item == kVerticalGearbox) return kGearbox;
    for (auto const& id : allBlocks())
        if (id == item) return id;
    return {};
}

bool isDedicatedCogItem(std::string_view item) { return item == kCogwheel || item == kLargeCogwheel; }

/// Create's RotatedPillarKineticBlock.getPreferredAxis: the axis of the neighbours with a shaft
/// facing the position (null when they disagree).
std::optional<Axis> preferredAxis(BlockSource& region, GlobalPos const& pos, bool horizontalOnly = false) {
    std::optional<Axis> preferred;
    for (Face side : ck::kFaces) {
        if (horizontalOnly && ck::axisOf(side) == Axis::Y) continue;
        if (!shaftTowards(region, stepPos(pos, side), ck::opposite(side))) continue;
        if (preferred && *preferred != ck::axisOf(side)) return std::nullopt;
        preferred = ck::axisOf(side);
    }
    return preferred;
}

Axis pillarAxis(BlockSource& region, PlaceContext const& ctx, Player const& player) {
    auto preferred = preferredAxis(region, ctx.pos);
    if (preferred && !ctx.sneaking) return *preferred;
    return preferred && ctx.sneaking ? ck::axisOf(ctx.face) : ck::axisOf(nearestLooking(player));
}

/// Create's CogWheelBlock.isValidCogwheelPosition.
bool validCogPosition(BlockSource& region, bool large, GlobalPos const& pos, Axis cogAxis) {
    for (Face f : ck::kFaces) {
        if (ck::axisOf(f) == cogAxis) continue;
        GlobalPos n = stepPos(pos, f);
        if (auto a = rotationAxisAt(region, n); a && *a == ck::axisOf(f)) continue;
        auto cog = cogAt(region, n);
        if (cog == ck::CogType::Large || (large && cog == ck::CogType::Small)) return false;
    }
    return true;
}

/// Create's CogWheelBlock.getAxisForPlacement.
Axis cogAxis(BlockSource& region, bool large, PlaceContext const& ctx, Player const& player) {
    (void)player;
    if (ctx.sneaking) return ck::axisOf(ctx.face);
    GlobalPos below = stepPos(ctx.pos, Face::Down);
    if (large && idAt(region, below) == kSpeedController) {
        return stateAt(region, below, kAxis, 0) == 0 ? Axis::Z : Axis::X;
    }
    GlobalPos against = stepPos(ctx.pos, ck::opposite(ctx.face));
    if (cogAt(region, against) == ck::CogType::Small) {
        if (auto a = rotationAxisAt(region, against)) return *a;
    }
    auto preferred = preferredAxis(region, ctx.pos);
    return preferred ? *preferred : ck::axisOf(ctx.face);
}

/// Create's GaugeBlock / DirectionalAxisKineticBlock.getStateForPlacement.
States gaugeState(BlockSource& region, PlaceContext const& ctx, Player const& player) {
    GlobalPos placedOn = stepPos(ctx.pos, ck::opposite(ctx.face));
    if (shaftTowards(region, placedOn, ctx.face)) {
        Face horizontal   = horizontalLooking(player);
        bool lookPositive = ck::isPositive(nearestLooking(player));
        switch (ck::axisOf(ctx.face)) {
        case Axis::X:
            return {{kFacing, static_cast<int>(lookPositive ? Face::North : Face::South)}, {kAlongFirst, 1}};
        case Axis::Y:
            return {{kFacing, static_cast<int>(ck::opposite(horizontal))}, {kAlongFirst, ck::axisOf(horizontal) == Axis::X ? 1 : 0}};
        default:
            return {{kFacing, static_cast<int>(lookPositive ? Face::West : Face::East)}, {kAlongFirst, 0}};
        }
    }
    Face facing     = ctx.face; // GaugeBlock.getFacingForPlacement
    bool alongFirst = false;
    Axis faceAxis   = ck::axisOf(facing);
    auto prefers    = [&](Face side) { return shaftTowards(region, stepPos(ctx.pos, side), ck::opposite(side)); };
    if (faceAxis != Axis::Y) {
        alongFirst = faceAxis == Axis::Z;
        if (prefers(Face::Up) || prefers(Face::Down)) alongFirst = faceAxis == Axis::X;
    } else {
        alongFirst = ck::axisOf(horizontalLooking(player)) != Axis::X; // GaugeBlock.getAxisAlignmentForPlacement
        std::optional<Face> preferredSide;
        bool                conflict = false;
        for (Face side : {Face::South, Face::West, Face::North, Face::East}) { // Iterate.horizontalDirections
            if (!prefers(side)) continue;
            if (preferredSide && ck::axisOf(*preferredSide) != ck::axisOf(side)) {
                conflict = true;
                break;
            }
            preferredSide = side;
        }
        if (preferredSide && !conflict) alongFirst = ck::axisOf(*preferredSide) == Axis::X;
    }
    return {{kFacing, static_cast<int>(facing)}, {kAlongFirst, alongFirst ? 1 : 0}};
}

ct::ChainState chainAt(BlockSource& region, GlobalPos const& pos) {
    return ct::ChainState{
        static_cast<Axis>(std::clamp(stateAt(region, pos, kAxis, 1), 0, 2)),
        static_cast<ct::ChainPart>(std::clamp(stateAt(region, pos, kPart, 0), 0, 3)),
        stateAt(region, pos, kAlongFirst, 0) != 0,
    };
}

ct::ChainState chainShape(BlockSource& region, GlobalPos const& pos, ct::ChainState s) {
    for (Face f : ck::kFaces) {
        if (ck::axisOf(f) == s.axis) continue;
        GlobalPos n = stepPos(pos, f);
        std::optional<ct::ChainState> other;
        if (isChainDrive(idAt(region, n))) other = chainAt(region, n);
        s = ct::chainUpdateShape(s, f, other);
    }
    return s;
}

} // namespace

void updateChainShape(BlockSource& region, GlobalPos const& pos) {
    if (!isChainDrive(idAt(region, pos))) return;
    auto cur  = chainAt(region, pos);
    auto next = cur;
    // Create re-runs updateShape for the changed side only; running it for every side gives the
    // same result for a chain that was consistent before.
    for (Face f : ck::kFaces) {
        if (ck::axisOf(f) == next.axis) continue;
        GlobalPos                     n = stepPos(pos, f);
        std::optional<ct::ChainState> other;
        if (isChainDrive(idAt(region, n))) other = chainAt(region, n);
        next = ct::chainUpdateShape(next, f, other);
    }
    if (next == cur) return;
    setStates(region, pos, {{kPart, static_cast<int>(next.part)}, {kAlongFirst, next.alongFirst ? 1 : 0}});
}

std::optional<States> stateForPlacement(BlockSource& region, std::string_view itemId, PlaceContext const& ctx, Player const& player) {
    std::string_view id = blockOfItem(itemId);
    if (id.empty()) return std::nullopt;
    if (itemId == kVerticalGearbox) {
        // Create's VerticalGearboxItem
        std::optional<Axis> preferred;
        bool                conflict = false;
        for (Face side : {Face::South, Face::West, Face::North, Face::East}) {
            if (!shaftTowards(region, stepPos(ctx.pos, side), ck::opposite(side))) continue;
            if (preferred && *preferred != ck::axisOf(side)) {
                conflict = true;
                break;
            }
            preferred = ck::axisOf(side);
        }
        Axis axis;
        if (!preferred || conflict) axis = ck::axisOf(horizontalLooking(player)) == Axis::X ? Axis::Z : Axis::X;
        else axis = *preferred == Axis::X ? Axis::Z : Axis::X;
        return States{{kAxis, axisInt(axis)}};
    }
    if (id == kGearbox) return States{{kAxis, 1}};
    if (id == kShaft || isEncasedShaft(id)) return States{{kAxis, axisInt(pillarAxis(region, ctx, player))}};
    if (id == kClutch || id == kGearshift) {
        return States{{kAxis, axisInt(pillarAxis(region, ctx, player))}, {kPowered, neighbourSignal(region, ctx.pos) > 0 ? 1 : 0}};
    }
    if (id == kCogwheel || id == kLargeCogwheel) return States{{kAxis, axisInt(cogAxis(region, id == kLargeCogwheel, ctx, player))}};
    if (isEncasedCog(id)) {
        Axis      axis    = pillarAxis(region, ctx, player);
        GlobalPos against = stepPos(ctx.pos, ck::opposite(ctx.face));
        if (cogAt(region, against) == ck::CogType::Small) {
            if (auto a = rotationAxisAt(region, against)) axis = *a;
        }
        return States{{kAxis, axisInt(axis)}, {kTopShaft, 0}, {kBottomShaft, 0}};
    }
    if (isChainDrive(id)) {
        Axis looked = ck::axisOf(nearestLooking(player));
        Axis axis   = looked;
        if (!ctx.sneaking) axis = preferredAxis(region, ctx.pos).value_or(looked);
        auto   s   = chainShape(region, ctx.pos, ct::ChainState{axis, ct::ChainPart::None, false});
        States out = {{kAxis, axisInt(s.axis)}, {kPart, static_cast<int>(s.part)}, {kAlongFirst, s.alongFirst ? 1 : 0}};
        if (id == kChainGearshift) out.emplace_back(kSignal, neighbourSignal(region, ctx.pos));
        return out;
    }
    if (id == kSequencedGearshift) {
        auto preferred = preferredAxis(region, ctx.pos);
        Axis axis      = preferred && !ctx.sneaking ? *preferred : ck::axisOf(nearestLooking(player));
        return States{{kAxis, axisInt(axis)}, {kSeqState, 0}, {kSeqMod, 0}};
    }
    if (id == kSpeedController) {
        GlobalPos above = stepPos(ctx.pos, Face::Up);
        if (isLargeCogBlock(idAt(region, above))) {
            int cog = stateAt(region, above, kAxis, 1);
            if (cog != 1) return States{{kAxis, cog == 0 ? 2 : 0}};
        }
        // Create's HorizontalAxisKineticBlock
        auto preferred = preferredAxis(region, ctx.pos, true);
        Axis axis      = preferred ? *preferred : (ck::axisOf(horizontalLooking(player)) == Axis::X ? Axis::Z : Axis::X);
        return States{{kAxis, axisInt(axis)}};
    }
    if (isGauge(id)) return gaugeState(region, ctx, player);
    return States{};
}

namespace {

/// Places a block like Create's PlacementOffset.placeInWorld (sound, item use).
bool placeLikeHelper(Player& player, ItemStack& item, BlockSource& region, GlobalPos const& pos, std::string_view id, States const& states) {
    if (!replaceableAt(region, pos)) return false;
    if (!place(region, pos, id, states)) return false;
    playSound(pos.dim, pos, isSmallCogBlock(id) || isLargeCogBlock(id) ? "use.wood" : "use.stone", 1.f, .8f);
    if (!player.isCreative()) {
        item.remove(1);
        player.setSelectedItem(item);
    }
    player.sendInventory(false);
    log().info("transmission: {} placed {} at {} by placement assist", player.getRealName(), id, pos.key());
    return true;
}

/// Create's PoleHelper (shafts): extend the line of shafts the player clicked.
bool shaftHelper(Player& player, ItemStack& item, BlockSource& region, GlobalPos const& clicked, double hx, double hy, double hz) {
    auto axis = static_cast<Axis>(std::clamp(stateAt(region, clicked, kAxis, 1), 0, 2));
    auto dirs = ct::orderedByDistance(hx - .5, hy - .5, hz - .5, [&](Face f) { return ck::axisOf(f) == axis; });
    constexpr int kRange = 12; // CEquipment.placementAssistRange
    for (Face dir : dirs) {
        int poles = 0;
        while (poles < kRange) {
            GlobalPos p = stepPos(clicked, dir, poles + 1);
            if (idAt(region, p) != kShaft || stateAt(region, p, kAxis, 1) != static_cast<int>(axis)) break;
            ++poles;
        }
        if (poles >= kRange) continue;
        GlobalPos target = stepPos(clicked, dir, poles + 1);
        if (!replaceableAt(region, target)) continue;
        return placeLikeHelper(player, item, region, target, kShaft, {{kAxis, static_cast<int>(axis)}});
    }
    return false;
}

bool hitOnShaft(Axis axis, double hx, double hy, double hz) {
    // AllShapes.SIX_VOXEL_POLE (5..11 px across the axis), inflated by 0.001
    auto inside = [](double v) { return v >= 5. / 16 - .001 && v <= 11. / 16 + .001; };
    double c[3] = {hx, hy, hz};
    for (int i = 0; i < 3; ++i)
        if (i != static_cast<int>(axis) && !inside(c[i])) return false;
    return true;
}

/// Create's CogwheelBlockItem helpers (SmallCogHelper / LargeCogHelper / DiagonalCogHelper).
std::optional<std::pair<GlobalPos, Axis>> cogHelper(BlockSource& region, bool largeItem, GlobalPos const& clicked, double hx, double hy, double hz) {
    auto cog  = cogAt(region, clicked);
    auto axis = rotationAxisAt(region, clicked);
    if (cog == ck::CogType::None || !axis) return std::nullopt;
    if (hitOnShaft(*axis, hx, hy, hz)) return std::nullopt;
    double cx = hx - .5, cy = hy - .5, cz = hz - .5;
    auto   exceptAxis = [&](Axis a) { return ct::orderedByDistance(cx, cy, cz, [a](Face f) { return ck::axisOf(f) != a; }); };
    bool   clickedLarge = cog == ck::CogType::Large;
    if (!largeItem && !clickedLarge) {
        for (Face dir : exceptAxis(*axis)) {
            GlobalPos p = stepPos(clicked, dir);
            if (!validCogPosition(region, false, p, *axis)) continue;
            if (!replaceableAt(region, p)) continue;
            return std::pair{p, *axis};
        }
        return std::nullopt;
    }
    if (largeItem && clickedLarge) {
        Face side = ct::orderedByDistance(cx, cy, cz, [a = *axis](Face f) { return ck::axisOf(f) == a; }).front();
        for (Face dir : exceptAxis(*axis)) {
            GlobalPos p = stepPos(stepPos(clicked, dir), side);
            if (!validCogPosition(region, true, p, ck::axisOf(dir))) continue;
            if (!replaceableAt(region, p)) continue;
            return std::pair{p, ck::axisOf(dir)};
        }
        return std::nullopt;
    }
    // diagonal gears of different size
    auto all     = exceptAxis(*axis);
    Face closest = all.front();
    for (Face dir : all) {
        if (ck::axisOf(dir) == ck::axisOf(closest)) continue;
        GlobalPos p = stepPos(stepPos(clicked, dir), closest);
        if (!replaceableAt(region, p)) continue;
        if (!validCogPosition(region, clickedLarge, p, *axis)) continue;
        return std::pair{p, *axis};
    }
    return std::nullopt;
}

/// Create's EncasableBlock.tryEncase for shafts and cogwheels.
bool tryEncase(Player& player, BlockSource& region, GlobalPos const& pos, std::string const& block, std::string_view held) {
    bool andesite = held == kAndesiteCasing, brass = held == kBrassCasing;
    if (!andesite && !brass) return false;
    int axis = stateAt(region, pos, kAxis, 1);
    if (block == kShaft) {
        place(region, pos, andesite ? kAndesiteShaft : kBrassShaft, {{kAxis, axis}});
    } else if (block == kCogwheel || block == kLargeCogwheel) {
        bool  large = block == kLargeCogwheel;
        auto  id    = large ? (andesite ? kAndesiteLargeCog : kBrassLargeCog) : (andesite ? kAndesiteCog : kBrassCog);
        int   top = 0, bottom = 0;
        Axis  a = static_cast<Axis>(std::clamp(axis, 0, 2));
        for (bool positive : {false, true}) {
            Face d = ck::faceOf(a, positive);
            if (!shaftTowards(region, stepPos(pos, d), ck::opposite(d))) continue;
            (positive ? top : bottom) ^= 1;
        }
        place(region, pos, id, {{kAxis, axis}, {kTopShaft, top}, {kBottomShaft, bottom}});
    } else {
        return false;
    }
    playSound(pos.dim, pos, "use.wood", 1.f, 1.f); // Create: the casing's place sound; the casing is not used up
    log().info("transmission: {} encased {} at {}", player.getRealName(), block, pos.key());
    return true;
}

} // namespace

bool useOn(Player& player, ItemStack& item, GlobalPos const& clicked, Face face, double hx, double hy, double hz) {
    auto* region = regionOf(clicked.dim);
    if (!region) return false;
    std::string block = idAt(*region, clicked);
    std::string held  = items::isEmpty(item) ? std::string{} : items::idOf(item);
    bool        sneak = player.isSneaking();
    (void)face;

    // CogwheelBlockItem.onItemUseFirst: runs before the block's own use
    if (isDedicatedCogItem(held) && !sneak) {
        if (auto spot = cogHelper(*region, held == kLargeCogwheel, clicked, hx, hy, hz)) {
            return placeLikeHelper(player, item, *region, spot->first, held, {{kAxis, static_cast<int>(spot->second)}});
        }
    }
    // the block's useItemOn (sneaking with an item in hand skips it, as in Java)
    if (sneak && !held.empty()) return false;
    if (block == kShaft || block == kCogwheel || block == kLargeCogwheel) {
        if (sneak) return false;
        if (tryEncase(player, *region, clicked, block, held)) return true;
        if (block == kShaft && held == kShaft) return shaftHelper(player, item, *region, clicked, hx, hy, hz);
        return false;
    }
    if (block == kSpeedController && held == kLargeCogwheel) {
        GlobalPos above   = stepPos(clicked, Face::Up);
        Axis      newAxis = stateAt(*region, clicked, kAxis, 0) == 0 ? Axis::Z : Axis::X;
        if (!replaceableAt(*region, above) || !validCogPosition(*region, true, above, newAxis)) return false;
        return placeLikeHelper(player, item, *region, above, kLargeCogwheel, {{kAxis, static_cast<int>(newAxis)}});
    }
    if (block == kSequencedGearshift) {
        if (held == kWrench) return false;
        if (!blockOfItem(held).empty() || kinetics::specOf(held)) {
            if (shaftTowards(*region, clicked, face)) return false;
        }
        return openSequencerScreen(player, clicked);
    }
    return false;
}

// ---- hook --------------------------------------------------------------------------------------------

namespace {

bool isServerPlayer(Player const& p) { return !p.isClientSide(); }

std::unordered_map<std::int64_t, std::uint64_t>& lastUse() {
    static std::unordered_map<std::int64_t, std::uint64_t> m;
    return m;
}

/// Bedrock repeats use-on while the button is held: one custom action per 5 ticks.
bool cooledDown(Player& player) {
    auto  now  = player.getLevel().getCurrentTick().tickID;
    auto& last = lastUse()[player.getOrCreateUniqueID().rawID];
    if (last != 0 && now - last < 5) return false;
    last = now;
    return true;
}

std::array<double, 3> localHit(BlockPos const& pos, Vec3 const& hit) {
    auto in = [](double v) { return v >= -0.01 && v <= 1.01; };
    if (in(hit.x) && in(hit.y) && in(hit.z)) return {hit.x, hit.y, hit.z};
    return {hit.x - pos.x, hit.y - pos.y, hit.z - pos.z};
}

InteractionResult used() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TransmissionUseItemOnHook,
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
    if (!isServerPlayer(player) || handSlot != HandSlot::Mainhand || face > 5) {
        return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    }
    auto&       region  = player.getDimensionBlockSource();
    auto        clicked = game::globalPos(region, at);
    Face        f       = static_cast<Face>(face);
    std::string held    = items::isEmpty(item) ? std::string{} : items::idOf(item);
    std::string block   = idAt(region, clicked);
    auto        h       = localHit(at, hit);

    // block / item interactions of the unit
    bool ours = !blockOfItem(block).empty() || isDedicatedCogItem(held);
    if (ours) {
        // the item may change while the interaction runs: decide on the cooldown first
        static thread_local bool busy = false;
        if (!busy) {
            busy = true;
            bool done = false;
            try {
                if (cooledDown(player)) done = useOn(player, item, clicked, f, h[0], h[1], h[2]);
                else done = block == kSequencedGearshift && held.empty(); // the screen is already open
            } catch (std::exception const& e) {
                log().error("transmission: use failed: {}", e.what());
            }
            busy = false;
            if (done) {
                player.sendInventory(false);
                return used();
            }
        }
    }

    // a plain placement of one of our items: Create's state for placement
    auto blockId = blockOfItem(held);
    if (blockId.empty()) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    PlaceContext ctx;
    ctx.clickedPos = clicked;
    ctx.face       = f;
    ctx.pos        = replaceableAt(region, clicked) ? clicked : stepPos(clicked, f);
    ctx.hitX = h[0], ctx.hitY = h[1], ctx.hitZ = h[2];
    ctx.sneaking = player.isSneaking();
    auto states  = stateForPlacement(region, held, ctx, player);
    // CogWheelBlock / EncasedCogwheelBlock.canSurvive
    if (states && (isSmallCogBlock(blockId) || isLargeCogBlock(blockId))) {
        int axis = 1;
        for (auto const& [k, v] : *states)
            if (k == std::string_view(kAxis)) axis = v;
        if (!validCogPosition(region, isLargeCogBlock(blockId), ctx.pos, static_cast<Axis>(axis))) {
            player.sendInventory(false);
            return InteractionResult{};
        }
    }
    if (held == kVerticalGearbox) {
        // the vertical gearbox item places the gearbox itself (Create's VerticalGearboxItem)
        if (states && cooledDown(player)
            && placeLikeHelper(player, item, region, ctx.pos, kGearbox, *states)) {
            return used();
        }
        return InteractionResult{};
    }
    auto result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    if (states && idAt(region, ctx.pos) == blockId) {
        setStates(region, ctx.pos, *states);
        if (isChainDrive(blockId)) {
            for (Face side : ck::kFaces) updateChainShape(region, stepPos(ctx.pos, side));
        }
    }
    return result;
}

void installPlacementHooks() { TransmissionUseItemOnHook::hook(); }
void uninstallPlacementHooks() {
    TransmissionUseItemOnHook::unhook();
    lastUse().clear();
}

} // namespace lv::feature::transmission
