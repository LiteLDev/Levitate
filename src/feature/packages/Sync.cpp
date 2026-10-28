// Client sync of the packages unit: chain conveyors (chains, packages on them), frogport tongue
// and packager tray animations, sent to companion players near them (SPEC §3.3 protocol).
// Vanilla clients see the static block models only (SPEC §8).

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/packages/PackagesInternal.h"

namespace lv::feature::packages {

namespace items = common::items;

namespace {
constexpr double kSyncRange = 96;

Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }
Json v3Json(cp::V3 v) { return Json::array({std::round(v.x * 1000) / 1000, std::round(v.y * 1000) / 1000, std::round(v.z * 1000) / 1000}); }

template <class Fn>
void forCompanionsNear(GlobalPos const& near, double range, Fn&& fn) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (!net::isCompanion(p) || int(p.getDimensionId()) != near.dim) return true;
        auto feet = p.getFeetPos();
        if (centre(near).distanceTo({feet.x, feet.y, feet.z}) <= range) fn(p);
        return true;
    });
}
} // namespace

void broadcast(GlobalPos const& near, std::string const& channel, Json const& payload) {
    forCompanionsNear(near, kSyncRange, [&](Player& p) { net::send(p, channel, payload); });
}

void syncFrog(GlobalPos const& pos, PortState const& s) {
    Json msg{
        {"dim",     pos.dim                     },
        {"pos",     posJson(pos)                },
        {"yaw",     s.passiveYaw                },
        {"deposit", s.depositing                },
        {"active",  s.animating                 },
        {"item",    items::idOf(s.animatedPackage)},
        {"failed",  s.failedLastExport          },
        {"goggles", stateAt(pos, kGoggles).value_or(0) != 0},
    };
    if (s.target)
        if (auto at = targetLocation(pos, *s.target)) msg["target"] = v3Json(*at);
    broadcast(pos, "packages.frog", msg);
}

void syncPackager(GlobalPos const& pos, PackagerState const& s) {
    broadcast(
        pos,
        "packages.packager",
        {
            {"dim",    pos.dim                                                                 },
            {"pos",    posJson(pos)                                                            },
            {"facing", stateAt(pos, kFacing).value_or(1)                                       },
            {"inward", s.animationInward                                                       },
            {"ticks",  s.animationTicks                                                        },
            {"item",   items::idOf(s.animationInward ? s.previouslyUnwrapped : s.held)         },
        }
    );
}

void syncConveyors() {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto all = tracked(Kind::ChainConveyor);
    if (all.empty()) return;
    level->forEachPlayer([&](Player& p) {
        if (!net::isCompanion(p)) return true;
        auto feet = p.getFeetPos();
        int  dim  = p.getDimensionId();
        Json list = Json::array();
        for (auto const& pos : all) {
            if (pos.dim != dim || centre(pos).distanceTo({feet.x, feet.y, feet.z}) > kSyncRange || !isLoaded(pos) || !hasConveyor(pos)) continue;
            auto& s     = conveyor(pos);
            Json  conns = Json::array(), boxes = Json::array();
            prepareStats(pos, s);
            for (auto const& c : s.connections) {
                auto it = s.stats.find(c);
                if (it == s.stats.end()) continue;
                conns.push_back({
                    {"to",     Json::array({c.x, c.y, c.z})                },
                    {"start",  v3Json(it->second.start)                    },
                    {"end",    v3Json(it->second.end)                      },
                    {"start2", v3Json(cp::mirroredStart(it->second, centre(pos)))},
                });
            }
            auto addBox = [&](ChainPackage const& b, double yaw) {
                boxes.push_back({{"id", b.netId}, {"at", v3Json(b.world)}, {"yaw", yaw}, {"item", items::idOf(b.item)}});
            };
            for (auto const& b : s.looping) addBox(b, s.reversed ? b.chainPosition + 180 : b.chainPosition);
            for (auto const& [c, list2] : s.travelling) {
                auto it = s.stats.find(c);
                if (it == s.stats.end()) continue;
                auto   d   = (it->second.end - it->second.start).normalized();
                double yaw = std::atan2(d.x, d.z) * 180.0 / 3.14159265358979 - 90;
                for (auto const& b : list2) addBox(b, yaw);
            }
            list.push_back({{"pos", posJson(pos)}, {"speed", lv::kinetics::speedAt(pos)}, {"chains", conns}, {"boxes", boxes}});
        }
        if (!list.empty()) net::send(p, "packages.conveyors", {{"dim", dim}, {"list", list}});
        return true;
    });
}

void resetState() {
    resetPackagers();
    resetPorts();
    resetConveyors();
}

} // namespace lv::feature::packages
