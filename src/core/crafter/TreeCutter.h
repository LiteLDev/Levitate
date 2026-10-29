#pragma once

// Create's TreeCutter (pure C++): which blocks fall when a saw cuts a log, bamboo, cactus, sugar
// cane, kelp or chorus plant. The world is read through a callback describing each block.

#include <cstdint>
#include <functional>
#include <vector>

#include "core/kinetics/Types.h"

namespace lv::core::crafter {

struct TreePos {
    int x = 0, y = 0, z = 0;
    friend bool operator==(TreePos const&, TreePos const&) = default;
    friend auto operator<=>(TreePos const&, TreePos const&) = default;
};

enum class TreeKind : std::uint8_t {
    Other,
    Log,           ///< BlockTags.LOGS, slimy logs, mushroom stems
    Root,          ///< mangrove roots
    Leaf,          ///< leaves (blocks with a leaf "distance")
    VerticalPlant, ///< bamboo, cactus, sugar cane, kelp
    Chorus,        ///< chorus plant / flower
    Attachment,    ///< Create's #tree_attachments (cocoa, vines, bee nests, ...)
};

struct TreeBlock {
    TreeKind kind = TreeKind::Other;
    /// Create's nonDecayingLeafDistance (-1 for every other block): red mushroom block 2, brown
    /// mushroom block 3, wart blocks and weeping vines 3.
    int nonDecaying = -1;
    /// Leaf distance if the world stores one (Java leaves); -1: computed from the logs around
    /// (Bedrock leaves have no distance state).
    int leafDistance = -1;
};

struct Tree {
    std::vector<TreePos> logs, leaves, attachments;
    [[nodiscard]] bool   empty() const { return logs.empty() && leaves.empty() && attachments.empty(); }
};

using TreeReader = std::function<TreeBlock(TreePos const&)>;

/// Create's TreeCutter.findTree for a cut at `pos` (the block being sawn, `broken` its kind).
/// `maxBlocks` bounds the search (safety only; Create has no limit).
[[nodiscard]] Tree findTree(TreeReader const& read, TreePos pos, TreeBlock broken, int maxBlocks = 8192);

/// Java's leaf distance (1..7, 7 = not connected): the shortest path through face-adjacent leaves
/// to a log.
[[nodiscard]] int computeLeafDistance(TreeReader const& read, TreePos pos);

} // namespace lv::core::crafter
