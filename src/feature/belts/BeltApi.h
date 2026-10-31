#pragma once

// Item transport contract of the belts unit (U11, SPEC §3.6.11). Other units act on belt / depot
// items through this header only (they never include the belts unit's other headers):
//
// - Processors 2 blocks above a belt segment or a depot (Create's BeltProcessingBehaviour:
//   presses, spouts, deployers, ...) register per block id and get each item that reaches the
//   segment's centre; they can hold it (locked) and later convert it.
// - Fans, presses, spouts, ... change the items at a position with `handleCenteredProcessing` /
//   `handleProcessingOnAllItems` (Create's TransportedItemStackHandlerBehaviour).
// - Funnels / tunnels sitting on a belt (the block above a segment) register an interactor that
//   sees every item passing the segment (Create's BeltFunnelInteractionHandler /
//   BeltTunnelInteractionHandler).
// - Blocks that accept items thrown or pushed at them (Create's DirectBeltInputBehaviour: belts,
//   depots, ejectors and, from other units, funnels, chutes, ...) register a direct input; belt
//   ends, ejectors and chutes insert through `insertDirect`.
//
// Belts, depots and ejectors are also plain item capabilities (`common/Capabilities.h`).
// Everything runs on the server thread. Sides are Bedrock FacingIDs (0 down, 1 up, 2 north,
// 3 south, 4 west, 5 east).

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mc/world/item/ItemStack.h"
#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"

namespace lv::feature::belts {

using core::GlobalPos;

inline constexpr char const* kBeltId          = "levitate:belt";
inline constexpr char const* kBeltConnectorId = "levitate:belt_connector";
inline constexpr char const* kDepotId         = "levitate:depot";
inline constexpr char const* kEjectorId       = "levitate:weighted_ejector";

/// Create's TransportedItemStack.
struct TransportedItem {
    ItemStack stack;
    /// Belts: 0 (outer edge of the START segment) .. length; depots / ejectors: 0 (edge) .. 0.5 (centre).
    float beltPosition = 0;
    float sideOffset   = 0;
    int   insertedFrom = 1;     ///< side the item came from (Create's insertedFrom)
    int   insertedAt   = 0;     ///< belt segment it was put on
    bool  locked       = false; ///< held by a processor (does not move)
    /// Free for processors (Create's processingTime / processedBy): -1 = not started.
    int            processingTime = -1;
    std::string    processedBy;
    nlohmann::json extra;
};

/// Create's TransportedItemStackHandlerBehaviour.TransportedResult.
struct TransportedResult {
    enum class Kind { DoNothing, Remove, Convert };
    Kind                     kind = Kind::DoNothing;
    std::vector<ItemStack>   outputs;    ///< Convert: stacks that continue where the item was
    std::optional<ItemStack> heldOutput; ///< Convert: a stack that stays held (locked) at the centre

    static TransportedResult doNothing() { return {}; }
    static TransportedResult removeItem() { return {Kind::Remove, {}, std::nullopt}; }
    static TransportedResult convertTo(std::vector<ItemStack> outputs) { return {Kind::Convert, std::move(outputs), std::nullopt}; }
    static TransportedResult convertToAndLeaveHeld(std::vector<ItemStack> outputs, ItemStack held) {
        return {Kind::Convert, std::move(outputs), std::move(held)};
    }
};
using ProcessFn = std::function<TransportedResult(TransportedItem&)>;

// ---- items at a position ---------------------------------------------------------------------------
/// A belt segment that carries items (horizontal or sloped), a depot or an ejector.
[[nodiscard]] bool isTransporter(GlobalPos const& pos);
/// Create's handleCenteredProcessingOnAllItems: `fn` on every item within `maxDistanceFromCentre`
/// of the segment's / depot's centre. Returns true when something changed.
bool handleCenteredProcessing(GlobalPos const& pos, float maxDistanceFromCentre, ProcessFn const& fn);
/// Create's handleProcessingOnAllItems: every item on the segment / depot (`fn` may also just read
/// and change `processingTime` / `processedBy` / `extra`, which are kept).
bool handleProcessingOnAllItems(GlobalPos const& pos, ProcessFn const& fn);
/// The items on a segment / depot (copies; the depot's output buffer is not included).
[[nodiscard]] std::vector<TransportedItem> itemsAt(GlobalPos const& pos);

// ---- processors 2 blocks above (Create's BeltProcessingBehaviour) ---------------------------------------
enum class ProcessingResult { Pass, Hold, Remove };
struct ProcessorContext {
    GlobalPos processor;   ///< the registered block (2 above)
    GlobalPos transporter; ///< the belt segment / depot / ejector
    bool      depot = false;
};
struct Processor {
    /// The item reached the centre below the processor (Create's handleReceivedItem). Hold keeps it
    /// locked at the centre; Remove deletes it.
    std::function<ProcessingResult(ProcessorContext const&, TransportedItem&)> received;
    /// Every tick while the item is held (Create's handleHeldItem); Pass releases it.
    std::function<ProcessingResult(ProcessorContext const&, TransportedItem&)> held;
};
/// Registers the processor of a block id (a second registration replaces the first). Processing
/// is skipped while the block between (1 above the belt) has a collision box, unless that block has
/// an interactor (funnels), as Create's `isBlocked`.
void registerProcessor(std::string const& blockId, Processor processor);

// ---- blocks on a belt (funnels, tunnels: the block 1 above a segment) ------------------------------
struct PassContext {
    GlobalPos        segment; ///< the belt segment (or depot) below
    GlobalPos        above;   ///< the registered block
    int              segmentIndex     = 0;
    int              length           = 1;
    int              movementFace     = 2; ///< direction the items move (horizontal FacingID)
    bool             movementPositive = true;
    float            nextOffset       = 0; ///< where the item will be after this tick's movement
    bool             depot            = false;
    TransportedItem& item; ///< the interactor may change the stack (empty = taken) or the position
};
enum class PassResult { Pass, Stop };
/// Called every tick for each item for every segment between the item's segment and `nextOffset`
/// whose block above is `blockId` (Create's checkForFunnels / flapTunnels); Stop keeps the item
/// where it is this tick. On depots it is called once per tick for the held item.
using PassFn = std::function<PassResult(PassContext&)>;
void registerAboveInteractor(std::string const& blockId, PassFn fn);

// ---- direct input (Create's DirectBeltInputBehaviour) -----------------------------------------------
/// `side` is the direction the item travels when it arrives (Create's convention: a belt moving
/// north inserts with side north; something dropping from above inserts with side down).
struct DirectInput {
    std::function<bool(GlobalPos const&, int side)>                                    canInsertFromSide;
    /// Returns what was not accepted (empty stack = all of it).
    std::function<ItemStack(GlobalPos const&, TransportedItem const&, int side, bool simulate)> insert;
};
void registerDirectInput(std::string const& blockId, DirectInput input);
[[nodiscard]] bool hasDirectInput(GlobalPos const& pos);
/// (Added by U41) The direct input at `pos` takes items travelling `side` (Create's
/// DirectBeltInputBehaviour.canInsertFromSide); false when there is none.
[[nodiscard]] bool canInsertDirect(GlobalPos const& pos, int side);
/// Inserts into the direct input at `pos` (returns `item.stack` unchanged when there is none or it
/// refuses the side).
[[nodiscard]] ItemStack insertDirect(GlobalPos const& pos, TransportedItem const& item, int side, bool simulate);

// ---- queries --------------------------------------------------------------------------------------
struct BeltInfo {
    GlobalPos controller;     ///< the START segment
    int       index  = 0;     ///< this segment's index (0 = START)
    int       length = 0;
    float     speed  = 0;     ///< RPM (effective, signed)
    int       movementFace = 2; ///< horizontal FacingID items move towards
    int       facing = 0;     ///< `levitate:facing` (0 south, 1 west, 2 north, 3 east)
    int       slope  = 0;     ///< 0 horizontal, 1 upward, 2 downward, 3 vertical, 4 sideways
    int       casing = 0;     ///< 0 none, 1 andesite, 2 brass
    int       color  = -1;    ///< -1 undyed, 0..15
};
[[nodiscard]] std::optional<BeltInfo> beltAt(GlobalPos const& pos);
/// (U55) Encases a belt segment (Create's BeltBlockEntity.setCasingType; 0 none, 1 andesite, 2 brass):
/// a tunnel placed on an uncased segment cases it. Returns false when `pos` is no belt segment.
bool setCasing(GlobalPos const& pos, int casing);

} // namespace lv::feature::belts
