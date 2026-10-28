#pragma once

// Capabilities (U3, SPEC §3.2): how machines, funnels, pipes, arms, ... talk to the blocks
// around them without knowing their units. Server thread only.
//
// A *handler* is looked up by the block at a position: first the handler registered for the
// exact block id, then the providers (generic adapters such as vanilla containers). `face` is
// the side of the target block being accessed (Bedrock FacingID: 0 down, 1 up, 2 north,
// 3 south, 4 west, 5 east; -1 = no particular side), like Create's `Direction side`.
//
//     items::registerBlock("levitate:depot", items::Handler{...});
//     auto rest = items::insert(region, pos, 1, stack, /*simulate*/ false);
//     auto got  = items::extract(region, pos, 0, filterPredicate, 64, /*simulate*/ true);

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"

#include "core/common/Recipe.h"

class Block;
class BlockSource;

namespace lv::feature::common {

/// Where a capability is accessed.
struct Access {
    BlockSource& region;
    BlockPos     pos;
    int          face = -1;
};

// ---- items -------------------------------------------------------------------------------------
namespace items {

using ItemPredicate = std::function<bool(ItemStack const&)>;
/// Accepts every item.
[[nodiscard]] ItemPredicate any();

struct Handler {
    /// Number of slots visible from `face`.
    std::function<int(Access const&)> slots;
    /// A copy of a slot's contents.
    std::function<ItemStack(Access const&, int slot)> getSlot;
    /// Inserts `stack`; returns what did not fit (empty stack = everything fit).
    std::function<ItemStack(Access const&, ItemStack const& stack, bool simulate)> insert;
    /// Extracts up to `amount` items of one kind matching `filter`; returns them.
    std::function<ItemStack(Access const&, ItemPredicate const& filter, int amount, bool simulate)> extract;
};

/// Handler for one block id (replaces an earlier one).
void registerBlock(std::string const& blockId, Handler handler);
/// Generic provider, tried in registration order when no block id handler exists.
using Provider = std::function<std::optional<Handler>(Access const&, Block const&)>;
void registerProvider(Provider provider);

[[nodiscard]] std::optional<Handler> find(Access const& at);
/// Convenience wrappers (no handler: insert returns `stack`, extract returns empty).
[[nodiscard]] ItemStack insert(BlockSource& region, BlockPos const& pos, int face, ItemStack const& stack, bool simulate);
[[nodiscard]] ItemStack
extract(BlockSource& region, BlockPos const& pos, int face, ItemPredicate const& filter, int amount, bool simulate);
[[nodiscard]] int  slotCount(BlockSource& region, BlockPos const& pos, int face);
[[nodiscard]] bool has(BlockSource& region, BlockPos const& pos, int face = -1);

} // namespace items

// ---- fluids ------------------------------------------------------------------------------------
namespace fluids {

/// A fluid amount: `fluid` is an id ("minecraft:water", "minecraft:lava", "levitate:honey",
/// "minecraft:powder_snow", ...), `amount` in mB (1 bucket = 1000 mB).
struct FluidStack {
    std::string fluid;
    int         amount = 0;
    [[nodiscard]] bool empty() const { return fluid.empty() || amount <= 0; }
    friend bool        operator==(FluidStack const&, FluidStack const&) = default;
};

struct Tank {
    FluidStack contents;
    int        capacity = 0;
};

struct Handler {
    std::function<std::vector<Tank>(Access const&)> tanks;
    /// Fills up to `stack.amount`; returns the amount accepted.
    std::function<int(Access const&, FluidStack const& stack, bool simulate)> fill;
    /// Drains up to `request.amount` of `request.fluid` ("" = any fluid); returns what was drained.
    std::function<FluidStack(Access const&, FluidStack const& request, bool simulate)> drain;
};

void registerBlock(std::string const& blockId, Handler handler);
using Provider = std::function<std::optional<Handler>(Access const&, Block const&)>;
void registerProvider(Provider provider);

[[nodiscard]] std::optional<Handler> find(Access const& at);
[[nodiscard]] int fill(BlockSource& region, BlockPos const& pos, int face, FluidStack const& stack, bool simulate);
[[nodiscard]] FluidStack drain(BlockSource& region, BlockPos const& pos, int face, FluidStack const& request, bool simulate);
[[nodiscard]] std::vector<Tank> tanks(BlockSource& region, BlockPos const& pos, int face = -1);

/// The fluid held by a filled container item ("minecraft:water_bucket" -> water 1000 mB, a
/// water bottle -> water 250 mB, ...) or an empty stack.
[[nodiscard]] FluidStack fluidInItem(ItemStack const& stack);
/// Registers a filled container item (units add their buckets: honey, chocolate, ...).
void registerFluidItem(std::string const& itemId, FluidStack contents, std::string const& emptyItemId);

} // namespace fluids

// ---- heat --------------------------------------------------------------------------------------
namespace heat {

using core::common::HeatLevel;

/// Heat for a block (e.g. U14's blaze burner reads its `blaze` state).
using Provider = std::function<HeatLevel(BlockSource&, BlockPos const&, Block const&)>;
void registerBlock(std::string const& blockId, Provider provider);

/// The heat level of the block at `pos` (the block under a basin / boiler): a registered
/// provider, else smouldering for Create's passive heaters (fire, soul fire, lava, magma
/// block, lit campfires), else none.
[[nodiscard]] HeatLevel level(BlockSource& region, BlockPos const& pos);
/// Whether a block id is one of Create's passive heaters (`#create:passive_boiler_heaters`).
[[nodiscard]] bool isPassiveHeater(Block const& block);

} // namespace heat

} // namespace lv::feature::common
