// `/create kinetics <pos>` (op / console): dumps the kinetic block at a position and its network
// (speed, source, stress, capacity, overstress, sources).
// `/create kinetics <pos> speed <rpm>` (op / console, Levitate debug): sets a generator's speed.

#include <climits>
#include <cmath>
#include <cstdio>

#include "ll/api/command/CommandHandle.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/kinetics/KineticsInternal.h"
#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::kinetics {

// Command parameter types need external linkage.
struct KineticsCommandParams {
    CommandPosition pos{};
};
struct KineticsSpeedParams {
    CommandPosition pos{};
    float           rpm = 0;
};

namespace {

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

std::string at(GlobalPos const& p) {
    return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z);
}

template <class... Args>
std::string text(Player const* viewer, std::string_view key, std::string_view fallback, Args const&... args) {
    return viewer ? trFor(*viewer, key, fallback, args...) : tr(key, fallback, args...);
}

} // namespace

namespace detail {

std::vector<std::string> describe(GlobalPos const& pos, Player const* viewer) {
    std::vector<std::string> out;
    auto                     n = solver().node(pos);
    if (!n) {
        out.push_back(text(viewer, "lv.kinetics.cmd.none", "No kinetic block at {0}.", at(pos)));
        return out;
    }
    auto rpm = text(viewer, "create.generic.unit.rpm", "RPM");
    auto su  = text(viewer, "create.generic.unit.stress", "su");
    out.push_back(text(viewer, "lv.kinetics.cmd.block", "{0} at {1}", n->state.id, at(pos)));
    out.push_back(text(
        viewer,
        "lv.kinetics.cmd.speed",
        "Speed: {0} {1} (theoretical {2} {1})",
        num(n->speed),
        rpm,
        num(n->theoreticalSpeed)
    ));
    if (n->generator) {
        out.push_back(text(viewer, "lv.kinetics.cmd.generated", "Generating: {0} {1}", num(n->generatedSpeed), rpm));
    }
    out.push_back(
        n->source ? text(viewer, "lv.kinetics.cmd.source", "Source: {0}", at(*n->source))
                  : text(viewer, "lv.kinetics.cmd.no_source", "Source: none")
    );
    out.push_back(text(
        viewer,
        "lv.kinetics.cmd.stress",
        "Stress impact: {0} {1}/{2}, capacity: {3} {1}/{2}",
        num(n->impact),
        su,
        rpm,
        num(n->capacity)
    ));
    auto net = solver().networkAt(pos);
    if (!net) {
        out.push_back(text(viewer, "lv.kinetics.cmd.no_network", "Not part of a network."));
        return out;
    }
    out.push_back(text(
        viewer,
        "lv.kinetics.cmd.network",
        "Network {0}: {1} blocks, {2} sources",
        at(net->id),
        net->members,
        net->sources
    ));
    auto usage = text(viewer, "lv.kinetics.cmd.usage", "Stress: {0} / {1} {2}", num(net->stress), num(net->capacity), su);
    if (net->overstressed) usage += " - " + text(viewer, "create.gui.stressometer.overstressed", "Overstressed");
    out.push_back(usage);
    return out;
}

} // namespace detail

namespace {
void runCommand(CommandOrigin const& origin, CommandOutput& output, KineticsCommandParams const& params) {
    if (!commands::isAdmin(origin)) return output.error("You need operator permission.");
    Vec3     v   = params.pos.getPosition(INT_MAX, origin, Vec3{0.f, 0.f, 0.f});
    BlockPos bp  = game::blockPosOf(v);
    auto*    dim = origin.getDimension();
    auto     pos = game::globalPos(dim ? static_cast<int>(dim->getDimensionId()) : 0, bp);
    auto*    viewer = commands::playerOf(origin);
    auto     lines  = detail::describe(pos, viewer);
    for (auto const& line : lines) output.success(line);
    if (!viewer) {
        for (auto const& line : lines) log().info("kinetics: {}", line);
    }
}

void runSpeed(CommandOrigin const& origin, CommandOutput& output, KineticsSpeedParams const& params) {
    if (!commands::isAdmin(origin)) return output.error("You need operator permission.");
    Vec3  v   = params.pos.getPosition(INT_MAX, origin, Vec3{0.f, 0.f, 0.f});
    auto* dim = origin.getDimension();
    auto  pos = game::globalPos(dim ? static_cast<int>(dim->getDimensionId()) : 0, game::blockPosOf(v));
    auto  n   = nodeAt(pos);
    if (!n || !n->generator) {
        return output.error(tr("lv.kinetics.cmd.not_generator", "No generator at {0}.", at(pos)));
    }
    setGeneratedSpeed(pos, params.rpm);
    auto line = tr("lv.kinetics.cmd.speed_set", "Generator at {0} now turns at {1} {2}.", at(pos), num(params.rpm), tr("create.generic.unit.rpm", "RPM"));
    log().info("kinetics: {}", line);
    output.success(line);
}
} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload<KineticsCommandParams>().text("kinetics").required("pos").execute(
        [](CommandOrigin const& origin, CommandOutput& output, KineticsCommandParams const& params) {
            runCommand(origin, output, params);
        }
    );
    cmd.overload<KineticsSpeedParams>().text("kinetics").required("pos").text("speed").required("rpm").execute(
        [](CommandOrigin const& origin, CommandOutput& output, KineticsSpeedParams const& params) {
            runSpeed(origin, output, params);
        }
    );
});

} // namespace lv::feature::kinetics