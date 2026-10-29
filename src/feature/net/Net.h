#pragma once

// Companion protocol (U3, SPEC §3.3): typed JSON messages between the Levitate client mod and
// the Levitate server mod (BDS or the integrated server of a local world).
//
// Handshake: the client mod sends "net.hello" {protocol, mod} when it joins a level (and repeats
// it until answered); the server answers "net.welcome" {protocol, accepted} and, when accepted,
// marks the player as a *companion*. The server never sends to non-companions, so vanilla
// clients never receive our packets. Wire format: two LeviLamina custom packets
// (`LevitateC2SPacket`, `LevitateS2CPacket`) carrying a channel name and a JSON payload.
//
// Server side (server thread):
//     net::on("filter.set", [](Player& p, nlohmann::json const& msg) { ... });   // from companions only
//     if (net::isCompanion(player)) net::send(player, "screen.open", {...});
// Client side (client build, client thread):
//     net::client::on("screen.open", [](nlohmann::json const& msg) { ... });
//     net::client::send("screen.action", {...});
//
// Channels are "<unit>.<name>" (see core/common/Protocol.h); a unit only listens to its own.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/common/Protocol.h"

class Player;

namespace lv::feature::net {

using Json          = nlohmann::json;
using ServerHandler = std::function<void(Player& sender, Json const& payload)>;

/// Handles messages on `channel` sent by companion players. One handler per channel (a second
/// registration replaces the first). Safe at static init and in `install()`.
void on(std::string const& channel, ServerHandler handler);
/// Removes the handler of `channel`.
void off(std::string const& channel);

/// The player runs the Levitate client mod and completed the handshake.
[[nodiscard]] bool isCompanion(Player const& player);
/// The player's protocol version (0 when not a companion).
[[nodiscard]] int companionProtocol(Player const& player);
/// Sends a message to a companion player. Returns false (and sends nothing) for other players.
bool send(Player& player, std::string_view channel, Json const& payload);
/// Called on the server thread after a player completed the handshake.
void onCompanionJoined(std::function<void(Player&)> fn);

/// Testing seam (selftests): marks / unmarks a player (e.g. a SimulatedPlayer) as companion
/// without a handshake; messages "sent" to it are recorded instead of sent.
void setTestCompanion(Player const& player, bool companion);
/// The messages recorded for a test companion (channel, payload), oldest first; cleared on read.
[[nodiscard]] std::vector<core::common::Envelope> takeTestMessages(Player const& player);
/// Delivers a message as if `player` had sent it (selftests).
void injectFromClient(Player& player, std::string_view channel, Json const& payload);
/// Testing seam: `setTestCompanion(player, true)` followed by the `onCompanionJoined` listeners,
/// as if the player had completed the handshake (selftests of units that sync on join).
void joinTestCompanion(Player& player);

#ifdef LV_CLIENT
namespace client {
using Handler = std::function<void(Json const& payload)>;
/// Handles messages from the server on `channel` (client thread).
void on(std::string const& channel, Handler handler);
/// Sends a message to the server. Returns false when not in a level.
bool send(std::string_view channel, Json const& payload);
/// The server accepted our handshake (it runs Levitate with a compatible protocol).
[[nodiscard]] bool connected();
/// Called on the client thread when the server accepts the handshake.
void onConnected(std::function<void()> fn);
} // namespace client
#endif

} // namespace lv::feature::net
