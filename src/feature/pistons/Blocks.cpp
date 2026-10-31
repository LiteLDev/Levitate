// Pistons unit: block registrations (kinetic specs, movement rules, rotation handlers, data
// carrier, value boxes), placement orientation and player interaction (GameMode hooks: they see
// network players and SimulatedPlayers alike), block removal, and the unit's module.

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pistons/Pistons.h"
#include "feature/redstone/Redstone.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include <string>
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "feature/goggles/GoggleInfo.h"

LV_REGISTER_CONFIG("pistons", lv::feature::pistons::Config);

namespace lv::feature::pistons {

namespace detail {
void runDeferred();
void clearDeferred();
} // namespace detail

namespace ct = feature::contraption;
namespace ia = feature::common::interactions;
namespace k  = core::kinetics;

bool& assemblingPiston() {
    static bool b = false;
    return b;
}

namespace {

// ---- kinetic specs ------------------------------------------------------------------------------

cc::Dir dirOf(k::BlockState const& s) { return static_cast<cc::Dir>(std::clamp(s.get(kFacing, 1), 0, 5)); }
k::Axis toK(cc::Axis a) { return static_cast<k::Axis>(static_cast<int>(a)); }

k::KineticSpec directionalAxisSpec() {
    k::KineticSpec s;
    s.states       = {kFacing, kAxisFirst};
    s.rotationAxis = [](k::BlockState const& b) -> std::optional<k::Axis> {
        return toK(cp::directionalAxis(dirOf(b), b.get(kAxisFirst, 0) != 0));
    };
    s.hasShaftTowards = [](k::BlockState const& b, k::Face f) {
        return k::axisOf(f) == toK(cp::directionalAxis(dirOf(b), b.get(kAxisFirst, 0) != 0));
    };
    s.rotorState   = kRotor;
    s.rotorModel   = "geometry.levitate.pistons_shaft_rotor";
    s.rotorTexture = "levitate_pistons_shaft";
    return s;
}

k::KineticSpec pistonSpec() {
    auto s      = directionalAxisSpec();
    s.impact    = 4;
    s.stressKey = "mechanical_piston";
    return s;
}

k::KineticSpec ropePulleySpec() {
    auto s         = k::specs::withModel(k::specs::shaft(kAxis), "geometry.levitate.pistons_shaft_rotor", "levitate_pistons_shaft");
    s.impact       = 4;
    return s;
}

k::KineticSpec elevatorPulleySpec() {
    k::KineticSpec s;
    s.states       = {kFacing};
    // Create: the rotation axis is the clockwise side of the horizontal facing
    auto axisOf    = [](k::BlockState const& b) {
        auto f = dirOf(b);
        return cc::axisOf(f) == cc::Axis::Z ? k::Axis::X : k::Axis::Z;
    };
    s.rotationAxis    = [axisOf](k::BlockState const& b) -> std::optional<k::Axis> { return axisOf(b); };
    s.hasShaftTowards = [axisOf](k::BlockState const& b, k::Face f) { return k::axisOf(f) == axisOf(b); };
    s.impact          = 4;
    s.rotorState      = kRotor;
    s.rotorModel      = "geometry.levitate.pistons_shaft_rotor";
    s.rotorTexture    = "levitate_pistons_shaft";
    return s;
}

bool isCarriageFacing(k::NodeView const& carriage, k::Face towardsCarriage) {
    return carriage.state && carriage.state->id == kGantryCarriage && carriage.state->get(kFacing, 1) == int(towardsCarriage);
}

k::KineticSpec gantryShaftSpec() {
    k::KineticSpec s;
    s.states       = {kFacing, kPowered};
    s.rotationAxis = [](k::BlockState const& b) -> std::optional<k::Axis> { return toK(cc::axisOf(dirOf(b))); };
    s.hasShaftTowards = [](k::BlockState const& b, k::Face f) { return k::axisOf(f) == toK(cc::axisOf(dirOf(b))); };
    s.propagate = [](k::NodeView const& from, k::NodeView const& to, k::Offset diff, bool byAxis, bool) -> float {
        if (byAxis || !from.state || !to.state) return 0;
        if (!from.state->get(kPowered, 0)) return 0;
        auto dir = k::nearestFace(diff.x, diff.y, diff.z);
        if (!isCarriageFacing(to, dir)) return 0;
        return cp::gantryPinionModifier(dirOf(*from.state), dirOf(*to.state));
    };
    s.customConnection = [](k::NodeView const& self, k::NodeView const& other) {
        auto d = k::diff(self.pos, other.pos);
        return isCarriageFacing(other, k::nearestFace(d.x, d.y, d.z));
    };
    s.stressKey    = "gantry_shaft";
    s.impact       = 0;
    s.rotorState   = kRotor;
    s.rotorModel   = "geometry.levitate.gantry_shaft_rotor";
    s.rotorTexture = "levitate_gantry_shaft";
    return s;
}

k::KineticSpec gantryCarriageSpec() {
    auto s = directionalAxisSpec();
    s.propagate = [](k::NodeView const& from, k::NodeView const& to, k::Offset diff, bool byAxis, bool) -> float {
        if (byAxis || !from.state || !to.state) return 0;
        if (to.state->id != kGantryShaft || !to.state->get(kPowered, 0)) return 0;
        auto dir = k::nearestFace(diff.x, diff.y, diff.z);
        if (from.state->get(kFacing, 1) != int(k::opposite(dir))) return 0;
        return cp::gantryPinionModifier(dirOf(*to.state), dirOf(*from.state));
    };
    s.customConnection = [](k::NodeView const& self, k::NodeView const& other) {
        if (!other.state || other.state->id != kGantryShaft || !self.state) return false;
        auto d = k::diff(self.pos, other.pos);
        return self.state->get(kFacing, 1) == int(k::opposite(k::nearestFace(d.x, d.y, d.z)));
    };
    s.impact       = 0;
    s.rotorModel   = "geometry.levitate.gantry_pinion_rotor";
    s.rotorTexture = "levitate_gantry_pinion";
    return s;
}

LV_REGISTER_KINETIC_BLOCK(std::string(kPiston), pistonSpec());
LV_REGISTER_KINETIC_BLOCK(std::string(kStickyPiston), pistonSpec());
LV_REGISTER_KINETIC_BLOCK(std::string(kRopePulley), ropePulleySpec());
LV_REGISTER_KINETIC_BLOCK(std::string(kElevatorPulley), elevatorPulleySpec());
LV_REGISTER_KINETIC_BLOCK(std::string(kGantryShaft), gantryShaftSpec());
LV_REGISTER_KINETIC_BLOCK(std::string(kGantryCarriage), gantryCarriageSpec());

// ---- movement rules / rotation / carried data -------------------------------------------------

int intOf(cc::States const& s, char const* key, int fallback) {
    auto it = s.find(key);
    if (it == s.end()) return fallback;
    if (auto const* v = std::get_if<std::int64_t>(&it->second)) return static_cast<int>(*v);
    return fallback;
}

cc::DirMask sidesOff(cc::Axis axis) {
    cc::DirMask m = 0;
    for (auto d : cc::kAllDirs)
        if (cc::axisOf(d) != axis) m |= cc::bit(d);
    return m;
}

void registerRules() {
    ct::MovementRules piston;
    piston.movable = [](cc::States const& s) { return intOf(s, kState, 0) != int(PistonState::Moving); };
    ct::registerMovementRules(std::string(kPiston), piston);
    ct::registerMovementRules(std::string(kStickyPiston), piston);

    ct::MovementRules pole;
    pole.notSupportive = [](cc::States const& s) -> cc::DirMask {
        if (assemblingPiston()) return 0x3F;
        return sidesOff(cc::axisOf(static_cast<cc::Dir>(std::clamp(intOf(s, kFacing, 1), 0, 5))));
    };
    // Create's movePistonPole / movePistonHead: a carried pole line stays whole
    pole.attachedTowards = [](cc::States const& s) -> cc::DirMask {
        if (assemblingPiston()) return 0;
        auto axis = cc::axisOf(static_cast<cc::Dir>(std::clamp(intOf(s, kFacing, 1), 0, 5)));
        return cc::bit(cc::dirOf(axis, true)) | cc::bit(cc::dirOf(axis, false));
    };
    ct::registerMovementRules(std::string(kPole), pole);
    auto head            = pole;
    head.attachedTowards = [](cc::States const& s) -> cc::DirMask {
        if (assemblingPiston()) return 0;
        return cc::bit(cc::opposite(static_cast<cc::Dir>(std::clamp(intOf(s, kFacing, 1), 0, 5))));
    };
    ct::registerMovementRules(std::string(kHead), head);
    // a carriage hangs on its gantry shaft (Create's canSurvive / moveGantryShaft)
    ct::MovementRules carriage;
    carriage.attachedTowards = [](cc::States const& s) -> cc::DirMask {
        return cc::bit(cc::opposite(static_cast<cc::Dir>(std::clamp(intOf(s, kFacing, 1), 0, 5))));
    };
    ct::registerMovementRules(std::string(kGantryCarriage), carriage);

    ct::MovementRules ropePulley;
    ropePulley.movable       = [](cc::States const& s) { return intOf(s, kRunning, 0) == 0; };
    ropePulley.notSupportive = [](cc::States const&) -> cc::DirMask { return cc::bit(cc::Dir::Down); };
    ct::registerMovementRules(std::string(kRopePulley), ropePulley);
    ct::MovementRules elevator;
    elevator.movable = [](cc::States const& s) { return intOf(s, kRunning, 0) == 0; };
    ct::registerMovementRules(std::string(kElevatorPulley), elevator);

    ct::MovementRules rope; // Create: PushReaction.BLOCK, #create:brittle
    rope.movable = [](cc::States const&) { return false; };
    rope.brittle = [](cc::States const&) { return true; };
    ct::registerMovementRules(std::string(kRope), rope);
    ct::registerMovementRules(std::string(kMagnet), rope);

    // Create's DirectionalAxisKineticBlock.transform: the shaft turns with the block
    auto axisFirst = [](cc::States& s, cc::Axis about, int quarter) {
        auto after  = static_cast<cc::Dir>(std::clamp(intOf(s, kFacing, 1), 0, 5));
        auto before = cc::rotate(after, about, -quarter);
        bool af     = cp::rotateAlongFirst(before, intOf(s, kAxisFirst, 0) != 0, about, quarter);
        s[kAxisFirst] = std::int64_t{af ? 1 : 0};
    };
    for (auto id : {kPiston, kStickyPiston, kGantryCarriage}) ct::registerRotationHandler(std::string(id), axisFirst);
    // horizontal-only states keep their value when they would turn vertical
    ct::registerRotationHandler(std::string(kRopePulley), [](cc::States& s, cc::Axis about, int quarter) {
        if (intOf(s, kAxis, 0) == 1) s[kAxis] = std::int64_t(int(cc::rotate(cc::Axis::Y, about, -quarter)));
    });
    ct::registerRotationHandler(std::string(kElevatorPulley), [](cc::States& s, cc::Axis about, int quarter) {
        auto f = static_cast<cc::Dir>(std::clamp(intOf(s, kFacing, 2), 0, 5));
        if (cc::axisOf(f) == cc::Axis::Y) s[kFacing] = std::int64_t(int(cc::rotate(f, about, -quarter)));
    });

    // the unit's block data travels with the blocks (Create keeps it in the block entities)
    ct::registerDataCarrier({
        "pistons",
        [](BlockSource&, GlobalPos const& p) -> std::optional<Json> {
            auto id = idAt(p);
            if (!actuatorKind(id) && id != kGantryShaft && id != kElevatorContact && id != kGantryCarriage) return std::nullopt;
            auto const* d = dataIf(p);
            if (!d) return std::nullopt;
            Json copy = *d;
            copy.erase("id");
            copy.erase("running");
            copy.erase("target");
            eraseData(p);
            return copy;
        },
        [](BlockSource&, GlobalPos const& p, Json const& d) {
            data(p) = d;
            auto id = idAt(p);
            if (id == kGantryShaft) trackShaft(p);
            if (id == kElevatorContact) trackContact(p);
        },
        nullptr,
    });
}

// ---- value boxes (movement mode) ----------------------------------------------------------------

std::vector<std::string> modeKeys() {
    return {
        "create.contraptions.movement_mode.move_place",
        "create.contraptions.movement_mode.move_place_returned",
        "create.contraptions.movement_mode.move_never_place",
    };
}
std::vector<std::string> modeFallbacks() {
    return {"Always Place when Stopped", "Place only in Starting Position", "Place only when Anchor Destroyed"};
}

void registerBoxes() {
    common::valuebox::BoxSpec box;
    box.id            = "movement_mode";
    box.label         = "create.contraptions.movement_mode";
    box.labelFallback = "Movement Mode";
    box.board         = [](Block const&) { return core::common::optionBoard(cp::kMovementModes, std::string(tr("create.contraptions.movement_mode", "Movement Mode"))); };
    box.get           = [](GlobalPos const& p) { return core::common::ValueSettings{0, int(movementMode(p))}; };
    box.set           = [](GlobalPos const& p, core::common::ValueSettings s, Player& editor) {
        setMovementMode(p, static_cast<cp::MovementMode>(std::clamp(s.value, 0, cp::kMovementModes - 1)));
        log().info("movement mode at {} {} {} set to {} by {}", p.x, p.y, p.z, s.value, editor.getRealName());
    };
    box.format = [](Block const&, core::common::ValueSettings s) {
        int i = std::clamp(s.value, 0, cp::kMovementModes - 1);
        return std::string(translate(modeKeys()[i], modeFallbacks()[i]));
    };
    // pistons: Create's DirectionalExtenderScrollOptionSlot (side faces off the extension and shaft axes)
    auto piston  = box;
    piston.centre = [](Block const& b, int face) -> std::optional<common::valuebox::Vec> {
        auto facing = static_cast<cc::Dir>(std::clamp(game::intState(b, kFacing).value_or(1), 0, 5));
        auto shaft  = cp::directionalAxis(facing, game::intState(b, kAxisFirst).value_or(0) != 0);
        auto f      = static_cast<cc::Dir>(std::clamp(face, 0, 5));
        if (cc::axisOf(f) == cc::axisOf(facing) || cc::axisOf(f) == shaft) return std::nullopt;
        auto n = cc::offset(f), m = cc::offset(facing);
        return common::valuebox::Vec{0.5 + n.x * 0.5 - m.x * 2 / 16.0, 0.5 + n.y * 0.5 - m.y * 2 / 16.0, 0.5 + n.z * 0.5 - m.z * 2 / 16.0};
    };
    common::valuebox::registerBox(std::string(kPiston), piston);
    common::valuebox::registerBox(std::string(kStickyPiston), piston);
    // rope pulley: the centre of the top face (Create's CenteredSideValueBoxTransform, UP)
    auto pulley   = box;
    pulley.centre = [](Block const&, int face) -> std::optional<common::valuebox::Vec> {
        if (face != 1) return std::nullopt;
        return common::valuebox::Vec{0.5, 1.0, 0.5};
    };
    common::valuebox::registerBox(std::string(kRopePulley), pulley);
}

// ---- placement --------------------------------------------------------------------------------

} // namespace

cc::Dir nearestLooking(Player const& player) {
    auto      rot   = player.getRotation();
    double    pitch = rot.x * std::numbers::pi / 180.0, yaw = rot.y * std::numbers::pi / 180.0;
    cc::Vec3d look{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    cc::Dir   best  = cc::Dir::Up;
    double    score = -2;
    for (auto d : cc::kAllDirs) {
        auto   o = cc::offset(d);
        double s = o.x * look.x + o.y * look.y + o.z * look.z;
        if (s > score) {
            score = s;
            best  = d;
        }
    }
    return best;
}

cc::Dir horizontalLooking(Player const& player) { return cc::fromHorizontalIndex(game::playerFacing(player)); }

bool shaftTowards(GlobalPos const& q, cc::Dir face) {
    auto const* b = blockAt(q);
    if (!b) return false;
    auto const* spec = kinetics::specOf(b->getTypeName());
    if (!spec) return false;
    k::BlockState snap;
    snap.id = b->getTypeName();
    for (auto const& name : spec->states) snap.values.emplace_back(name, game::intState(*b, name).value_or(0));
    return spec->shaftTowards(snap, static_cast<k::Face>(int(face)));
}

std::optional<cc::Dir> preferredSide(GlobalPos const& p, bool horizontalOnly) {
    std::optional<cc::Dir> pref;
    for (auto side : cc::kAllDirs) {
        if (horizontalOnly && cc::axisOf(side) == cc::Axis::Y) continue;
        if (!shaftTowards(rel(p, side), cc::opposite(side))) continue;
        if (pref && cc::axisOf(*pref) != cc::axisOf(side)) return std::nullopt;
        pref = side;
    }
    return pref;
}

namespace {

/// Create's DirectionalAxisKineticBlock.getStateForPlacement (after the facing is chosen).
bool axisAlongFirstFor(GlobalPos const& p, cc::Dir facing, Player const& player) {
    auto faceAxis = cc::axisOf(facing);
    if (faceAxis != cc::Axis::Y) {
        bool    alongFirst = faceAxis == cc::Axis::Z;
        cc::Dir positivePerp = faceAxis == cc::Axis::X ? cc::Dir::South : cc::Dir::East;
        bool    above = shaftTowards(rel(p, cc::Dir::Up), cc::Dir::Down);
        bool    below = shaftTowards(rel(p, cc::Dir::Down), cc::Dir::Up);
        (void)positivePerp; // Create's prefersConnectionTo(..., false) is always false
        if (above || below) alongFirst = faceAxis == cc::Axis::X;
        return alongFirst;
    }
    bool alongFirst = cc::axisOf(horizontalLooking(player)) == cc::Axis::X;
    std::optional<cc::Dir> preferred;
    bool                   conflict = false;
    for (auto side : {cc::Dir::South, cc::Dir::West, cc::Dir::North, cc::Dir::East}) {
        if (!shaftTowards(rel(p, side), cc::opposite(side))) continue;
        if (preferred && cc::axisOf(*preferred) != cc::axisOf(side)) {
            conflict = true;
            break;
        }
        preferred = side;
    }
    if (preferred && !conflict) alongFirst = cc::axisOf(*preferred) == cc::Axis::X;
    return alongFirst;
}

void orient(GlobalPos const& p, std::string const& id, Player& player, cc::Dir clickedFace) {
    EditGuard g;
    bool      sneak = player.isSneaking();
    if (isPiston(id)) {
        cc::Dir look   = nearestLooking(player);
        cc::Dir facing = sneak ? look : cc::opposite(look);
        setStates(p, {{kFacing, int(facing)}, {kAxisFirst, axisAlongFirstFor(p, facing, player) ? 1 : 0}, {kState, 0}});
    } else if (id == kPole || id == kHead) {
        setStates(p, {{kFacing, int(cc::opposite(clickedFace))}});
    } else if (id == kRopePulley) {
        auto pref = preferredSide(p, true);
        auto axis = pref ? cc::axisOf(*pref) : cc::axisOf(cc::rotate(horizontalLooking(player), cc::Axis::Y, -1));
        setStates(p, {{kAxis, int(axis)}});
    } else if (id == kElevatorPulley) {
        auto    pref = preferredSide(p, true);
        cc::Dir facing;
        if (!pref || sneak) {
            auto look = horizontalLooking(player);
            facing    = sneak ? look : cc::opposite(look);
        } else {
            facing = cc::opposite(*pref);
        }
        setStates(p, {{kFacing, int(facing)}});
    } else if (id == kGantryShaft) {
        orientShaft(p, player, clickedFace);
    } else if (id == kGantryCarriage) {
        orientCarriage(p, clickedFace);
        auto facing = facingAt(p);
        setStates(p, {{kAxisFirst, axisAlongFirstFor(p, facing, player) ? 1 : 0}});
        orientCarriage(p, clickedFace); // Create's cycleAxisIfNecessary against the shaft behind
        checkCarriage(p);
    } else if (id == kElevatorContact) {
        trackContact(p);
    }
    log().info("{} placed at {} {} {} by {}", id, p.x, p.y, p.z, player.getRealName());
}

bool isOurBlockItem(std::string_view id) {
    return isPiston(id) || id == kPole || id == kRopePulley || id == kElevatorPulley || id == kGantryShaft || id == kGantryCarriage;
}

/// Create's PoleHelper: a pole / gantry shaft used on the same block extends the line.
bool poleHelper(Player& player, ItemStack& item, GlobalPos const& at, Vec3 const& hit) {
    auto    id     = idAt(at);
    cc::Dir facing = facingAt(at);
    cc::Axis axis  = cc::axisOf(facing);
    // directions along the axis, the one whose face is nearer to the hit first
    cc::Vec3d centre = cc::Vec3d::center(cpos(at));
    std::vector<cc::Dir> dirs{cc::dirOf(axis, true), cc::dirOf(axis, false)};
    auto dist = [&](cc::Dir d) {
        auto o = cc::offset(d);
        cc::Vec3d f = centre + cc::Vec3d::of(o) * 0.5;
        return (f - cc::Vec3d{hit.x, hit.y, hit.z}).lengthSqr();
    };
    std::sort(dirs.begin(), dirs.end(), [&](cc::Dir a, cc::Dir b) { return dist(a) < dist(b); });
    int limit = config::get<Config>().maxPistonPoles;
    for (auto d : dirs) {
        int poles = 0;
        while (poles < limit) {
            auto q = rel(at, d, poles + 1);
            if (idAt(q) != id || cc::axisOf(facingAt(q)) != axis) break;
            ++poles;
        }
        auto target = rel(at, d, poles + 1);
        if (!replaceableAt(target)) continue;
        std::vector<std::pair<std::string_view, int>> states{{kFacing, int(facing)}};
        if (id == kGantryShaft) states.emplace_back(kPowered, stateOr(at, kPowered, 0));
        {
            EditGuard g;
            if (!place(target, id, states, 3)) return false;
        }
        if (id == kGantryShaft) trackShaft(target);
        if (!player.isCreative()) {
            item.remove(1);
            player.sendInventory(false);
        }
        sound(target, id == kPole ? "dig.scaffold" : "dig.stone", 1.f, 0.8f);
        log().info("{} extended to {} {} {} by {}", id, target.x, target.y, target.z, player.getRealName());
        return true;
    }
    return false;
}

bool onValueBox(Block const& block, uchar face, BlockPos const& at, Vec3 const& hit) {
    common::valuebox::Vec local{hit.x - at.x, hit.y - at.y, hit.z - at.z};
    return common::valuebox::boxAt(block, face, local) != nullptr;
}

/// Our block interactions; true when handled (Create's useItemOn of the blocks).
bool useOnBlock(Player& player, ItemStack& item, BlockPos const& at, uchar face, Vec3 const& hit, Block const& target) {
    std::string const& tid   = target.getTypeName();
    GlobalPos          p     = game::globalPos(player.getDimensionBlockSource(), at);
    bool               empty = item.isNull() || item.mCount == 0;
    std::string        held  = empty ? std::string() : std::string(item.getTypeName());
    bool               sneak = player.isSneaking();

    if (tid == kElevatorContact) {
        if (held == kWrench) {
            // Create's ElevatorContactBlock.onWrenched: rotate, then back to a redstone contact
            // unless it now faces a known column
            cc::Dir now = facingAt(p), clicked = static_cast<cc::Dir>(std::min<int>(face, 5));
            cc::Dir next = cc::rotate(now, cc::axisOf(clicked), cc::positive(clicked) ? 1 : -1);
            {
                EditGuard g;
                setStates(p, {{kFacing, int(next)}});
            }
            // Create: ElevatorColumn.get(coords) != null (other contacts of that column, or an elevator)
            ColumnCoords col{p.dim, p.x, p.z, next};
            auto         others = columnContacts(col);
            std::erase(others, p.y);
            if (cc::axisOf(next) == cc::Axis::Y || (others.empty() && !columnActive(col))) {
                EditGuard g;
                if (game::blockWith(kRedstoneContact)) place(p, kRedstoneContact, {{kFacing, int(next)}}, 3);
            }
            return true;
        }
        if (sneak && !empty) return false;
        openContactScreen(player, p);
        return true;
    }
    if (sneak) return false;
    if (isPiston(tid) || tid == kRopePulley || tid == kGantryCarriage || tid == kElevatorPulley) {
        if (onValueBox(target, face, at, hit)) return false; // the movement mode box (U3)
    }
    if (isPiston(tid)) {
        if (held == "minecraft:slime_ball") {
            if (stateOr(p, kState, 0) != int(PistonState::Retracted)) return false;
            if (face != stateOr(p, kFacing, 1) || tid == kStickyPiston) return false;
            {
                EditGuard g;
                place(p, kStickyPiston, {{kFacing, stateOr(p, kFacing, 1)}, {kAxisFirst, stateOr(p, kAxisFirst, 0)}}, 3);
            }
            sound(p, "mob.slime.big", 0.5f, 1.f);
            if (!player.isCreative()) {
                item.remove(1);
                player.sendInventory(false);
            }
            log().info("piston at {} {} {} made sticky by {}", p.x, p.y, p.z, player.getRealName());
            return true;
        }
        if (!empty) return false;
        requestAssemble(p);
        return true;
    }
    if (tid == kRopePulley) {
        if (!empty) return false;
        requestAssemble(p);
        return true;
    }
    if (tid == kElevatorPulley) {
        if (!empty) return false;
        elevatorClicked(p);
        return true;
    }
    if (tid == kGantryCarriage) {
        if (!empty) return false;
        checkCarriage(p);
        return true;
    }
    if ((tid == kPole || tid == kGantryShaft) && held == tid) return poleHelper(player, item, p, hit);
    return false;
}

std::uint64_t gLastUseTick = 0;
std::int64_t  gLastUser    = 0;

} // namespace

// ---- interactions (U36: on the shared dispatch) --------------------------------------------------

/// Right-clicks on the unit's blocks (one action per 4 ticks per player while the button is held).
ia::Result pistonsUseOn(ia::UseOnContext const& c) {
    std::string const& tid = c.blockId;
    if (!tid.starts_with("levitate:")) return ia::Result::Pass;
    bool ours = isPiston(tid) || isPulley(tid) || tid == kGantryCarriage || tid == kElevatorContact || tid == kPole
             || tid == kGantryShaft;
    if (!ours) return ia::Result::Pass;
    auto uid = c.player.getOrCreateUniqueID().rawID;
    if (uid == gLastUser && currentTick() - gLastUseTick < 4 && !c.firstEvent) return ia::Result::Consume;
    BlockPos at{c.pos.x, c.pos.y, c.pos.z};
    Vec3 hit{static_cast<float>(at.x + c.hit[0]), static_cast<float>(at.y + c.hit[1]), static_cast<float>(at.z + c.hit[2])};
    // the held stack is the player's own (non-const) inventory slot: using it may take one item
    auto& item = const_cast<ItemStack&>(c.item);
    if (!useOnBlock(c.player, item, at, static_cast<uchar>(c.face), hit, c.block)) return ia::Result::Pass;
    gLastUser    = uid;
    gLastUseTick = currentTick();
    c.player.sendInventory(false);
    return ia::Result::Consume;
}

/// A placed block of the unit faces the way Create's getStateForPlacement says.
void pistonsPlaced(ia::PlaceContext const& c) {
    if (c.itemId != c.blockId || !isOurBlockItem(c.itemId)) return;
    orient(c.pos, c.blockId, c.player, static_cast<cc::Dir>(std::min<int>(c.face, 5)));
}

/// A player breaking a piston, pole or head breaks the whole chain (once the block is gone).
/// The facing of a chain block a player is breaking (read before vanilla removes it).
std::map<GlobalPos, cc::Dir>& breakingFacing() {
    static std::map<GlobalPos, cc::Dir> m;
    return m;
}

ia::Result pistonsDestroy(ia::DestroyContext const& c) {
    std::string const& id = c.blockId;
    if (isPiston(id) || id == kPole || id == kHead) breakingFacing()[c.pos] = facingAt(c.pos);
    return ia::Result::Pass;
}

void pistonsDestroyed(ia::DestroyContext const& c, bool destroyed) {
    auto it = breakingFacing().find(c.pos);
    if (it == breakingFacing().end()) return;
    cc::Dir facing = it->second;
    breakingFacing().erase(it);
    std::string const& id = c.blockId;
    if (!destroyed || idAt(c.pos) == id) return; // not broken (refused, protected, ...)
    bool drops = !c.player.isCreative();
    if (isPiston(id)) destroyPistonChain(c.pos, facing, drops);
    else pistonChainBroken(c.pos, id, facing, drops);
}

namespace {

/// Watches block changes the unit didn't make itself: ropes breaking, pulleys and shafts
/// removed, contacts gone.
void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    if (ev.layer() != 0 || editing()) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    std::string prev = ev.previousBlock().getTypeName();
    std::string now  = ev.newBlock().getTypeName();
    if (prev == now) return;
    GlobalPos p = game::globalPos(region, ev.pos());
    if (isRopeLike(prev) || prev == kRopePulley) {
        bool pulley = prev == kRopePulley;
        defer([p, pulley] { ropeBroken(p, pulley); });
    }
    if (prev == kGantryShaft || now == kGantryShaft) {
        defer([p] {
            for (auto d : cc::kAllDirs) {
                auto q = rel(p, d);
                if (idAt(q) == kGantryShaft) trackShaft(q);
                // Create's GantryCarriageBlock.canSurvive: a carriage without its shaft breaks
                if (idAt(q) == kGantryCarriage && facingAt(q) == d && idAt(p) != kGantryShaft) {
                    log().info("gantry carriage at {} {} {} lost its shaft", q.x, q.y, q.z);
                    destroyAt(q, true);
                }
            }
            if (idAt(p) == kGantryShaft) trackShaft(p);
        });
    }
    if (now == kElevatorContact) defer([p] { trackContact(p); });
}

class PistonsModule {
public:
    void install() {
        static bool registered = false;
        if (!registered) {
            registered = true;
            registerRules();
            registerBoxes();
            // Create's PulleyBlockEntity implements ThresholdSwitchObservable: the rope end's Y
            // between the bottom of the world and the pulley (added by U58)
            redstone::threshold::registerObservable(
                std::string(kRopePulley),
                redstone::threshold::Observable{
                    .min     = [](BlockSource& r, BlockPos const&) { return static_cast<int>(r.getMinHeight()); },
                    .max     = [](BlockSource&, BlockPos const& p) { return p.y; },
                    .current = [](BlockSource& r, BlockPos const& p) { return p.y - static_cast<int>(offsetOf(game::globalPos(r, p))); },
                    .format  = [](int v) { return tr("create.gui.threshold_switch.pulley_y_level", "{0}y", v); },
                }
            );
            game::db::onWorldReady([] {
                markRopesClimbable();
                loadActuators();
                loadGantries();
                loadElevators();
            });
            game::db::onWorldClosed([] {
                detail::clearDeferred();
                resetActuators();
                resetGantries();
                resetElevators();
                resetRopeSync();
            });
            // Create's remove(): a running actuator that loses its block disassembles
            common::blockdata::onErase([](GlobalPos const& p, Json const& doc, std::string const& prevId) {
                if (prevId.empty() || !doc.contains("pistons")) return;
                auto kind = actuatorKind(prevId);
                if (!kind) return;
                if (editing()) return;
                Json d = doc["pistons"];
                defer([p, kind = *kind, d, prevId] { actuatorRemoved(p, kind, d, prevId); });
            });
        }
        for (auto id : {kPiston, kStickyPiston, kRopePulley}) {
            mSpeed.push_back(kinetics::onSpeedChanged(std::string(id), [](kinetics::SpeedChange const& c) {
                if (c.removed) return;
                if (c.prevSpeed == c.speed) return;
                onActuatorSpeed(c.pos, c.prevSpeed, c.speed);
            }));
        }
        mSpeed.push_back(kinetics::onSpeedChanged(std::string(kGantryShaft), [](kinetics::SpeedChange const& c) {
            if (!c.removed) onShaftSpeed(c.pos);
        }));
        mHandlers.push_back(ia::onUseOn({}, pistonsUseOn));
        mHandlers.push_back(ia::onPlace({}, pistonsPlaced));
        mHandlers.push_back(ia::onDestroy({}, pistonsDestroy));
        mHandlers.push_back(ia::onDestroyed({}, pistonsDestroyed));
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) { onBlockChanged(ev); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); }));
        if (game::db::available()) {
            markRopesClimbable();
            loadActuators();
            loadGantries();
            loadElevators();
        }
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        for (auto id : mSpeed) kinetics::removeListener(id);
        mSpeed.clear();
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
    }

private:
    static void tick() {
        try {
            detail::runDeferred();
            tickActuators();
            tickGantries();
            tickElevators();
            tickRopeSync();
        } catch (std::exception const& e) {
            log().error("pistons: tick failed: {}", e.what());
        }
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    std::vector<kinetics::ListenerId>   mSpeed;
    std::vector<ia::HandlerId>          mHandlers;
};

} // namespace

std::vector<std::string> movementModeNames(Player const* viewer) {
    std::vector<std::string> out;
    auto keys = modeKeys(), fb = modeFallbacks();
    for (std::size_t i = 0; i < keys.size(); ++i) out.emplace_back(viewer ? trFor(*viewer, keys[i], fb[i]) : std::string(translate(keys[i], fb[i])));
    return out;
}

} // namespace lv::feature::pistons

LV_REGISTER_MODULE(lv::feature::pistons::PistonsModule);

// Goggle line of piston extension poles (Create's GoggleOverlayRenderer: "Pole Length: N" while
// wearing goggles and looking at a pole line that ends at a mechanical piston).
namespace lv::feature::pistons {
namespace pole_goggles {


namespace {

namespace goggles = ::lv::feature::goggles;

int facingState(Block const& block) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states") || !nbt["states"].contains(kFacing)) return 1;
    auto const& tag = nbt["states"][kFacing];
    if (tag.hold(Tag::Type::Int)) return std::clamp(tag.get<IntTag>().data, 0, 5);
    if (tag.hold(Tag::Type::Byte)) return std::clamp<int>(tag.get<ByteTag>().data, 0, 5);
    return 1;
}

LV_REGISTER_GOGGLE_INFO(std::string(kPole), {
    .goggle = [](goggles::InfoContext const& c, goggles::Tooltip& t) {
        auto axis  = cc::axisOf(static_cast<cc::Dir>(facingState(c.block)));
        int  poles = 1;
        bool pistonFound = false;
        for (bool positive : {true, false}) {
            auto     dir = cc::offset(cc::dirOf(axis, positive));
            BlockPos at{c.pos.x, c.pos.y, c.pos.z};
            int      attached = 0;
            // PistonExtensionPoleBlock.PlacementHelper#attachedPoles: poles of the same axis in a row
            while (attached < 1024) {
                BlockPos next{at.x + dir.x, at.y + dir.y, at.z + dir.z};
                auto const& b = c.region.getBlock(next);
                if (b.getTypeName() != kPole || cc::axisOf(static_cast<cc::Dir>(facingState(b))) != axis) break;
                at = next;
                ++attached;
            }
            poles += attached;
            auto id = c.region.getBlock(BlockPos{at.x + dir.x, at.y + dir.y, at.z + dir.z}).getTypeName();
            pistonFound = pistonFound || id == kPiston || id == kStickyPiston;
        }
        if (!pistonFound) return false;
        t.push_back(goggles::line(c.tr("create.gui.goggles.pole_length", "Pole Length:") + " " + std::to_string(poles)));
        return true;
    },
});

} // namespace

} // namespace pole_goggles
} // namespace lv::feature::pistons
