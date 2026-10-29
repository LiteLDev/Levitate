#pragma once

// Display items (U33, SPEC §3.3): items the Levitate client mod draws in the world for blocks
// (Create's block entity renderers draw them: items on belts, in chutes, filter slots, placards,
// basins, ...). Server side, server thread. Shared by every unit that shows items.
//
// Two ways to publish items, both keyed by (owner block, slot):
// - Live items, set every time they change (belts, depots, chutes): `set(owner, slot, item)`,
//   `remove(owner, slot)`, `clear(owner)`. Set an item every tick while it moves with
//   `lerpTicks = 1`: the client interpolates between the updates.
// - Provided items, built on demand from block data (filter slots, placards): a provider per block
//   id (`registerProvider`) is asked when a client mod first sees such a block and whenever the
//   unit calls `refresh(owner)` after a change. Provided items are dropped by themselves when the
//   block changes.
//
// Only companions (players running the client mod, net::isCompanion) within 64 blocks get the
// items ("itemrender.batch"; they also arrive later when a player comes closer). Vanilla clients
// get nothing: units keep their own fallback for them (item entities, name tags, ...). A fallback
// item entity that companions must not see is named in `hideActor`.
//
//     itemrender::DisplayItem d;
//     d.stack = stack;
//     d.at    = {x, y, z};               // world position (block units); see core::itemrender::Style
//     d.style = itemrender::Style::Lying;
//     d.at.scale = .5f;
//     d.lerpTicks = 1;
//     itemrender::set(beltPos, itemId, d);

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mc/world/item/ItemStack.h"

#include "core/GlobalPos.h"
#include "core/itemrender/DisplayItem.h"

class Block;

namespace lv::feature::itemrender {

using core::GlobalPos;
using core::itemrender::Style;
using core::itemrender::Transform;

struct DisplayItem {
    ItemStack    stack;
    Transform    at;                    ///< world position, angles (degrees) and size (blocks)
    Style        style     = Style::Facing;
    float        spin      = 0;     ///< degrees per tick about the item's own up axis (Create's basin)
    bool         pile      = false; ///< extra copies for bigger stacks (Create's belts / depots)
    int          lerpTicks = 0;     ///< ticks the client takes from the previous transform to this one
    std::int64_t hideActor = 0;     ///< runtime id of a fallback entity companions should not draw
};

/// Adds or updates an item. Cheap when nothing changed.
void set(GlobalPos const& owner, std::int64_t slot, DisplayItem const& item);
void remove(GlobalPos const& owner, std::int64_t slot);
/// Removes every item of `owner` (live and provided).
void clear(GlobalPos const& owner);
/// The item at (owner, slot), nullptr if none (selftests).
[[nodiscard]] DisplayItem const* find(GlobalPos const& owner, std::int64_t slot);
/// Number of items of `owner` (all owners when nullptr).
[[nodiscard]] std::size_t count(GlobalPos const* owner = nullptr);

/// Builds the items of a block: `out.emplace_back(slot, item)`.
using Provider = std::function<void(GlobalPos const& owner, Block const& block, std::vector<std::pair<std::int64_t, DisplayItem>>& out)>;
/// A provider for one block id (call in `install()`).
void registerProvider(std::string const& blockId, Provider fn);
/// A provider for every block id `matches` accepts (e.g. every block with filter slots).
void registerProvider(std::function<bool(std::string_view blockId)> matches, Provider fn);
/// Whether some provider draws items for this block id (thread safe; the client uses it to know
/// which blocks to ask about).
[[nodiscard]] bool hasProvider(std::string_view blockId);
/// Re-runs the providers of the block at `owner` (after its data changed). Replaces its provided
/// items; a block without a provider loses them.
void refresh(GlobalPos const& owner);

/// Pose of an item lying on (Style::Lying) / facing out of (Style::Facing) a block face: face ids
/// as Bedrock's FacingID (0 down, 1 up, 2 north, 3 south, 4 west, 5 east). Only the angles are set.
[[nodiscard]] Transform facingFace(int face);

} // namespace lv::feature::itemrender
