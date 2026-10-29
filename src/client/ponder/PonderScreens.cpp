#ifdef LV_CLIENT

// Ponder screens (SPEC §3.6.30 / §4):
//  - PonderUI: the scene player. Header (subject item, "Pondering about...", scene title), the
//    timeline with key frames (click to jump), the scene drawn in 3D (drag to turn the camera),
//    text bubbles, the tags of the subject (left), buttons: previous / close / next scene,
//    identify, comfy reading, replay, think back. Keys as Create: A / D previous / next scene,
//    Q identify, S replay; the mouse wheel changes the scene.
//  - TagIndexScreen: "Welcome to Ponder", the categories (tags) of the index.
//  - TagScreen: a tag's title, description and associated entries.
//  - IndexScreen: every entry with a Ponder (Create's PonderIndexScreen).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>

#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/ponder/Ponder.h"
#include "client/ponder/PonderConfig.h"
#include "client/screen/Screen.h"
#include "feature/common/Items.h"
#include "mod/Log.h"

namespace lv::client::ponder {

using screen::Align;
using screen::Button;
using screen::Canvas;
using screen::Rect;
namespace color = screen::color;

namespace {

constexpr int kVkA = 0x41, kVkD = 0x44, kVkQ = 0x51, kVkS = 0x53;

// ---- small pixel icons (original art, drawn with rectangles) ------------------------------------

/// 10x10 icons, one string per row ('#' = lit pixel).
using Icon = std::array<char const*, 10>;
constexpr Icon kIconLeft{"          ", "    ##    ", "   ###    ", "  ####    ", " #######  ", " #######  ", "  ####    ", "   ###    ", "    ##    ", "          "};
constexpr Icon kIconRight{"          ", "    ##    ", "    ###   ", "    ####  ", "  ####### ", "  ####### ", "    ####  ", "    ###   ", "    ##    ", "          "};
constexpr Icon kIconClose{"          ", " ##    ## ", "  ##  ##  ", "   ####   ", "    ##    ", "    ##    ", "   ####   ", "  ##  ##  ", " ##    ## ", "          "};
constexpr Icon kIconIdentify{"   ####   ", "  #    #  ", " #  ##  # ", " # #  # # ", " #    # # ", " #   #  # ", "  #    #  ", "   ####   ", "     # #  ", "       ## "};
constexpr Icon kIconSlow{"          ", "  ####### ", "  #     ##", "  #     # ", "  #     ##", "  ####### ", "   #####  ", "          ", " ######## ", "          "};
constexpr Icon kIconReplay{"   #####  ", "  #     # ", " #     ###", " #      # ", " #        ", " #      # ", "  #    #  ", "   ####   ", "          ", "          "};
constexpr Icon kIconBack{"   #      ", "  ##      ", " ######## ", "  ##    # ", "   #    # ", "        # ", "        # ", "  ######  ", "          ", "          "};

void drawIcon(Canvas& c, Icon const& icon, float x, float y, unsigned argb) {
    for (int r = 0; r < 10; ++r)
        for (int col = 0; col < 10; ++col)
            if (icon[static_cast<std::size_t>(r)][col] == '#') c.fill({x + static_cast<float>(col), y + static_cast<float>(r), 1, 1}, argb);
}

struct UiButton {
    Rect                  r;
    Icon const*           icon = nullptr;
    std::string           tip;
    std::string           key; ///< shortcut shown in the tooltip
    std::function<void()> fn;
    bool                  enabled = true;
    bool                  lit     = false;
};

void drawButton(Canvas& c, UiButton const& b, float mx, float my) {
    bool hover = b.enabled && b.r.contains(mx, my);
    unsigned bg = b.lit ? 0xF0304060 : 0xE0101010;
    c.fill(b.r, bg);
    c.frame(b.r, hover ? 0xFFFFFFFF : b.lit ? 0xFF7FCDE0 : 0xFF505050);
    unsigned fg = !b.enabled ? 0xFF404040 : hover ? 0xFFFFFFFF : 0xFFC0C0C0;
    if (b.icon) drawIcon(c, *b.icon, b.r.x + (b.r.w - 10) / 2, b.r.y + (b.r.h - 10) / 2, fg);
}

/// Swallows the mouse wheel over the whole screen and forwards it.
class WheelCatcher : public screen::Widget {
public:
    WheelCatcher(Rect r, std::function<void(int)> fn) : Widget(r), mFn(std::move(fn)) {}
    void render(Canvas&, float, float) override {}
    bool scroll(float mx, float my, int delta, bool) override {
        if (!hovered(mx, my)) return false;
        mFn(delta);
        return true;
    }

private:
    std::function<void(int)> mFn;
};

std::shared_ptr<ItemStack> stackOf(std::string const& id) {
    auto s = feature::common::items::make(id);
    return s ? std::make_shared<ItemStack>(*s) : nullptr;
}

std::string tagTitle(std::string const& id) { return ctr("create.ponder.tag." + id, id); }
std::string tagDescription(std::string const& id) { return ctr("create.ponder.tag." + id + ".description", ""); }

// ---- the scene player ------------------------------------------------------------------------

class PonderUI;
PonderUI* gActive = nullptr;

class PonderUI : public screen::Screen {
public:
    PonderUI(std::string component, int sceneIndex)
    : Screen("Ponder", 0, 0),
      mComponent(std::move(component)),
      mIndex(sceneIndex) {
        auto [w, h] = screen::guiSize();
        width       = std::max(w, 200.f);
        height      = std::max(h, 150.f);
        mScenes     = library().scenesOf(mComponent);
        mIndex      = std::clamp(mIndex, 0, std::max(0, static_cast<int>(mScenes.size()) - 1));
        mSlow       = config().comfyReading;
        title.clear();
    }

    void init() override {
        gActive = this;
        add<WheelCatcher>(Rect{0, 0, width, height}, [this](int d) { changeScene(d > 0 ? -1 : 1); });
        startScene();
    }

    void onClose() override {
        if (gActive == this) gActive = nullptr;
        log().info("ponder: closed {}", mComponent);
    }

    bool key(int vk) {
        switch (vk) {
        case kVkA: changeScene(-1); return true;
        case kVkD: changeScene(1); return true;
        case kVkQ: toggleIdentify(); return true;
        case kVkS: replay(); return true;
        default: return false;
        }
    }

    void tick() override {
        auto now = std::chrono::steady_clock::now();
        float dt = mLastFrame.time_since_epoch().count() == 0 ? 0.f : std::chrono::duration<float>(now - mLastFrame).count();
        mLastFrame = now;
        dt         = std::min(dt, 0.25f);
        if (auto const* s = scene()) {
            float speed = 20.f;
            if (mSlow && !cp::evaluate(*s, mTime).texts.empty()) speed *= 0.5f;
            if (!mIdentify) mTime = std::min(mTime + dt * speed, static_cast<float>(s->length()));
        }
        mFade = std::min(1.f, mFade + dt * 4.f);
        // camera drag
        if (leftMouseDown() && mDragging) {
            mView.drag(mMouseX - mDragX, mMouseY - mDragY);
            mDragX = mMouseX, mDragY = mMouseY;
        } else mDragging = false;
    }

    void renderExtra(Canvas& c, float mx, float my) override {
        mMouseX = mx, mMouseY = my;
        c.fill({0, 0, width, height}, 0xF0101018);
        auto const* s = scene();
        if (!s) {
            c.text(width / 2, height / 2, ctr("lv.ponder.unknown", "There is nothing to ponder about {0}.", mComponent), 0xFFFFFFFF, Align::Centre);
            return;
        }
        auto st  = cp::evaluate(*s, mTime);
        auto res = drawScene(c, *s, st, mView, mIdentify, mx, my);
        if (!mLoggedDraw) {
            mLoggedDraw = true;
            log().info("ponder: scene {} drawn ({} blocks, {} placeholders)", s->id, res.drawnBlocks, res.missingBlocks);
        }
        drawTexts(c, *s, st, mView, width, mSlow);
        drawHeader(c, *s, mx, my);
        drawTimeline(c, *s, mx, my);
        drawTags(c, mx, my);
        layoutButtons();
        std::vector<std::string> tip;
        for (auto const& b : mButtons) {
            drawButton(c, b, mx, my);
            if (b.r.contains(mx, my)) {
                tip = {b.tip};
                if (!b.key.empty()) tip.push_back("[" + b.key + "]");
            }
        }
        if (mIdentify) {
            auto msg = ctr("ponder.ui.identify_mode", "Identify mode active.\nUnpause with [{0}]", "Q");
            auto nl  = msg.find('\n');
            c.text(width / 2, height - 52, msg.substr(0, nl), 0xFFFFFFFF, Align::Centre);
            if (nl != std::string::npos) c.text(width / 2, height - 42, msg.substr(nl + 1), 0xFFA0A0A0, Align::Centre);
            if (!res.hoveredBlock.empty()) {
                auto name = itemName(res.hoveredBlock);
                tip       = {name.empty() ? res.hoveredBlock : name};
                if (name.empty()) tip.push_back(ctr("lv.ponder.missing_block", "Not available yet: {0}", res.hoveredBlock));
            }
        }
        if (!mTagTip.empty()) tip = mTagTip;
        if (mSubjectHover) tip = {ctr("ponder.ui.subject", "Subject of this scene"), itemName(mComponent)};
        // scene change fade
        if (mFade < 1.f) c.fill({0, 0, width, height}, static_cast<unsigned>((1.f - mFade) * 255) << 24 | 0x101018);
        if (!tip.empty()) c.tooltip(mx, my, tip);
    }

    bool clickBackground(float mx, float my, Button b, bool) override {
        if (b != Button::Left) return false;
        for (auto const& btn : mButtons) {
            if (btn.enabled && btn.r.contains(mx, my)) {
                btn.fn();
                return true;
            }
        }
        if (clickTimeline(mx, my) || clickTags(mx, my)) return true;
        mDragging = true;
        mDragX = mx, mDragY = my;
        return true;
    }

    void changeScene(int delta) {
        int next = mIndex + delta;
        if (next < 0 || next >= static_cast<int>(mScenes.size())) return;
        mIndex = next;
        startScene();
    }

    void replay() {
        mTime     = 0;
        mIdentify = false;
        log().info("ponder: replay {}", scene() ? scene()->id : "?");
    }

    void toggleIdentify() {
        mIdentify = !mIdentify;
        log().info("ponder: identify mode {}", mIdentify ? "on" : "off");
    }

    [[nodiscard]] std::string const& component() const { return mComponent; }
    [[nodiscard]] int                index() const { return mIndex; }

private:
    cp::Scene const* scene() const { return mIndex < static_cast<int>(mScenes.size()) ? mScenes[static_cast<std::size_t>(mIndex)] : nullptr; }

    void startScene() {
        mTime       = 0;
        mFade       = 0;
        mIdentify   = false;
        mLoggedDraw = false;
        if (auto const* s = scene()) {
            mView = cp::View::fit(*s, width / 2, height / 2 + 14, width * 0.62f, height * 0.62f);
            log().info("ponder: scene {} ({}/{}) of {}", s->id, mIndex + 1, mScenes.size(), mComponent);
        }
    }

    void drawHeader(Canvas& c, cp::Scene const& s, float mx, float my) {
        Rect icon{8, 8, 24, 24};
        c.fill(icon, 0xE0101010);
        c.frame(icon, 0xFF505050);
        if (!mSubject) mSubject = stackOf(mComponent);
        if (mSubject) c.item(*mSubject, icon.x + 4, icon.y + 4);
        mSubjectHover = icon.contains(mx, my);
        c.text(38, 9, ctr("ponder.ui.pondering", "Pondering about..."), 0xFFA0A0A0);
        c.text(38, 20, translateClient(s.headerKey(), s.id), 0xFFFFFFFF);
        // scene counter (dots)
        float n = static_cast<float>(mScenes.size());
        for (int i = 0; i < static_cast<int>(mScenes.size()); ++i) {
            float x = width - 10 - (n - static_cast<float>(i)) * 7;
            c.fill({x, 12, 5, 5}, i == mIndex ? 0xFFFFFFFF : 0xFF505050);
        }
    }

    Rect timelineRect() const { return {8, 36, width - 16, 3}; }

    void drawTimeline(Canvas& c, cp::Scene const& s, float mx, float my) {
        auto  r   = timelineRect();
        float len = static_cast<float>(std::max(1, s.length()));
        c.fill(r, 0xFF303030);
        c.fill({r.x, r.y, r.w * std::min(1.f, mTime / len), r.h}, 0xFFDDDDDD);
        for (int kf : s.keyframes()) {
            float x = r.x + r.w * static_cast<float>(kf) / len;
            bool  h = std::abs(mx - x) < 3 && my > r.y - 4 && my < r.y + 8;
            c.fill({x - 0.5f, r.y - 2, 1.5f, r.h + 4}, h ? 0xFFFFD86B : 0xFF7FCDE0);
        }
    }

    bool clickTimeline(float mx, float my) {
        auto const* s = scene();
        auto        r = timelineRect();
        if (!s || my < r.y - 5 || my > r.y + 8 || mx < r.x || mx > r.x + r.w) return false;
        float len = static_cast<float>(std::max(1, s->length()));
        float t   = (mx - r.x) / r.w * len;
        // snap to a key frame close by (Create jumps to key frames)
        for (int kf : s->keyframes())
            if (std::abs(static_cast<float>(kf) - t) < len * 0.03f) t = static_cast<float>(kf);
        mTime = std::clamp(t, 0.f, len);
        log().info("ponder: jump to tick {}", static_cast<int>(mTime));
        return true;
    }

    std::vector<cp::Tag const*> tags() const { return library().tagsOf(mComponent); }

    Rect tagRect(int i) const { return {8, 48 + static_cast<float>(i) * 24, 22, 22}; }

    void drawTags(Canvas& c, float mx, float my) {
        mTagTip.clear();
        auto list = tags();
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            auto r = tagRect(i);
            c.fill(r, 0xE0101010);
            bool h = r.contains(mx, my);
            c.frame(r, h ? 0xFFFFFFFF : 0xFF505050);
            if (auto st = stackOf(list[static_cast<std::size_t>(i)]->icon)) c.item(*st, r.x + 3, r.y + 3);
            if (h) mTagTip = {tagTitle(list[static_cast<std::size_t>(i)]->id), tagDescription(list[static_cast<std::size_t>(i)]->id)};
        }
    }

    bool clickTags(float mx, float my) {
        auto list = tags();
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            if (!tagRect(i).contains(mx, my)) continue;
            auto comp = mComponent;
            int  idx  = mIndex;
            pushHistory([comp, idx] { openComponent(comp, library().scenesOf(comp).size() > static_cast<std::size_t>(idx) ? library().scenesOf(comp)[static_cast<std::size_t>(idx)]->id : ""); });
            openTag(list[static_cast<std::size_t>(i)]->id);
            return true;
        }
        return false;
    }

    void layoutButtons() {
        mButtons.clear();
        float y = height - 30, cx = width / 2, s = 20;
        bool  hasPrev = mIndex > 0, hasNext = mIndex + 1 < static_cast<int>(mScenes.size());
        mButtons.push_back({{cx - s * 1.5f - 6, y, s, s}, &kIconLeft, ctr("ponder.ui.previous", "Previous Scene"), "A", [this] { changeScene(-1); }, hasPrev});
        mButtons.push_back({{cx - s / 2, y, s, s}, &kIconClose, ctr("ponder.ui.close", "Close"), "Esc", [] { deferUi([] { screen::close(); }); }});
        mButtons.push_back({{cx + s / 2 + 6, y, s, s}, &kIconRight, ctr("ponder.ui.next", "Next Scene"), "D", [this] { changeScene(1); }, hasNext});
        if (hasNext) {
            // Create highlights the next scene once this one is over
            if (scene() && mTime >= static_cast<float>(scene()->length())) mButtons.back().lit = true;
        }
        mButtons.push_back({{8, y, s, s}, &kIconIdentify, ctr("ponder.ui.identify", "Identify"), "Q", [this] { toggleIdentify(); }, true, mIdentify});
        mButtons.push_back({{8 + s + 4, y, s, s}, &kIconSlow, ctr("ponder.ui.slow_text", "Comfy Reading"), "", [this] { mSlow = !mSlow; }, true, mSlow});
        mButtons.push_back({{width - 8 - s, y, s, s}, &kIconReplay, ctr("ponder.ui.replay", "Replay"), "S", [this] { replay(); }});
        if (canThinkBack()) {
            mButtons.push_back({{width - 8 - 2 * s - 4, y, s, s}, &kIconBack, ctr("ponder.ui.think_back", "Think Back"), "", [] { thinkBack(); }});
        }
    }

    std::string                     mComponent;
    std::vector<cp::Scene const*>   mScenes;
    int                             mIndex = 0;
    float                           mTime  = 0;
    float                           mFade  = 1;
    bool                            mIdentify = false, mSlow = false, mLoggedDraw = false;
    cp::View                        mView;
    std::chrono::steady_clock::time_point mLastFrame{};
    float                           mMouseX = 0, mMouseY = 0, mDragX = 0, mDragY = 0;
    bool                            mDragging = false, mSubjectHover = false;
    std::vector<UiButton>           mButtons;
    std::vector<std::string>        mTagTip;
    std::shared_ptr<ItemStack>      mSubject;
};

// ---- entry grids -------------------------------------------------------------------------------

constexpr float kCell = 18;

/// Adds a grid of item slots; `onClick(id)` for entries with scenes.
void addEntryGrid(screen::Screen& s, float x, float y, int columns, std::vector<std::string> const& ids, std::function<void(std::string const&)> onClick) {
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
        auto const& id   = ids[static_cast<std::size_t>(i)];
        bool        has  = library().has(id);
        auto&       slot = s.add<screen::Slot>(lv::core::common::gridCell(x, y, columns, kCell, i), [id, has, onClick](Button b, bool) {
            if (b == Button::Left && has) onClick(id);
        });
        slot.stack = stackOf(id);
        auto name  = itemName(id);
        slot.tooltip = {name.empty() ? id : name};
        if (!has) slot.tooltip.push_back("-");
        slot.enabled = has;
    }
}

class TagScreen : public screen::Screen {
public:
    explicit TagScreen(std::string tag) : Screen(tagTitle(tag), 0, 0), mTag(std::move(tag)) {}
    void init() override {
        auto const* t = library().tag(mTag);
        auto        items = t ? t->items : std::vector<std::string>{};
        int         cols  = 9;
        int         rows  = std::max(1, (static_cast<int>(items.size()) + cols - 1) / cols);
        width             = cols * kCell + 16;
        height            = 64 + rows * kCell + 8;
        add<screen::Label>(Rect{8, 46, width - 16, 10}, ctr("ponder.ui.associated", "Associated Entries"), color::kTextDark);
        addEntryGrid(*this, 8, 58, cols, items, [this](std::string const& id) {
            auto tag = mTag;
            pushHistory([tag] { openTag(tag); });
            openComponent(id);
        });
        if (canThinkBack()) add<screen::IconButton>(Rect{width - 20, 4, 14, 14}, "<", [] { thinkBack(); }).tooltip = {ctr("ponder.ui.think_back", "Think Back")};
        log().info("ponder: tag {} opened ({} entries)", mTag, items.size());
    }
    void renderExtra(Canvas& c, float, float) override {
        auto lines = cp::wrapText(tagDescription(mTag), width - 16, [&](std::string const& s) { return c.textWidth(s); });
        for (std::size_t i = 0; i < lines.size() && i < 3; ++i) c.text(8, 18 + static_cast<float>(i) * 9, lines[i], color::kTextDark, Align::Left, false);
    }

private:
    std::string mTag;
};

class IndexScreen : public screen::Screen {
public:
    IndexScreen() : Screen(ctr("ponder.ui.index_title", "Ponder Index"), 0, 0) {}
    void init() override {
        auto comps = library().components();
        int  cols  = 18;
        int  rows  = std::max(1, (static_cast<int>(comps.size()) + cols - 1) / cols);
        width      = cols * kCell + 16;
        height     = 20 + rows * kCell + 8;
        addEntryGrid(*this, 8, 20, cols, comps, [](std::string const& id) {
            pushHistory([] { openIndexEntries(); });
            openComponent(id);
        });
        if (canThinkBack()) add<screen::IconButton>(Rect{width - 20, 4, 14, 14}, "<", [] { thinkBack(); }).tooltip = {ctr("ponder.ui.think_back", "Think Back")};
        log().info("ponder: index opened ({} entries)", comps.size());
    }
    static void openIndexEntries() {
        deferUi([] { screen::open(std::make_unique<IndexScreen>()); });
    }
};

class TagIndexScreen : public screen::Screen {
public:
    TagIndexScreen() : Screen(ctr("ponder.ui.welcome", "Welcome to Ponder"), 0, 0) {}
    void init() override {
        std::vector<cp::Tag const*> tags;
        for (auto const& t : library().tags())
            if (t.index) tags.push_back(&t);
        int cols = 7;
        int rows = std::max(1, (static_cast<int>(tags.size()) + cols - 1) / cols);
        width    = 230;
        height   = 34 + rows * 26 + 46;
        add<screen::Label>(Rect{8, 18, width - 16, 10}, ctr("ponder.ui.categories", "Available Categories for {0}", ctr("create.ponder.mod_name", "Create")), color::kTextDark, Align::Centre);
        float gx = (width - cols * 26) / 2;
        for (int i = 0; i < static_cast<int>(tags.size()); ++i) {
            auto id   = tags[static_cast<std::size_t>(i)]->id;
            auto r    = lv::core::common::gridCell(gx, 32, cols, 26, i);
            auto& slot = add<screen::Slot>(Rect{r.x + 2, r.y + 2, 22, 22}, [id](Button b, bool) {
                if (b != Button::Left) return;
                pushHistory([] { openIndex(); });
                openTag(id);
            });
            slot.stack   = stackOf(tags[static_cast<std::size_t>(i)]->icon);
            slot.tooltip = {tagTitle(id), tagDescription(id)};
        }
        auto& all = add<screen::IconButton>(Rect{width / 2 - 50, height - 22, 100, 16}, ctr("ponder.ui.index_title", "Ponder Index"), [] {
            pushHistory([] { openIndex(); });
            IndexScreen::openIndexEntries();
        });
        (void)all;
        log().info("ponder: index of {} categories opened", tags.size());
    }
    void renderExtra(Canvas& c, float, float) override {
        auto lines = cp::wrapText(ctr("ponder.ui.index_description", "Click one of the icons below to learn about its associated Items and Blocks"), width - 24,
                                  [&](std::string const& s) { return c.textWidth(s); });
        for (std::size_t i = 0; i < lines.size() && i < 2; ++i) c.text(width / 2, height - 44 + static_cast<float>(i) * 9, lines[i], color::kTextDark, Align::Centre, false);
    }
};

} // namespace

bool ponderKey(int vk) { return gActive && gActive->key(vk); }

bool openComponent(std::string const& component, std::string const& scene) {
    auto const& lib    = library();
    auto        scenes = lib.scenesOf(component);
    if (scenes.empty()) {
        log().info("ponder: no scenes for {}", component);
        return false;
    }
    int idx = 0;
    for (int i = 0; i < static_cast<int>(scenes.size()); ++i)
        if (scenes[static_cast<std::size_t>(i)]->id == scene) idx = i;
    log().info("ponder: opened {} ({} scenes)", component, scenes.size());
    deferUi([component, idx] { screen::open(std::make_unique<PonderUI>(component, idx)); });
    return true;
}

void openIndex() {
    deferUi([] { screen::open(std::make_unique<TagIndexScreen>()); });
}

bool openTag(std::string const& tag) {
    if (!library().tag(tag)) return false;
    deferUi([tag] { screen::open(std::make_unique<TagScreen>(tag)); });
    return true;
}

} // namespace lv::client::ponder

#endif // LV_CLIENT
