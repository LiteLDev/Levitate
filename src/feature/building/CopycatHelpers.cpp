// Building unit: Create's copycat placement helpers (CopycatPanelBlock / CopycatStepBlock
// PlacementHelper), shared by the server (the placement) and the client mod (the ghost preview,
// src/client/building/CopycatGhost.cpp).
//
// - Panel: a copycat panel used on a copycat panel goes to the free side of it nearest to the
//   clicked point, across its facing (IPlacementHelper.orderedByDistanceExceptAxis), with the
//   same facing.
// - Step: a copycat step used on a copycat step continues the line of steps across its facing
//   (PoleHelper on the axis of facing.getClockWise()): the direction nearest to the clicked point
//   first, past up to `range` (CEquipment.placementAssistRange) steps on that axis, with the same
//   facing and half.

#include <algorithm>
#include <array>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/building/Building.h"
#include "game/Blocks.h"

namespace lv::feature::building {

namespace {

constexpr char kPanelId[] = "levitate:copycat_panel";
constexpr char kStepId[]  = "levitate:copycat_step";

/// Create's IPlacementHelper.orderedByDistance: the directions passing `filter`, nearest to the
/// hit (relative to the block centre) first.
template <class Filter>
std::vector<int> orderedByDistance(double hx, double hy, double hz, Filter const& filter) {
    std::vector<std::pair<double, int>> d;
    for (int dir = 0; dir < 6; ++dir) {
        if (!filter(dir)) continue;
        double ex = kDx[dir] - (hx - .5), ey = kDy[dir] - (hy - .5), ez = kDz[dir] - (hz - .5);
        d.emplace_back(ex * ex + ey * ey + ez * ez, dir);
    }
    std::stable_sort(d.begin(), d.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<int> out;
    for (auto const& [dist, dir] : d) out.push_back(dir);
    return out;
}

BlockPos bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }

} // namespace

std::optional<CopycatOffset> copycatHelperOffset(
    BlockSource&       region,
    GlobalPos const&   pos,
    Block const&       clicked,
    std::string const& heldId,
    double             hx,
    double             hy,
    double             hz,
    int                range
) {
    std::string const& clickedId = clicked.getTypeName();
    int                facing    = game::intState(clicked, "levitate:facing").value_or(Up);
    auto               free      = [&](GlobalPos const& p) {
        BlockPos b = bp(p);
        return b.y >= region.getMinHeight() && b.y < region.getMaxHeight() && region.hasChunksAt(b, 0, false)
            && game::replaceable(region, b);
    };

    if (heldId == kPanelId && clickedId == kPanelId) {
        int axis = axisOf(facing);
        auto dirs = orderedByDistance(hx, hy, hz, [&](int d) { return axisOf(d) != axis && free(offset(pos, d)); });
        if (dirs.empty()) return std::nullopt;
        auto const* block = game::blockWith(kPanelId, {{"levitate:facing", facing}});
        if (!block) return std::nullopt;
        return CopycatOffset{offset(pos, dirs.front()), block};
    }

    if (heldId == kStepId && clickedId == kStepId) {
        int  axis  = axisOf(clockwise(facing)); // PoleHelper's axis: facing.getClockWise()
        int  top   = game::intState(clicked, "levitate:top").value_or(0);
        auto onAxis = [&](GlobalPos const& p) {
            Block const& b = region.getBlock(bp(p));
            return b.getTypeName() == kStepId && axisOf(clockwise(game::intState(b, "levitate:facing").value_or(North))) == axis;
        };
        for (int dir : orderedByDistance(hx, hy, hz, [&](int d) { return axisOf(d) == axis; })) {
            int poles = 0;
            for (GlobalPos q = offset(pos, dir); poles < range + 1 && region.hasChunksAt(bp(q), 0, false) && onAxis(q); q = offset(q, dir)) ++poles;
            if (poles >= range) continue;
            GlobalPos target = offset(pos, dir, poles + 1);
            if (!free(target)) continue;
            auto const* block = game::blockWith(kStepId, {{"levitate:facing", facing}, {"levitate:top", top}});
            if (!block) return std::nullopt;
            return CopycatOffset{target, block};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace lv::feature::building
