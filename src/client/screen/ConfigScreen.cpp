// Native config screen (U42): Create's config GUI for Levitate's `config/<section>.json` files,
// opened by `/create config` (server type "common.config", feature/common/ConfigScreen.cpp).
// The first page lists the sections; a section page lists its entries (scroll with the wheel),
// booleans as On / Off toggles, numbers and text as text boxes (Enter applies), and a reset
// button on entries that differ from their default. Changes go to the server, which checks the
// player is an operator, writes the file, reloads the configs and sends fresh values.

#ifdef LV_CLIENT

#include <algorithm>
#include <cctype>

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::screen {

namespace {

/// A bevelled button with a text label.
class TextButton : public Widget {
public:
    TextButton(Rect r, std::string text, std::function<void()> onClick) : Widget(r), text(std::move(text)), onClick(std::move(onClick)) {}
    std::string           text;
    std::function<void()> onClick;
    unsigned              fillColour = color::kButton;
    void render(Canvas& c, float mx, float my) override {
        c.bevel(rect, enabled ? fillColour : color::kSlotDark, color::kPanelLight, color::kPanelDark, false);
        if (enabled && hovered(mx, my)) c.fill(rect, color::kHover);
        c.text(rect.x + rect.w / 2, rect.y + (rect.h - 8) / 2, text, enabled ? color::kText : color::kTextGray, Align::Centre);
    }
    bool click(float mx, float my, Button b, bool) override {
        if (!enabled || b != Button::Left || !hovered(mx, my)) return false;
        if (onClick) onClick();
        return true;
    }
};

/// The whole panel: forwards the wheel to the list.
class WheelArea : public Widget {
public:
    explicit WheelArea(std::function<void(int)> fn) : Widget(Rect{0, 0, 10000, 10000}), mFn(std::move(fn)) {}
    void render(Canvas&, float, float) override {}
    bool scroll(float, float, int delta, bool) override {
        mFn(delta);
        return true;
    }

private:
    std::function<void(int)> mFn;
};

/// Create's config entry names: "maxRotationSpeed" -> "Max Rotation Speed", "large_cogwheel" ->
/// "Large Cogwheel"; a nested path shows its group first ("Impact > Large Cogwheel").
std::string humanize(std::string const& path) {
    std::string out;
    bool        up = true;
    for (std::size_t i = 0; i < path.size(); ++i) {
        char ch = path[i];
        if (ch == '.') {
            out += " > ";
            up = true;
            continue;
        }
        if (ch == '_' || ch == '-') {
            out += ' ';
            up = true;
            continue;
        }
        if (std::isupper(static_cast<unsigned char>(ch)) && i > 0 && std::islower(static_cast<unsigned char>(path[i - 1]))) out += ' ';
        out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch;
        up = false;
    }
    return out;
}

std::string valueText(Json const& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_float()) {
        auto s = v.dump();
        return s;
    }
    return v.dump();
}

class ConfigScreen : public Screen {
public:
    ConfigScreen(Json const& data, SendFn send) : Screen(ctr("lv.common.config.title", "Levitate Configuration"), kWidth, 0), mSend(std::move(send)), mData(data) {
        height = 30 + kRows * kRowH + 8;
    }

    void init() override { build(); }

    void onData(Json const& data) override {
        mData  = data;
        mDirty = true;
    }

    // widgets are rebuilt between frames, never inside one of their own callbacks
    void tick() override {
        if (!mDirty) return;
        mDirty = false;
        build();
    }

    void renderExtra(Canvas& c, float, float) override {
        if (!mData.value("admin", false)) c.text(8, height - 12, ctr("lv.common.config.need_op", "Only operators can change the configuration."), color::kButtonOff, Align::Left, false);
        auto entries = mData.value("entries", Json::array());
        if (entries.size() > kRows) {
            // the scroll bar
            float track = kRows * kRowH, top = 26;
            float h     = std::max(8.f, track * kRows / static_cast<float>(entries.size()));
            float y     = top + (track - h) * static_cast<float>(mOffset) / static_cast<float>(entries.size() - kRows);
            c.fill({kWidth - 6, top, 3, track}, color::kSlotDark);
            c.fill({kWidth - 6, y, 3, h}, color::kSlotLight);
        }
    }

private:
    static constexpr float       kWidth = 300;
    static constexpr std::size_t kRows  = 10;
    static constexpr float       kRowH  = 18;

    void build() {
        widgets.clear();
        bool admin   = mData.value("admin", false);
        auto section = mData.value("section", std::string{});
        if (section.empty()) {
            title      = ctr("lv.common.config.title", "Levitate Configuration");
            auto names = mData.value("sections", Json::array());
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto  name = names[i].get<std::string>();
                float col  = static_cast<float>(i % 2), row = static_cast<float>(i / 2);
                add<TextButton>(Rect{8 + col * 144, 26 + row * 20, 140, 18}, humanize(name), [this, name] { mSend({{"op", "open"}, {"section", name}}); });
            }
            height = std::max(height, 26 + static_cast<float>((names.size() + 1) / 2) * 20 + 22);
            return;
        }
        title = ctr("lv.common.config.title", "Levitate Configuration") + ": " + humanize(section);
        add<TextButton>(Rect{kWidth - 52, 4, 44, 14}, ctr("lv.common.config.back", "Back"), [this] { mSend({{"op", "open"}, {"section", ""}}); });
        auto entries = mData.value("entries", Json::array());
        mOffset      = std::min(mOffset, entries.size() > kRows ? entries.size() - kRows : 0);
        for (std::size_t r = 0; r < kRows && mOffset + r < entries.size(); ++r) {
            auto const& e    = entries[mOffset + r];
            auto        path = e.value("path", std::string{});
            auto        v    = e.value("value", Json());
            auto        def  = e.value("default", Json());
            float       y    = 26 + static_cast<float>(r) * kRowH;
            // long names keep their end (the leaf), cut in front so they fit beside the editor
            auto name = humanize(path);
            if (name.size() > 28) name = ".." + name.substr(name.size() - 26);
            auto&       label = add<Label>(Rect{8, y + 4, 160, 9}, name, color::kTextDark);
            label.tooltip = {humanize(path), "§7" + path, "§8" + ctr("lv.common.config.default", "Default: {}", valueText(def))};
            if (v.is_boolean()) {
                bool  on = v.get<bool>();
                auto& b  = add<TextButton>(Rect{172, y, 80, 16}, on ? ctr("lv.common.config.on", "On") : ctr("lv.common.config.off", "Off"), [this, path, on] {
                    mSend({{"op", "set"}, {"path", path}, {"value", !on}});
                });
                b.fillColour = on ? color::kButtonOn : color::kButtonOff;
                b.enabled    = admin;
            } else {
                auto& f = add<TextField>(Rect{172, y, 80, 16}, valueText(v), [this, path](std::string const& text) {
                    mSend({{"op", "set"}, {"path", path}, {"value", text}});
                });
                f.maxLength = 64;
                f.enabled   = admin;
            }
            if (!def.is_null() && def != v) {
                auto& reset = add<TextButton>(Rect{256, y, 16, 16}, "⟲", [this, path] { mSend({{"op", "reset"}, {"path", path}}); });
                reset.tooltip = {ctr("lv.common.config.reset", "Reset to default")};
                reset.enabled = admin;
            }
        }
        add<WheelArea>([this](int delta) { scrollList(delta); });
    }

    void scrollList(int delta) {
        auto        entries = mData.value("entries", Json::array());
        std::size_t max     = entries.size() > kRows ? entries.size() - kRows : 0;
        std::size_t next    = delta > 0 ? (mOffset == 0 ? 0 : mOffset - 1) : std::min(max, mOffset + 1);
        if (next == mOffset) return;
        mOffset = next;
        mDirty  = true;
    }

    SendFn      mSend;
    Json        mData;
    std::size_t mOffset = 0;
    bool        mDirty  = false;
};

} // namespace

class ConfigScreenClientModule {
public:
    void install() {
        registerServerScreen("common.config", [](Json const& data, SendFn send) {
            log().info("config client: screen {} opened", data.value("section", std::string{"(sections)"}));
            return std::make_unique<ConfigScreen>(data, std::move(send));
        });
    }
    void uninstall() {}
};

} // namespace lv::client::screen

LV_REGISTER_MODULE(lv::client::screen::ConfigScreenClientModule);

#endif // LV_CLIENT
