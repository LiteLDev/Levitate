#pragma once

// Redstone link networks (U23, SPEC §3.6.23): the contract other units use to send and
// receive on Create's two-item link frequencies without knowing the redstone link block.
//
// Users: the linked controller (U31, a player's buttons transmit on frequency pairs), links on
// contraptions, the lectern controller, ... A *node* is registered with its position, its
// frequency pair and whether it listens (receiver) or transmits. Whenever a node changes, the
// network of nodes with the same pair in the same dimension is re-evaluated around it exactly
// like Create's RedstoneLinkNetworkHandler: the power is the strongest transmission within
// `linkRange` (config `redstone.linkRange`, default 256 blocks) of the changed node, and every
// listener within range of it receives that power through its `onReceive` callback.
//
//     using namespace lv::feature::redstone;
//     auto key = link::keyOf(firstItem, secondItem);            // the frequency pair
//     auto id  = link::add({.pos = pos, .key = key, .transmit = 15});   // a transmitter
//     link::setTransmit(id, 0);                                  // released
//     link::remove(id);                                          // gone (chunk unloaded, ...)
//
// Redstone link blocks are nodes too: they are registered by the redstone unit itself while
// their chunk is loaded. Server thread only. Ids stay valid until `remove` or the world closes
// (every node is dropped when the world closes; re-add on `game::db::onWorldReady`).

#include <cstdint>
#include <functional>
#include <string>

#include "core/GlobalPos.h"
#include "core/redstone/LinkNetwork.h"

class ItemStack;

namespace lv::feature::redstone::link {

using core::redstone::LinkKey;
using core::redstone::LinkId;

/// The frequency of one slot: the item's identity as Create compares it (the item, plus the
/// dye colour of dyed items; count and other data are ignored). "" for an empty stack.
[[nodiscard]] std::string frequencyOf(ItemStack const& stack);
/// The frequency pair of two slots (Create's Couple<Frequency>).
[[nodiscard]] LinkKey keyOf(ItemStack const& first, ItemStack const& last);

struct Node {
    core::GlobalPos pos;               ///< where the node is (range is measured between block positions)
    LinkKey         key;               ///< frequency pair
    bool            listening = false; ///< receiver: gets `onReceive` calls
    int             transmit  = 0;     ///< transmitters: strength 0..15
    /// Receivers: the network power reaching this node changed or was re-evaluated (0..15).
    std::function<void(int power)> onReceive;
};

/// Joins the network of `node.key` and updates it. Returns the node's id.
LinkId add(Node node);
/// Leaves the network (the remaining nodes are updated around it).
void remove(LinkId id);
/// Changes a transmitter's strength and updates the network (Create's notifySignalChange).
void setTransmit(LinkId id, int strength);
/// Changes the frequency pair (leaves the old network, joins the new one).
void setKey(LinkId id, LinkKey key);
/// Switches between receiver and transmitter.
void setListening(LinkId id, bool listening, int transmit = 0);
/// Moves a node (contraptions, players). `update` re-evaluates the network around it.
void move(LinkId id, core::GlobalPos const& pos, bool update = true);

/// The node's last received power (receivers; 0 otherwise).
[[nodiscard]] int received(LinkId id);
/// The power a receiver at `pos` would get on `key` right now.
[[nodiscard]] int powerAt(core::GlobalPos const& pos, LinkKey const& key);
/// Create's hasAnyLoadedPower: something transmits on `key` (any dimension).
[[nodiscard]] bool anyPower(LinkKey const& key);
/// A receiver listens on `key` in dimension `dim` (Create: a LinkBehaviour#isListening in the
/// network of a frequency; the linked controller's advancement).
[[nodiscard]] bool hasListeningLinks(int dim, LinkKey const& key);
/// The configured range in blocks.
[[nodiscard]] int range();

} // namespace lv::feature::redstone::link
