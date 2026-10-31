// Kinetic speed sync, server side, over U3's companion protocol (src/feature/net/Net.h).
//
//   server -> client  "kinetics.sync" {"batches": ["<batch>", ...]}   (core/kinetics/SyncCodec.h)
//
// Only companions (players whose client mod completed the handshake) get it. A companion gets a
// full sync of its dimension right after the handshake and again after each dimension change
// (the first batch of a full sync has the reset flag); then, once per tick, the changed speeds
// grouped per chunk. Batches are bundled into messages of at most kMessageChars characters.

#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "core/kinetics/SyncCodec.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/kinetics/KineticsInternal.h"
#include "feature/net/Net.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::kinetics {

namespace k  = core::kinetics;
namespace ks = core::kinetics::sync;

namespace {

/// Characters of batch text per message (well below the protocol's 1 MiB limit).
constexpr std::size_t kMessageChars = 48 * 1024;

struct SyncState {
    std::map<std::int64_t, int>           companions; ///< unique id -> dimension of the last full sync (-1: none)
    std::map<int, std::vector<ks::Entry>> deltas;     ///< per dimension
    bool                                  registered = false;
};

SyncState& state() {
    static SyncState s;
    return s;
}

int sourceFaceOf(GlobalPos const& pos, std::optional<GlobalPos> const& source) {
    if (!source) return -1;
    auto d = k::diff(pos, *source);
    return static_cast<int>(k::nearestFace(d.x, d.y, d.z));
}

std::vector<ks::Entry> entriesOf(int dim) {
    std::vector<ks::Entry> out;
    detail::solver().forEachNode([&](NodeInfo const& n) {
        if (n.pos.dim != dim) return;
        // the same rule as the deltas: overstressed while the effective speed is 0 and the theoretical is not
        out.push_back(ks::Entry{n.pos.x, n.pos.y, n.pos.z, n.speed, sourceFaceOf(n.pos, n.source), false, n.speed == 0 && n.theoreticalSpeed != 0});
    });
    return out;
}

/// Bundles encoded batches into messages.
std::vector<nlohmann::json> bundle(std::vector<std::string> const& batches) {
    std::vector<nlohmann::json> out;
    nlohmann::json              current = nlohmann::json::array();
    std::size_t                 chars   = 0;
    for (auto const& b : batches) {
        if (!current.empty() && chars + b.size() > kMessageChars) {
            out.push_back({{"batches", std::move(current)}});
            current = nlohmann::json::array();
            chars   = 0;
        }
        chars += b.size();
        current.push_back(b);
    }
    if (!current.empty()) out.push_back({{"batches", std::move(current)}});
    return out;
}

void sendAll(Player& player, std::vector<nlohmann::json> const& messages) {
    for (auto const& m : messages) net::send(player, detail::kSyncChannel, m);
}

Player* findPlayer(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

} // namespace

namespace detail {

void syncChanges(std::vector<k::Change> const& changes) {
    auto& s = state();
    if (s.companions.empty()) return;
    for (auto const& c : changes) {
        s.deltas[c.pos.dim].push_back(
            // the effective speed is 0 exactly while the network is overstressed (Solver::effective)
            ks::Entry{c.pos.x, c.pos.y, c.pos.z, c.speed, sourceFaceOf(c.pos, c.source), c.removed, !c.removed && c.speed == 0 && c.theoretical != 0}
        );
    }
}

void syncTick() {
    auto& s = state();
    if (s.companions.empty()) {
        s.deltas.clear();
        return;
    }
    bool enabled = config::get<KineticsConfig>().syncToClients;
    // one batch per chunk per dimension, built once for every receiver
    std::map<int, std::vector<nlohmann::json>> messages;
    for (auto& [dim, entries] : s.deltas) {
        // keep the last change of each position
        std::map<std::tuple<int, int, int>, ks::Entry> last;
        for (auto const& e : entries) last[{e.x, e.y, e.z}] = e;
        std::vector<ks::Entry> merged;
        for (auto const& [p, e] : last) merged.push_back(e);
        std::vector<std::string> texts;
        for (auto const& b : ks::byChunk(dim, std::move(merged), false)) texts.push_back(ks::encode(b));
        messages[dim] = bundle(texts);
    }
    s.deltas.clear();

    for (auto it = s.companions.begin(); it != s.companions.end();) {
        auto* player = findPlayer(it->first);
        if (!player || !net::isCompanion(*player)) {
            it = s.companions.erase(it);
            continue;
        }
        if (enabled) {
            int dim = static_cast<int>(player->getDimensionId());
            if (it->second != dim) {
                it->second = dim;
                sendAll(*player, bundle(fullSyncPayloads(dim)));
            } else if (auto m = messages.find(dim); m != messages.end()) {
                sendAll(*player, m->second);
            }
        }
        ++it;
    }
}

void syncReset() {
    auto& s = state();
    s.deltas.clear();
    for (auto& [uid, dim] : s.companions) dim = -1; // the new world gets a full sync
}

std::vector<std::string> fullSyncPayloads(int dim) {
    std::vector<std::string> out;
    for (auto const& b : ks::byChunk(dim, entriesOf(dim), true)) out.push_back(ks::encode(b));
    return out;
}

int companionCount() { return static_cast<int>(state().companions.size()); }

} // namespace detail

class KineticsSyncModule {
public:
    void install() {
        auto& s = state();
        if (!s.registered) { // the protocol has no removal of join listeners
            s.registered = true;
            net::onCompanionJoined([](Player& player) {
                state().companions[player.getOrCreateUniqueID().rawID] = -1; // full sync on the next tick
                log().info("kinetics: syncing speeds to {}", player.getRealName());
                // Create syncs the server's stress config: the client mod's item tooltips use it
                net::send(player, "kinetics.stress", stressTablesJson());
            });
            // `/create reload`: companions get the new stress tables
            config::onReload([] {
                auto level = ll::service::getLevel();
                if (!level) return;
                auto tables = stressTablesJson();
                for (auto const& [uid, dim] : state().companions) {
                    ActorUniqueID id;
                    id.rawID = uid;
                    if (auto* p = level->getPlayer(id)) net::send(*p, "kinetics.stress", tables);
                }
            });
        }
        mDisconnect = ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDisconnectEvent>(
            [](ll::event::PlayerDisconnectEvent& ev) { state().companions.erase(ev.self().getOrCreateUniqueID().rawID); }
        );
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mDisconnect);
        state().companions.clear();
        state().deltas.clear();
    }

private:
    ll::event::ListenerPtr mDisconnect;
};

} // namespace lv::feature::kinetics

LV_REGISTER_MODULE(lv::feature::kinetics::KineticsSyncModule);
