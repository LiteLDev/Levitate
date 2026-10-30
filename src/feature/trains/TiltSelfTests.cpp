// Selftest of a train over a tilted track (U48, R18; testPos dx 4830..4869): a line climbs over
// sloped curves and slopes, through a track tilted between two sloped curves (Create's
// TrackBlockEntityTilt), to an upper line. The train runs from A below to B above and
// never jumps: its front point climbs no steeper than the track does.

#include <cmath>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/trains/Motion.h"
#include "core/trains/Schedule.h"
#include "core/tracks/Placement.h"
#include "feature/contraption/Contraption.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace tn = lv::feature::trains;
namespace tr = lv::feature::tracks;
namespace cn = lv::feature::contraption;
namespace kt = lv::core::tracks;
namespace ct = lv::core::trains;

GlobalPos at(GlobalPos b, int dx, int dy = 0, int dz = 0) { return {b.dim, b.x + dx, b.y + dy, b.z + dz}; }
kt::BlockPos3 cp(GlobalPos const& p) { return {p.x, p.y, p.z}; }

std::vector<std::pair<std::string, int>> shapeStates(kt::Shape shape) {
    int s = int(shape);
    return {{std::string(tr::kShapeState), s % 16}, {std::string(tr::kShapeHiState), s / 16}, {std::string(tr::kTurnState), 0}};
}

void clearBox(GlobalPos a, GlobalPos b) {
    auto* region    = tn::regionOf(a.dim);
    auto const* air = lv::game::blockWith("minecraft:air");
    if (!region || !air) return;
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
            for (int z = std::min(a.z, b.z); z <= std::max(a.z, b.z); ++z) {
                BlockPos bp{x, y, z};
                if (region->getBlock(bp).isAir()) continue;
                tr::state().noDrops.insert({a.dim, {x, y, z}});
                lv::game::setBlock(*region, bp, *air);
            }
}

} // namespace

LV_REGISTER_SELFTEST("trains.tilted_ramp", [](Context& ctx) -> ll::coro::CoroTask<> {
    GlobalPos b = at(ctx.testPos(4830, 12), 0, 2);
    ctx.require(co_await ctx.ensureLoaded(b), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(at(b, 30)), "test area loaded (middle)");
    ctx.require(co_await ctx.ensureLoaded(at(b, 60)), "test area loaded (east)");
    clearBox(at(b, -2, -1, -2), at(b, 64, 14, 2));
    co_await ctx.ticks(2);

    // lower line y + 0 (x 0..14), a sloped curve up to a slope (19, 20), a sloped curve to the flat
    // track at 25 (+6) which tilts, a sloped curve to a slope (30, 31), a sloped curve to the upper
    // line (+12, x 36..62). Create's trains can't go from a flat track straight onto a slope block.
    for (int x = 0; x <= 14; ++x) ctx.setBlock(at(b, x), std::string(tr::kTrackId), shapeStates(kt::Shape::XO));
    ctx.setBlock(at(b, 19, 2), std::string(tr::kTrackId), shapeStates(kt::Shape::AE));
    ctx.setBlock(at(b, 20, 3), std::string(tr::kTrackId), shapeStates(kt::Shape::AE));
    GlobalPos m = at(b, 25, 6);
    ctx.setBlock(m, std::string(tr::kTrackId), shapeStates(kt::Shape::XO));
    ctx.setBlock(at(b, 30, 8), std::string(tr::kTrackId), shapeStates(kt::Shape::AE));
    ctx.setBlock(at(b, 31, 9), std::string(tr::kTrackId), shapeStates(kt::Shape::AE));
    for (int x = 36; x <= 62; ++x) ctx.setBlock(at(b, x, 12), std::string(tr::kTrackId), shapeStates(kt::Shape::XO));
    co_await ctx.ticks(3);
    struct Link {
        GlobalPos from;
        kt::Vec3d look;
        GlobalPos to;
    };
    for (auto const& l : {Link{at(b, 14), {1, 0, 0}, at(b, 19, 2)},
                          Link{m, {-1, 0, 0}, at(b, 20, 3)},
                          Link{m, {1, 0, 0}, at(b, 30, 8)},
                          Link{at(b, 36, 12), {-1, 0, 0}, at(b, 31, 9)}}) {
        kt::PlacementRequest req;
        req.from   = kt::selectEnd(cp(l.from), kt::Shape::XO, l.look);
        req.pos2   = cp(l.to);
        req.shape2 = kt::Shape::AE;
        req.look   = l.look;
        auto plan  = kt::planConnection(req, kt::Shape::XO);
        ctx.require(plan.valid && plan.curve && plan.curve->block(0) == cp(l.from), "a sloped curve (" + plan.message + ")");
        tr::addCurve(m.dim, *plan.curve);
    }
    co_await ctx.ticks(3);
    auto angle = tr::tiltAt(m.dim, cp(m));
    ctx.require(angle.has_value(), "the middle track is tilted");

    {
        auto ga = tr::railways().nearestEdge(b.dim, kt::Vec3d{b.x + 8.5, double(b.y), b.z + .5});
        auto gb = tr::railways().nearestEdge(b.dim, kt::Vec3d{b.x + 50.5, double(b.y + 12), b.z + .5});
        ctx.check(ga && gb && ga->graph == gb->graph, "both lines are one graph");
    }
    GlobalPos trackA = at(b, 8), stationA = at(b, 8, 0, 1);
    GlobalPos trackB = at(b, 50, 12), stationB = at(b, 50, 12, 1);
    ctx.setBlock(stationA, std::string(tn::kStation));
    ctx.setBlock(stationB, std::string(tn::kStation));
    co_await ctx.ticks(2);
    std::string err;
    ctx.require(tn::createStation(stationA, trackA, 1, 0, &err), "station A: " + err);
    ctx.require(tn::createStation(stationB, trackB, 1, 0, &err), "station B: " + err);
    tn::stationAt(stationA)->name = "U48 Ramp A";
    tn::stationAt(stationB)->name = "U48 Ramp B";
    tn::refreshStationPoints();

    // a one-bogey carriage at A
    auto* s = tn::stationAt(stationA);
    tn::setAssembling(*s, true, nullptr);
    auto track0 = at(trackA, -1);
    auto msg    = tn::useCasingOnTrack(nullptr, track0, true);
    ctx.check(msg.starts_with("+"), "train casing creates a bogey (" + msg + ")");
    ctx.setBlock(at(track0, 0, 2), std::string(tn::kControls), {{"levitate:facing", 4}}); // on the bogey
    auto id = tn::assemble(*s, nullptr);
    if (!id) ctx.info("assembly error: " + s->lastError.dump());
    ctx.require(id.has_value(), "the train assembles");
    auto* t          = tn::findTrain(*id);
    t->testConductor = true;
    ct::Schedule sch;
    auto         e             = ct::Entry::makeDefault();
    e.instruction.data["Text"] = "U48 Ramp B";
    sch.entries.push_back(e);
    sch.cyclic = false;
    t->runtime.setSchedule(sch, false);

    bool                      arrived = false, crossed = false, smooth = true;
    std::optional<kt::Vec3d>  last;
    double                    worst = 0;
    for (int i = 0; i < 1600 && !arrived; ++i) {
        co_await ctx.ticks(1);
        t = tn::findTrain(*id);
        if (!t) break;
        auto p = ct::positionOf(tr::railways(), t->body.front);
        if (p && last) {
            double dh = std::hypot(p->x - last->x, p->z - last->z);
            double dy = std::abs(p->y - last->y);
            // the track is never steeper than 45 degrees: a jump onto a raised node would be
            if (dy > dh * 1.05 + 1e-3) {
                smooth = false;
                worst  = std::max(worst, dy - dh);
            }
            if (p->x > m.x + .2 && p->x < m.x + .8) {
                crossed = true;
                ctx.check(p->y > m.y && p->y < m.y + 1, "the front point is on the tilted block (y " + std::to_string(p->y - m.y) + ")");
            }
        }
        if (p) last = p;
        arrived = t->currentStation == stationB.key();
    }
    ctx.check(crossed, "the train crossed the tilted track");
    ctx.check(smooth, "the train followed the ramp without jumping (worst " + std::to_string(worst) + ")");
    ctx.check(arrived, "the train arrived at B on the upper line");

    std::vector<std::uint64_t> ids;
    for (auto const& [tid, tt] : tn::state().trains) ids.push_back(tid);
    for (auto tid : ids) {
        auto* tt = tn::findTrain(tid);
        auto  p  = tt ? ct::positionOf(tr::railways(), tt->body.front) : std::nullopt;
        if (!p || (std::abs(p->x - b.x - 30) < 40 && std::abs(p->z - b.z) < 8)) tn::removeTrain(tid);
    }
    cn::clearGlue(b.dim, {b.x + 30, b.y, b.z}, 40);
    tn::removeStation(stationA);
    tn::removeStation(stationB);
    for (auto const& p : {m, at(b, 14), at(b, 36, 12)})
        for (auto const& c : tr::state().curves.curvesAt(p.dim, cp(p))) tr::destroyCurve(p.dim, *c, false, nullptr);
    clearBox(at(b, -2, -1, -2), at(b, 64, 14, 2));
    co_await ctx.ticks(2);
});
