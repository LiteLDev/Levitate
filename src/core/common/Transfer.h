#pragma once

// Slot arithmetic shared by the item / fluid capabilities (U3), independent of the game.

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lv::core::common {

/// What a slot holds, as seen by an insertion of one particular item.
struct SlotView {
    bool empty     = true;  ///< nothing in the slot
    bool stackable = false; ///< holds the inserted item with the same data (can merge)
    int  count     = 0;
    int  maxCount  = 64;    ///< the slot's limit for the inserted item (min of item and slot limits)
};

/// Plans inserting `amount` items: first merges into matching stacks, then fills empty slots,
/// in slot order (vanilla hopper behaviour). Returns (slot index, items added) pairs.
[[nodiscard]] std::vector<std::pair<int, int>> planInsert(std::span<SlotView const> slots, int amount);
/// Items of `planInsert` that fit.
[[nodiscard]] int plannedTotal(std::vector<std::pair<int, int>> const& plan);

/// Fluid units.
inline constexpr int kBucketMb = 1000;

/// Chunk coordinate of a block coordinate (floor division by 16).
[[nodiscard]] constexpr int chunkOf(int block) { return block >= 0 ? block / 16 : -((-block + 15) / 16); }
/// World DB key of a block data chunk: "lv:common:bd:<dim>:<cx>:<cz>".
[[nodiscard]] std::string blockDataChunkKey(int dim, int cx, int cz);
/// Parses a key suffix "<dim>:<cx>:<cz>" (as `forEachKey` returns it).
[[nodiscard]] bool parseChunkSuffix(std::string_view suffix, int& dim, int& cx, int& cz);
/// Position key inside a chunk document: "x,y,z".
[[nodiscard]] std::string posKey(int x, int y, int z);
[[nodiscard]] bool        parsePosKey(std::string_view key, int& x, int& y, int& z);

} // namespace lv::core::common
