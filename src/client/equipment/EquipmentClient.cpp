// Client side of the equipment unit (U31): the toolbox screen and the toolbox radial menu (hold
// Left Alt, Create's "Access Nearby Toolboxes" key), the wand of symmetry, worldshaper and linked
// controller screens, the linked controller's key capture and the backtank air / toolbox HUD.

#ifdef LV_CLIENT

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/equipment/Gear.h"
#include "core/equipment/Shaper.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::equipment {

namespace {

namespace net   = lv::feature::net;
namespace items = lv::feature::common::items;
namespace ce    = lv::core::equipment;
using namespace lv::client::screen;

constexpr int kKeyAlt = 18; ///< Create's default toolbox key: Left Alt

std::shared_ptr<ItemStack> stackOf(Json const& j) {
    if (j.is_null()) return nullptr;
    auto s = items::fromJson(j);
    if (items::isEmpty(s)) return nullptr;
    return std::make_shared<ItemStack>(s);
}

Json posOf(Json const& j) { return j.value("pos", Json::array()); }

// ---- toolbox screen -----------------------------------------------------------------------------------

/// Create's toolbox GUI: eight compartments in an octagon around the lid, the inventory below.
class ToolboxScreen : public Screen {
public:
    ToolboxScreen(Json const& data, SendFn send) : Screen(data.value("title", std::string("Toolbox")), 176, 110), mSend(std::move(send)) {
        showInventory = true;
        mData         = data;
    }

    void init() override {
        static constexpr std::array<std::pair<float, float>, 8> kPos = {
            {{79, 18}, {109, 28}, {121, 50}, {109, 72}, {79, 82}, {49, 72}, {37, 50}, {49, 28}}};
        for (int c = 0; c < 8; ++c) {
            auto [x, y] = kPos[static_cast<std::size_t>(c)];
            mSlots.push_back(&add<Slot>(Rect{x, y, 18, 18}, [this, c](Button b, bool) {
                mSend({{"op", "take"}, {"c", c}, {"half", b == Button::Right}});
            }));
        }
        onData(mData);
    }

    void onData(Json const& data) override {
        mData      = data;
        auto comps = data.value("compartments", Json::array());
        for (std::size_t c = 0; c < mSlots.size(); ++c) {
            auto& slot  = *mSlots[c];
            Json  comp  = c < comps.size() ? comps[c] : Json::object();
            slot.stack  = stackOf(comp.value("item", Json()));
            int count   = comp.value("count", 0);
            auto filter = stackOf(comp.value("filter", Json()));
            if (!slot.stack && filter) slot.stack = filter; // locked, empty compartment: the filter in grey
            slot.highlight = !comp.value("item", Json()).is_null();
            if (slot.stack) slot.stack = std::make_shared<ItemStack>(items::withCount(*slot.stack, std::max(1, std::min(count, 64))));
            slot.tooltip = slot.stack ? std::vector<std::string>{slot.stack->getName() + " x" + std::to_string(count)} : std::vector<std::string>{};
        }
    }

    void renderExtra(Canvas& c, float, float) override {
        // the lid in the middle
        c.bevel({70, 44, 36, 30}, color::kPanelDark, color::kPanelLight, color::kSlotDark, false);
        c.fill({84, 54, 8, 6}, color::kTextGold);
    }

    void clickInventory(int slot, ItemStack const& stack, Button, bool) override {
        if (items::isEmpty(stack)) return;
        mSend({{"op", "put"}, {"slot", slot}});
    }

private:
    SendFn             mSend;
    Json               mData;
    std::vector<Slot*> mSlots;
};

// ---- toolbox radial menu --------------------------------------------------------------------------------------

struct RadialState {
    bool keyDown   = false;
    bool requested = false;
};
RadialState& radial() {
    static RadialState s;
    return s;
}

/// Create's RadialToolboxMenu: SELECT_BOX (several toolboxes) or SELECT_ITEM (the compartments of
/// one); the centre unequips / returns items; releasing the key confirms the hovered entry.
class RadialScreen : public Screen {
public:
    explicit RadialScreen(Json data) : Screen(ctr("create.keyinfo.toolbelt", "Access Nearby Toolboxes"), 180, 180), mData(std::move(data)) {
        auto boxes = mData.value("boxes", Json::array());
        mSelectBox = boxes.size() > 1;
        if (boxes.size() == 1) mBox = 0;
    }

    void renderExtra(Canvas& c, float mx, float my) override {
        float cx = width / 2, cy = height / 2;
        mHover    = hoveredIndex(mx - cx, my - cy);
        auto boxes = mData.value("boxes", Json::array());
        auto equipped = mData.value("equipped", Json());
        if (!equipped.is_null() && !equipped.value("inRange", true)) {
            c.text(cx, 8, ctr("create.toolbox.outOfRange", "Toolbox of held item not in Range"), color::kTextGold, Align::Centre);
        }
        for (int i = 0; i < 8; ++i) {
            double a  = (i * 45.0 - 90.0) * std::numbers::pi / 180.0;
            float  x  = cx + static_cast<float>(std::cos(a)) * 62 - 11;
            float  y  = cy + static_cast<float>(std::sin(a)) * 62 - 11;
            bool   on = mHover == i;
            c.bevel({x, y, 22, 22}, on ? color::kButtonOn : color::kSlot, color::kSlotDark, color::kSlotLight, true);
            if (mSelectBox) {
                if (i < static_cast<int>(boxes.size())) {
                    if (auto item = items::make("levitate:" + boxes[static_cast<std::size_t>(i)].value("colour", std::string("brown")) + "_toolbox", 1))
                        c.item(*item, x + 3, y + 3);
                }
            } else if (mBox >= 0 && mBox < static_cast<int>(boxes.size())) {
                auto comps = boxes[static_cast<std::size_t>(mBox)].value("compartments", Json::array());
                if (i < static_cast<int>(comps.size())) {
                    auto s = stackOf(comps[static_cast<std::size_t>(i)].value("item", Json()));
                    if (s) {
                        c.item(*s, x + 3, y + 3);
                        int n = comps[static_cast<std::size_t>(i)].value("count", 0);
                        c.text(x + 21, y + 14, std::to_string(n), color::kText, Align::Right);
                    }
                }
            }
        }
        // centre: unequip / return items
        bool onCentre = mHover == -2;
        c.bevel({cx - 16, cy - 16, 32, 32}, onCentre ? color::kButtonOff : color::kButton, color::kPanelLight, color::kPanelDark, false);
        c.text(cx, cy - 4, centreLabel(), color::kText, Align::Centre);
        if (onCentre) c.tooltip(mx, my, {centreTooltip()});
    }

    bool clickBackground(float mx, float my, Button b, bool) override {
        if (b != Button::Left) return false;
        confirm(hoveredIndex(mx - width / 2, my - height / 2));
        return true;
    }

    /// The key was released: apply the hovered entry (Create: on key release).
    void release() { confirm(mHover); }

private:
    int hoveredIndex(float dx, float dy) const {
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < 18) return -2;
        if (d < 30) return -1;
        double a = std::atan2(dy, dx) * 180.0 / std::numbers::pi + 90.0 + 22.5;
        while (a < 0) a += 360;
        return static_cast<int>(a / 45.0) % 8;
    }

    std::string centreLabel() const {
        auto equipped = mData.value("equipped", Json());
        if (!equipped.is_null()) return "-";
        return "<<";
    }
    std::string centreTooltip() const {
        auto equipped = mData.value("equipped", Json());
        if (!equipped.is_null()) {
            if (!equipped.value("inRange", true)) return ctr("create.toolbox.detach", "Stop tracking and keep item");
            auto s = stackOf(equipped.value("item", Json()));
            return ctr("create.toolbox.unequip", "Unequip: {0}", s ? s->getName() : std::string());
        }
        return mSelectBox ? ctr("create.toolbox.depositAll", "Return items to nearby Toolboxes") : ctr("create.toolbox.depositBox", "Return items to Toolbox");
    }

    void confirm(int index) {
        auto boxes    = mData.value("boxes", Json::array());
        auto equipped = mData.value("equipped", Json());
        if (index == -2) {
            if (!equipped.is_null()) net::client::send("equipment.toolbox.unequip", {{"keep", !equipped.value("inRange", true)}});
            else if (mSelectBox || mBox < 0) net::client::send("equipment.toolbox.deposit", Json::object());
            else net::client::send("equipment.toolbox.deposit", {{"pos", boxes[static_cast<std::size_t>(mBox)].value("pos", Json::array())}});
            close();
            return;
        }
        if (index < 0) {
            close();
            return;
        }
        if (mSelectBox) {
            if (index < static_cast<int>(boxes.size())) {
                mBox       = index;
                mSelectBox = false;
            }
            return;
        }
        if (mBox >= 0 && mBox < static_cast<int>(boxes.size())) {
            auto comps = boxes[static_cast<std::size_t>(mBox)].value("compartments", Json::array());
            if (index < static_cast<int>(comps.size()) && !comps[static_cast<std::size_t>(index)].value("item", Json()).is_null())
                net::client::send("equipment.toolbox.select", {{"pos", boxes[static_cast<std::size_t>(mBox)].value("pos", Json::array())}, {"compartment", index}});
        }
        close();
    }

    Json mData;
    bool mSelectBox = false;
    int  mBox       = -1;
    int  mHover     = -1;
};

RadialScreen* gRadial = nullptr;

// ---- wand of symmetry -----------------------------------------------------------------------------------------

class SymmetryScreen : public Screen {
public:
    SymmetryScreen(Json const& data, SendFn send) : Screen(data.value("title", std::string()), 200, 80), mSend(std::move(send)), mData(data) {}

    void init() override {
        std::vector<std::string> types;
        for (auto const& t : mData.value("types", Json::array())) types.push_back(t.get<std::string>());
        mType = mData.value("type", 0);
        mOrientation = mData.value("orientation", 0);
        auto& t = add<ScrollInput>(Rect{10, 20, 180, 16}, mData.value("typeLabel", std::string("Mirror")), 0, 2, mType, [this](int v) {
            mType        = v;
            mOrientation = 0;
            refreshOrientation();
        });
        t.options = types;
        mOrient   = &add<ScrollInput>(Rect{10, 40, 180, 16}, mData.value("orientationLabel", std::string("Orientation")), 0, 1, mOrientation,
                                    [this](int v) { mOrientation = v; });
        refreshOrientation();
        add<IconButton>(Rect{172, 58, 18, 18}, "v", [this] {
            mSend({{"type", mType}, {"orientation", mOrientation}});
            close();
        });
    }

private:
    void refreshOrientation() {
        auto labels = mData.value("orientations", Json::array());
        mOrient->options.clear();
        if (mType < static_cast<int>(labels.size()))
            for (auto const& l : labels[static_cast<std::size_t>(mType)]) mOrient->options.push_back(l.get<std::string>());
        mOrient->max   = std::max(0, static_cast<int>(mOrient->options.size()) - 1);
        mOrient->value = std::clamp(mOrientation, 0, mOrient->max);
        mOrientation   = mOrient->value;
    }

    SendFn       mSend;
    Json         mData;
    ScrollInput* mOrient = nullptr;
    int          mType = 0, mOrientation = 0;
};

// ---- worldshaper ------------------------------------------------------------------------------------------------

class WorldshaperScreen : public Screen {
public:
    WorldshaperScreen(Json const& data, SendFn send) : Screen(data.value("title", std::string()), 220, 150), mSend(std::move(send)), mData(data) {
        mSettings = ce::ShaperSettings::fromJson(data.value("settings", Json::object()));
    }

    void init() override { build(); }
    void tick() override {
        if (!mRebuild) return;
        mRebuild = false;
        build();
    }

private:
    std::vector<std::string> strings(char const* key) {
        std::vector<std::string> out;
        for (auto const& s : mData.value(key, Json::array())) out.push_back(s.is_string() ? s.get<std::string>() : s.value("name", std::string()));
        return out;
    }

    void build() {
        widgets.clear();
        auto labels  = mData.value("labels", Json::object());
        auto brushes = mData.value("brushes", Json::array());
        auto& b = add<ScrollInput>(Rect{10, 18, 200, 14}, labels.value("brush", std::string("Brush")), 0, 4, static_cast<int>(mSettings.brush), [this](int v) {
            mSettings.brush = ce::Brush(v);
            ce::sanitize(mSettings);
            mRebuild = true; // not from inside the widget's own callback
        });
        b.options = strings("brushes");
        auto brush = brushes.size() > static_cast<std::size_t>(mSettings.brush) ? brushes[static_cast<std::size_t>(mSettings.brush)] : Json::object();
        auto params = brush.value("params", Json::array());
        float y = 36;
        for (std::size_t i = 0; i < params.size() && i < 3; ++i) {
            auto const& p = params[i];
            auto& in = add<ScrollInput>(Rect{10, y, 200, 14}, p.value("label", std::string()), p.value("min", 0), p.value("max", 1), mSettings.params[i],
                                        [this, i](int v) { mSettings.params[i] = v; });
            if (p.value("toggle", false)) in.options = {"On", "Off"}; // Create: param == 0 means on
            y += 16;
        }
        std::vector<std::string> toolNames = strings("tools"), toolOptions;
        std::vector<int>         toolIds;
        for (auto const& t : brush.value("tools", Json::array())) {
            toolIds.push_back(t.get<int>());
            toolOptions.push_back(toolNames[static_cast<std::size_t>(t.get<int>())]);
        }
        int cur = 0;
        for (std::size_t i = 0; i < toolIds.size(); ++i)
            if (toolIds[i] == static_cast<int>(mSettings.tool)) cur = static_cast<int>(i);
        auto& t = add<ScrollInput>(Rect{10, y, 200, 14}, labels.value("tool", std::string("Tool")), 0, std::max(0, static_cast<int>(toolIds.size()) - 1), cur,
                                   [this, toolIds](int v) { mSettings.tool = ce::ShaperTool(toolIds[static_cast<std::size_t>(v)]); });
        t.options = toolOptions;
        y += 16;
        if (brush.value("placement", false)) {
            auto& p = add<ScrollInput>(Rect{10, y, 200, 14}, labels.value("placement", std::string("Placement")), 0, 2, static_cast<int>(mSettings.placement),
                                       [this](int v) { mSettings.placement = ce::Placement(v); });
            p.options = strings("placements");
            y += 16;
        }
        auto& pat = add<ScrollInput>(Rect{10, y, 200, 14}, labels.value("pattern", std::string("Pattern")), 0, 5, static_cast<int>(mSettings.pattern),
                                     [this](int v) { mSettings.pattern = ce::Pattern(v); });
        pat.options = strings("patterns");
        auto& ok    = add<IconButton>(Rect{192, 128, 18, 18}, "v", [this] {
            mSend({{"settings", mSettings.toJson()}});
            close();
        });
        ok.tooltip = {labels.value("confirm", std::string("Confirm"))};
    }

    SendFn             mSend;
    Json               mData;
    ce::ShaperSettings mSettings;
    bool               mRebuild = false;
};

// ---- linked controller ----------------------------------------------------------------------------------------

class ControllerScreen : public Screen {
public:
    ControllerScreen(Json const& data, SendFn send) : Screen(data.value("title", std::string()), 176, 140), mSend(std::move(send)), mData(data) {
        showInventory = true;
    }

    void init() override {
        for (int b = 0; b < 6; ++b)
            for (int k = 0; k < 2; ++k) {
                Rect r{100.0f + 20.0f * k, 18.0f + 20.0f * b, 18, 18};
                mSlots.push_back(&add<Slot>(r, [this, b, k](Button btn, bool) {
                    if (btn == Button::Left && cursor && !cursor->isNull()) mSend({{"button", b}, {"slot", k}, {"item", items::toJson(*cursor)}});
                    else mSend({{"button", b}, {"slot", k}, {"item", nullptr}});
                }));
            }
        onData(mData);
    }

    void onData(Json const& data) override {
        mData     = data;
        auto rows = data.value("rows", Json::array());
        for (int b = 0; b < 6; ++b) {
            auto row = b < static_cast<int>(rows.size()) ? rows[static_cast<std::size_t>(b)] : Json::object();
            auto its = row.value("items", Json::array());
            for (int k = 0; k < 2; ++k) {
                auto& s = *mSlots[static_cast<std::size_t>(b * 2 + k)];
                s.stack = k < static_cast<int>(its.size()) ? stackOf(its[static_cast<std::size_t>(k)]) : nullptr;
                s.tooltip = {ctr(k == 0 ? "create.linked_controller.frequency_slot_1" : "create.linked_controller.frequency_slot_2",
                                 k == 0 ? "Keybind: {0}, Freq. #1" : "Keybind: {0}, Freq. #2", row.value("key", std::string()))};
            }
        }
    }

    void renderExtra(Canvas& c, float, float) override {
        auto rows = mData.value("rows", Json::array());
        for (int b = 0; b < 6 && b < static_cast<int>(rows.size()); ++b)
            c.text(92, 23.0f + 20.0f * b, rows[static_cast<std::size_t>(b)].value("key", std::string()), color::kTextDark, Align::Right, false);
    }

    void clickInventory(int, ItemStack const& stack, Button b, bool) override {
        if (items::isEmpty(stack)) cursor.reset();
        else if (b == Button::Left) cursor = std::make_shared<ItemStack>(items::withCount(stack, 1));
    }

private:
    SendFn             mSend;
    Json               mData;
    std::vector<Slot*> mSlots;
};

// ---- controller key capture ------------------------------------------------------------------------------------

struct ControllerState {
    bool                active = false;
    std::array<bool, 6> pressed{};
    int                 keepAlive = 0;
    bool                dirty     = false;
};
ControllerState& controller() {
    static ControllerState s;
    return s;
}
int buttonOf(int key) {
    switch (key) {
    case 87: return 0; // W
    case 83: return 1; // S
    case 65: return 2; // A
    case 68: return 3; // D
    case 32: return 4; // Space
    case 16: return 5; // Shift
    default: return -1;
    }
}
void sendInput() {
    Json p = Json::array();
    for (bool b : controller().pressed) p.push_back(b);
    net::client::send("equipment.controller.input", {{"pressed", p}});
}

// ---- HUD ----------------------------------------------------------------------------------------------------------

struct Hud {
    bool                        air = false;
    int                         airValue = 0, airMax = 900;
    std::vector<std::pair<int, bool>> linked; ///< hotbar slot, in range
};
Hud& hud() {
    static Hud h;
    return h;
}

void drawHud(MinecraftUIRenderContext& ctx) {
    auto [sw, sh] = guiSize();
    Canvas c(ctx);
    auto&  h = hud();
    if (h.air) {
        auto  t   = ce::backtankTimeLeft(h.airValue);
        bool  low = h.airValue < 60;
        float x = sw / 2 + 96, y = sh - 20;
        c.bevel({x, y, 46, 14}, 0xC0303030, 0xFF606060, 0xFF101010, false);
        c.fill({x + 3, y + 3, 6, 8}, low ? 0xFFE04040 : 0xFFC8754E);
        c.text(x + 12, y + 3, t, low ? 0xFFFF6060 : color::kText);
    }
    for (auto [slot, inRange] : h.linked) {
        float x = sw / 2 - 91 + 3 + 20.0f * static_cast<float>(slot);
        c.fill({x + 6, sh - 26, 4, 2}, inRange ? 0xFF7FD45E : 0xFF808080);
    }
    if (controller().active) c.text(sw / 2, sh - 60, ctr("lv.equipment.controller.active", "Linked controller active (Esc or right-click to stop)"), color::kTextGold, Align::Centre);
}

} // namespace

class EquipmentClientModule {
public:
    // Client input / render listeners are added at mod load (see screen/Screen.cpp).
    void install() {}
    void load() {
        registerServerScreen("equipment.toolbox", [](Json const& data, SendFn send) { return std::make_unique<ToolboxScreen>(data, std::move(send)); });
        registerServerScreen("equipment.symmetry", [](Json const& data, SendFn send) { return std::make_unique<SymmetryScreen>(data, std::move(send)); });
        registerServerScreen("equipment.worldshaper", [](Json const& data, SendFn send) { return std::make_unique<WorldshaperScreen>(data, std::move(send)); });
        registerServerScreen("equipment.controller", [](Json const& data, SendFn send) { return std::make_unique<ControllerScreen>(data, std::move(send)); });

        net::client::on("equipment.toolbox.radial", [](Json const& msg) {
            radial().requested = false;
            if (!radial().keyDown || current()) return;
            auto boxes = msg.value("boxes", Json::array());
            if (boxes.empty() && msg.value("equipped", Json()).is_null()) return;
            auto s  = std::make_unique<RadialScreen>(msg);
            gRadial = s.get();
            open(std::move(s));
            log().info("equipment: toolbox radial menu opened ({} toolboxes)", boxes.size());
        });
        net::client::on("equipment.toolbox.linked", [](Json const& msg) {
            auto& h = hud();
            h.linked.clear();
            for (auto const& e : msg.value("linked", Json::array()))
                if (e.is_array() && e.size() == 2) h.linked.emplace_back(e[0].get<int>(), e[1].get<bool>());
        });
        net::client::on("equipment.air", [](Json const& msg) {
            auto& h    = hud();
            h.air      = msg.value("show", false);
            h.airValue = msg.value("air", 0);
            h.airMax   = msg.value("max", 900);
        });
        net::client::on("equipment.controller.active", [](Json const& msg) {
            auto& c  = controller();
            c.active = msg.value("active", false);
            c.pressed.fill(false);
            log().info("equipment: linked controller {}", c.active ? "active" : "inactive");
        });

        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>(
            [](ll::event::KeyInputEvent& ev) {
                int key = ev.keyCode();
                // the toolbox key: hold to open the radial menu, release to confirm
                if (key == kKeyAlt) {
                    auto& r = radial();
                    if (ev.isDown() && !r.keyDown) {
                        r.keyDown = true;
                        if (!current() && inLevel() && net::client::connected() && !r.requested) {
                            r.requested = true;
                            net::client::send("equipment.toolbox.radial", Json::object());
                        }
                    } else if (!ev.isDown() && r.keyDown) {
                        r.keyDown = false;
                        if (gRadial && current() == gRadial) gRadial->release();
                        gRadial = nullptr;
                    }
                    return;
                }
                // the linked controller takes the movement keys
                auto& c = controller();
                if (!c.active || current()) return;
                if (key == 27 && ev.isDown()) { // Esc stops using it
                    c.active = false;
                    net::client::send("equipment.controller.stop", Json::object());
                    return;
                }
                int b = buttonOf(key);
                if (b < 0) return;
                if (c.pressed[static_cast<std::size_t>(b)] != ev.isDown()) {
                    c.pressed[static_cast<std::size_t>(b)] = ev.isDown();
                    sendInput();
                }
                ev.cancel();
            },
            ll::event::EventPriority::Highest
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) {
            auto& c = controller();
            if (c.active && ++c.keepAlive % 5 == 0) sendInput();
            if (gRadial && current() != gRadial) gRadial = nullptr;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            hud()        = Hud{};
            controller() = ControllerState{};
            radial()     = RadialState{};
            gRadial      = nullptr;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            if (current() || ev.screenView().getScreenName() != "hud_screen") return;
            drawHud(ev.uiRenderContext());
        }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::equipment

LV_REGISTER_MODULE(lv::client::equipment::EquipmentClientModule);

#endif // LV_CLIENT
