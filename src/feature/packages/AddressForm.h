#pragma once

// Address suggestions in the vanilla forms (U56): Create's address boxes offer the "#address"
// entries of nearby clipboards (Packages.h addressSuggestions); a form can't complete a text box,
// so it gets a dropdown of them next to the address input. Picking one replaces the typed address.

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ll/api/form/CustomForm.h"

#include "mc/world/actor/player/Player.h"

#include "feature/packages/Packages.h"
#include "mod/I18n.h"

namespace lv::feature::packages {

inline constexpr char const* kSuggestionField = "address_suggestion";

/// Appends the dropdown when there are suggestions ("Keeping Track": (typed address), then each one).
inline void appendAddressSuggestions(ll::form::CustomForm& form, Player& player, std::vector<std::string> const& suggestions) {
    if (suggestions.empty()) return;
    std::vector<std::string> options{trFor(player, "lv.packages.form.typed_address", "(the address typed above)")};
    for (auto const& s : suggestions) options.push_back("#" + s);
    form.appendDropdown(kSuggestionField, trFor(player, "create.gui.address_box.clipboard_tip", "Keeping Track"), options, 0);
}

/// The suggestion picked in the form, nullopt when none (keep the typed address).
[[nodiscard]] inline std::optional<std::string> chosenSuggestion(ll::form::CustomFormResult const& result, std::vector<std::string> const& suggestions) {
    if (!result) return std::nullopt;
    auto it = result->find(kSuggestionField);
    if (it == result->end()) return std::nullopt;
    auto const* v = std::get_if<uint64>(&it->second);
    if (!v || *v == 0 || *v > suggestions.size()) return std::nullopt;
    return suggestions[*v - 1];
}

} // namespace lv::feature::packages
