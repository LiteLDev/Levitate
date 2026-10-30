// Tracks unit: tilted tracks between two sloped curves (Create's TrackBlockEntityTilt).
//
// The maths is in core/tracks/Tilt.h. Here: the world side. The smoothing angle is kept in
// BlockData["tracks"]["tilt"] of the tilted track (Create keeps it in the track's block entity);
// the re-shaped curves (raised start, tilted axes, `smoothing` pixels) are the curves in the
// curve store, so the graph, the trains, the client mod's curve renderer and the persistence all
// see the tilted shape. Companions get the angle on "tracks.curves" ({"op":"tilt"}) and draw the
// block turned onto the ramp (src/client/tracks); vanilla clients keep the flat block model.
// Contraptions don't carry the tilt: a moved track loses its curves (Create's transform restores
// them and drops the angle too).

#include <set>

#include "feature/common/BlockData.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "core/tracks/Tilt.h"
#include "mod/Log.h"

namespace lv::feature::tracks {

namespace bd = common::blockdata;

namespace {

core::GlobalPos gp(int dim, ct::BlockPos3 const& p) { return {dim, p.x, p.y, p.z}; }

void setTilt(int dim, ct::BlockPos3 const& pos, std::optional<double> angle) {
    auto at = gp(dim, pos);
    if (angle) {
        bd::edit(at)[kBlockDataKey]["tilt"] = *angle;
    } else if (auto const* d = bd::get(at); d && d->contains(kBlockDataKey)) {
        nlohmann::json copy = *d;
        copy[kBlockDataKey].erase("tilt");
        if (copy[kBlockDataKey].empty()) copy.erase(kBlockDataKey);
        bd::set(at, copy.empty() ? nlohmann::json() : copy);
    }
    syncTilt(dim, pos, angle);
}

/// Puts a re-shaped curve into the store in place of the old one (client sync, fake tracks).
void replaceCurve(int dim, ct::Curve const& curve) {
    auto& s        = state();
    auto  replaced = s.curves.add(dim, curve);
    if (replaced) {
        markCurveChunkDirty(dim, replaced->block(0));
        syncCurveRemoved(dim, *replaced);
        removeFakeTracks(dim, *replaced);
    }
    ct::Curve primary = curve.primary() ? curve : curve.secondary();
    markCurveChunkDirty(dim, primary.block(0));
    syncCurveAdded(dim, primary);
    placeFakeTracks(dim, primary);
    s.railAdded.insert({dim, curve.block(0)});
    s.railAdded.insert({dim, curve.block(1)});
}

} // namespace

std::optional<double> tiltAt(int dim, ct::BlockPos3 const& pos) {
    auto const* d = bd::get(gp(dim, pos));
    if (!d) return std::nullopt;
    auto it = d->find(kBlockDataKey);
    if (it == d->end() || !it->is_object()) return std::nullopt;
    auto t = it->find("tilt");
    if (t == it->end() || !t->is_number()) return std::nullopt;
    return t->get<double>();
}

void updateTilt(int dim, ct::BlockPos3 const& pos, std::optional<ct::TrackBlockView> const& before) {
    auto& s     = state();
    auto  angle = tiltAt(dim, pos);
    auto  view  = world().trackAt(dim, pos);

    // (as Create, the curves change first and the graph then forgets the block as it was: the
    // tracks it re-walks around the block already see the new curves)
    if (angle) {
        // Create's undoSmoothing: only once the block no longer has exactly its two curves
        if (view && view->curves.size() == 2) return;
        std::optional<ct::TrackBlockView> old = before ? before : view;
        for (auto const& c : s.curves.curvesAt(dim, pos)) replaceCurve(dim, ct::restoreToOriginalCurve(*c));
        setTilt(dim, pos, std::nullopt);
        if (old && world().trackAt(dim, pos)) s.rail.onRailRemoved(world(), dim, pos, *old);
        s.railAdded.insert({dim, pos});
        log().info("tracks: the track at {} is flat again", pos.str());
        return;
    }

    if (!view) return;
    auto sm = ct::trySmoothing(pos, view->shape, view->portalLink.has_value(), view->curves);
    if (!sm) return;
    replaceCurve(dim, sm->low);
    replaceCurve(dim, sm->high);
    setTilt(dim, pos, sm->angle);
    s.rail.onRailRemoved(world(), dim, pos, *view);
    log().info("tracks: tilted the track at {} by {:.2f} degrees", pos.str(), sm->angle);
}

std::vector<std::tuple<int, ct::BlockPos3, double>> allTilts() {
    std::vector<std::tuple<int, ct::BlockPos3, double>> out;
    std::set<std::pair<int, ct::BlockPos3>>             seen;
    for (auto const& [dim, c] : state().curves.all()) {
        for (int i = 0; i < 2; ++i) {
            // only a block whose curve leaves it sloped can be tilted
            if (c->spec().axes[i].y == 0) continue;
            if (!seen.insert({dim, c->block(i)}).second) continue;
            if (auto a = tiltAt(dim, c->block(i))) out.emplace_back(dim, c->block(i), *a);
        }
    }
    return out;
}

} // namespace lv::feature::tracks
