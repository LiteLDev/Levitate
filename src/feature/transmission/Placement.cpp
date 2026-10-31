// Placing and using the transmission blocks (U6): Create's getStateForPlacement of every block,
// the placement helpers (shaft on shaft, cog on cog, large cog on a speed controller), encasing
// shafts and cogwheels with a casing, and opening the sequenced gearshift screen.
//
// Everything goes through GameMode::$useItemOn, where every player action ends up (network players
// and SimulatedPlayers alike). A plain placement is done by the game; the state Create would have
// chosen (computed from the world *before* the placement) is applied right after, in the same
// call, so the kinetic network only ever sees the final state.

#include <optional>
#include <unordered_map>
#include <vector>

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

#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::transmission {

namespace items = feature::common::items;
namespace ia    = feature::common::interactions;
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

/// Create's CogWheelBlock.isValidCogwheelPosition. A neighbour is skipped only when it has the
/// AXIS block property (`levitate:axis`: cogwheels, encased cogwheels) along the facing; blocks
/// with an integrated cog (mixer, crafter, pump, ...) have no AXIS property and always count.
bool validCogPosition(BlockSource& region, bool large, GlobalPos const& pos, Axis cogAxis) {
    for (Face f : ck::kFaces) {
        if (ck::axisOf(f) == cogAxis) continue;
        GlobalPos n   = stepPos(pos, f);
        auto      cog = cogAt(region, n);
        if (cog == ck::CogType::None) continue;
        if (stateAt(region, n, kAxis, -1) == axisInt(ck::axisOf(f))) continue;
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

ct::ChainState chainShapeAt(BlockSource& region, GlobalPos const& pos, ct::ChainState state) { return chainShape(region, pos, state); }

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
    if (id == kShaft || isEncasedShaft(id) || id == kFlywheel) return States{{kAxis, axisInt(pillarAxis(region, ctx, player))}};
    if (isCuckooClock(id)) {
        // Create's HorizontalKineticBlock.getPreferredHorizontalFacing: the back goes to a shaft
        // facing the position, else the clock faces the player
        std::optional<Face> preferred;
        for (Face side : {Face::South, Face::West, Face::North, Face::East}) {
            if (!shaftTowards(region, stepPos(ctx.pos, side), ck::opposite(side))) continue;
            if (preferred && ck::axisOf(*preferred) != ck::axisOf(side)) {
                preferred.reset();
                break;
            }
            preferred = side;
        }
        Face front = preferred ? ck::opposite(*preferred) : ck::opposite(horizontalLooking(player));
        return States{{kHorizontalFacing, horizontalFacingOf(front)}};
    }
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

/// ShaftBlock.PlacementHelper's line members: every AbstractSimpleShaftBlock (shaft, cogwheel,
/// large cogwheel) and the powered shaft.
bool isPoleMember(std::string_view id) { return id == kShaft || id == kCogwheel || id == kLargeCogwheel || id == kPoweredShaft; }

/// The extendo grip in the other hand adds 4 to the range (its reach modifier).
bool holdsExtendoGrip(Player& player) {
    auto const& off = player.getOffhandSlot();
    return !off.isNull() && off.getTypeName() == kExtendoGrip;
}

/// Create's ShaftBlock.PlacementHelper (a PoleHelper): a shaft or cogwheel item used on a shaft
/// extends the line of shafts and cogs on its axis with the held block.
bool shaftHelper(Player& player, ItemStack& item, BlockSource& region, GlobalPos const& clicked, double hx, double hy, double hz) {
    auto        axis   = static_cast<Axis>(std::clamp(stateAt(region, clicked, kAxis, 1), 0, 2));
    auto        dirs   = ct::orderedByDistance(hx - .5, hy - .5, hz - .5, [&](Face f) { return ck::axisOf(f) == axis; });
    std::string heldId = items::idOf(item);
    int         range  = kPlacementAssistRange + (holdsExtendoGrip(player) ? 4 : 0);
    for (Face dir : dirs) {
        int poles = 0;
        while (poles < range) {
            GlobalPos p = stepPos(clicked, dir, poles + 1);
            if (!isPoleMember(idAt(region, p)) || stateAt(region, p, kAxis, 1) != static_cast<int>(axis)) break;
            ++poles;
        }
        if (poles >= range) continue;
        GlobalPos target = stepPos(clicked, dir, poles + 1);
        if (!replaceableAt(region, target)) continue;
        return placeLikeHelper(player, item, region, target, heldId, {{kAxis, static_cast<int>(axis)}});
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

namespace {

/// Create's CogwheelBlockItem.onItemUseFirst up to the helper's offset: where the held cog goes
/// (nullopt: no helper applies, the block's use and the plain placement follow).
std::optional<std::pair<GlobalPos, Axis>> cogUseFirstSpot(BlockSource& region, std::string_view held, GlobalPos const& clicked,
                                                          double hx, double hy, double hz) {
    if (!isDedicatedCogItem(held)) return std::nullopt;
    // DiagonalCogHelper.getStatePredicate: any ICogWheel, the cogwheels and every block with an
    // integrated cog alike (Create's IntegratedLarge/SmallCogHelper match a subset of these
    // states, so in 6.0.10 they never decide a click)
    if (cogAt(region, clicked) == ck::CogType::None) return std::nullopt;
    return cogHelper(region, held == kLargeCogwheel, clicked, hx, hy, hz);
}

} // namespace

bool cogItemUseFirst(Player& player, ItemStack& item, GlobalPos const& clicked, double hx, double hy, double hz) {
    auto* region = regionOf(clicked.dim);
    if (!region || player.isSneaking() || items::isEmpty(item)) return false;
    std::string held = items::idOf(item);
    auto        spot = cogUseFirstSpot(*region, held, clicked, hx, hy, hz);
    return spot && placeLikeHelper(player, item, *region, spot->first, held, {{kAxis, static_cast<int>(spot->second)}});
}

bool useOn(Player& player, ItemStack& item, GlobalPos const& clicked, Face face, double hx, double hy, double hz) {
    auto* region = regionOf(clicked.dim);
    if (!region) return false;
    std::string block = idAt(*region, clicked);
    std::string held  = items::isEmpty(item) ? std::string{} : items::idOf(item);
    bool        sneak = player.isSneaking();

    // the block's useItemOn (sneaking with an item in hand skips it, as in Java)
    if (sneak && !held.empty()) return false;
    bool shaftItem = held == kShaft || held == kCogwheel || held == kLargeCogwheel; // AbstractSimpleShaftBlock items
    if (block == kShaft || block == kCogwheel || block == kLargeCogwheel) {
        if (sneak) return false;
        if (tryEncase(player, *region, clicked, block, held)) return true;
        if (block == kShaft && shaftItem) return shaftHelper(player, item, *region, clicked, hx, hy, hz);
        return false;
    }
    if (block == kPoweredShaft) return !sneak && shaftItem && shaftHelper(player, item, *region, clicked, hx, hy, hz); // PoweredShaftBlock.useItemOn
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

/// Create's state for placement, decided before vanilla places the block (the neighbours as the
/// player saw them) and applied once it exists.
struct PendingPlacement {
    std::int64_t player = 0;
    GlobalPos    pos;
    std::string  blockId;
    States       states;
};
std::optional<PendingPlacement> gPending;

std::vector<ia::HandlerId> gHandlers;

// U36: the unit's clicks come from the shared interaction dispatch.
ia::Result transmissionUseOn(ia::UseOnContext const& c) {
    if (c.face > 5 || c.offhand) return ia::Result::Pass; // Create: main hand only
    Player&            player  = c.player;
    GlobalPos          clicked = c.pos;
    Face               f       = static_cast<Face>(c.face);
    std::string const& held    = c.itemId;
    std::string const& block   = c.blockId;
    auto const&        h       = c.hit;
    // the held stack is the player's own (non-const) inventory slot: using it may take items
    auto& item = const_cast<ItemStack&>(c.item);
    gPending.reset();

    // block interactions of the unit (and the powered shaft's placement helper, U18's block)
    if (!blockOfItem(block).empty() || block == kPoweredShaft) {
        // the item may change while the interaction runs: decide on the cooldown first
        static thread_local bool busy = false;
        if (!busy) {
            busy      = true;
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
                return ia::Result::Consume;
            }
        }
    }
    return ia::Result::Pass;
}

/// A plain placement of one of our items: Create's state for placement and canSurvive (BlockItem
/// .place runs after the clicked block's use: Low, after the blocks' own handlers).
ia::Result transmissionPlacement(ia::UseOnContext const& c) {
    if (c.face > 5 || c.offhand) return ia::Result::Pass; // Create: main hand only
    Player&            player  = c.player;
    auto&              region  = player.getDimensionBlockSource();
    GlobalPos          clicked = c.pos;
    Face               f       = static_cast<Face>(c.face);
    std::string const& held    = c.itemId;
    auto const&        h       = c.hit;
    auto&              item    = ia::mutableItem(c.item);
    gPending.reset();
    auto blockId = blockOfItem(held);
    if (blockId.empty()) return ia::Result::Pass;
    PlaceContext ctx;
    ctx.clickedPos = clicked;
    ctx.face       = f;
    ctx.pos        = replaceableAt(region, clicked) ? clicked : stepPos(clicked, f);
    ctx.hitX = h[0], ctx.hitY = h[1], ctx.hitZ = h[2];
    ctx.sneaking = c.sneaking;
    auto states  = stateForPlacement(region, held, ctx, player);
    // CogWheelBlock / EncasedCogwheelBlock.canSurvive
    if (states && (isSmallCogBlock(blockId) || isLargeCogBlock(blockId))) {
        int axis = 1;
        for (auto const& [k, v] : *states)
            if (k == std::string_view(kAxis)) axis = v;
        // a vanilla block that takes the click (a chest, a door) uses it before the placement
        bool blockUses = !c.sneaking && c.block.getBlockType().isInteractiveBlock();
        if (!blockUses && !validCogPosition(region, isLargeCogBlock(blockId), ctx.pos, static_cast<Axis>(axis))) {
            player.sendInventory(false);
            return ia::Result::Deny;
        }
    }
    if (held == kVerticalGearbox) {
        // the vertical gearbox item places the gearbox itself (Create's VerticalGearboxItem)
        if (states && placeLikeHelper(player, item, region, ctx.pos, kGearbox, *states)) return ia::Result::Consume;
        return ia::Result::Deny;
    }
    if (states) gPending = PendingPlacement{player.getOrCreateUniqueID().rawID, ctx.pos, std::string(blockId), *states};
    return ia::Result::Pass;
}

/// Create's CogwheelBlockItem.onItemUseFirst: the cog placement helpers run before the clicked
/// block's own use (a crafter takes a cog clicked on its slot only when no helper spot applies),
/// so above the blocks' handlers (Normal) and below value boxes / filter slots (Highest).
ia::Result cogUseFirst(ia::UseOnContext const& c) {
    if (c.face > 5 || c.offhand || c.sneaking) return ia::Result::Pass;
    try {
        auto& region = c.player.getDimensionBlockSource();
        auto  spot   = cogUseFirstSpot(region, c.itemId, c.pos, c.hit[0], c.hit[1], c.hit[2]);
        if (!spot) return ia::Result::Pass;
        // Bedrock repeats the use while the button is held: one helper placement per 5 ticks, the
        // repeats in between are swallowed (they would reach the block's use or a plain placement)
        if (!cooledDown(c.player)) return ia::Result::Consume;
        gPending.reset();
        auto& item = ia::mutableItem(c.item);
        if (placeLikeHelper(c.player, item, region, spot->first, c.itemId, {{kAxis, static_cast<int>(spot->second)}}))
            return ia::Result::Consume;
    } catch (std::exception const& e) {
        log().error("transmission: cog placement helper failed: {}", e.what());
    }
    return ia::Result::Pass;
}

/// Only a block this click placed takes the placement state (never re-orient an existing one).
void transmissionPlaced(ia::PlaceContext const& c) {
    if (!gPending) return;
    auto pending = std::move(*gPending);
    gPending.reset();
    if (pending.player != c.player.getOrCreateUniqueID().rawID || pending.pos != c.pos || pending.blockId != c.blockId) return;
    auto& region = c.player.getDimensionBlockSource();
    setStates(region, c.pos, pending.states);
    if (isChainDrive(pending.blockId)) {
        for (Face side : ck::kFaces) updateChainShape(region, stepPos(c.pos, side));
    }
}

} // namespace

void installPlacementHooks() {
    for (char const* cog : {kCogwheel, kLargeCogwheel}) gHandlers.push_back(ia::onUseOn({"", cog}, cogUseFirst, ia::priority::High));
    gHandlers.push_back(ia::onUseOn({}, transmissionUseOn));
    gHandlers.push_back(ia::onUseOn({}, transmissionPlacement, ia::priority::Low));
    gHandlers.push_back(ia::onPlace({}, transmissionPlaced));
}
void uninstallPlacementHooks() {
    for (auto id : gHandlers) ia::remove(id);
    gHandlers.clear();
    lastUse().clear();
}

} // namespace lv::feature::transmission
