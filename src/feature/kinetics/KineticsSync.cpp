// Kinetic speed sync, server side: a minimal, self-contained channel to players with the client
// mod, to be absorbed by the companion protocol of U3 (docs/SPEC.md §3.3) when it exists.
//
// Transport: Bedrock's ScriptMessagePacket (id + string value), which both the client and the
// server deserialize and which vanilla clients ignore.
//   client -> server  "levitate:kinetics/hello" <protocol>   (the client mod announces itself)
//   server -> client  "levitate:kinetics/ack"   <protocol>   (the server runs Levitate)
//   server -> client  "levitate:kinetics/sync"  <batch>      (core/kinetics/SyncCodec.h)
// After the hello the player gets a full sync of its dimension (and again after each dimension
// change); then, once per tick, the changed speeds grouped per chunk.

#include <map>
#include <tuple>
#include <string>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/ScriptMessagePacket.h"
#include "mc/platform/UUID.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "core/kinetics/SyncCodec.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/kinetics/KineticsInternal.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::kinetics {

namespace k  = core::kinetics;
namespace ks = core::kinetics::sync;

namespace {

struct Companion {
    int protocol = 0;
    int dim      = -1; ///< dimension of the last full sync (-1: none yet)
};

struct SyncState {
    std::map<std::string, Companion>                       companions; ///< by player UUID
    std::map<int, std::vector<ks::Entry>>                  deltas; ///< per dimension
};

SyncState& state() {
    static SyncState s;
    return s;
}

void send(Player& player, std::string_view id, std::string value) {
    auto pkt = MinecraftPackets::createPacket(MinecraftPacketIds::ScriptMessagePacket);
    if (!pkt) return;
    auto& msg         = static_cast<ScriptMessagePacket&>(*pkt);
    *msg.mMessageId    = std::string(id);
    *msg.mMessageValue = std::move(value);
    msg.sendTo(player);
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
        out.push_back(ks::Entry{n.pos.x, n.pos.y, n.pos.z, n.speed, sourceFaceOf(n.pos, n.source), false});
    });
    return out;
}

void fullSync(Player& player, int dim) {
    for (auto const& b : ks::byChunk(dim, entriesOf(dim), true)) send(player, ks::kSyncId, ks::encode(b));
}

Player* findPlayer(std::string const& uuid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    return level->getPlayer(mce::UUID::fromString(uuid));
}

void onHello(ServerPlayer& player, std::string const& value) {
    int protocol = 0;
    try {
        protocol = std::stoi(value);
    } catch (...) {
        return;
    }
    auto  uuid  = player.getUuid().asString();
    auto& comps = state().companions;
    bool  known = comps.contains(uuid);
    auto& c     = comps[uuid];
    c.protocol  = protocol;
    c.dim       = -1; // full sync on the next tick
    send(player, ks::kAckId, std::to_string(ks::kProtocolVersion));
    if (!known) log().info("kinetics: {} has the client mod (protocol {})", player.getRealName(), protocol);
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    KineticsHelloHook,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::$handle,
    void,
    NetworkIdentifier const&   source,
    ScriptMessagePacket const& packet
) {
    if (*packet.mMessageId == ks::kHelloId) {
        if (auto* player = thisFor<NetEventCallback>()->_getServerPlayer(source, packet.mSenderSubId)) {
            onHello(*player, *packet.mMessageValue);
        }
        return; // ours: not a script event
    }
    origin(source, packet);
}

namespace detail {

void syncChanges(std::vector<k::Change> const& changes) {
    auto& s = state();
    if (s.companions.empty()) return;
    for (auto const& c : changes) {
        s.deltas[c.pos.dim].push_back(
            ks::Entry{c.pos.x, c.pos.y, c.pos.z, c.speed, sourceFaceOf(c.pos, c.source), c.removed}
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
    std::map<int, std::vector<std::string>> payloads;
    for (auto& [dim, entries] : s.deltas) {
        // keep the last change of each position
        std::map<std::tuple<int, int, int>, ks::Entry> last;
        for (auto const& e : entries) last[{e.x, e.y, e.z}] = e;
        std::vector<ks::Entry> merged;
        for (auto const& [p, e] : last) merged.push_back(e);
        for (auto const& b : ks::byChunk(dim, std::move(merged), false)) payloads[dim].push_back(ks::encode(b));
    }
    s.deltas.clear();

    for (auto it = s.companions.begin(); it != s.companions.end();) {
        auto* player = findPlayer(it->first);
        if (!player) {
            it = s.companions.erase(it);
            continue;
        }
        if (enabled) {
            int dim = static_cast<int>(player->getDimensionId());
            if (it->second.dim != dim) {
                it->second.dim = dim;
                fullSync(*player, dim);
            } else if (auto p = payloads.find(dim); p != payloads.end()) {
                for (auto const& text : p->second) send(*player, ks::kSyncId, text);
            }
        }
        ++it;
    }
}

void syncReset() {
    auto& s = state();
    s.deltas.clear();
    for (auto& [uuid, c] : s.companions) c.dim = -1; // the new world gets a full sync
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
        KineticsHelloHook::hook();
        mDisconnect = ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDisconnectEvent>(
            [](ll::event::PlayerDisconnectEvent& ev) { state().companions.erase(ev.self().getUuid().asString()); }
        );
    }
    void uninstall() {
        KineticsHelloHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mDisconnect);
        state().companions.clear();
        state().deltas.clear();
    }

private:
    ll::event::ListenerPtr mDisconnect;
};

} // namespace lv::feature::kinetics

LV_REGISTER_MODULE(lv::feature::kinetics::KineticsSyncModule);
