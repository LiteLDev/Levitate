// `/create trains` (Create's train summary), `/create killtrain <name>` / `/create train remove
// <name>` (removes trains by name or id; Create 6.0.10's `/create train remove <uuid>`) and
// `/create train tp <name>` (Create's teleport to a train). Op only.

#include "ll/api/command/CommandHandle.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/contraption/Contraption.h"
#include "feature/tracks/Tracks.h"
#include "game/Blocks.h"
#include "feature/trains/Trains.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::trains {

struct KillTrainParams {
    std::string name;
};

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    using Origin = CommandOrigin const;
    cmd.overload().text("trains").execute([](Origin& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.trains.need_op", "You need operator permission."));
        auto& s = state();
        int   live = 0;
        for (auto const& [id, t] : s.trains) live += t.invalid ? 0 : 1;
        output.success("-+------<< Train Summary: >>------+-");
        output.success("Track Networks: " + std::to_string(tracks::railways().graphs().size()));
        output.success("Stations: " + std::to_string(s.stations.size()));
        output.success("Trains: " + std::to_string(live));
        for (auto const& [id, t] : s.trains)
            if (!t.invalid) output.success(statusLine(t));
        output.success("-+--------------------------------+-");
    });
    // Levitate demo (e2e): on a line laid east of the player (x + 3 .. x + 20) that curves north
    // at x + 29: station "Demo A" at x + 8 (facing east) with a one-carriage train, station
    // "Demo B" on the north branch, and the train scheduled there (demo conductor).
    cmd.overload().text("trains").text("demo").execute([](Origin& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.trains.need_op", "You need operator permission."));
        auto* player = commands::playerOf(origin);
        if (!player) return output.error("players only");
        int  dim  = int(player->getDimensionId());
        auto feet = game::blockPosOf(player->getFeetPos());
        GlobalPos trackA{dim, feet.x + 8, feet.y, feet.z}, blockA{dim, feet.x + 8, feet.y, feet.z + 1};
        GlobalPos trackB{dim, feet.x + 29, feet.y, feet.z - 24}, blockB{dim, feet.x + 30, feet.y, feet.z - 24};
        setBlockAt(blockA, kStation);
        setBlockAt(blockB, kStation);
        std::string err;
        if (!createStation(blockA, trackA, 1, 0, &err) || !createStation(blockB, trackB, 0, -1, &err))
            return output.error("stations: " + err);
        stationAt(blockA)->name = "Demo A";
        stationAt(blockB)->name = "Demo B";
        refreshStationPoints();
        auto& a = *stationAt(blockA);
        setAssembling(a, true, player);
        useCasingOnTrack(nullptr, {dim, trackA.x - 1, trackA.y, trackA.z}, true);
        setBlockAt({dim, trackA.x - 1, trackA.y + 2, trackA.z}, kControls, {{"levitate:facing", 4}});
        auto id = assemble(a, player);
        if (!id) return output.error("assembly: " + a.lastError.dump());
        auto*        t = findTrain(*id);
        ct::Schedule s;
        auto         e = ct::Entry::makeDefault();
        e.instruction.data["Text"] = "Demo B";
        s.entries.push_back(e);
        s.cyclic         = false;
        t->testConductor = true;
        t->runtime.setSchedule(s, false);
        output.success("Train #" + std::to_string(*id) + " heading to Demo B");
    });
    // Levitate demo (e2e, U37): on a straight line laid east of the player (x + 2 .. x + 20 at
    // least), station "Demo Manual" at x + 8 with a one-carriage train (controls and a steam whistle
    // glued on its bogey at x + 7) and no schedule, to be driven by hand.
    cmd.overload().text("trains").text("demo_manual").execute([](Origin& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.trains.need_op", "You need operator permission."));
        auto* player = commands::playerOf(origin);
        if (!player) return output.error("players only");
        int  dim   = int(player->getDimensionId());
        auto feet  = game::blockPosOf(player->getFeetPos());
        GlobalPos track{dim, feet.x + 8, feet.y, feet.z}, block{dim, feet.x + 8, feet.y, feet.z + 1};
        setBlockAt(block, kStation);
        std::string err;
        if (!createStation(block, track, 1, 0, &err)) return output.error("station: " + err);
        stationAt(block)->name = "Demo Manual";
        refreshStationPoints();
        auto& st = *stationAt(block);
        setAssembling(st, true, player);
        GlobalPos bogey{dim, track.x - 1, track.y + 1, track.z};
        useCasingOnTrack(nullptr, {dim, track.x - 1, track.y, track.z}, true);
        setBlockAt({dim, bogey.x, bogey.y + 1, bogey.z}, kControls, {{"levitate:facing", 4}});
        setBlockAt({dim, bogey.x, bogey.y + 2, bogey.z}, "levitate:steam_whistle");
        contraption::addGlue(dim, {{bogey.x, bogey.y, bogey.z}, {bogey.x, bogey.y + 2, bogey.z}});
        auto id = assemble(st, player);
        if (!id) return output.error("assembly: " + st.lastError.dump());
        // where the controls are now (the scenario aims at them)
        auto* t = findTrain(*id);
        if (t && !t->carriages.empty() && !t->carriages[0].controls.empty()) {
            if (auto* con = contraption::find(t->carriages[0].contraption)) {
                auto w = con->toWorld(cc::Vec3d::center(t->carriages[0].controls[0].first));
                log().info("trains: demo train #{} has its controls at {:.2f} {:.2f} {:.2f}", *id, w.x, w.y, w.z);
            }
        }
        output.success("Train #" + std::to_string(*id) + " stands at Demo Manual");
    });
    cmd.overload().text("trains").text("screen").execute([](Origin& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.trains.need_op", "You need operator permission."));
        auto* player = commands::playerOf(origin);
        if (!player) return output.error("players only");
        auto      f    = player->getFeetPos();
        Station*  best = nullptr;
        double    bd   = 16 * 16;
        for (auto& [k, s] : state().stations) {
            double dx = s.pos.x + .5 - f.x, dy = s.pos.y + .5 - f.y, dz = s.pos.z + .5 - f.z;
            if (s.pos.dim == int(player->getDimensionId()) && dx * dx + dy * dy + dz * dz < bd) {
                bd   = dx * dx + dy * dy + dz * dz;
                best = &s;
            }
        }
        if (!best) return output.error("no station nearby");
        openStationScreen(*player, best->pos);
        output.success("opened " + best->name);
    });
    // Create's /create train remove: the carriages vanish (nothing is placed or dropped)
    auto removeByName = [](Origin& origin, CommandOutput& output, KillTrainParams const& p) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.trains.need_op", "You need operator permission."));
        std::vector<std::uint64_t> ids;
        for (auto const& [id, t] : state().trains)
            if (!t.invalid && (t.name == p.name || std::to_string(id) == p.name || "#" + std::to_string(id) == p.name)) ids.push_back(id);
        if (ids.empty()) return output.error(tr("lv.trains.no_train", "No Train with name '{0}' was found", p.name));
        for (auto id : ids) {
            auto name = state().trains[id].name;
            removeTrain(id);
            log().info("trains: train #{} '{}' removed by command", id, name);
            output.success(tr("lv.trains.removed", "Train '{0}' removed successfully", name));
        }
    };
    cmd.overload<KillTrainParams>().text("killtrain").required("name").execute(removeByName);
    cmd.overload<KillTrainParams>().text("train").text("remove").required("name").execute(removeByName);
    // Create's /create train tp <train>: creative or spectator players go 5 blocks above the train
    cmd.overload<KillTrainParams>().text("train").text("tp").required("name").execute([](Origin& origin, CommandOutput& output, KillTrainParams const& p) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.trains.need_op", "You need operator permission."));
        auto* player = commands::playerOf(origin);
        if (!player) return output.error("players only");
        if (!player->isCreative() && !player->isSpectator())
            return output.error(tr("lv.trains.tp_mode", "Can only teleport to train when in Creative or Spectator Mode!"));
        Train* t = nullptr;
        for (auto& [id, train] : state().trains)
            if (!train.invalid && (train.name == p.name || std::to_string(id) == p.name || "#" + std::to_string(id) == p.name)) t = &train;
        if (!t) return output.error(tr("lv.trains.no_train", "No Train with name '{0}' was found", p.name));
        // Create's getPositionInDimension: the first carriage present in the first dimension
        std::optional<cc::Vec3d> at;
        int                      dim = t->dim;
        for (auto const& c : t->carriages)
            if (auto* con = contraption::find(c.contraption)) {
                dim = con->dimension();
                at  = con->toWorld(c.bogeyLocal.empty() ? cc::Vec3d{} : cc::Vec3d::center(c.bogeyLocal.front()));
                break;
            }
        if (!at) return output.error(tr("lv.trains.tp_none", "Unable to teleport to Train. No valid location found"));
        player->teleport(Vec3{float(std::floor(at->x)), float(std::floor(at->y) + 5), float(std::floor(at->z))}, DimensionType(dim));
        log().info("trains: {} teleported to train #{} '{}'", player->getRealName(), t->id, t->name);
        output.success(tr("lv.trains.tp_done", "Teleported to Train '{0}' successfully", t->name));
    });

});

} // namespace lv::feature::trains
