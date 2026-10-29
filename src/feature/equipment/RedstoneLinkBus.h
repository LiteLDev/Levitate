#pragma once

// Redstone link frequency bus (contract-style header added by U31 `equipment`; U23 `redstone`
// had not landed). The linked controller transmits on Create's redstone link frequencies; the
// redstone link network (U23) reads them through this header without either unit including the
// other's code. Server thread only.
//
// A *frequency* is Create's Couple<Frequency>: two item ids (the two frequency slots of a link;
// "" for an empty slot), keyed with `core::equipment::frequencyKey(first, second)`.
//
// Transmitters (the controller, Create's LinkedControllerServerHandler):
//     linkbus::transmit({owner, button}, key, pos, 15);      // every tick while the button is held
//     linkbus::release({owner, button});
// Receivers (U23's redstone link in receive mode, Create's RedstoneLinkNetworkHandler):
//     int power = linkbus::powerAt(key, pos, range);         // strongest transmitter within range
//     linkbus::onChanged([](std::string const& key) { ... }); // a frequency's transmitters changed
// Binding (U23 tells the controller what a link block's frequency is, Create's
// LinkBehaviour.getNetworkKey):
//     linkbus::registerFrequencySource("levitate:redstone_link", [](GlobalPos const& p) { ... });

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/GlobalPos.h"

namespace lv::feature::equipment::linkbus {

/// Who transmits: an owner id (a player's unique id, a lectern position hash) and a channel
/// (the controller button 0..5).
struct TransmitterId {
    std::int64_t owner   = 0;
    int          channel = 0;
    friend auto  operator<=>(TransmitterId const&, TransmitterId const&) = default;
};

struct Transmission {
    TransmitterId   id;
    std::string     frequency; ///< frequencyKey(first, second)
    core::GlobalPos pos;       ///< where the signal comes from
    int             power = 15;
};

/// Starts / refreshes a transmission (kept until `release`).
void transmit(TransmitterId id, std::string const& frequency, core::GlobalPos const& pos, int power = 15);
void release(TransmitterId id);
/// Releases every transmission of an owner.
void releaseOwner(std::int64_t owner);
/// Every active transmission (selftests, U23).
[[nodiscard]] std::vector<Transmission> transmissions();
/// The strongest power on `frequency` from a transmitter within `range` blocks (same dimension;
/// Create: the link's range check is per axis, |dx|,|dy|,|dz| <= range).
[[nodiscard]] int powerAt(std::string const& frequency, core::GlobalPos const& at, int range);
/// Called after the transmitters of a frequency changed.
using ChangeListener = std::function<void(std::string const& frequency)>;
int  onChanged(ChangeListener fn);
void removeListener(int id);

/// The frequency (first, second item ids) of a link block at `pos`, nullopt when unknown.
using FrequencySource = std::function<std::optional<std::pair<std::string, std::string>>(core::GlobalPos const&)>;
void registerFrequencySource(std::string const& blockId, FrequencySource fn);
[[nodiscard]] std::optional<std::pair<std::string, std::string>> frequencyAt(std::string const& blockId, core::GlobalPos const& pos);

/// Drops every transmission (world closed).
void reset();

} // namespace lv::feature::equipment::linkbus
