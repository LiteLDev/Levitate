#include "feature/common/FunnelsExt.h"

#include <unordered_map>

#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

namespace lv::feature::common {

namespace {
template <class Query>
std::unordered_map<std::string, Query>& table() {
    static std::unordered_map<std::string, Query> m;
    return m;
}

Block const* loadedBlock(BlockSource& region, BlockPos const& pos) {
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return nullptr;
    if (!region.hasChunksAt(pos, 0, false)) return nullptr;
    return &region.getBlock(pos);
}
} // namespace

namespace airflow {

void registerFan(std::string const& blockId, FanQuery query) { table<FanQuery>()[blockId] = std::move(query); }
void unregisterFan(std::string const& blockId) { table<FanQuery>().erase(blockId); }

std::optional<Fan> fanAt(BlockSource& region, BlockPos const& pos) {
    auto& t = table<FanQuery>();
    if (t.empty()) return std::nullopt;
    auto const* block = loadedBlock(region, pos);
    if (!block) return std::nullopt;
    auto it = t.find(block->getTypeName());
    if (it == t.end() || !it->second) return std::nullopt;
    return it->second(region, pos, *block);
}

} // namespace airflow

namespace beltlink {

void registerSegment(std::string const& blockId, SegmentQuery query) { table<SegmentQuery>()[blockId] = std::move(query); }
void unregisterSegment(std::string const& blockId) { table<SegmentQuery>().erase(blockId); }

std::optional<Segment> segmentAt(BlockSource& region, BlockPos const& pos) {
    auto& t = table<SegmentQuery>();
    if (t.empty()) return std::nullopt;
    auto const* block = loadedBlock(region, pos);
    if (!block) return std::nullopt;
    auto it = t.find(block->getTypeName());
    if (it == t.end() || !it->second) return std::nullopt;
    return it->second(region, pos, *block);
}

void registerAbove(std::string const& blockId, AboveHandler handler) { table<AboveHandler>()[blockId] = std::move(handler); }

Passing itemEntering(BlockSource& region, BlockPos const& beltPos, ItemStack& stack, int movementFacing) {
    BlockPos above{beltPos.x, beltPos.y + 1, beltPos.z};
    auto const* block = loadedBlock(region, above);
    if (!block) return Passing::Continue;
    auto& t  = table<AboveHandler>();
    auto  it = t.find(block->getTypeName());
    if (it == t.end() || !it->second) return Passing::Continue;
    return it->second(region, above, beltPos, stack, movementFacing);
}

} // namespace beltlink

} // namespace lv::feature::common
