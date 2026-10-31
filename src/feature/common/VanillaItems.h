#pragma once

// Items shown inside / on blocks for **vanilla** clients (U54, SPEC §8): Create draws them with
// block entity renderers (the items in a basin, the deployer's held item, the arm's claw item, the
// crafter's slot items). Players with the client mod get display items (feature/itemrender, U33)
// and draw them as Create does; vanilla players get item entities that exist only on their client
// (AddItemActor / MoveActorAbsolute / RemoveActor packets with runtime ids of our own range, never
// a server entity, so nobody can pick them up). Their clients bob and spin them like dropped items.
//
// Units register a source that lists what should be shown now; every 4 ticks the sources are
// asked (only while a vanilla player is online) and the entities of the vanilla players within 64
// blocks are added, moved or removed to match. Whatever a source stops listing disappears, so
// removed blocks need no extra call. Server thread.
//
//     vanillaitems::registerSource([](std::vector<vanillaitems::Shown>& out) {
//         for (auto const& [pos, s] : basins()) out.push_back({pos, 0, s.item, {x, y, z}});
//     });

#include <cstdint>
#include <functional>
#include <vector>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/item/ItemStack.h"

#include "core/GlobalPos.h"

namespace lv::feature::common::vanillaitems {

struct Shown {
    core::GlobalPos owner;    ///< the block showing the item
    int             slot = 0; ///< distinct per item of one block
    ItemStack       stack;
    Vec3            pos{0, 0, 0}; ///< world position of the entity (its feet)
};

using Source = std::function<void(std::vector<Shown>& out)>;
/// Adds a source (call in `install()`; sources live for the process).
void registerSource(Source source);

/// What the sources list now (selftests; asks them even without vanilla players).
[[nodiscard]] std::vector<Shown> collect();
/// Number of item entities currently sent to vanilla players (selftests).
[[nodiscard]] std::size_t sentCount();

} // namespace lv::feature::common::vanillaitems
