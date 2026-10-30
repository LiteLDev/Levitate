#pragma once

// Clipboard contents (U24, Create's ClipboardEntry / ClipboardOverrides): the public part other
// units use to write onto a clipboard item (U25's package ports add their address, U29's
// schematicannon prints its material checklist onto one). Server thread only.

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

class ItemStack;

namespace lv::feature::display {
inline constexpr char const* kClipboardId = "levitate:clipboard";
} // namespace lv::feature::display

namespace lv::feature::display::clipboard {

using Json = nlohmann::json;

/// Create's ClipboardEntry: a line of text, its check mark and an optional item icon with an
/// amount (`displayItem(stack, amount)`, used by the material checklist).
struct Entry {
    std::string text;
    bool        checked = false;
    Json        icon;       ///< item JSON (`items::toJson`), null: none
    int         amount = 0; ///< shown next to the icon when > 0
};
struct Contents {
    int                             type = 0; ///< 0 empty, 1 editing, 2 written (Create's ClipboardType)
    std::vector<std::vector<Entry>> pages;
    int                             previouslyOpenedPage = 0;
    Json                            copied;           ///< copied machine settings (null when none)
    bool                            readOnly = false; ///< Create's "Readonly" tag: entries can't be edited
};
[[nodiscard]] Contents fromJson(Json const& j);
[[nodiscard]] Json     toJson(Contents const& c);
[[nodiscard]] Contents ofItem(ItemStack const& stack);
void                   setOnItem(ItemStack& stack, Contents const& c);
[[nodiscard]] bool     isClipboard(ItemStack const& stack);

/// Create's PackagePortBlockEntity.addAddressToClipboard: adds "#address" as an unchecked entry to
/// the first page with at most 11 entries (a new page when all are full) and switches the
/// clipboard to WRITTEN. False (unchanged) when the address is blank or already on the clipboard.
bool addAddress(ItemStack& clipboard, std::string const& address);

/// Create's MaterialChecklist.MAX_ENTRIES_PER_CLIPBOARD_PAGE.
inline constexpr int kChecklistPageEntries = 7;
/// A new written, read-only clipboard with `pages` and the custom name `name` (Create's
/// MaterialChecklist.createWrittenClipboard). nullopt when the item can't be made.
[[nodiscard]] std::optional<ItemStack> makeWritten(std::vector<std::vector<Entry>> pages, std::string const& name);

} // namespace lv::feature::display::clipboard
