// Companion protocol: packets, handshake and channel dispatch (see Net.h).

#include "feature/net/Net.h"

#include <chrono>
#include <map>
#include <mutex>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/network/NetEventCallback.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#ifdef LV_CLIENT
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#endif

#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::net {

using core::common::decodePayload;
using core::common::encodePayload;
using core::common::Envelope;

namespace {
constexpr std::uint64_t kMaxChannel = 64;
constexpr std::uint64_t kMaxPayload = 1 << 20; ///< 1 MiB of JSON per message

/// Bedrock::Result error from a failed read, carried through.
template <class R>
Bedrock::Result<void> forward(R const& r) {
    return nonstd::make_unexpected(r.error());
}

void writeEnvelope(BinaryStream& s, std::string const& channel, std::string const& payload) {
    s.writeString(channel, nullptr, nullptr);
    s.writeString(payload, nullptr, nullptr);
}

Bedrock::Result<void> readEnvelope(ReadOnlyBinaryStream& s, std::string& channel, std::string& payload) {
    auto c = s.getString(kMaxChannel);
    if (!c) return forward(c);
    auto p = s.getString(kMaxPayload);
    if (!p) return forward(p);
    channel = std::move(*c);
    payload = std::move(*p);
    return {};
}
} // namespace

/// Client -> server.
class LevitateC2SPacket : public ll::network::PacketBase<LevitateC2SPacket> {
public:
    std::string channel;
    std::string payload;

    void                  write(BinaryStream& s) const override { writeEnvelope(s, channel, payload); }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& s) override { return readEnvelope(s, channel, payload); }
};

/// Server -> client.
class LevitateS2CPacket : public ll::network::PacketBase<LevitateS2CPacket> {
public:
    std::string channel;
    std::string payload;

    void                  write(BinaryStream& s) const override { writeEnvelope(s, channel, payload); }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& s) override { return readEnvelope(s, channel, payload); }
};

// ---- server side -------------------------------------------------------------------------------

namespace {
struct ServerState {
    std::unordered_map<std::string, ServerHandler>      handlers;
    std::unordered_map<std::int64_t, int>               companions; ///< unique id -> protocol
    std::unordered_map<std::int64_t, std::vector<Envelope>> testInbox; ///< test companions
    std::vector<std::function<void(Player&)>>           joined;
};
ServerState& server() {
    static ServerState s;
    return s;
}
std::int64_t keyOf(Player const& p) { return p.getOrCreateUniqueID().rawID; }

void dispatchServer(Player& player, std::string const& channel, Json const& payload) {
    auto& s  = server();
    auto  it = s.handlers.find(channel);
    if (it == s.handlers.end()) {
        log().debug("net: no handler for '{}' from {}", channel, player.getRealName());
        return;
    }
    try {
        it->second(player, payload);
    } catch (std::exception const& e) {
        log().error("net: handler '{}' failed: {}", channel, e.what());
    }
}

void handleHello(Player& player, Json const& payload) {
    int  protocol = payload.value("protocol", 0);
    bool accepted = core::common::protocolAccepted(protocol);
    auto mod      = payload.value("mod", std::string{});
    auto& s       = server();
    bool  known   = s.companions.contains(keyOf(player));
    if (accepted) s.companions[keyOf(player)] = protocol;
    else s.companions.erase(keyOf(player));

    LevitateS2CPacket pkt;
    pkt.channel = "net.welcome";
    pkt.payload = encodePayload({
        {"protocol", core::common::kProtocolVersion},
        {"accepted", accepted                      },
    });
    pkt.sendTo(player);
    if (!accepted) {
        log().warn(
            "companion {} uses protocol {} (server speaks {}..{}); using vanilla fallbacks",
            player.getRealName(),
            protocol,
            core::common::kMinProtocolVersion,
            core::common::kProtocolVersion
        );
        return;
    }
    if (known) return; // a repeated hello (the welcome crossed it)
    log().info("companion joined: {} (protocol {}, client {})", player.getRealName(), protocol, mod);
    for (auto const& fn : s.joined) fn(player);
}
} // namespace

class LevitateC2SHandler : public ll::network::PacketHandlerBase<LevitateC2SHandler, LevitateC2SPacket> {
public:
    void handlePacket(NetworkIdentifier const& id, NetEventCallback& callback, LevitateC2SPacket const& packet) const {
        auto* player = static_cast<ServerNetworkHandler&>(callback)._getServerPlayer(id, packet.mSenderSubId);
        if (!player) return;
        if (!core::common::validChannel(packet.channel)) return;
        auto payload = decodePayload(packet.payload);
        if (!payload) {
            log().warn("net: unreadable '{}' message from {}", packet.channel, player->getRealName());
            return;
        }
        if (packet.channel == "net.hello") return handleHello(*player, *payload);
        if (!isCompanion(*player)) return; // handshake first
        dispatchServer(*player, packet.channel, *payload);
    }
};

void on(std::string const& channel, ServerHandler handler) { server().handlers[channel] = std::move(handler); }
void off(std::string const& channel) { server().handlers.erase(channel); }

bool isCompanion(Player const& player) { return server().companions.contains(keyOf(player)); }

int companionProtocol(Player const& player) {
    auto it = server().companions.find(keyOf(player));
    return it == server().companions.end() ? 0 : it->second;
}

bool send(Player& player, std::string_view channel, Json const& payload) {
    auto& s = server();
    if (!s.companions.contains(keyOf(player))) return false;
    if (auto it = s.testInbox.find(keyOf(player)); it != s.testInbox.end()) {
        it->second.push_back({std::string(channel), payload});
        return true;
    }
    LevitateS2CPacket pkt;
    pkt.channel = std::string(channel);
    pkt.payload = encodePayload(payload);
    pkt.sendTo(player);
    return true;
}

void onCompanionJoined(std::function<void(Player&)> fn) { server().joined.push_back(std::move(fn)); }

void setTestCompanion(Player const& player, bool companion) {
    auto& s = server();
    if (companion) {
        s.companions[keyOf(player)] = core::common::kProtocolVersion;
        s.testInbox[keyOf(player)];
    } else {
        s.companions.erase(keyOf(player));
        s.testInbox.erase(keyOf(player));
    }
}

std::vector<Envelope> takeTestMessages(Player const& player) {
    auto& s  = server();
    auto  it = s.testInbox.find(keyOf(player));
    if (it == s.testInbox.end()) return {};
    return std::exchange(it->second, {});
}

void injectFromClient(Player& player, std::string_view channel, Json const& payload) {
    if (!isCompanion(player)) return;
    dispatchServer(player, std::string(channel), payload);
}

// ---- client side -------------------------------------------------------------------------------

#ifdef LV_CLIENT
namespace client {
namespace {
struct ClientState {
    std::unordered_map<std::string, Handler> handlers;
    std::vector<std::function<void()>>       connectedListeners;
    bool                                     inLevel   = false;
    bool                                     connected = false;
    int                                      helloSent = 0;
    int                                      ticks     = 0;
};
ClientState& state() {
    static ClientState s;
    return s;
}
constexpr int kHelloInterval = 40; ///< ticks between hello retries
constexpr int kHelloAttempts = 15;

void sendHello() {
    LevitateC2SPacket pkt;
    pkt.channel = "net.hello";
    pkt.payload = encodePayload({
        {"protocol", core::common::kProtocolVersion},
        {"mod",      "Levitate"                    },
    });
    pkt.sendToServer();
    ++state().helloSent;
}
} // namespace

void on(std::string const& channel, Handler handler) { state().handlers[channel] = std::move(handler); }

bool send(std::string_view channel, Json const& payload) {
    if (!state().inLevel) return false;
    LevitateC2SPacket pkt;
    pkt.channel = std::string(channel);
    pkt.payload = encodePayload(payload);
    pkt.sendToServer();
    return true;
}

bool connected() { return state().connected; }

void onConnected(std::function<void()> fn) { state().connectedListeners.push_back(std::move(fn)); }

} // namespace client

class LevitateS2CHandler : public ll::network::PacketHandlerBase<LevitateS2CHandler, LevitateS2CPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, LevitateS2CPacket const& packet) const {
        auto payload = decodePayload(packet.payload);
        if (!payload) return;
        auto& s = client::state();
        if (packet.channel == "net.welcome") {
            bool accepted = payload->value("accepted", false);
            if (accepted && !s.connected) {
                s.connected = true;
                log().info("companion: server accepted the handshake (protocol {})", payload->value("protocol", 0));
                for (auto const& fn : s.connectedListeners) fn();
            } else if (!accepted) {
                s.helloSent = client::kHelloAttempts; // stop retrying
                log().warn("companion: the server refused protocol {}", core::common::kProtocolVersion);
            }
            return;
        }
        auto it = s.handlers.find(packet.channel);
        if (it == s.handlers.end()) return;
        try {
            it->second(*payload);
        } catch (std::exception const& e) {
            log().error("net: client handler '{}' failed: {}", packet.channel, e.what());
        }
    }
};
#endif

// ---- module ------------------------------------------------------------------------------------

class NetModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& ev) {
            auto& s = server();
            s.companions.erase(keyOf(ev.self()));
            s.testInbox.erase(keyOf(ev.self()));
        }));
        game::db::onWorldClosed([] {
            server().companions.clear();
            server().testInbox.clear();
        });
    }
#ifdef LV_CLIENT
    // client listeners at mod load, before the game's threads run (see ScreenModule)
    void load() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientJoinLevelEvent>([](auto&) {
            auto& s     = client::state();
            s.inLevel   = true;
            s.connected = false;
            s.helloSent = 0;
            s.ticks     = 0;
            client::sendHello();
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto& s     = client::state();
            s.inLevel   = false;
            s.connected = false;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) {
            auto& s = client::state();
            if (!s.inLevel || s.connected || s.helloSent >= client::kHelloAttempts) return;
            if (++s.ticks % client::kHelloInterval == 0) client::sendHello();
        }));
    }
#endif
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::feature::net

LV_REGISTER_MODULE_ORDERED(lv::feature::net::NetModule, lv::ModuleOrder::Net);
