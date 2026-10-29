// Train controls of the client mod (Create's ControlsHandler + TrainHUD, SPEC §3.6.27): while the
// server says we control a train ("trains.controls"), the movement keys go to the train instead of
// the player (W / S throttle and brake, A / D steer, Space approach, Shift stop, Esc stop), the
// mouse wheel sets the throttle and the sprint key sounds the horn. They are sent on
// "trains.interact" / "trains.throttle"; the HUD bar shows speed and throttle. A right-click with an
// empty hand in the air sends "trains.interact" (vanilla clients send nothing for it).

#ifdef LV_CLIENT

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::trains {

namespace {

namespace net = lv::feature::net;
using namespace lv::client::screen;

constexpr int kEsc = 27, kCtrl = 17;

/// Create's ControlsUtil.getControls order: forward, back, left, right, jump, sneak.
int controlOf(int key) {
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

/// Written by the window thread (input events), read on the client thread.
struct Input {
    std::atomic<bool>                active{false}; ///< controlling a train (client thread sets it)
    std::atomic<bool>                screenOpen{false};
    std::array<std::atomic<bool>, 6> pressed{};
    std::atomic<bool>                sprint{false};
    std::atomic<int>                 scroll{0};
    std::atomic<bool>                esc{false};
    std::atomic<bool>                rightClick{false};
};
Input& input() {
    static Input in;
    return in;
}

/// Client thread only.
struct Controls {
    std::array<bool, 6> sent{};
    int                 keepAlive = 0;
    bool                honking   = false;
    int                 honkCooldown = 0;
    double              throttle  = 1; ///< Create's editedThrottle
    bool                throttleDirty = false;
    int                 throttleCooldown = 0;
    Json                hud;
    std::string         name;
};
Controls& controls() {
    static Controls c;
    return c;
}

void release() {
    auto& in = input();
    in.active = false;
    for (auto& p : in.pressed) p = false;
    in.sprint = false;
    in.scroll = 0;
    in.esc    = false;
    auto& c   = controls();
    c         = Controls{};
}

void sendPressed(std::array<bool, 6> const& keys) {
    Json p = Json::array();
    for (bool b : keys) p.push_back(b);
    net::client::send("trains.interact", {{"pressed", p}});
}

/// Create's ControlsHandler.tick / TrainHUD.tick.
void tick() {
    auto& in = input();
    auto& c  = controls();
    if (in.rightClick.exchange(false)) {
        auto* player = currentPlayer();
        if (player && player->getSelectedItem().isNull()) {
            net::client::send("trains.interact", Json::object());
            log().info("trains client: empty-hand right-click sent");
        }
    }
    if (!in.active) return;
    if (in.esc.exchange(false)) {
        net::client::send("trains.interact", {{"stop", true}});
        log().info("trains client: stopped controlling (Esc)");
        release();
        return;
    }
    std::array<bool, 6> keys{};
    bool                any = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i] = !in.screenOpen && in.pressed[i];
        any     = any || keys[i];
    }
    // key packets on every change and every 5 ticks while held (the server forgets keys after 30)
    ++c.keepAlive;
    if (keys != c.sent || (any && c.keepAlive % 5 == 0)) {
        c.sent = keys;
        sendPressed(keys);
    }
    // the mouse wheel edits the throttle in 18 steps; sent at most every 5 ticks
    if (int delta = in.scroll.exchange(0); delta != 0) {
        c.throttle      = std::clamp(c.throttle + delta / 18.0, 1 / 18.0, 1.0);
        c.throttleDirty = true;
    }
    if (c.throttleCooldown > 0) --c.throttleCooldown;
    if (c.throttleDirty && c.throttleCooldown == 0) {
        c.throttleDirty    = false;
        c.throttleCooldown = 5;
        net::client::send("trains.throttle", {{"throttle", c.throttle}});
    }
    // the horn: the sprint key, repeated every 5 ticks while held
    bool sprint = in.sprint && !in.screenOpen;
    if (c.honkCooldown > 0) --c.honkCooldown;
    if (sprint && c.honkCooldown == 0) {
        net::client::send("trains.throttle", {{"honk", true}});
        c.honkCooldown = 5;
        c.honking      = true;
    } else if (!sprint && c.honking) {
        net::client::send("trains.throttle", {{"honk", false}});
        c.honkCooldown = 0;
        c.honking      = false;
    }
}

/// Create's TrainHUD: a bar above the hotbar, filled to the speed (of the top speed) with a marker
/// at the throttle.
void drawHud(MinecraftUIRenderContext& ctx) {
    auto const& c = controls();
    if (!input().active || c.hud.is_null()) return;
    auto [sw, sh] = guiSize();
    Canvas cv(ctx);
    double top      = std::max(1e-6, c.hud.value("top", 1.4));
    double speed    = std::abs(c.hud.value("speed", 0.0));
    double throttle = std::clamp(c.throttleDirty ? c.throttle : c.hud.value("throttle", c.throttle), 0.0, 1.0);
    float  w = 120, h = 8, x = sw / 2 - w / 2, y = sh - 39; // just above the hotbar, below the action bar
    cv.bevel({x - 2, y - 2, w + 4, h + 4}, 0xC0303030, 0xFF6E5F4C, 0xFF1A140F, true);
    cv.fill({x, y, static_cast<float>(w * std::clamp(speed / top, 0.0, 1.0)), h}, 0xFF7FB8E0);
    float mx = x + static_cast<float>(w * throttle);
    cv.fill({mx - 1, y - 3, 2, h + 6}, color::kTextGold);
    auto label = ctr("lv.trains.hud.throttle", "Throttle: {0}%", static_cast<int>(std::lround(throttle * 100)));
    if (c.hud.value("approach", false)) label = ctr("lv.trains.hud.approaching", "Approaching station...");
    // beside the bar: the action bar above it carries the game's prompts
    cv.text(x + w + 6, y - 1, label, color::kText, Align::Left);
}

} // namespace

class TrainControlsModule {
public:
    // Client input / render listeners are added at mod load (see screen/Screen.cpp).
    void install() {}
    void load() {
        net::client::on("trains.controls", [](Json const& msg) {
            bool active = msg.value("active", false);
            if (!active) {
                if (input().active) log().info("trains client: stopped controlling");
                release();
                return;
            }
            release();
            auto& c    = controls();
            c.throttle = std::clamp(msg.value("throttle", 1.0), 1 / 18.0, 1.0);
            c.name     = msg.value("name", std::string{});
            input().active = true;
            log().info("trains client: controlling '{}'", c.name);
        });
        net::client::on("trains.hud", [](Json const& msg) {
            if (input().active) controls().hud = msg;
        });

        auto& bus = ll::event::EventBus::getInstance();
        // window thread: flags only
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>(
            [](ll::event::KeyInputEvent& ev) {
                auto& in   = input();
                int   key  = ev.keyCode();
                bool  down = ev.isDown();
                // presses are taken only while controlling in the world (no screen, chat or menu);
                // a release is taken only for a press we took, so the game never keeps a key down
                bool take = in.active && !in.screenOpen;
                if (key == kEsc) {
                    if (down && take) in.esc = true;
                    return;
                }
                std::atomic<bool>* flag = nullptr;
                if (key == kCtrl) flag = &in.sprint; // Create: the sprint key is the horn while controlling
                else if (int i = controlOf(key); i >= 0) flag = &in.pressed[static_cast<std::size_t>(i)];
                if (!flag) return;
                if (down && take) {
                    *flag = true;
                    ev.cancel(); // Create sets the movement keys up: the player doesn't walk
                } else if (!down && flag->exchange(false)) {
                    ev.cancel();
                }
            },
            ll::event::EventPriority::High
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::MouseInputEvent>(
            [](ll::event::MouseInputEvent& ev) {
                auto& in   = input();
                char  id   = ev.actionButtonId();
                auto  data = ev.buttonData();
                if (in.screenOpen) return;
                if (id == 4 && data != 0 && in.active) {
                    in.scroll += data > 0 ? 1 : -1;
                    ev.cancel(); // TrainHUD.onScroll: the wheel doesn't change the hotbar slot
                    return;
                }
                if (id == 2 && data == 1) in.rightClick = true;
            },
            ll::event::EventPriority::High
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { release(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            // client thread, every frame: the controls run at game tick rate (50 ms)
            static auto last = std::chrono::steady_clock::now();
            auto        now  = std::chrono::steady_clock::now();
            auto&       in   = input();
            // any screen over the HUD (ours, chat, inventory, pause) keeps the keys for itself
            auto* client  = currentClient();
            in.screenOpen = current() != nullptr || (client && client->getTopScreenName() != "hud_screen");
            if (now - last >= std::chrono::milliseconds(50) && inLevel()) {
                last = now;
                if (in.screenOpen) {
                    // held keys stop counting while a screen is open
                    for (auto& p : in.pressed) p = false;
                    in.sprint = false;
                }
                if (net::client::connected()) tick();
            }
            if (!input().active || current() || ev.screenView().getScreenName() != "hud_screen") return;
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

} // namespace lv::client::trains

LV_REGISTER_MODULE(lv::client::trains::TrainControlsModule);

#endif // LV_CLIENT
