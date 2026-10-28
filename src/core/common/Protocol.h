#pragma once

// Companion protocol envelope (U3, SPEC §3.3), independent of the game.
//
// Every message is a channel name plus a JSON payload. The handshake is:
//   client -> server  "hello"   {"protocol": N, "mod": "<mod version>"}
//   server -> client  "welcome" {"protocol": N, "accepted": true|false}
// A server marks a player as companion only after an accepted hello. The server never sends
// anything to a player that is not a companion, so vanilla clients never see these packets.

#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace lv::core::common {

/// Bumped whenever a message changes incompatibly.
inline constexpr int kProtocolVersion = 1;
/// The oldest client protocol this build still accepts.
inline constexpr int kMinProtocolVersion = 1;

[[nodiscard]] constexpr bool protocolAccepted(int clientVersion) {
    return clientVersion >= kMinProtocolVersion && clientVersion <= kProtocolVersion;
}

struct Envelope {
    std::string    channel;
    nlohmann::json payload;
};

/// Payload text of a packet ("{}" for null payloads).
[[nodiscard]] std::string encodePayload(nlohmann::json const& payload);
/// nullopt when the payload is not valid JSON.
[[nodiscard]] std::optional<nlohmann::json> decodePayload(std::string_view text);

/// Channels are "<unit>.<name>" (lowercase, digits, '_', '.'); at most 64 characters.
[[nodiscard]] bool validChannel(std::string_view channel);

} // namespace lv::core::common
