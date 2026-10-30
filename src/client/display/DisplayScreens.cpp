// Native screens of the display unit (SPEC §4): the Display Link configuration (source, target
// line, the source's options) and the Clipboard (pages of checklist entries). Both talk to the
// server through "screen.action"; text (clipboard entries, text options of display sources) is
// typed into native text fields and sent when Enter is pressed or the field is left.

#ifdef LV_CLIENT

#include <algorithm>
#include <memory>

#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::display {

namespace {
using namespace lv::client::screen;

/// Screens that rebuild their widgets on new data wait while the player types in one of them
/// (the rebuild would drop the focused field and what was typed).
bool anyFocused(std::vector<TextField*> const& fields) {
    return std::any_of(fields.begin(), fields.end(), [](TextField const* f) { return f->focused(); });
}

// ---- display link -----------------------------------------------------------------------------

class LinkScreen : public Screen {
public:
    LinkScreen(Json const& data, SendFn send)
    : Screen(ctr("create.display_link.title", "Display Link"), 220, 150),
      mSend(std::move(send)),
      mData(data) {}

    void init() override { build(); }

    void onData(Json const& data) override {
        mData = data;
        if (anyFocused(mFields)) {
            mDirty = true;
            return;
        }
        rebuild();
    }

    void tick() override {
        if (mDirty && !anyFocused(mFields)) rebuild();
    }

private:
    void rebuild() {
        mDirty = false;
        mFields.clear();
        widgets.clear();
        build();
    }

    void build() {
        float y = 18;
        add<Label>(Rect{8, y, 204, 10}, ctr("create.display_link.reading_from", "Read from:") + " " + mData.value("sourceName", std::string{}));
        y += 11;
        add<Label>(Rect{8, y, 204, 10}, ctr("create.display_link.writing_to", "Send to:") + " " + mData.value("targetName", std::string{}));
        y += 14;
        auto const& sources  = mData["sources"];
        auto        selected = mData.value("selected", std::string{});
        int         current  = 0;
        Json const* active   = nullptr;
        std::vector<std::string> names{ctr("lv.display.link.none", "(none)")};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            names.push_back(sources[i].value("name", std::string{}));
            if (sources[i].value("id", std::string{}) == selected) {
                current = static_cast<int>(i) + 1;
                active  = &sources[i];
            }
        }
        if (sources.empty()) {
            add<Label>(Rect{8, y, 204, 10}, ctr("create.display_link.no_source", "Not a Display Source"), color::kButtonOff);
        } else {
            auto& src   = add<ScrollInput>(Rect{8, y, 204, 16}, "", 0, static_cast<int>(names.size()) - 1, current, [this, sources](int v) {
                mSend({{"op", "source"}, {"id", v == 0 ? std::string{} : sources[static_cast<std::size_t>(v - 1)].value("id", std::string{})}});
            });
            src.options = names;
            src.tooltip = {ctr("create.display_link.information_type", "Type of Information")};
        }
        y += 19;
        auto lines = mData.value("lines", Json::array());
        if (lines.empty()) {
            add<Label>(Rect{8, y, 204, 10}, ctr("create.display_link.no_target", "Not a Display Target"), color::kButtonOff);
        } else {
            bool multi = active && active->value("multi", false);
            auto label = multi ? ctr("create.display_link.display_on_multiline", "Start writing at:") : ctr("create.display_link.display_on", "Write data to:");
            std::vector<std::string> opts;
            for (auto const& l : lines) opts.push_back(l.get<std::string>());
            auto& line = add<ScrollInput>(Rect{8, y, 204, 16}, label, 0, static_cast<int>(opts.size()) - 1,
                                          std::clamp(mData.value("line", 0), 0, static_cast<int>(opts.size()) - 1),
                                          [this](int v) { mSend({{"op", "line"}, {"value", v}}); });
            line.options = opts;
        }
        y += 19;
        if (active) {
            for (auto const& o : (*active)["options"]) {
                if (o.value("text", false)) {
                    // Create's text options (e.g. the label of a source): a text box
                    auto key   = o.value("key", std::string{});
                    auto value = o["value"].is_string() ? o["value"].get<std::string>() : std::string{};
                    add<Label>(Rect{8, y + 3, 80, 10}, o.value("label", std::string{}), color::kTextDark);
                    auto& field = add<TextField>(Rect{90, y, 122, 16}, value, [this, key, value](std::string const& text) {
                        if (text == value) return;
                        mSend({{"op", "option"}, {"key", key}, {"value", text}});
                    });
                    field.maxLength = 100;
                    mFields.push_back(&field);
                    y += 19;
                    continue;
                }
                auto choices = o["choices"].get<std::vector<std::string>>();
                if (choices.empty()) continue;
                int  v   = o["value"].is_number_integer() ? o["value"].get<int>() : 0;
                auto key = o.value("key", std::string{});
                auto& in = add<ScrollInput>(Rect{8, y, 204, 16}, o.value("label", std::string{}), 0, static_cast<int>(choices.size()) - 1,
                                            std::clamp(v, 0, static_cast<int>(choices.size()) - 1),
                                            [this, key](int value) { mSend({{"op", "option"}, {"key", key}, {"value", value}}); });
                in.options = choices;
                y += 19;
            }
        }
        height = std::max(110.f, y + 8);
    }

    SendFn                  mSend;
    Json                    mData;
    std::vector<TextField*> mFields;
    bool                    mDirty = false;
};

// ---- clipboard ------------------------------------------------------------------------------

class ClipboardScreen : public Screen {
public:
    ClipboardScreen(Json const& data, SendFn send)
    : Screen(ctr("lv.display.clipboard.title", "Clipboard"), 200, 236),
      mSend(std::move(send)),
      mData(data) {
        mPage = data.value("page", 0);
    }

    void init() override { build(); }

    void onData(Json const& data) override {
        mData = data;
        if (anyFocused(mFields)) {
            mDirty = true;
            return;
        }
        rebuild();
    }

    void tick() override {
        if (mDirty && !anyFocused(mFields)) rebuild();
    }

private:
    void rebuild() {
        mDirty = false;
        mFields.clear();
        widgets.clear();
        build();
    }

    /// The page as typed: every field's text with the entry's check mark (blank entries are
    /// dropped by the server, as Create's clipboard drops empty lines).
    void submitPage() {
        auto pages   = mData.value("pages", Json::array());
        Json current = mPage < static_cast<int>(pages.size()) ? pages[static_cast<std::size_t>(mPage)] : Json::array();
        Json entries = Json::array();
        bool changed = false;
        for (std::size_t i = 0; i < mFields.size(); ++i) {
            bool        has  = i < current.size();
            std::string was  = has ? current[i].value("t", std::string{}) : std::string{};
            auto const& text = mFields[i]->text;
            changed          = changed || text != was;
            entries.push_back({{"t", text}, {"c", has && current[i].value("c", false)}});
        }
        if (!changed) return;
        mSend({{"op", "set"}, {"page", mPage}, {"entries", entries}});
        log().info("clipboard page {} typed ({} entries)", mPage + 1, entries.size());
    }

    void go(int page) {
        int pages = static_cast<int>(mData.value("pages", Json::array()).size());
        int max   = std::min(mData.value("maxPage", 50), pages); // one empty page after the last
        page      = std::clamp(page, 0, max);
        if (page == mPage) return;
        mPage = page;
        mSend({{"op", "page"}, {"page", page}});
        // the page shown changes after the server's answer (onData rebuilds)
        mDirty = true;
    }

    void build() {
        auto pages = mData.value("pages", Json::array());
        int  count = static_cast<int>(pages.size());
        mPage      = std::clamp(mPage, 0, std::max(0, std::min(count, mData.value("maxPage", 50))));
        add<Label>(Rect{8, 18, 184, 10}, ctr("create.display_target.page", "Page {}", mPage + 1) + " / " + std::to_string(std::max(count, mPage + 1)),
                   color::kTextDark, Align::Right);
        Json entries = mPage < count ? pages[static_cast<std::size_t>(mPage)] : Json::array();
        float y      = 32;
        int maxEntries = mData.value("maxEntries", 15);
        bool readOnly  = mData.value("readonly", false);
        bool icons     = false;
        for (auto const& e : entries) icons = icons || e.contains("i");
        float rowHeight = icons ? 22.f : 11.5f; // Create's item entries are taller (icon + amount)
        mIcons.clear();
        // one text field per entry (Create edits the page in place), plus a blank one to add an
        // entry; read-only clipboards (material checklists) only show their entries
        std::size_t rows = std::min<std::size_t>(entries.size() + (readOnly ? 0 : 1), static_cast<std::size_t>(std::max(1, maxEntries)));
        for (std::size_t i = 0; i < rows; ++i) {
            bool has     = i < entries.size();
            bool checked = has && entries[i].value("c", false);
            auto text    = has ? entries[i].value("t", std::string{}) : std::string{};
            bool address = text.size() > 1 && text[0] == '#';
            float textX  = 22;
            if (has && entries[i].contains("i")) {
                if (auto icon = iconOf(entries[i]["i"])) mIcons.push_back({icon, 22, y - 3, entries[i].value("n", 0)});
                textX = 42;
            }
            if (has && !address) {
                auto& box   = add<IconButton>(Rect{8, y, 11, 11}, checked ? "v" : "", [this, i] {
                    mSend({{"op", "toggle"}, {"page", mPage}, {"entry", static_cast<int>(i)}});
                });
                box.toggled = checked;
            }
            if (readOnly) {
                add<Label>(Rect{textX, y + 1, 192 - textX, 10}, text, checked ? 0xFF8B8B8B : color::kTextDark);
            } else {
                auto& field     = add<TextField>(Rect{textX, y - 1, 192 - textX, 12}, text, [this](std::string const&) { submitPage(); });
                field.maxLength = 512;
                if (!has) field.placeholder = ctr("lv.display.clipboard.new_entry", "New entry...");
                mFields.push_back(&field);
            }
            y += rowHeight;
        }
        auto& prev = add<IconButton>(Rect{8, 212, 18, 18}, "<", [this] { go(mPage - 1); });
        prev.enabled = mPage > 0;
        auto& next = add<IconButton>(Rect{174, 212, 18, 18}, ">", [this] { go(mPage + 1); });
        next.enabled = mPage < 50;
        if (!readOnly) {
            auto& erase   = add<IconButton>(Rect{30, 212, 18, 18}, "x", [this] { mSend({{"op", "erase_checked"}, {"page", mPage}}); });
            erase.tooltip = {ctr("create.gui.clipboard.erase_checked", "Erase checked items")};
        }
    }

    void renderExtra(Canvas& c, float, float) override {
        // Create's ClipboardEntry.displayItem: the item with its amount next to the entry
        for (auto const& i : mIcons) {
            c.item(*i.stack, i.x, i.y);
            if (i.amount > 0) c.text(i.x + 17, i.y + 9, std::to_string(i.amount), color::kText, Align::Right);
        }
    }

    struct Icon {
        std::shared_ptr<ItemStack> stack;
        float                      x, y;
        int                        amount;
    };
    SendFn                  mSend;
    Json                    mData;
    std::vector<TextField*> mFields;
    std::vector<Icon>       mIcons;
    bool                    mDirty = false;
    int                     mPage  = 0;
};

class DisplayScreensModule {
public:
    void install() {
        registerServerScreen("display.link", [](Json const& data, SendFn send) { return std::make_unique<LinkScreen>(data, std::move(send)); });
        registerServerScreen("display.clipboard", [](Json const& data, SendFn send) {
            log().info("clipboard screen opened (page {})", data.value("page", 0) + 1);
            return std::make_unique<ClipboardScreen>(data, std::move(send));
        });
    }
    void uninstall() {}
};

} // namespace

} // namespace lv::client::display

LV_REGISTER_MODULE(lv::client::display::DisplayScreensModule);

#endif // LV_CLIENT
