// Rope sync for the client mod: running rope / elevator pulleys draw their rope (or belt) and
// magnet down to the moving structure (Create's AbstractPulleyRenderer). The server sends
// "pistons.rope" {op: "set" | "remove" | "reset", dim, ropes: [[x, y, z, offset, speed, elevator]]}
// to companions when a rope changes (every 3 ticks while moving, Create's lazy sync) and the
// whole list when a companion joins. Vanilla clients see the ropes only once the pulley stops
// (rope blocks), see SPEC §8.

#include <map>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "feature/net/Net.h"
#include "feature/pistons/Pistons.h"

namespace lv::feature::pistons {

namespace {

constexpr char kChannel[] = "pistons.rope";

struct Rope {
    float offset   = 0;
    float speed    = 0;
    bool  elevator = false;
};

struct SyncState {
    std::map<GlobalPos, Rope> ropes;
    std::map<GlobalPos, bool> changed; ///< pos -> still running
    bool                      hooked = false;
};
SyncState& st() {
    static SyncState s;
    return s;
}

Json entry(GlobalPos const& p, Rope const& r) { return Json::array({p.x, p.y, p.z, r.offset, r.speed, r.elevator}); }

void sendTo(Player& player, Json const& msg) { net::send(player, kChannel, msg); }

void broadcast(int dim, Json const& msg) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) == dim && net::isCompanion(p)) sendTo(p, msg);
        return true;
    });
}

void sendAll(Player& player) {
    int  dim = static_cast<int>(player.getDimensionId());
    Json ropes = Json::array();
    for (auto const& [p, r] : st().ropes)
        if (p.dim == dim) ropes.push_back(entry(p, r));
    sendTo(player, {{"op", "reset"}, {"dim", dim}});
    if (!ropes.empty()) sendTo(player, {{"op", "set"}, {"dim", dim}, {"ropes", ropes}});
}

} // namespace

void syncRope(GlobalPos const& pulley, float offset, float speed, bool running, bool elevator) {
    auto& s = st();
    if (!s.hooked) {
        s.hooked = true;
        net::onCompanionJoined([](Player& p) { sendAll(p); });
    }
    if (running) s.ropes[pulley] = Rope{offset, speed, elevator};
    else s.ropes.erase(pulley);
    s.changed[pulley] = running;
}

void syncRopeRemoved(GlobalPos const& pulley) { syncRope(pulley, 0, 0, false, false); }

void tickRopeSync() {
    auto& s = st();
    if (s.changed.empty()) return;
    std::map<int, Json> sets, removes;
    for (auto const& [p, running] : s.changed) {
        if (running) {
            auto it = s.ropes.find(p);
            if (it == s.ropes.end()) continue;
            auto& a = sets[p.dim];
            if (!a.is_array()) a = Json::array();
            a.push_back(entry(p, it->second));
        } else {
            auto& a = removes[p.dim];
            if (!a.is_array()) a = Json::array();
            a.push_back(Json::array({p.x, p.y, p.z}));
        }
    }
    s.changed.clear();
    for (auto const& [dim, a] : sets) broadcast(dim, {{"op", "set"}, {"dim", dim}, {"ropes", a}});
    for (auto const& [dim, a] : removes) broadcast(dim, {{"op", "remove"}, {"dim", dim}, {"ropes", a}});
}

void resetRopeSync() {
    st().ropes.clear();
    st().changed.clear();
}

} // namespace lv::feature::pistons
