// `/create trains` (Create's train summary) and `/create killtrain <name>` (removes trains by
// name or id; Create 6.0.10's `/create train remove <uuid>`). Op only.

#include "ll/api/command/CommandHandle.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

#include "feature/tracks/Tracks.h"
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
    cmd.overload<KillTrainParams>()
        .text("killtrain")
        .required("name")
        .execute([](Origin& origin, CommandOutput& output, KillTrainParams const& p) {
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
        });
});

} // namespace lv::feature::trains
