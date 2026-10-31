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

#include "nlohmann/json.hpp"

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
    /// (U55) The board of the box at a position, used instead of `board` when set (Create builds
    /// some boards from the block entity, e.g. the amount board from the filter's max stack size).
    std::function<BoardSpec(GlobalPos const&, Block const&)> boardAt;
    /// (U55) Whether the box takes clicks at a position (Create's `acceptsValueSettings` /
    /// `isActive`); unset: always. The client mod still outlines it (it can't see block data).
    std::function<bool(GlobalPos const&)> activeAt;
    /// (U55) Create's BulkScrollValueBehaviour: confirming the board with Ctrl applies the value to
    /// the whole group (`set` reads `ctrlDown()`); vanilla players get a toggle in the form.
    bool bulk = false;
    /// (U55) `format` with the position (e.g. "Any" at the filter's max stack size); used first.
    std::function<std::string(GlobalPos const&, Block const&, ValueSettings)> formatAt;
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
bool apply(Player& player, GlobalPos const& pos, std::string const& boxId, ValueSettings settings, bool ctrlDown = false);
/// (U55) True while a box's `set` runs for a board confirmed with Ctrl held (Create's
/// `ValueSettingsPacket.ctrlDown`, used by BulkScrollValueBehaviour).
[[nodiscard]] bool ctrlDown();
/// (U55) The box hit at `pos` (as `boxAt`, also checking `activeAt`).
[[nodiscard]] BoxSpec const* boxAt(GlobalPos const& pos, Block const& block, int face, Vec const& hit);

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
    /// (U55) Create's `FilteringBehaviour.withPredicate`: items the slot refuses ("Cannot use this
    /// item here.", the old filter stays). Unset: every item.
    std::function<bool(GlobalPos const&, ItemStack const&)> accepts;
    /// (U55) Title of the hover label (Create's `getLabel`); empty: "Filter".
    std::string label, labelFallback;
    /// (U55) The count shown with the filter item (Create's `getCountLabelForValueBox`: "*" for
    /// any amount, the number otherwise, "" when the count isn't visible). Unset: none.
    std::function<std::string(GlobalPos const&)> countLabel;
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
/// (U55) Create's `FilteringBehaviour.canShortInteract`: the wrench and the mechanical arm never
/// become a filter (a click with them on a slot does nothing).
[[nodiscard]] bool canShortInteract(std::string const& heldId);
/// (U55) Create's `FilteringBehaviour.destroy`: drops the filter items of the slots stored in a
/// BlockData document (next tick, at the block's centre). The value box module calls it from one
/// central erase listener for every unit's block, so units don't drop filters themselves.
void dropFiltersOf(GlobalPos const& pos, nlohmann::json const& doc);

} // namespace filterslot

} // namespace lv::feature::common
