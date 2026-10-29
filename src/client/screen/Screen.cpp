#ifdef LV_CLIENT

#include "client/screen/Screen.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/FontHandle.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/gui/ScreenSizeData.h"
#include "mc/client/gui/TextAlignment.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/screens/SceneFactory.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ItemRenderer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

#include <windows.h>
#pragma comment(lib, "user32.lib")

namespace lv::client::screen {

namespace {
mce::Color toColor(unsigned argb) {
    return mce::Color{
        static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(argb & 0xFF) / 255.0f,
        static_cast<float>((argb >> 24) & 0xFF) / 255.0f,
    };
}

RectangleArea area(float x, float y, float w, float h) { return RectangleArea{x, x + w, y, y + h}; }

constexpr float kSlotSize      = 18;
constexpr float kInvWidth      = 176;
constexpr float kInvHeight     = 90;
constexpr float kFontHeight    = 9;
constexpr int   kKeyShift      = 16;
constexpr int   kKeyEscape     = 27;
constexpr int   kKeyInventory  = 69; ///< E

/// A click / wheel notch from the input thread, handled on the client thread.
struct PendingInput {
    float  x = 0, y = 0; ///< GUI pixels at the time of the event
    Button button = Button::Left;
    int    scroll = 0; ///< 0: a click
    bool   shift  = false;
    // text input for the focused field (kind != Mouse)
    enum class Kind { Mouse, Key, Char, Paste } kind = Kind::Mouse;
    int         key  = 0;
    bool        ctrl = false;
    char32_t    ch   = 0;
    std::string paste;
};

struct State {
    std::unique_ptr<Screen> screen;
    std::mutex                inputMutex;
    std::vector<PendingInput> pendingInput;
    std::atomic<bool>         pendingClose{false}; ///< Esc / inventory key (input thread)
    TextField*                focused = nullptr;    ///< the focused text field (client thread)
    std::atomic<bool>         textFocus{false};     ///< a field is focused (read by the input thread)
    bool                      ctrl = false;
    char16_t                  highSurrogate = 0;    ///< window thread: first half of a WM_CHAR pair
    std::string             baseScene; ///< top scene before the host was pushed
    std::string             hostScene; ///< the pushed host scene ("" until it appeared)
    int                     framesWaiting = 0;
    float                   mouseX = 0, mouseY = 0; ///< GUI pixels
    bool                    shift  = false;
    bool                    closing = false;
    // server screens
    std::string             serverType;
    Json                    serverContext;
};
State& state() {
    static State s;
    return s;
}

std::unordered_map<std::string, ServerFactory>& factories() {
    static std::unordered_map<std::string, ServerFactory> m;
    return m;
}

ISceneStack* sceneStack() {
    auto* c = currentClient();
    if (!c) return nullptr;
    return c->getClientSceneStack().get();
}

std::string topScene() {
    auto* s = sceneStack();
    return s ? s->getScreenName() : std::string{};
}

/// Panel and inventory rectangles in GUI pixels.
struct Layout {
    Rect panel;
    Rect inventory;
};
Layout layoutOf(Screen const& s) {
    auto [sw, sh] = guiSize();
    float total   = s.height + (s.showInventory ? kInvHeight + 4 : 0);
    float w       = std::max(s.width, s.showInventory ? kInvWidth : 0.0f);
    auto  outer   = core::common::centred(sw, sh, w, total);
    Rect  panel{outer.x + (w - s.width) / 2, outer.y, s.width, s.height};
    Rect  inv{outer.x + (w - kInvWidth) / 2, outer.y + s.height + 4, kInvWidth, kInvHeight};
    return {panel, inv};
}

/// Inventory slot index under (x, y) relative to the inventory panel: main rows (9..35) then
/// the hotbar (0..8), as Java's container layout.
int inventorySlotAt(float x, float y) {
    int main = core::common::gridIndexAt(8, 8, 9, 27, kSlotSize, x, y);
    if (main >= 0) return main + 9;
    int hot = core::common::gridIndexAt(8, 8 + 3 * kSlotSize + 4, 9, 9, kSlotSize, x, y);
    return hot;
}

Rect inventorySlotRect(int slot) {
    if (slot >= 9) return core::common::gridCell(8, 8, 9, kSlotSize, slot - 9);
    return core::common::gridCell(8, 8 + 3 * kSlotSize + 4, 9, kSlotSize, slot);
}

void drawInventory(Canvas& c, Rect const& inv, float mx, float my, std::vector<std::string>& tip) {
    c.setOrigin(inv.x, inv.y);
    c.bevel({0, 0, inv.w, inv.h}, color::kPanel, color::kPanelLight, color::kPanelDark, false);
    auto* player = currentPlayer();
    if (!player) return;
    auto& items = player->getInventory();
    for (int slot = 0; slot < 36; ++slot) {
        auto r = inventorySlotRect(slot);
        c.bevel(r, color::kSlot, color::kSlotDark, color::kSlotLight, true);
        auto const& s = items.getItem(slot);
        if (!s.isNull() && s.mCount > 0) {
            c.item(s, r.x + 1, r.y + 1, true);
            if (r.contains(mx - inv.x, my - inv.y)) tip = {s.getName()};
        }
        if (r.contains(mx - inv.x, my - inv.y)) c.fill(r.inset(1), color::kHover);
    }
}

/// `notify`: the player closed it (Esc, inventory key, the host scene went): a server screen tells
/// the server. Closes the server asked for, replacements and leaving the level don't.
void closeNow(bool popHost, bool notify = true) {
    auto& s = state();
    if (!s.screen) return;
    s.focused      = nullptr;
    s.textFocus    = false;
    s.pendingClose = false;
    {
        std::lock_guard lock(s.inputMutex);
        s.pendingInput.clear();
    }
    auto screen = std::move(s.screen);
    if (notify && !s.serverType.empty()) {
        lv::feature::net::client::send("screen.closed", {{"type", s.serverType}, {"context", s.serverContext}});
    }
    if (popHost && !s.hostScene.empty() && topScene() == s.hostScene) {
        if (auto* stack = sceneStack()) stack->schedulePopScreen(1);
    }
    s.hostScene.clear();
    s.serverType.clear();
    screen->onClose();
}
} // namespace

// ---- text input ----------------------------------------------------------------------------------

namespace {
constexpr int kKeyControl = 17;

std::u32string decodeUtf8(std::string_view in) {
    std::u32string out;
    for (std::size_t i = 0; i < in.size();) {
        auto     c = static_cast<unsigned char>(in[i]);
        char32_t cp;
        int      n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, n = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
        else {
            ++i;
            continue;
        }
        if (i + n > in.size()) break;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string encodeUtf8(std::u32string_view in) {
    std::string out;
    for (char32_t cp : in) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

/// The clipboard's text as UTF-8 (window thread).
std::string clipboardText() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto const* w = static_cast<wchar_t const*>(GlobalLock(h))) {
            std::u32string u;
            for (auto const* p = w; *p; ++p) {
                char32_t c = static_cast<char32_t>(*p);
                if (c >= 0xD800 && c <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(p[1]) - 0xDC00);
                    ++p;
                }
                if (c == U'\r' || c == U'\n' || c == U'\t') c = U' ';
                if (c < 32 || c == 127 || (c >= 0xD800 && c <= 0xDFFF)) continue; // controls, lone surrogates
                u.push_back(c);
            }
            GlobalUnlock(h);
            out = encodeUtf8(u);
        }
    }
    CloseClipboard();
    return out;
}

HWND gameWindow();

/// The game's own window procedure (the window class's), detoured: every character message
/// reaches it, whether it comes from the system, from a subclass in front of it or straight from
/// a tool that calls it directly (LeviHeadlessInput's injected text).
WNDPROC gGameWndProc = nullptr;

/// A character for the focused text field (window thread); false when no field is focused.
bool takeChar(WPARAM w) {
    auto& s = state();
    if (!s.textFocus) return false;
    auto     unit = static_cast<char16_t>(w);
    char32_t ch   = unit;
    if (unit >= 0xD800 && unit <= 0xDBFF) { // first half of a surrogate pair
        s.highSurrogate = unit;
        return true;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (!s.highSurrogate) return true;
        ch              = 0x10000 + ((static_cast<char32_t>(s.highSurrogate) - 0xD800) << 10) + (unit - 0xDC00);
        s.highSurrogate = 0;
    }
    if (ch >= 32 && ch != 127) {
        PendingInput in;
        in.kind = PendingInput::Kind::Char;
        in.ch   = ch;
        std::lock_guard lock(s.inputMutex);
        s.pendingInput.push_back(std::move(in));
    }
    return true;
}

LRESULT CALLBACK textInputWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CHAR && takeChar(w)) return 0; // the field takes it, the game doesn't see it
    return gGameWndProc(h, msg, w, l);
}

std::atomic<bool> gTextInputInstalled{false};
bool              textInputInstalled() { return gTextInputInstalled; }

/// Detours the game window's class procedure once (any thread; called from the input events).
void installTextInput() {
    static std::mutex m;
    if (gTextInputInstalled) return;
    std::lock_guard lock(m);
    if (gTextInputInstalled) return;
    HWND w = gameWindow();
    if (!w || GetWindowThreadProcessId(w, nullptr) != GetCurrentThreadId()) return; // the window thread only
    auto proc = reinterpret_cast<WNDPROC>(GetClassLongPtrW(w, GCLP_WNDPROC));
    if (!proc) return;
    void* original = nullptr;
    if (ll::memory::hook(
            reinterpret_cast<ll::memory::FuncPtr>(proc),
            reinterpret_cast<ll::memory::FuncPtr>(&textInputWndProc),
            reinterpret_cast<ll::memory::FuncPtr*>(&original),
            ll::memory::HookPriority::Normal,
            false // no thread pause (it can deadlock a running game); only this (window) thread runs the procedure
        )
        != 0) {
        log().warn("text fields: could not hook the game's window procedure; typing uses key presses");
        gTextInputInstalled = false;
        return;
    }
    gGameWndProc        = reinterpret_cast<WNDPROC>(original);
    gTextInputInstalled = true;
}
} // namespace

TextField::TextField(Rect r, std::string t, std::function<void(std::string const&)> submit)
: Widget(r),
  text(std::move(t)),
  onSubmit(std::move(submit)) {
    caret = static_cast<int>(decodeUtf8(text).size());
}

TextField::~TextField() {
    auto& s = state();
    if (s.focused == this) {
        s.focused   = nullptr;
        s.textFocus = false;
    }
}

bool TextField::focused() const { return state().focused == this; }

void TextField::focus() {
    auto& s = state();
    if (s.focused == this) return;
    auto* previous = s.focused;
    s.focused      = this;
    s.textFocus    = true;
    mBefore        = text;
    caret          = static_cast<int>(decodeUtf8(text).size());
    // the previous field submits last: its onSubmit may rebuild the screen (then this field's
    // destructor drops the focus again)
    if (previous && previous->onSubmit) previous->onSubmit(previous->text);
}

void TextField::blur(bool submit) {
    auto& s = state();
    if (s.focused != this) return;
    s.focused   = nullptr;
    s.textFocus = false;
    if (submit) {
        if (onSubmit) onSubmit(text);
    } else {
        bool changed = text != mBefore;
        text         = mBefore;
        caret        = static_cast<int>(decodeUtf8(text).size());
        if (changed && onChange) onChange(text);
        if (onCancel) onCancel();
    }
}

void TextField::typeChar(char32_t ch) {
    if (ch < 32 || ch == 127 || (ch >= 0xD800 && ch <= 0xDFFF) || ch > 0x10FFFF) return;
    if (filter && !filter(ch)) return;
    auto u = decodeUtf8(text);
    if (static_cast<int>(u.size()) >= maxLength) return;
    caret = std::clamp(caret, 0, static_cast<int>(u.size()));
    u.insert(u.begin() + caret, ch);
    ++caret;
    text = encodeUtf8(u);
    if (onChange) onChange(text);
}

void TextField::typeText(std::string const& utf8) {
    for (char32_t ch : decodeUtf8(utf8)) typeChar(ch);
}

bool TextField::key(int vk, bool ctrl) {
    auto u = decodeUtf8(text);
    caret  = std::clamp(caret, 0, static_cast<int>(u.size()));
    switch (vk) {
    case VK_RETURN: blur(true); return true;
    case VK_ESCAPE: blur(false); return true;
    case VK_LEFT: caret = std::max(0, caret - 1); return true;
    case VK_RIGHT: caret = std::min(static_cast<int>(u.size()), caret + 1); return true;
    case VK_HOME: caret = 0; return true;
    case VK_END: caret = static_cast<int>(u.size()); return true;
    case VK_BACK:
        if (caret == 0) return true;
        u.erase(u.begin() + caret - 1);
        --caret;
        break;
    case VK_DELETE:
        if (caret >= static_cast<int>(u.size())) return true;
        u.erase(u.begin() + caret);
        break;
    default:
        if (ctrl && vk == 'A') caret = static_cast<int>(u.size());
        return ctrl; // printable keys arrive as characters (WM_CHAR)
    }
    text = encodeUtf8(u);
    if (onChange) onChange(text);
    return true;
}

bool TextField::click(float mx, float my, Button b, bool) {
    if (!enabled || !hovered(mx, my) || b != Button::Left) return false;
    focus();
    return true;
}

void TextField::render(Canvas& c, float, float) {
    bool on = focused();
    c.bevel(rect, color::kSlotDark, on ? color::kTextGold : color::kSlotDark, color::kSlotLight, true);
    float pad = 3, inner = rect.w - 2 * pad, ty = rect.y + (rect.h - kFontHeight) / 2;
    if (text.empty() && !on) {
        c.text(rect.x + pad, ty, placeholder, color::kTextGray);
        return;
    }
    auto u = decodeUtf8(text);
    int  cp = std::clamp(caret, 0, static_cast<int>(u.size()));
    // scroll so the caret stays visible: drop characters from the left
    std::size_t start = 0;
    while (start < static_cast<std::size_t>(cp) && c.textWidth(encodeUtf8(std::u32string_view(u).substr(start, cp - start))) > inner) ++start;
    auto shown = std::u32string_view(u).substr(start);
    std::string visible;
    for (std::size_t n = shown.size(); n > 0; --n) {
        visible = encodeUtf8(shown.substr(0, n));
        if (c.textWidth(visible) <= inner) break;
    }
    c.text(rect.x + pad, ty, visible, color::kText);
    if (on && (GetTickCount64() / 500) % 2 == 0) {
        float x = rect.x + pad + c.textWidth(encodeUtf8(std::u32string_view(u).substr(start, cp - start)));
        c.fill({x, ty - 1, 1, kFontHeight + 1}, color::kText);
    }
}

// ---- canvas ------------------------------------------------------------------------------------

Canvas::Canvas(MinecraftUIRenderContext& ctx, float originX, float originY) : mCtx(ctx), mX(originX), mY(originY) {}

Canvas::~Canvas() { mCtx.flushText(0.0f, std::nullopt); }

void Canvas::fill(Rect const& r, unsigned argb) {
    auto col = toColor(argb);
    mCtx.fillRectangle(area(mX + r.x, mY + r.y, r.w, r.h), col, col.a);
}

void Canvas::frame(Rect const& r, unsigned argb, float t) {
    fill({r.x, r.y, r.w, t}, argb);
    fill({r.x, r.y + r.h - t, r.w, t}, argb);
    fill({r.x, r.y, t, r.h}, argb);
    fill({r.x + r.w - t, r.y, t, r.h}, argb);
}

void Canvas::bevel(Rect const& r, unsigned fillArgb, unsigned light, unsigned dark, bool sunken) {
    fill(r, fillArgb);
    unsigned tl = sunken ? dark : light, br = sunken ? light : dark;
    fill({r.x, r.y, r.w - 1, 1}, tl);
    fill({r.x, r.y, 1, r.h - 1}, tl);
    fill({r.x + 1, r.y + r.h - 1, r.w - 1, 1}, br);
    fill({r.x + r.w - 1, r.y + 1, 1, r.h - 1}, br);
}

void Canvas::text(float x, float y, std::string const& s, unsigned argb, Align align, bool shadow) {
    auto* client = currentClient();
    if (!client || s.empty()) return;
    auto  font = client->getFontHandle();
    float w    = textWidth(s);
    float left = align == Align::Left ? x : align == Align::Centre ? x - w / 2 : x - w;
    // showColorSymbol off: "§7" etc. colour the text (server lines carry Create's colours)
    // instead of being drawn as characters
    TextMeasureData  measure{1.0f, 0.0f, shadow, false, false, ui::TextAlignment::Left};
    CaretMeasureData caret{-1, false};
    auto             col = toColor(argb);
    mCtx.drawText(font.getFont(), area(mX + left, mY + y, w + 4, kFontHeight + 2), std::string(s), col, col.a, ui::TextAlignment::Left, measure, caret);
}

float Canvas::textWidth(std::string const& s) const {
    auto* client = currentClient();
    if (!client) return 0;
    auto font = client->getFontHandle();
    return static_cast<float>(mCtx.getLineLength(font.getFont(), s, 1.0f, false));
}

void Canvas::item(ItemStack const& stack, float x, float y, bool showCount) {
    if (stack.isNull() || stack.mCount == 0) return;
    auto* client = currentClient();
    bool  drawn  = false;
    if (client) {
        if (auto* renderer = client->getItemRenderer()) {
            BaseActorRenderContext rc(mCtx.mScreenContext, *client, client->getMinecraftGame());
            renderer->renderGuiItemNew(rc, stack, 0, mX + x, mY + y, false, 1.0f, 1.0f, 1.0f, 0);
            drawn = true;
        }
    }
    if (!drawn) {
        auto name = stack.getName();
        text(x + 8, y + 4, name.substr(0, 2), color::kText, Align::Centre);
    }
    if (showCount && stack.mCount > 1) text(x + 17, y + 9, std::to_string(stack.mCount), color::kText, Align::Right);
}

void Canvas::tooltip(float x, float y, std::vector<std::string> const& lines) {
    if (lines.empty()) return;
    float w = 0;
    for (auto const& l : lines) w = std::max(w, textWidth(l));
    float h       = static_cast<float>(lines.size()) * (kFontHeight + 1) + 6;
    auto [sw, sh] = guiSize();
    float ox = mX, oy = mY;
    mX = mY = 0;
    float bx = std::min(ox + x + 12, sw - w - 10), by = std::min(oy + y - 12, sh - h - 4);
    bx = std::max(bx, 2.0f), by = std::max(by, 2.0f);
    fill({bx, by, w + 8, h}, color::kTooltip);
    frame({bx, by, w + 8, h}, color::kTooltipEdge);
    for (std::size_t i = 0; i < lines.size(); ++i) text(bx + 4, by + 4 + static_cast<float>(i) * (kFontHeight + 1), lines[i], i == 0 ? color::kText : color::kTextGray);
    mX = ox, mY = oy;
}

// ---- widgets -----------------------------------------------------------------------------------

Label::Label(Rect r, std::string t, unsigned c, Align a, bool s) : Widget(r), text(std::move(t)), argb(c), align(a), shadow(s) {}

void Label::render(Canvas& c, float, float) {
    float x = align == Align::Left ? rect.x : align == Align::Centre ? rect.x + rect.w / 2 : rect.x + rect.w;
    c.text(x, rect.y, text, argb, align, shadow);
}

IconButton::IconButton(Rect r, std::string s, std::function<void()> fn) : Widget(r), symbol(std::move(s)), onClick(std::move(fn)) {}

void IconButton::render(Canvas& c, float mx, float my) {
    unsigned fillCol = toggled ? litColour : color::kButton;
    if (!enabled) fillCol = color::kSlot;
    c.bevel(rect, fillCol, toggled ? color::kSlotDark : color::kPanelLight, toggled ? color::kPanelLight : color::kSlotDark, toggled);
    if (hovered(mx, my) && enabled) c.fill(rect.inset(1), color::kHover);
    c.text(rect.x + rect.w / 2, rect.y + (rect.h - kFontHeight) / 2 + 1, symbol, enabled ? color::kText : color::kTextGray, Align::Centre);
}

bool IconButton::click(float mx, float my, Button b, bool) {
    if (!enabled || b != Button::Left || !hovered(mx, my)) return false;
    if (onClick) onClick();
    return true;
}

Slot::Slot(Rect r, std::function<void(Button, bool)> fn) : Widget(r), onClick(std::move(fn)) {}

void Slot::render(Canvas& c, float mx, float my) {
    c.bevel(rect, color::kSlot, color::kSlotDark, color::kSlotLight, true);
    if (stack && !stack->isNull() && stack->mCount > 0) c.item(*stack, rect.x + 1, rect.y + 1);
    if (highlight) c.frame(rect, color::kTextGold);
    if (hovered(mx, my)) c.fill(rect.inset(1), color::kHover);
}

bool Slot::click(float mx, float my, Button b, bool shift) {
    if (!hovered(mx, my)) return false;
    if (onClick) onClick(b, shift);
    return true;
}

ScrollInput::ScrollInput(Rect r, std::string l, int mn, int mx, int v, std::function<void(int)> fn)
: Widget(r),
  label(std::move(l)),
  min(mn),
  max(mx),
  value(v),
  onChange(std::move(fn)) {}

void ScrollInput::render(Canvas& c, float mx, float my) {
    c.bevel(rect, color::kSlot, color::kSlotDark, color::kSlotLight, true);
    std::string shown = format ? format(value)
                      : (!options.empty() && value >= 0 && value < static_cast<int>(options.size())) ? options[static_cast<std::size_t>(value)]
                                                                                                       : std::to_string(value);
    std::string text = label.empty() ? shown : label + ": " + shown;
    c.text(rect.x + 4, rect.y + (rect.h - kFontHeight) / 2 + 1, text, color::kText);
    if (hovered(mx, my)) c.fill(rect.inset(1), color::kHover);
}

bool ScrollInput::scroll(float mx, float my, int delta, bool shift) {
    if (!enabled || !hovered(mx, my) || delta == 0) return false;
    int step = shift ? 10 : 1;
    int next = std::clamp(value + (delta > 0 ? step : -step), min, max);
    if (next != value) {
        value = next;
        if (onChange) onChange(value);
    }
    return true;
}

bool ScrollInput::click(float mx, float my, Button b, bool shift) {
    // clicks step forward / backward too (handy without a wheel)
    if (b == Button::Middle) return false;
    return scroll(mx, my, b == Button::Left ? 1 : -1, shift);
}

// ---- screen management -------------------------------------------------------------------------

std::pair<float, float> guiSize() {
    auto* c = currentClient();
    if (!c) return {0, 0};
    auto gui = c->getGuiData();
    Vec2 sz  = gui->mScreenSizeData->clientUIScreenSize.get();
    return {sz.x, sz.y};
}

void open(std::unique_ptr<Screen> screen) {
    auto& s    = state();
    auto  host      = s.hostScene;
    bool  hadScreen = s.screen != nullptr;
    if (hadScreen) closeNow(false, false); // keep the host for the new screen
    s.pendingClose = false;
    s.hostScene    = host;
    // one host scene per screen: a screen replaced before its first frame (e.g. opened twice)
    // keeps the host already pushed for it, which is still being learned
    bool needHost = !hadScreen && s.hostScene.empty();
    s.screen      = std::move(screen);
    s.screen->init();
    s.closing = false;
    {
        auto l       = layoutOf(*s.screen);
        auto [gw, gh] = guiSize();
        // one line per opened screen; e2e scenarios read the layout to click widgets
        log().info(
            "screen opened: {} panel={},{} inv={},{} gui={}x{}",
            s.screen->title, l.panel.x, l.panel.y, l.inventory.x, l.inventory.y, gw, gh
        );
    }
    if (!needHost) return;
    auto* c = currentClient();
    if (!c) return;
    s.baseScene     = topScene();
    s.framesWaiting = 0;
    if (auto scene = c->getSceneFactory().createMobEffectsScreen()) c->getClientSceneStack()->pushScreen(scene, false);
}

void close() { closeNow(true); }

Screen* current() { return state().screen.get(); }

void registerServerScreen(std::string const& type, ServerFactory factory) { factories()[type] = std::move(factory); }

// ---- events ------------------------------------------------------------------------------------

namespace {
void renderScreen(MinecraftUIRenderContext& ctx) {
    auto& s = state();
    if (!s.screen) return;
    s.screen->tick();
    if (!s.screen) return; // closed by tick
    auto  layout = layoutOf(*s.screen);
    float mx = s.mouseX, my = s.mouseY;
    auto [sw, sh] = guiSize();
    Canvas c(ctx);
    c.fill({0, 0, sw, sh}, color::kDim);

    std::vector<std::string> tip;
    if (s.screen->showInventory) drawInventory(c, layout.inventory, mx, my, tip);

    c.setOrigin(layout.panel.x, layout.panel.y);
    c.bevel({0, 0, layout.panel.w, layout.panel.h}, color::kPanel, color::kPanelLight, color::kPanelDark, false);
    c.frame({-1, -1, layout.panel.w + 2, layout.panel.h + 2}, color::kSlotDark);
    c.text(layout.panel.w / 2, 5, s.screen->title, color::kTextDark, Align::Centre, false);
    float px = mx - layout.panel.x, py = my - layout.panel.y;
    for (auto const& w : s.screen->widgets) {
        if (!w->visible) continue;
        w->render(c, px, py);
        if (w->hovered(px, py) && !w->tooltip.empty()) tip = w->tooltip;
    }
    s.screen->renderExtra(c, px, py);
    if (s.screen->cursor && !s.screen->cursor->isNull()) { // render-only icon of cursorItem
        c.setOrigin(0, 0);
        c.item(*s.screen->cursor, mx - 8, my - 8);
    }
    if (!tip.empty()) {
        c.setOrigin(0, 0);
        c.tooltip(mx, my, tip);
    }
}

} // namespace

void Screen::setCursorItem(Json item) {
    cursorItem = std::move(item);
    cursor.reset();
    if (cursorItem.is_null()) return;
    // a fresh stack built from data on the client thread (never a copy of a game-owned stack)
    auto icon = std::make_shared<ItemStack>();
    *icon     = lv::feature::common::items::fromJson(cursorItem);
    if (!icon->isNull()) cursor = std::move(icon);
}

namespace {

void handleClick(Button b) {
    auto& s = state();
    if (!s.screen) return;
    auto  layout = layoutOf(*s.screen);
    float px = s.mouseX - layout.panel.x, py = s.mouseY - layout.panel.y;
    // a click outside the focused text field leaves it (keeping the text, as Create)
    if (s.focused && !s.focused->rect.contains(px, py)) s.focused->blur(true);
    if (!s.screen) return;
    for (auto it = s.screen->widgets.rbegin(); it != s.screen->widgets.rend(); ++it) {
        auto& w = **it;
        if (w.visible && w.click(px, py, b, s.shift)) return;
        if (!s.screen) return;
    }
    if (s.screen->showInventory) {
        float ix = s.mouseX - layout.inventory.x, iy = s.mouseY - layout.inventory.y;
        int   slot = inventorySlotAt(ix, iy);
        if (slot >= 0) {
            if (auto* player = currentPlayer()) s.screen->clickInventory(slot, player->getInventory().getItem(slot), b, s.shift);
            return;
        }
    }
    if (s.screen->clickBackground(px, py, b, s.shift)) return;
    // clicking outside everything drops the ghost cursor (as dropping a ghost item)
    if (s.screen->cursor || !s.screen->cursorItem.is_null()) s.screen->setCursorItem(nullptr);
}

void handleScroll(int delta) {
    auto& s = state();
    if (!s.screen) return;
    auto  layout = layoutOf(*s.screen);
    float px = s.mouseX - layout.panel.x, py = s.mouseY - layout.panel.y;
    for (auto& w : s.screen->widgets) {
        if (w->visible && w->scroll(px, py, delta, s.shift)) return;
    }
}

/// The game's main window (the largest visible top-level window of this process).
HWND gameWindow() {
    static HWND cached = nullptr;
    if (cached && IsWindow(cached)) return cached;
    struct Best {
        HWND h    = nullptr;
        long area = 0;
    } best;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
            RECT r{};
            GetClientRect(h, &r);
            auto* b    = reinterpret_cast<Best*>(p);
            long  area = (r.right - r.left) * (r.bottom - r.top);
            if (area > b->area) b->h = h, b->area = area;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best)
    );
    cached = best.h;
    return cached;
}

std::pair<float, float> toGui(short x, short y);

/// The pointer in GUI pixels. The pointer is read from the OS cursor (the mouse events carry
/// stale positions while a screen is open); LeviHeadlessInput virtualises that cursor.
std::pair<float, float> pointerGui() {
    POINT p{};
    HWND  w = gameWindow();
    if (!w || !GetCursorPos(&p) || !ScreenToClient(w, &p)) return {state().mouseX, state().mouseY};
    return toGui(static_cast<short>(p.x), static_cast<short>(p.y));
}

/// Pointer position in GUI pixels from window pixels.
std::pair<float, float> toGui(short x, short y) {
    auto* c = currentClient();
    if (!c) return {0, 0};
    auto  gui   = c->getGuiData();
    Vec2  ui    = gui->mScreenSizeData->clientUIScreenSize.get();
    Vec2  px    = gui->mScreenSizeData->clientScreenSize.get();
    float sx    = px.x > 0 ? ui.x / px.x : 1.0f;
    float sy    = px.y > 0 ? ui.y / px.y : 1.0f;
    return {static_cast<float>(x) * sx, static_cast<float>(y) * sy};
}
} // namespace

class ScreenModule {
public:
    // Client input / render listeners are added at mod load, before the game's threads run:
    // hooking them while the client is running (mod enable) can deadlock the game.
    void install() {}
    void load() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BeforeUIRenderEvent>([](ll::event::BeforeUIRenderEvent& ev) {
            auto& s = state();
            if (!s.screen) return;
            auto name = ev.screenView().getScreenName();
            // learn which scene is our host, notice when it was closed (Esc)
            auto top = topScene();
            if (s.hostScene.empty()) {
                if (!top.empty() && top != s.baseScene) s.hostScene = top;
                else if (++s.framesWaiting > 600) s.hostScene = top; // never appeared: draw anyway
            } else if (top != s.hostScene) {
                closeNow(false);
                return;
            }
            if (!s.hostScene.empty() && name == s.hostScene) {
                // clicks queued by the input thread run here, on the client thread: screens read
                // the inventory and items, which the game only allows on this thread
                std::vector<PendingInput> inputs;
                {
                    std::lock_guard lock(s.inputMutex);
                    inputs.swap(s.pendingInput);
                }
                for (auto const& in : inputs) {
                    if (!state().screen) break;
                    s.mouseX = in.x, s.mouseY = in.y;
                    bool shift = s.shift;
                    s.shift    = in.shift;
                    using K = PendingInput::Kind;
                    if (in.kind == K::Key) {
                        if (s.focused) s.focused->key(in.key, in.ctrl);
                    } else if (in.kind == K::Char) {
                        if (s.focused) s.focused->typeChar(in.ch);
                    } else if (in.kind == K::Paste) {
                        if (s.focused) s.focused->typeText(in.paste);
                    } else if (in.scroll) {
                        handleScroll(in.scroll);
                    } else {
                        handleClick(in.button);
                    }
                    s.shift = shift;
                }
                if (s.pendingClose) {
                    s.pendingClose = false;
                    closeNow(true);
                }
                if (!state().screen) return;
                std::tie(s.mouseX, s.mouseY) = pointerGui();
                renderScreen(ev.uiRenderContext());
                ev.cancel(); // the host scene itself stays invisible
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::MouseInputEvent>([](ll::event::MouseInputEvent& ev) {
            auto& s = state();
            if (!s.screen || s.hostScene.empty()) return;
            installTextInput();
            auto [gx, gy] = pointerGui();
            char id   = ev.actionButtonId();
            auto data = ev.buttonData();
            PendingInput in{gx, gy, Button::Left, 0, s.shift};
            bool         any = true;
            if (id == 1 && data == 1) in.button = Button::Left;
            else if (id == 2 && data == 1) in.button = Button::Right;
            else if (id == 3 && data == 1) in.button = Button::Middle;
            else if (id == 4 && data != 0) in.scroll = data > 0 ? 1 : -1;
            else any = false;
            if (any) { // handled on the client thread (the render listener)
                std::lock_guard lock(s.inputMutex);
                s.pendingInput.push_back(in);
            }
            ev.cancel();
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>([](ll::event::KeyInputEvent& ev) {
            auto& s = state();
            if (ev.keyCode() == kKeyShift) s.shift = ev.isDown();
            s.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 || (ev.keyCode() == kKeyControl && ev.isDown());
            installTextInput(); // the window thread: subclass the game window once (WM_CHAR)
            if (!s.screen) return;
            if (s.textFocus) { // every key goes to the focused field
                if (ev.isDown() && ev.keyCode() != kKeyShift && ev.keyCode() != kKeyControl) {
                    PendingInput in;
                    in.kind = PendingInput::Kind::Key;
                    in.key  = ev.keyCode();
                    in.ctrl = s.ctrl;
                    if (s.ctrl && ev.keyCode() == 'V') {
                        in.kind  = PendingInput::Kind::Paste;
                        in.paste = clipboardText();
                    }
                    std::lock_guard lock(s.inputMutex);
                    s.pendingInput.push_back(std::move(in));
                    // no window subclass (this thread doesn't own the window): characters from the key
                    if (!textInputInstalled() && !s.ctrl) {
                        BYTE    keys[256]{};
                        wchar_t buf[8]{};
                        GetKeyboardState(keys);
                        int n = ToUnicode(static_cast<UINT>(ev.keyCode()), MapVirtualKeyW(static_cast<UINT>(ev.keyCode()), MAPVK_VK_TO_VSC), keys, buf, 8, 0);
                        for (int i = 0; i < n; ++i) {
                            if (buf[i] < 32 || buf[i] == 127 || (buf[i] >= 0xD800 && buf[i] <= 0xDFFF)) continue;
                            PendingInput c;
                            c.kind = PendingInput::Kind::Char;
                            c.ch   = buf[i];
                            s.pendingInput.push_back(std::move(c));
                        }
                    }
                }
                if (ev.keyCode() != kKeyShift) ev.cancel();
                return;
            }
            if (ev.keyCode() == kKeyEscape) { // as Create: Esc closes the screen (and its host scene)
                if (ev.isDown()) s.pendingClose = true; // closed on the client thread (render listener)
                ev.cancel();
                return;
            }
            if (ev.keyCode() == kKeyInventory && ev.isDown()) s.pendingClose = true;
            if (ev.keyCode() != kKeyShift) ev.cancel();
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto& s = state();
            s.screen.reset();
            s.hostScene.clear();
            s.serverType.clear();
        }));

        // server-opened screens
        namespace net = lv::feature::net;
        net::client::on("screen.open", [](Json const& msg) {
            auto type = msg.value("type", std::string{});
            auto it   = factories().find(type);
            if (it == factories().end()) {
                log().warn("server opened unknown screen type {}", type);
                return;
            }
            auto context = msg.value("context", Json::object());
            auto send    = [type, context](Json const& action) {
                net::client::send("screen.action", {{"type", type}, {"context", context}, {"action", action}});
            };
            auto screen = it->second(msg.value("data", Json::object()), send);
            if (!screen) return;
            open(std::move(screen));
            state().serverType    = type;
            state().serverContext = context;
        });
        net::client::on("screen.update", [](Json const& msg) {
            auto& s = state();
            if (s.screen && msg.value("type", std::string{}) == s.serverType) s.screen->onData(msg.value("data", Json::object()));
        });
        net::client::on("screen.close", [](Json const& msg) {
            auto& s = state();
            if (s.screen && msg.value("type", std::string{}) == s.serverType) closeNow(true, false); // the server knows
        });
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        state().screen.reset();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::screen

LV_REGISTER_MODULE(lv::client::screen::ScreenModule);

#endif // LV_CLIENT
