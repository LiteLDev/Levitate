#ifdef LV_CLIENT

#include "client/screen/Screen.h"

#include <algorithm>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
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
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

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

struct State {
    std::unique_ptr<Screen> screen;
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

void closeNow(bool popHost) {
    auto& s = state();
    if (!s.screen) return;
    auto screen = std::move(s.screen);
    if (popHost && !s.hostScene.empty() && topScene() == s.hostScene) {
        if (auto* stack = sceneStack()) stack->schedulePopScreen(1);
    }
    s.hostScene.clear();
    s.serverType.clear();
    screen->onClose();
}
} // namespace

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
    TextMeasureData  measure{1.0f, 0.0f, shadow, true, false, ui::TextAlignment::Left};
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
    auto  host = s.hostScene;
    if (s.screen) closeNow(false); // keep the host for the new screen
    s.hostScene   = host;
    bool needHost = s.hostScene.empty();
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
    if (s.screen->cursor && !s.screen->cursor->isNull()) {
        c.setOrigin(0, 0);
        c.item(*s.screen->cursor, mx - 8, my - 8);
    }
    if (!tip.empty()) {
        c.setOrigin(0, 0);
        c.tooltip(mx, my, tip);
    }
}

void handleClick(Button b) {
    auto& s = state();
    if (!s.screen) return;
    auto  layout = layoutOf(*s.screen);
    float px = s.mouseX - layout.panel.x, py = s.mouseY - layout.panel.y;
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
    if (s.screen->cursor) s.screen->cursor.reset();
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
    void install() {
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
                renderScreen(ev.uiRenderContext());
                ev.cancel(); // the host scene itself stays invisible
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::MouseInputEvent>([](ll::event::MouseInputEvent& ev) {
            auto& s = state();
            if (!s.screen || s.hostScene.empty()) return;
            auto [gx, gy] = toGui(ev.x(), ev.y());
            if (ev.x() != 0 || ev.y() != 0) s.mouseX = gx, s.mouseY = gy;
            char id   = ev.actionButtonId();
            auto data = ev.buttonData();
            if (id == 1 && data == 1) handleClick(Button::Left);
            else if (id == 2 && data == 1) handleClick(Button::Right);
            else if (id == 3 && data == 1) handleClick(Button::Middle);
            else if (id == 4 && data != 0) handleScroll(data > 0 ? 1 : -1);
            ev.cancel();
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>([](ll::event::KeyInputEvent& ev) {
            auto& s = state();
            if (ev.keyCode() == kKeyShift) s.shift = ev.isDown();
            if (!s.screen) return;
            if (ev.keyCode() == kKeyEscape) return; // the host scene closes, we follow
            if (ev.keyCode() == kKeyInventory && ev.isDown()) close();
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
            if (s.screen && msg.value("type", std::string{}) == s.serverType) close();
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
