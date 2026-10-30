#pragma once

// Value boxes and filter slots on block faces (U3, SPEC §3.2).
//
// Value box (Create's scroll value / scroll option, edited on a "value settings" board):
//
//     valuebox::registerBox("levitate:creative_motor", {
//         .id       = "speed",
//         .label    = "create.kinetics.creative_motor.rotation_speed", .labelFallback = "Generated Speed",
//         .board    = [](Block const&) { return core::common::signedBoard(256, 32, "⟳", "⟲"); },
//         .centre   = [](Block const& b, int face) -> std::optional<core::common::Vec> { ... },
//         .get      = [](GlobalPos const& p) { return core::common::signedToSettings(speedOf(p)); },
//         .set      = [](GlobalPos const& p, core::common::ValueSettings s, Player&) { ... },
//         .format   = [](Block const&, core::common::ValueSettings s) { return std::to_string(...) + " RPM"; },
//     });
//
// Clicks reach boxes and slots through the shared interaction dispatch (Interactions.h, priority
// Highest), so they win over the wrench's rotate and other units' block handlers.
//
// - Client mod: *holding* right-click on the box with the wrench or an empty hand opens the board
//   (Create's ValueSettingsClient: the board opens after the button was held for more than 3
//   ticks; releasing earlier is a short click, `onShortClick`). Native screen: move the pointer
//   along the row to pick a value, scroll for single steps, Shift+scroll for milestones, click to
//   confirm. Looking at a block with boxes outlines them (the hovered one white) and shows the
//   hovered box's label, value and "Click and hold to edit".
// - Vanilla: right-click the box with the wrench (or an empty hand) opens a form (row dropdown +
//   value slider).
//
// Filter slot (Create's FilteringBehaviour): a box that holds a filter item stack in BlockData
// (`data["filter"][slot.id]`): right-click with an item sets it (filter items are taken from
// the hand in survival and given back when replaced), right-click with an empty hand clears it.
// A slot with `onHold` (Create's filters with an editable count) runs it when a companion holds
// right-click on it; the short click still sets / clears the filter.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/GlobalPos.h"
#include "core/common/ValueSettings.h"

class Block;
class ItemStack;
class Player;

namespace lv::feature::common {

namespace valuebox {

using core::GlobalPos;
using core::common::BoardSpec;
using core::common::ValueSettings;
using core::common::Vec;

struct BoxSpec {
    std::string id = "value"; ///< distinct per block ("speed", "mode", ...)
    std::string label;         ///< translation key of the title
    std::string labelFallback; ///< English title
    std::function<BoardSpec(Block const&)>                              board;
    std::function<std::optional<Vec>(Block const&, int face)>           centre; ///< nullopt: no box on that face
    double                                                              scale = 0.5;
    std::function<ValueSettings(GlobalPos const&)>                      get;
    std::function<void(GlobalPos const&, ValueSettings, Player& editor)> set;
    /// Text of a value ("64 RPM", an option name, ...); default: the number.
    std::function<std::string(Block const&, ValueSettings)> format;
    /// Only the wrench opens it, not an empty hand (Create's `requiresWrench()`, e.g. chassis).
    bool requiresWrench = false;
    /// Create's `ValueSettingsBehaviour#onShortInteract`: a companion's right-click on the box
    /// released before the board opened (e.g. the valve handle turns). Vanilla players: their
    /// click opens the form; units keep their own vanilla fallback for the short click.
    std::function<void(GlobalPos const&, Player&)> onShortClick;
};

/// What happens when a held right-click resolves (the player is looked up again, server thread).
using HoldAction = std::function<void(Player&)>;

/// Create's click-and-hold on a value box (ValueSettingsClient), for any target: asks the
/// player's client mod to watch the use button (`valuebox.hold {token, pos}`); it answers
/// `valuebox.release {token, hold}`: held for more than 3 ticks -> `onHold`, released earlier ->
/// `onShort`, looked away / sneaked -> neither. Returns false (nothing sent) for vanilla players,
/// who keep the unit's own click fallback. While a hold is pending, further use-on packets of the
/// held button on `pos` should be consumed without action (`holdPending`).
bool awaitHold(Player& player, GlobalPos const& pos, HoldAction onShort, HoldAction onHold);
/// A hold of `player` at `pos` waits for the client's answer.
[[nodiscard]] bool holdPending(Player const& player, GlobalPos const& pos);

void registerBox(std::string const& blockId, BoxSpec spec);
/// Removes a box (selftests, reloads).
void unregisterBox(std::string const& blockId, std::string const& boxId);
/// The box hit by a click at block-local `hit` on `face` (nullptr if none).
[[nodiscard]] BoxSpec const* boxAt(Block const& block, int face, Vec const& hit);
[[nodiscard]] std::vector<BoxSpec> const* boxesOf(std::string const& blockId);

/// Opens the editor for a box (native board for companions, form otherwise).
void openEditor(Player& player, GlobalPos const& pos, std::string const& boxId);
/// Applies settings as if the player confirmed them (validated and clamped).
bool apply(Player& player, GlobalPos const& pos, std::string const& boxId, ValueSettings settings);

} // namespace valuebox

namespace filterslot {

using core::GlobalPos;
using core::common::Vec;

struct SlotSpec {
    std::string id = "filter"; ///< distinct per block
    std::function<std::optional<Vec>(Block const&, int face)> centre;
    double                                                    scale = 0.5;
    /// Called after the filter changed.
    std::function<void(GlobalPos const&)> onChanged;
    /// Companions holding right-click on the slot (Create: the amount board of a filter whose
    /// count is editable). Unset: a click acts at once, as before.
    std::function<void(Player&, GlobalPos const&)> onHold;
};

void registerSlot(std::string const& blockId, SlotSpec spec);
void unregisterSlot(std::string const& blockId, std::string const& slotId);
[[nodiscard]] SlotSpec const* slotAt(Block const& block, int face, Vec const& hit);
/// Whether a block id has filter slots.
[[nodiscard]] bool hasSlots(std::string const& blockId);
/// The slots of a block id (nullptr if none).
[[nodiscard]] std::vector<SlotSpec> const* slotsOf(std::string const& blockId);

/// The filter stack in a slot (empty stack when none).
[[nodiscard]] ItemStack get(GlobalPos const& pos, std::string const& slotId = "filter");
void                    set(GlobalPos const& pos, ItemStack const& filter, std::string const& slotId = "filter");
/// Whether `stack` passes the slot's filter (an empty slot passes everything).
[[nodiscard]] bool matches(GlobalPos const& pos, ItemStack const& stack, std::string const& slotId = "filter");
/// Applies a player's right-click on a slot (Create's FilteringBehaviour.onShortInteract).
void interact(Player& player, GlobalPos const& pos, SlotSpec const& slot);

} // namespace filterslot

} // namespace lv::feature::common
