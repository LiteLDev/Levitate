// Belts unit: belt speeds for the client mod (companion protocol, channels "belts.*").
//
// The client mod draws belts itself (scrolling band, turning pulleys) and needs their speeds:
// - "belts.query" {dim, pos: [[x, y, z], ...]} (client -> server): speeds of belts it sees;
// - "belts.speed" {dim, speeds: [[x, y, z, rpm], ...]} (server -> client): the answer, and every
//   change of a belt's speed afterwards (batched once per tick).

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "mod/Module.h"

namespace lv::feature::belts {

namespace {

std::map<int, std::vector<std::pair<GlobalPos, float>>>& pending() {
    static std::map<int, std::vector<std::pair<GlobalPos, float>>> p;
    return p;
}

Json entry(GlobalPos const& p, float rpm) { return Json::array({p.x, p.y, p.z, rpm}); }

void flush() {
    auto& p = pending();
    if (p.empty()) return;
    auto batches = std::exchange(p, {});
    auto level   = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& player) {
        if (!net::isCompanion(player)) return true;
        auto it = batches.find(int(player.getDimensionId()));
        if (it == batches.end()) return true;
        Json speeds = Json::array();
        for (auto const& [pos, rpm] : it->second) speeds.push_back(entry(pos, rpm));
        net::send(player, "belts.speed", {{"dim", it->first}, {"speeds", speeds}});
        return true;
    });
}

} // namespace

class BeltSyncModule {
public:
    void install() {
        mListener = kinetics::onSpeedChanged(kBeltId, [](kinetics::SpeedChange const& c) {
            pending()[c.pos.dim].emplace_back(c.pos, c.removed ? 0.f : c.speed);
        });
        net::on("belts.query", [](Player& player, Json const& msg) {
            int  dim  = int(player.getDimensionId());
            Json list = msg.value("pos", Json::array());
            if (!list.is_array()) return;
            Json speeds = Json::array();
            int  n      = 0;
            for (auto const& p : list) {
                if (++n > 512) break; // a client asks for what it sees
                if (!p.is_array() || p.size() != 3) continue;
                GlobalPos pos{dim, p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
                if (idAt(pos) != kBeltId) continue;
                speeds.push_back(entry(pos, kinetics::speedAt(pos)));
            }
            net::send(player, "belts.speed", {{"dim", dim}, {"speeds", speeds}});
        });
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { flush(); });
    }
    void uninstall() {
        kinetics::removeListener(mListener);
        net::off("belts.query");
        ll::event::EventBus::getInstance().removeListener(mTick);
        pending().clear();
    }

private:
    kinetics::ListenerId   mListener = 0;
    ll::event::ListenerPtr mTick;
};

} // namespace lv::feature::belts

LV_REGISTER_MODULE(lv::feature::belts::BeltSyncModule);
