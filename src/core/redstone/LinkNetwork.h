#pragma once

// Redstone link networks (Create's RedstoneLinkNetworkHandler), pure C++ (U23).
//
// A *node* is anything that sends or receives on a frequency pair: a redstone link block, a
// linked controller held by a player, a link on a contraption. Nodes with the same pair of
// frequency keys in the same dimension form a network. When a node changes (added, removed,
// its transmitted strength changed) the network is re-evaluated *around that node*, exactly
// like Create: the power is the strongest transmission among the nodes within range of the
// changed node, and every listener within range of the changed node receives it.
//
// The world adapter turns the deliveries into received strengths (block states, controller
// feedback); this class never touches the game.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lv::core::redstone {

/// The frequency pair: Create's Couple<Frequency>. A key is the item identity of a frequency
/// slot ("minecraft:diamond", "" when empty, plus the dye colour for dyed items).
struct LinkKey {
    std::string first;
    std::string last;
    friend auto operator<=>(LinkKey const&, LinkKey const&) = default;
};

using LinkId = std::uint64_t;

struct LinkNode {
    int     dim = 0;
    int     x = 0, y = 0, z = 0;
    LinkKey key;
    bool    listening = false; ///< receiver
    int     transmit  = 0;     ///< transmitted strength (transmitters), 0..15
};

/// A received strength for a listening node.
struct Delivery {
    LinkId id    = 0;
    int    power = 0;
    friend bool operator==(Delivery const&, Delivery const&) = default;
};

class LinkNetwork {
public:
    /// Create's `linkRange` (blocks, default 256): nodes are in range when the squared distance
    /// of their block positions is below range².
    void               setRange(int range) { mRange = range; }
    [[nodiscard]] int  range() const { return mRange; }

    /// Adds a node (Create's addToNetwork) and updates its network.
    [[nodiscard]] std::pair<LinkId, std::vector<Delivery>> add(LinkNode node);
    /// Removes a node (removeFromNetwork); the rest of its network is updated around it.
    [[nodiscard]] std::vector<Delivery> remove(LinkId id);
    /// Re-evaluates the network around a node (updateNetworkOf), e.g. after `setTransmit`.
    [[nodiscard]] std::vector<Delivery> update(LinkId id);

    /// Changes the transmitted strength and updates the network (Create's notifySignalChange).
    [[nodiscard]] std::vector<Delivery> setTransmit(LinkId id, int strength);
    /// Changes the frequency pair: leaves the old network and joins the new one (setFrequency).
    [[nodiscard]] std::vector<Delivery> setKey(LinkId id, LinkKey key);
    /// Switches between transmitter and receiver (keeps the frequencies).
    [[nodiscard]] std::vector<Delivery> setListening(LinkId id, bool listening, int transmit);
    /// Moves a node (contraptions, controllers); no update (call `update` when wanted).
    void move(LinkId id, int dim, int x, int y, int z);

    [[nodiscard]] LinkNode const* node(LinkId id) const;
    [[nodiscard]] std::vector<LinkId> members(int dim, LinkKey const& key) const;
    /// The power a listener at (dim, x, y, z) would receive on `key` (no state change).
    [[nodiscard]] int powerAt(int dim, LinkKey const& key, int x, int y, int z) const;
    /// Create's hasAnyLoadedPower: some node on `key` (any dimension) transmits.
    [[nodiscard]] bool anyPower(LinkKey const& key) const;
    [[nodiscard]] std::size_t size() const { return mNodes.size(); }
    void clear();

    [[nodiscard]] static bool withinRange(LinkNode const& a, LinkNode const& b, int range);

private:
    using NetKey = std::pair<int, LinkKey>;
    std::vector<Delivery> evaluate(LinkNode const& actor, std::optional<LinkId> actorId);

    int                                     mRange  = 256;
    LinkId                                  mNextId = 1;
    std::map<LinkId, LinkNode>              mNodes;
    std::map<NetKey, std::vector<LinkId>>   mNetworks; ///< insertion order (Create: LinkedHashSet)
};

} // namespace lv::core::redstone
