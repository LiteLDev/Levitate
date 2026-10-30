// Selftest of the station flag sync (U38, dx 3895; testPos shifted back into the run's ticking
// area with dz): the companions are told when a station's flag goes up (assembly mode or a train
// standing there) and down again (Create's StationBlockEntity#flag).

#include <optional>

#include "feature/contraption/U38Test.h"
#include "mc/server/SimulatedPlayer.h"

#include "core/tracks/Shape.h"
#include "feature/net/Net.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "selftest/SelfTest.h"

namespace lv::feature::trains {

using ::lv::feature::contraption::kU38Dz;
std::optional<bool> stationFlagRaised(GlobalPos const& pos); // Stations.cpp
} // namespace lv::feature::trains

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace tn  = lv::feature::trains;
namespace tr  = lv::feature::tracks;
namespace net = lv::feature::net;

GlobalPos at(GlobalPos b, int dx, int dy = 0, int dz = 0) { return {b.dim, b.x + dx, b.y + dy, b.z + dz}; }

/// The last "trains.flags" entry of `pos` the test companion got (-1: none).
int lastFlag(std::vector<lv::core::common::Envelope> const& msgs, GlobalPos const& pos) {
    int out = -1;
    for (auto const& m : msgs) {
        if (m.channel != "trains.flags") continue;
        for (auto const& e : m.payload.value("flags", nlohmann::json::array()))
            if (e.size() >= 7 && e[0] == pos.dim && e[1] == pos.x && e[2] == pos.y && e[3] == pos.z) out = e[4].get<int>();
    }
    return out;
}

} // namespace

LV_REGISTER_SELFTEST("trains.station_flag", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(3895, kU38Dz);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(at(base, 9)), "test area loaded");
    int s = int(lv::core::tracks::Shape::XO);
    std::vector<std::pair<std::string, int>> xo{
        {std::string(tr::kShapeState), s % 16},
        {std::string(tr::kShapeHiState), s / 16},
        {std::string(tr::kTurnState), 0}
    };
    for (int x = 0; x < 10; ++x) ctx.setBlock(at(base, x), std::string(tr::kTrackId), xo);
    co_await ctx.ticks(3);
    auto station = at(base, 5, 0, 1);
    ctx.setBlock(station, std::string(tn::kStation));
    co_await ctx.ticks(2);
    std::string err;
    ctx.require(tn::createStation(station, at(base, 5), 1, 0, &err), "station created: " + err);
    tn::refreshStationPoints();

    auto& player = ctx.player();
    net::setTestCompanion(player, true);
    (void)net::takeTestMessages(player);
    co_await ctx.ticks(4);
    ctx.check(tn::stationFlagRaised(station) == std::optional<bool>(false), "an idle station's flag is down");

    auto* st = tn::stationAt(station);
    ctx.require(st != nullptr, "station known");
    tn::setAssembling(*st, true, nullptr);
    co_await ctx.ticks(4);
    ctx.check(tn::stationFlagRaised(station) == std::optional<bool>(true), "assembly mode raises the flag");
    ctx.check(lastFlag(net::takeTestMessages(player), station) == 1, "companions are told the flag went up");

    if ((st = tn::stationAt(station))) tn::setAssembling(*st, false, nullptr);
    co_await ctx.ticks(4);
    ctx.check(tn::stationFlagRaised(station) == std::optional<bool>(false), "leaving assembly mode lowers it");
    ctx.check(lastFlag(net::takeTestMessages(player), station) == 0, "companions are told the flag went down");

    net::setTestCompanion(player, false);
    tn::removeStation(station);
    co_await ctx.ticks(4);
    ctx.check(!tn::stationFlagRaised(station).has_value(), "a removed station has no flag");
});
