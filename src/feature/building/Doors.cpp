// Building unit: Create's sliding doors (SlidingDoorBlock: two halves, slide or fold open,
// double doors open together, redstone) and train / framed glass trapdoors.
//
// States: levitate:facing 2..5, levitate:upper, levitate:hinge (0 left, 1 right),
// levitate:open, levitate:powered; trapdoors: levitate:facing, levitate:top, levitate:open,
// levitate:powered. Custom blocks get no neighbour updates, so the unit polls the redstone input
// of the doors and trapdoors it knows every 2 ticks (SPEC §8).

#include <random>
#include <set>

#include "nlohmann/json.hpp"

#include "mc/world/actor/player/Player.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include "feature/building/Building.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::building {

namespace {

constexpr char kDbRedstone[] = "lv:building:redstone";

std::set<GlobalPos>& tracked() {
    static std::set<GlobalPos> s;
    return s;
}
bool& trackedDirty() {
    static bool d = false;
    return d;
}

bool isKind(Block const* b, Kind k) {
    auto const* i = b ? infoOf(*b) : nullptr;
    return i && i->kind == k;
}

void doorSound(GlobalPos const& p, Info const& info, bool open) {
    // Create's block set types: iron door sounds (glass doors too), pitch 0.9..1.0
    static std::mt19937 r{12345};
    float pitch = 0.9f + std::uniform_real_distribution<float>(0.f, 0.1f)(r);
    (void)info;
    playSound(p, open ? "open.iron_door" : "close.iron_door", 1.f, pitch);
}

/// The other door of a double door (Java SlidingDoorBlock.isDoubleDoor, `state` already toggled).
std::optional<GlobalPos> doubleDoorPartner(GlobalPos const& lower, Block const& state) {
    int  facing = hfacingOf(state);
    int  hinge  = stateOf(state, "levitate:hinge");
    auto other  = offset(lower, hinge == 0 ? clockwise(facing) : counterClockwise(facing));
    auto const* o = blockAt(other);
    if (!o || o->getTypeName() != state.getTypeName()) return std::nullopt;
    if (stateOf(*o, "levitate:hinge") == hinge || hfacingOf(*o) != facing) return std::nullopt;
    if (stateOf(*o, "levitate:open") == stateOf(state, "levitate:open")) return std::nullopt;
    if (stateOf(*o, "levitate:upper") != stateOf(state, "levitate:upper")) return std::nullopt;
    return other;
}

/// Sets open / powered on both halves of the door whose lower half is at `lower`.
void setDoor(GlobalPos const& lower, int open, std::optional<int> powered) {
    for (auto const& p : {lower, offset(lower, Up)}) {
        auto const* b = blockAt(p);
        if (!isKind(b, Kind::Door)) continue;
        Block const* n = withStates(*b, {{"levitate:open", open}});
        if (powered) n = withStates(*n, {{"levitate:powered", *powered}});
        if (n && n != b) set(p, *n);
    }
}

GlobalPos lowerHalf(GlobalPos const& p, Block const& b) { return stateOf(b, "levitate:upper") ? offset(p, Down) : p; }

} // namespace

void trackRedstone(GlobalPos const& pos) {
    if (tracked().insert(pos).second) trackedDirty() = true;
}

bool useDoor(Player* player, GlobalPos const& pos) {
    auto const* b = blockAt(pos);
    if (!isKind(b, Kind::Door)) return false;
    auto const& info  = *infoOf(*b);
    auto        lower = lowerHalf(pos, *b);
    auto const* lb    = blockAt(lower);
    if (!isKind(lb, Kind::Door)) return false;
    int open = stateOf(*lb, "levitate:open") ? 0 : 1;
    // Java useWithoutItem: toggle, then the double door partner (it was in the opposite state)
    auto const* toggled = withStates(*lb, {{"levitate:open", open}});
    auto        partner = doubleDoorPartner(lower, *toggled);
    setDoor(lower, open, std::nullopt);
    if (partner) setDoor(*partner, open, std::nullopt);
    doorSound(lower, info, open == 1);
    log().info("building: {} at {} {} {} {}{}{}", info.id, lower.x, lower.y, lower.z, open ? "opened" : "closed", partner ? " (double door)" : "",
               player ? " by " + player->getRealName() : std::string());
    return true;
}

bool useTrapdoor(Player* player, GlobalPos const& pos) {
    auto const* b = blockAt(pos);
    if (!isKind(b, Kind::Trapdoor)) return false;
    auto const& info = *infoOf(*b);
    int         open = stateOf(*b, "levitate:open") ? 0 : 1;
    set(pos, *withStates(*b, {{"levitate:open", open}}));
    playSound(pos, open ? "open.iron_trapdoor" : "close.iron_trapdoor", 1.f, 0.95f);
    log().info("building: {} at {} {} {} {}{}", info.id, pos.x, pos.y, pos.z, open ? "opened" : "closed", player ? " by " + player->getRealName() : std::string());
    return true;
}

void redstoneTick() {
    std::vector<GlobalPos> gone;
    for (auto const& p : tracked()) {
        auto const* b = blockAt(p);
        if (!b) continue;
        auto const* info = infoOf(*b);
        if (info && info->kind == Kind::Door && !stateOf(*b, "levitate:upper")) {
            // Java isDoorPowered: either half, or either half of the double door partner
            bool powered = hasNeighborSignal(p) || hasNeighborSignal(offset(p, Up));
            int  facing  = hfacingOf(*b);
            int  hinge   = stateOf(*b, "levitate:hinge");
            auto other   = offset(p, hinge == 0 ? clockwise(facing) : counterClockwise(facing));
            if (auto const* o = blockAt(other); o && o->getTypeName() == info->id && stateOf(*o, "levitate:hinge") != hinge
                                                 && hfacingOf(*o) == facing && !stateOf(*o, "levitate:upper")) {
                powered = powered || hasNeighborSignal(other) || hasNeighborSignal(offset(other, Up));
            }
            if (powered == (stateOf(*b, "levitate:powered") != 0)) continue;
            bool wasOpen = stateOf(*b, "levitate:open") != 0;
            setDoor(p, powered, powered);
            if (powered != wasOpen) {
                doorSound(p, *info, powered);
                auto const* now = blockAt(p);
                if (auto partner = now ? doubleDoorPartner(p, *now) : std::nullopt) setDoor(*partner, powered, powered);
                log().info("building: {} at {} {} {} {} by redstone", info->id, p.x, p.y, p.z, powered ? "opened" : "closed");
            }
            continue;
        }
        if (info && info->kind == Kind::Trapdoor) {
            bool powered = hasNeighborSignal(p);
            if (powered == (stateOf(*b, "levitate:powered") != 0)) continue;
            bool wasOpen = stateOf(*b, "levitate:open") != 0;
            set(p, *withStates(*b, {{"levitate:powered", powered}, {"levitate:open", powered}}));
            if (powered != wasOpen) {
                playSound(p, powered ? "open.iron_trapdoor" : "close.iron_trapdoor", 1.f, 0.95f);
                log().info("building: {} at {} {} {} {} by redstone", info->id, p.x, p.y, p.z, powered ? "opened" : "closed");
            }
            continue;
        }
        gone.push_back(p);
    }
    for (auto const& p : gone) tracked().erase(p);
    if (!gone.empty()) trackedDirty() = true;
}

namespace detail {

/// Places the upper half and orients a door just placed at `pos` (Java DoorBlock placement).
void placeDoor(Player& player, GlobalPos const& pos, Vec3 const& hitRel, double hitX, double hitZ) {
    (void)hitRel;
    auto const* b = blockAt(pos);
    if (!isKind(b, Kind::Door)) return;
    auto const& info  = *infoOf(*b);
    auto        upper = offset(pos, Up);
    if (!replaceableAt(upper)) {
        // Java: no placement when the upper half doesn't fit
        clear(pos);
        if (!player.isCreative()) give(player, makeItem(info.id));
        return;
    }
    int facing = horizontalFacing(player);
    // Java DoorBlock.getHinge
    int  ccw = counterClockwise(facing), cw = clockwise(facing);
    auto full = [&](GlobalPos const& q) { return solidAt(q); };
    int  i    = (full(offset(pos, ccw)) ? -1 : 0) + (full(offset(offset(pos, ccw), Up)) ? -1 : 0) + (full(offset(pos, cw)) ? 1 : 0)
          + (full(offset(offset(pos, cw), Up)) ? 1 : 0);
    auto doorAt = [&](GlobalPos const& q) {
        auto const* d = blockAt(q);
        return d && d->getTypeName() == info.id && !stateOf(*d, "levitate:upper");
    };
    bool ccwDoor = doorAt(offset(pos, ccw)), cwDoor = doorAt(offset(pos, cw));
    int  hinge   = 0;
    if ((!ccwDoor || cwDoor) && i <= 0) {
        if ((!cwDoor || ccwDoor) && i >= 0) {
            int    sx = kDx[facing], sz = kDz[facing];
            double d0 = hitX, d1 = hitZ;
            bool left = (sx >= 0 || !(d1 < 0.5)) && (sx <= 0 || !(d1 > 0.5)) && (sz >= 0 || !(d0 > 0.5)) && (sz <= 0 || !(d0 < 0.5));
            hinge     = left ? 0 : 1;
        } else {
            hinge = 0;
        }
    } else {
        hinge = 1;
    }
    auto const* lower = withStates(*b, {{"levitate:facing", facing}, {"levitate:hinge", hinge}, {"levitate:upper", 0}, {"levitate:open", 0}, {"levitate:powered", 0}});
    set(pos, *lower);
    set(upper, *withStates(*lower, {{"levitate:upper", 1}}));
    trackRedstone(pos);
    log().info("building: {} placed at {} {} {} (facing {}, hinge {})", info.id, pos.x, pos.y, pos.z, kDirNames[facing], hinge ? "right" : "left");
}

/// A door half changed: the other half follows (Java updateShape / canSurvive).
void doorHalfChanged(GlobalPos const& pos, Block const& previous, Block const& now, bool creativeBreak) {
    auto const* info = infoOf(previous);
    if (!info || info->kind != Kind::Door) return;
    if (infoOf(now) == info) {
        // a state change of one half (open, powered): mirror it onto the other half
        int  upper = stateOf(now, "levitate:upper");
        auto other = offset(pos, upper ? Down : Up);
        defer([pos, other, upper, id = info->id] {
            auto const* a = blockAt(pos);
            auto const* o = blockAt(other);
            if (!a || !o || a->getTypeName() != id || o->getTypeName() != id || stateOf(*o, "levitate:upper") == upper) return;
            auto const* n = withStates(*o, {{"levitate:open", stateOf(*a, "levitate:open")},
                                            {"levitate:powered", stateOf(*a, "levitate:powered")},
                                            {"levitate:facing", hfacingOf(*a)},
                                            {"levitate:hinge", stateOf(*a, "levitate:hinge")}});
            if (n && n != o) set(other, *n);
        });
        return;
    }
    // the half is gone: remove the other one (the lower half carries the drop)
    int  upper = stateOf(previous, "levitate:upper");
    auto other = offset(pos, upper ? Down : Up);
    defer([other, upper, id = info->id, creativeBreak] {
        auto const* o = blockAt(other);
        if (!o || o->getTypeName() != id || stateOf(*o, "levitate:upper") == upper) return;
        clear(other);
        // breaking the upper half breaks the lower one, which drops the door (Create's loot table)
        if (upper && !creativeBreak) drop(other, makeItem(id));
    });
}

void loadRedstone() {
    tracked().clear();
    if (auto j = game::db::load(kDbRedstone); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) tracked().insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
    trackedDirty() = false;
}
void saveRedstone() {
    if (!trackedDirty() || !game::db::available()) return;
    nlohmann::json j = nlohmann::json::array();
    for (auto const& p : tracked()) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbRedstone, j);
    trackedDirty() = false;
}
void clearRedstone() { tracked().clear(); }

} // namespace detail

} // namespace lv::feature::building
