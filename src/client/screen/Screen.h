#pragma once

// Native screen framework of the client mod (U3, SPEC §3.3). Client build only.
//
// A Screen is a tree-less list of widgets laid out in GUI pixels around a centred panel. The
// framework pushes a vanilla host scene so the game releases the mouse, draws the screen on top
// of it every frame (the host itself is not drawn), routes mouse and keyboard input to the
// widgets and closes the screen with Esc or the inventory key (as Create's screens).
//
//     class MyScreen : public screen::Screen {
//     public:
//         MyScreen() : Screen("My title", 176, 120) {}
//         void init() override {
//             add<screen::Button>(Rect{8, 90, 18, 18}, "+", [this] { ... });
//         }
//     };
//     screen::open(std::make_unique<MyScreen>());
//
// Screens opened by the server (screens::open on the server, feature/common/Screens.h) are
// built by the factory registered for their type: `screen::registerServerScreen(type, factory)`.
// Widget coordinates are relative to the panel's top-left corner.

#ifdef LV_CLIENT

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/common/Layout.h"

class ItemStack;
class MinecraftUIRenderContext;

namespace lv::client::screen {

using core::common::Rect;
using Json = nlohmann::json;

/// Create-like palette (ARGB).
namespace color {
inline constexpr unsigned kPanel       = 0xFFB9A786;
inline constexpr unsigned kPanelDark   = 0xFF5B4B3A;
inline constexpr unsigned kPanelLight  = 0xFFE3D3B0;
inline constexpr unsigned kSlot        = 0xFF6E5F4C;
inline constexpr unsigned kSlotDark    = 0xFF3A3026;
inline constexpr unsigned kSlotLight   = 0xFFA89676;
inline constexpr unsigned kHover       = 0x60FFFFFF;
inline constexpr unsigned kText        = 0xFFFFFFFF;
inline constexpr unsigned kTextDark    = 0xFF3F3428;
inline constexpr unsigned kTextGold    = 0xFFFFD86B;
inline constexpr unsigned kTextGray    = 0xFFB0B0B0;
inline constexpr unsigned kButton      = 0xFF8C7A5E;
inline constexpr unsigned kButtonOn    = 0xFF5E9C4F;
inline constexpr unsigned kButtonOff   = 0xFF9C4F4F;
inline constexpr unsigned kTooltip     = 0xF0100010;
inline constexpr unsigned kTooltipEdge = 0xFF5000FF;
inline constexpr unsigned kDim         = 0xA0101010;
} // namespace color

enum class Align { Left, Centre, Right };

/// Drawing helpers over the game's UI render context (valid during one frame).
class Canvas {
public:
    explicit Canvas(MinecraftUIRenderContext& ctx, float originX = 0, float originY = 0);
    ~Canvas();

    void  fill(Rect const& r, unsigned argb);
    void  frame(Rect const& r, unsigned argb, float thickness = 1);
    /// A raised (Create panel) or sunken (slot) bevel box.
    void  bevel(Rect const& r, unsigned fillArgb, unsigned light, unsigned dark, bool sunken);
    void  text(float x, float y, std::string const& s, unsigned argb = color::kText, Align align = Align::Left, bool shadow = true);
    [[nodiscard]] float textWidth(std::string const& s) const;
    /// An item icon (16x16) with its count; falls back to a short label if the icon can't draw.
    void item(ItemStack const& stack, float x, float y, bool showCount = false);
    /// A tooltip box with lines next to (x, y), kept inside the screen.
    void tooltip(float x, float y, std::vector<std::string> const& lines);

    [[nodiscard]] float originX() const { return mX; }
    [[nodiscard]] float originY() const { return mY; }
    void                setOrigin(float x, float y) { mX = x, mY = y; }
    [[nodiscard]] MinecraftUIRenderContext& context() { return mCtx; }

private:
    MinecraftUIRenderContext& mCtx;
    float                     mX, mY;
};

/// Mouse buttons as the screen sees them.
enum class Button { Left, Right, Middle };

class Widget {
public:
    explicit Widget(Rect r) : rect(r) {}
    virtual ~Widget() = default;

    Rect                     rect;
    bool                     visible = true;
    bool                     enabled = true;
    std::vector<std::string> tooltip; ///< shown while hovered

    virtual void render(Canvas& c, float mx, float my) = 0;
    /// Returns true when the click was used.
    virtual bool click(float /*mx*/, float /*my*/, Button /*b*/, bool /*shift*/) { return false; }
    /// Returns true when the scroll was used (`delta` > 0 = up).
    virtual bool scroll(float /*mx*/, float /*my*/, int /*delta*/, bool /*shift*/) { return false; }
    [[nodiscard]] bool hovered(float mx, float my) const { return visible && rect.contains(mx, my); }
};

class Label : public Widget {
public:
    Label(Rect r, std::string text, unsigned argb = color::kTextDark, Align align = Align::Left, bool shadow = false);
    std::string text;
    unsigned    argb;
    Align       align;
    bool        shadow;
    void        render(Canvas& c, float mx, float my) override;
};

/// A Create icon button: a small square with a symbol; `toggled` draws it pressed / lit.
class IconButton : public Widget {
public:
    IconButton(Rect r, std::string symbol, std::function<void()> onClick);
    std::string           symbol;
    std::function<void()> onClick;
    bool                  toggled   = false;
    unsigned              litColour = color::kButtonOn;
    void                  render(Canvas& c, float mx, float my) override;
    bool                  click(float mx, float my, Button b, bool shift) override;
};

/// A ghost item slot (Create's filter slots): shows a stack, reports clicks.
class Slot : public Widget {
public:
    Slot(Rect r, std::function<void(Button, bool shift)> onClick);
    std::shared_ptr<ItemStack>                   stack; ///< nullptr / empty = nothing
    std::function<void(Button, bool shift)>      onClick;
    bool                                         highlight = false;
    void                                         render(Canvas& c, float mx, float my) override;
    bool                                         click(float mx, float my, Button b, bool shift) override;
};

/// Create's ScrollInput: a value changed with the mouse wheel (Shift: ×10 steps), shown as
/// "label: value"; `options` turns it into a selection.
class ScrollInput : public Widget {
public:
    ScrollInput(Rect r, std::string label, int min, int max, int value, std::function<void(int)> onChange);
    std::string                         label;
    int                                 min, max, value;
    std::vector<std::string>            options; ///< when set, value indexes it
    std::function<std::string(int)>     format;
    std::function<void(int)>            onChange;
    void                                render(Canvas& c, float mx, float my) override;
    bool                                scroll(float mx, float my, int delta, bool shift) override;
    bool                                click(float mx, float my, Button b, bool shift) override;
};

class Screen {
public:
    Screen(std::string title, float width, float height) : title(std::move(title)), width(width), height(height) {}
    virtual ~Screen() = default;

    std::string title;
    float       width, height;

    /// Builds the widgets (called once when opened).
    virtual void init() {}
    /// Called every frame before drawing (animations, state polling).
    virtual void tick() {}
    /// Extra drawing on top of the widgets (panel coordinates).
    virtual void renderExtra(Canvas&, float /*mx*/, float /*my*/) {}
    /// Clicks not used by a widget.
    virtual bool clickBackground(float /*mx*/, float /*my*/, Button /*b*/, bool /*shift*/) { return false; }
    /// Server data for screens opened by the server (`screen.update`).
    virtual void onData(Json const& /*data*/) {}
    virtual void onClose() {}
    /// Whether the player's inventory is drawn below the panel (and clickable).
    bool showInventory = false;
    /// Called when an inventory slot is clicked (`slot` in the player's inventory).
    virtual void clickInventory(int /*slot*/, ItemStack const& /*stack*/, Button /*b*/, bool /*shift*/) {}

    template <class W, class... Args>
    W& add(Args&&... args) {
        auto w   = std::make_unique<W>(std::forward<Args>(args)...);
        auto& r  = *w;
        widgets.push_back(std::move(w));
        return r;
    }

    std::vector<std::unique_ptr<Widget>> widgets;
    /// The item held on the cursor (ghost: picking from the inventory copies, never moves).
    std::shared_ptr<ItemStack> cursor;
};

/// Opens a screen (replaces the current one).
void open(std::unique_ptr<Screen> screen);
/// Closes the current screen (and its host scene).
void close();
[[nodiscard]] Screen* current();

/// Factory for a server-opened screen type: builds the screen from the first data message;
/// `send(action)` sends a "screen.action" for this screen.
using SendFn        = std::function<void(Json const& action)>;
using ServerFactory = std::function<std::unique_ptr<Screen>(Json const& data, SendFn send)>;
void registerServerScreen(std::string const& type, ServerFactory factory);

/// Size of the GUI in GUI pixels (0 outside a world).
[[nodiscard]] std::pair<float, float> guiSize();

} // namespace lv::client::screen

#endif // LV_CLIENT
