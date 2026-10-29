// Building unit: states that follow the neighbours (Create's updateShape): connected textures,
// pane arms, stair shapes, girder beams, scaffolding support, ladder / placard survival.
//
// Every block change (any cause) marks the position and its neighbours dirty; the dirty set is
// processed at the start of the next tick, so chains settle within a few ticks.

#include <set>
#include <string>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include "feature/building/Building.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::building {

namespace {

std::set<GlobalPos>& dirty() {
    static std::set<GlobalPos> d;
    return d;
}

bool isPaneLike(Block const& b) {
    if (auto const* i = infoOf(b)) return i->kind == Kind::Pane || i->kind == Kind::Bars;
    std::string const& id = b.getTypeName();
    return id.ends_with("_bars") || id.find("glass_pane") != std::string::npos;
}

bool isWall(Block const& b) {
    std::string const& id = b.getTypeName();
    return id.size() > 5 && id.compare(id.size() - 5, 5, "_wall") == 0;
}

/// Java IronBarsBlock.attachsTo: a sturdy face, another pane / bars, or a wall.
bool paneAttaches(GlobalPos const& p, int dir) {
    auto        np = offset(p, dir);
    auto const* n  = blockAt(np);
    if (!n || n->isAir()) return false;
    if (isPaneLike(*n) || isWall(*n)) return true;
    if (auto const* i = infoOf(*n)) {
        // our full blocks are sturdy; thin ones are not
        switch (i->kind) {
        case Kind::Casing:
        case Kind::CtGlass:
        case Kind::Window:
        case Kind::TiledGlass:
        case Kind::CopperBlock: return true;
        default: return false;
        }
    }
    return sturdy(np, opposite(dir));
}

struct StairView {
    bool stairs = false;
    int  facing = North;
    int  upside = 0;
};
StairView stairAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    if (!b) return {};
    auto const* i = infoOf(*b);
    if (!i || i->kind != Kind::CopperStairs) return {};
    return {true, hfacingOf(*b), stateOf(*b, "levitate:upside")};
}

/// Java StairBlock.getStairsShape (0 straight, 1 inner_left, 2 inner_right, 3 outer_left, 4 outer_right).
int stairShape(GlobalPos const& p, int facing, int upside) {
    auto canTakeShape = [&](int face) {
        auto s = stairAt(offset(p, face));
        return !s.stairs || s.facing != facing || s.upside != upside;
    };
    auto front = stairAt(offset(p, facing));
    if (front.stairs && front.upside == upside && axisOf(front.facing) != axisOf(facing) && canTakeShape(opposite(front.facing))) {
        return front.facing == counterClockwise(facing) ? 3 : 4;
    }
    auto back = stairAt(offset(p, opposite(facing)));
    if (back.stairs && back.upside == upside && axisOf(back.facing) != axisOf(facing) && canTakeShape(back.facing)) {
        return back.facing == counterClockwise(facing) ? 1 : 2;
    }
    return 0;
}

bool isGirderLike(Block const& b) {
    auto const* i = infoOf(b);
    return i && (i->kind == Kind::Girder || i->kind == Kind::GirderShaft);
}

/// The neighbour on `dir` carries a horizontal beam along `dir`'s axis into this girder.
bool beamFrom(GlobalPos const& p, int dir) {
    auto const* n = blockAt(offset(p, dir));
    if (!n) return false;
    auto const* i = infoOf(*n);
    if (!i) return false;
    int axis = axisOf(dir); // 0 x, 2 z
    if (i->kind == Kind::Girder) return stateOf(*n, axis == 0 ? "levitate:x" : "levitate:z") == 1;
    if (i->kind == Kind::GirderShaft) return stateOf(*n, "levitate:axis") != axis; // the beam runs across the shaft
    if (i->kind == Kind::Placard) return stateOf(*n, "levitate:facing") == dir;
    return false;
}

/// Java GirderBlock.isConnected: what keeps an existing beam end.
bool beamHeld(GlobalPos const& p, int dir) {
    auto np = offset(p, dir);
    if (beamFrom(p, dir)) return true;
    if (bracketAt(np) || bracketAt(p)) return true;
    return sturdy(np, opposite(dir));
}

/// Java GirderBlock.updateVerticalProperty: girders, walls, chains, lanterns, attached blocks.
bool verticalAttach(GlobalPos const& p, int dir) {
    auto const* n = blockAt(offset(p, dir));
    if (!n || n->isAir()) return false;
    if (isGirderLike(*n)) return true;
    std::string const& id = n->getTypeName();
    if (isWall(*n)) return true;
    if (id == "minecraft:chain" || id.ends_with("_chain")) return stateOf(*n, "pillar_axis", 0) == 0; // (string state: kept if present)
    if (id == "minecraft:lantern" || id == "minecraft:soul_lantern") return (dir == Down) == (stateOf(*n, "hanging") == 1);
    if (id == kPlacardId) return stateOf(*n, "levitate:facing") == dir;
    return false;
}

void scheduleBreak(GlobalPos const& p, std::string const& id, bool dropsItem) {
    defer([p, id, dropsItem] {
        if (idAt(p) != id) return;
        clear(p);
        if (dropsItem) drop(p, makeItem(id));
        log().info("building: {} at {} {} {} popped off", id, p.x, p.y, p.z);
    });
}

} // namespace

void markDirty(GlobalPos const& p) {
    auto& d = dirty();
    d.insert(p);
    for (int dir = 0; dir < 6; ++dir) d.insert(offset(p, dir));
}

bool updateConnections(GlobalPos const& p) {
    auto const* block = blockAt(p);
    if (!block) return false;
    auto const* info = infoOf(*block);
    if (!info) return false;
    Block const* next = block;
    switch (info->kind) {
    case Kind::Casing:
    case Kind::CtGlass:
    case Kind::Window: {
        std::vector<std::pair<std::string, int>> s;
        for (int d = 0; d < 6; ++d) {
            bool used = info->ct == Ct::Omni || (info->ct == Ct::Horizontal && horizontal(d)) || (info->ct == Ct::Vertical && !horizontal(d));
            if (!used) continue;
            s.emplace_back(std::string("levitate:c_") + kDirNames[d], idAt(offset(p, d)) == info->id ? 1 : 0);
        }
        next = withStates(*block, s);
        break;
    }
    case Kind::Pane:
    case Kind::Bars:
        next = withStates(
            *block,
            {{"levitate:north", paneAttaches(p, North)}, {"levitate:south", paneAttaches(p, South)}, {"levitate:west", paneAttaches(p, West)}, {"levitate:east", paneAttaches(p, East)}}
        );
        break;
    case Kind::CopperStairs: {
        int facing = hfacingOf(*block), upside = stateOf(*block, "levitate:upside");
        next       = withStates(*block, {{"levitate:shape", stairShape(p, facing, upside)}});
        break;
    }
    case Kind::Scaffold: {
        // Create: BOTTOM when the block below is neither this scaffolding nor sturdy on top;
        // distance as vanilla (0 on a sturdy block, else 7: metal scaffolds don't count as vanilla scaffolding)
        auto below  = offset(p, Down);
        bool onSame = idAt(below) == info->id;
        bool firm   = sturdy(below, Up);
        next        = withStates(*block, {{"levitate:bottom", (!onSame && !firm) ? 1 : 0}, {"levitate:distance", firm ? 0 : 7}});
        break;
    }
    case Kind::Girder: {
        int  axis = stateOf(*block, "levitate:axis", 1);
        auto beam = [&](int ax, char const* state, int a, int b) {
            if (axis == ax) return 1;
            if (beamFrom(p, a) || beamFrom(p, b)) return 1;
            // an existing beam stays while something holds one of its ends
            return stateOf(*block, state) == 1 && (beamHeld(p, a) || beamHeld(p, b)) ? 1 : 0;
        };
        int x = beam(0, "levitate:x", West, East), z = beam(2, "levitate:z", North, South);
        auto vert = [&](char const* state, int dir) {
            if (axis == 1 || verticalAttach(p, dir)) return 1;
            return stateOf(*block, state) == 1 && solidAt(offset(p, dir)) ? 1 : 0;
        };
        next = withStates(*block, {{"levitate:x", x}, {"levitate:z", z}, {"levitate:top", vert("levitate:top", Up)}, {"levitate:bottom", vert("levitate:bottom", Down)}});
        break;
    }
    case Kind::GirderShaft: {
        auto vert = [&](char const* state, int dir) {
            if (verticalAttach(p, dir)) return 1;
            return stateOf(*block, state) == 1 && solidAt(offset(p, dir)) ? 1 : 0;
        };
        next = withStates(*block, {{"levitate:top", vert("levitate:top", Up)}, {"levitate:bottom", vert("levitate:bottom", Down)}});
        break;
    }
    case Kind::Ladder: {
        // MetalLadderBlock.canSurvive: a sturdy wall behind, or the same ladder above
        int  facing = hfacingOf(*block);
        if (!blockAt(offset(p, opposite(facing)))) return false; // wall not loaded
        auto above  = blockAt(offset(p, Up));
        bool held   = sturdy(offset(p, opposite(facing)), facing)
                 || (above && above->getTypeName() == info->id && hfacingOf(*above) == facing);
        if (!held) scheduleBreak(p, info->id, true);
        return false;
    }
    case Kind::Placard: {
        int  facing  = stateOf(*block, "levitate:facing", Up);
        auto support = offset(p, opposite(facing));
        if (blockAt(support) && !canHang(support)) scheduleBreak(p, info->id, true);
        if (stateOf(*block, "levitate:powered")) ensurePlacardPulse(p);
        return false;
    }
    default: return false;
    }
    if (!next || next == block) return false;
    set(p, *next);
    return true;
}

namespace detail {
void processDirty() {
    if (dirty().empty()) return;
    auto work = std::move(dirty());
    dirty().clear();
    for (auto const& p : work) updateConnections(p);
}
} // namespace detail

} // namespace lv::feature::building
