#pragma once

// Create's AddressEditBox suggestions (AddressEditBoxHelper / DestinationSuggestions), client mod:
// while an address box is focused, the "#address" entries of the player's clipboards (and of the
// clipboards placed nearby) that start with the typed text are listed under it; a click puts one
// into the box. The server sends the entries with the screen data (`suggestions`). Used by the
// package port, package filter, stock keeper, redstone requester and factory gauge screens.

#ifdef LV_CLIENT

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "client/Client.h"
#include "client/screen/Screen.h"

namespace lv::client::packages {

class AddressSuggestions : public screen::Widget {
public:
    AddressSuggestions(screen::TextField& field, std::vector<std::string> options)
    : Widget(screen::Rect{field.rect.x, field.rect.y + field.rect.h, field.rect.w, 0}),
      mField(field),
      options(std::move(options)) {}

    std::vector<std::string> options;

    void render(screen::Canvas& c, float mx, float my) override {
        mShown.clear();
        if (!mField.focused()) return;
        auto typed = lower(mField.text);
        for (auto const& o : options) {
            if (o == mField.text || !lower(o).starts_with(typed)) continue;
            mShown.push_back(o);
            if (mShown.size() >= kMaxRows) break;
        }
        for (std::size_t i = 0; i < mShown.size(); ++i) {
            screen::Rect row = rowRect(i);
            c.fill(row, row.contains(mx, my) ? 0xF0303030 : 0xF0101010);
            c.text(row.x + 2, row.y + 1, mShown[i], row.contains(mx, my) ? screen::color::kTextGold : screen::color::kTextGray);
        }
    }

    bool click(float mx, float my, screen::Button b, bool) override {
        if (b != screen::Button::Left) return false;
        for (std::size_t i = 0; i < mShown.size(); ++i) {
            if (!rowRect(i).contains(mx, my)) continue;
            auto chosen = mShown[i];
            mShown.clear();
            mField.text = chosen;
            if (mField.onSubmit) mField.onSubmit(chosen);
            return true;
        }
        return false;
    }

private:
    static constexpr std::size_t kMaxRows = 5;

    [[nodiscard]] screen::Rect rowRect(std::size_t i) const {
        return screen::Rect{mField.rect.x, mField.rect.y + mField.rect.h + 1 + 11.0f * static_cast<float>(i), mField.rect.w, 11};
    }
    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s;
    }

    screen::TextField&       mField;
    std::vector<std::string> mShown; ///< rows drawn last frame
};

/// The suggestions of a screen's data.
[[nodiscard]] inline std::vector<std::string> suggestionsOf(nlohmann::json const& data) {
    std::vector<std::string> out;
    for (auto const& s : data.value("suggestions", nlohmann::json::array()))
        if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

/// Create's clipboard tip next to the address box ("Keeping Track", ...).
[[nodiscard]] inline std::vector<std::string> clipboardTip() {
    return {ctr("create.gui.address_box.clipboard_tip", "Keeping Track"), "§7" + ctr("create.gui.address_box.clipboard_tip_1", "Entries starting with '#' on"),
            "§7" + ctr("create.gui.address_box.clipboard_tip_2", "held or nearby clipboards will"), "§7" + ctr("create.gui.address_box.clipboard_tip_3", "help auto-complete this input."),
            "§8" + ctr("create.gui.address_box.clipboard_tip_4", "(e.g. #storage, #factory)")};
}

} // namespace lv::client::packages

#endif // LV_CLIENT
