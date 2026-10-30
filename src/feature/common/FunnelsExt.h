#pragma once

// Contract extension added by U12 (funnels): hooks between item logistics and the units that own
// fans (U13) and belts (U11), so that none of them includes another's headers. Server thread only.
//
// - airflow: an encased fan registers how it blows; chutes ask the fan above / below them
//   (Create's ChuteBlockEntity.calculatePull / calculatePush).
// - beltlink: a belt registers its segments (movement direction, speed); tunnels and funnels ask
//   for the belt under them. The belt reports every item that enters a segment so that the block
//   above it (a tunnel) can take it, split it or hold it back (Create's
//   BeltTunnelInteractionHandler.flapTunnelsAndCheckIfStuck).
//
// Items going *into* a belt or a depot go through the item capability (Capabilities.h):
// `items::insert(region, beltPos, face, stack, simulate)` where `face` is the side of the belt the
// item comes from (Create's DirectBeltInputBehaviour.handleInsertion with the opposite direction).

#include <functional>
#include <optional>
#include <string>

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"

class Block;
class BlockSource;

namespace lv::feature::common {

namespace airflow {

/// An encased fan as Create's EncasedFanBlockEntity sees it.
struct Fan {
    int   facing = 0; ///< the fan block's FACING (FacingID: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east)
    float speed  = 0; ///< `getSpeed()`: signed RPM of the fan's kinetic block
};
using FanQuery = std::function<std::optional<Fan>(BlockSource&, BlockPos const&, Block const&)>;

/// Registers a fan block id (replaces an earlier query for the same id).
void registerFan(std::string const& blockId, FanQuery query);
void unregisterFan(std::string const& blockId);
/// The fan at `pos` (nullopt when there is none or its chunk isn't loaded).
[[nodiscard]] std::optional<Fan> fanAt(BlockSource& region, BlockPos const& pos);

} // namespace airflow

namespace beltlink {

/// A belt segment (Create's BeltBlockEntity seen from above).
struct Segment {
    int   movementFacing = -1;    ///< horizontal FacingID the items move along (Create's getMovementFacing)
    float speed          = 0;     ///< signed RPM of the belt (0 = stopped)
    bool  horizontal     = true;  ///< not sloped
    bool  casing         = false; ///< encased (tunnels need a casing in Create; placing one adds it)
};
using SegmentQuery = std::function<std::optional<Segment>(BlockSource&, BlockPos const&, Block const&)>;

/// Registers a belt block id (replaces an earlier query for the same id).
void registerSegment(std::string const& blockId, SegmentQuery query);
void unregisterSegment(std::string const& blockId);
/// The belt segment at `pos`, if any.
[[nodiscard]] std::optional<Segment> segmentAt(BlockSource& region, BlockPos const& pos);

/// What happens to a belt item entering a segment with a registered block above it.
enum class Passing {
    Continue, ///< the item goes on (the handler may have reduced `stack`)
    Stuck,    ///< the item waits at the segment border (Create: stuck at the tunnel)
    Taken,    ///< the block above took the whole item (`stack` is now empty)
};
/// `abovePos` = the registered block, `beltPos` = the segment under it, `movementFacing` = the
/// belt's direction of travel.
using AboveHandler =
    std::function<Passing(BlockSource& region, BlockPos const& abovePos, BlockPos const& beltPos, ItemStack& stack, int movementFacing)>;
void registerAbove(std::string const& blockId, AboveHandler handler);

/// Called by the belt (U11) when an item crosses into segment `beltPos`. Without a registered
/// block above the segment: Continue.
[[nodiscard]] Passing itemEntering(BlockSource& region, BlockPos const& beltPos, ItemStack& stack, int movementFacing);

/// (Added by U41) A block registered with `registerAbove` sits on the segment (Create's
/// BeltTunnelBlock.getTunnelOnPosition: dropped item entities don't land on such a segment).
[[nodiscard]] bool hasAbove(BlockSource& region, BlockPos const& beltPos);

/// (Added by U41) Items inserted into a segment from a side (Create's
/// BeltBlockEntity.tryInsertingFromSide): the block above may take them instead of the belt (a
/// distributing brass tunnel). `side` is the direction the item travels. Returns nullopt when the
/// block above leaves the item to the belt, else what it did not take.
using SideInputHandler = std::function<
    std::optional<ItemStack>(BlockSource& region, BlockPos const& abovePos, ItemStack const& stack, int side, bool simulate)>;
void registerAboveSideInput(std::string const& blockId, SideInputHandler handler);
[[nodiscard]] std::optional<ItemStack>
sideInputAbove(BlockSource& region, BlockPos const& beltPos, ItemStack const& stack, int side, bool simulate);

} // namespace beltlink

} // namespace lv::feature::common
