#ifdef LV_CLIENT

// Ponder on the client mod: scene library, hold-[W]-over-an-item, `/create ponder` and the module.
//
// Hovered item: the vanilla container screens (inventory, creative inventory, chests, ...) report
// the slot under the pointer (`ContainerScreenController::_onContainerSlotHovered`); while an
// item with Ponder scenes is hovered, "Hold [W] to Ponder" is shown with Create's progress bar and
// holding the Ponder key (W, Create's default) opens the item's Ponder (Create's
// PonderTooltipHandler: ~11 ticks with its accelerating progress).

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/gui/screens/controllers/ContainerScreenController.h"
#include "mc/client/gui/screens/controllers/CraftingScreenController.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/gui/ViewRequest.h"
#include "mc/world/item/ItemStackBase.h"

#include "client/Client.h"
#include "client/ponder/Ponder.h"
#include "client/ponder/PonderConfig.h"
#include "client/screen/Screen.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("ponder", lv::client::ponder::PonderConfig);

namespace lv::client::ponder {

namespace {

constexpr int kVkW = 0x57; ///< Create's default Ponder key

struct State {
    std::mutex                          mutex;   ///< hovered item (set from UI callbacks)
    std::string                         hovered; ///< item id under the pointer in a container screen
    // client thread
    bool                                keyDown   = false;
    bool                                leftDown  = false;
    float                               progress  = 0;
    float                               tickAcc   = 0;
    std::chrono::steady_clock::time_point last{};
    std::deque<std::function<void()>>   deferred;
    std::vector<std::function<void()>>  history;
};
State& state() {
    static State s;
    return s;
}

void setHovered(std::string id) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    s.hovered = std::move(id);
}

std::string hovered() {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    return s.hovered;
}

/// Screen changes happen between frames: a widget's click handler must not destroy its screen.
void defer(std::function<void()> fn) { state().deferred.push_back(std::move(fn)); }
} // namespace
void deferUi(std::function<void()> fn) { defer(std::move(fn)); }
namespace {

std::unique_ptr<cp::Library> loadLibrary() {
    auto lib  = std::make_unique<cp::Library>();
    auto dir  = lv::Mod::getInstance().getSelf().getModDir() / "ponder";
    auto read = [](std::filesystem::path const& p) {
        std::ifstream in(p);
        return nlohmann::json::parse(in, nullptr, false);
    };
    for (auto const& e : lib->setIndex(read(dir / "index.json"))) log().warn("ponder: {}", e);
    std::error_code ec;
    int             bad = 0;
    for (auto const& entry : std::filesystem::directory_iterator(dir / "scenes", ec)) {
        if (entry.path().extension() != ".json") continue;
        auto r = cp::parseScene(read(entry.path()));
        if (!r.scene) {
            ++bad;
            log().warn("ponder: {}: {}", entry.path().filename().string(), r.errors.empty() ? "invalid" : r.errors.front());
            continue;
        }
        lib->add(std::move(*r.scene));
    }
    log().info("ponder: loaded {} scenes for {} entries ({} invalid)", lib->sceneCount(), lib->components().size(), bad);
    return lib;
}

/// "Hold [W] to Ponder" with Create's progress bar, while a ponderable item is hovered.
void drawHoldHint(MinecraftUIRenderContext& ctx, std::string const& id, float progress) {
    auto [sw, sh] = screen::guiSize();
    screen::Canvas c(ctx);
    auto           text = ctr("ponder.ui.hold_to_ponder", "Hold [{0}] to Ponder", "W");
    float          w    = c.textWidth(text) + 12;
    screen::Rect   box{(sw - w) / 2, 6, w, 20};
    c.fill(box, 0xF0100010);
    c.frame(box, 0xFF5000FF);
    c.text(box.x + 6, box.y + 4, text, 0xFFA0A0A0);
    c.fill({box.x + 3, box.y + 15, (box.w - 6) * progress, 2}, 0xFFFFFFFF);
    (void)id;
}

} // namespace

cp::Library const& library() {
    static std::unique_ptr<cp::Library> lib = loadLibrary();
    return *lib;
}

bool leftMouseDown() { return state().leftDown; }

void pushHistory(std::function<void()> reopen) {
    auto& h = state().history;
    h.push_back(std::move(reopen));
    if (h.size() > 32) h.erase(h.begin());
}
void clearHistory() { state().history.clear(); }
bool canThinkBack() { return !state().history.empty(); }
bool thinkBack() {
    auto& h = state().history;
    if (h.empty()) return false;
    auto fn = std::move(h.back());
    h.pop_back();
    defer([fn = std::move(fn)] {
        // the reopened screen must not push itself again
        fn();
    });
    log().info("ponder: think back");
    return true;
}

// ---- hooks: the slot under the pointer in container screens ------------------------------------

LL_TYPE_INSTANCE_HOOK(
    PonderSlotHoverHook,
    ll::memory::HookPriority::Normal,
    ContainerScreenController,
    &ContainerScreenController::$_onContainerSlotHovered,
    ui::ViewRequest,
    std::string const& collectionName,
    int                index
) {
    auto r = origin(collectionName, index);
    try {
        auto const& stack = this->_getVisualItemStack(collectionName, index);
        setHovered(stack.isNull() ? std::string{} : stack.getTypeName());
    } catch (...) {}
    return r;
}

LL_TYPE_INSTANCE_HOOK(
    PonderCraftingHoverHook,
    ll::memory::HookPriority::Normal,
    CraftingScreenController,
    &CraftingScreenController::$_onContainerSlotHovered,
    ui::ViewRequest,
    std::string const& collectionName,
    int                index
) {
    auto r = origin(collectionName, index);
    try {
        auto const& stack = this->_getVisualItemStack(collectionName, index);
        setHovered(stack.isNull() ? std::string{} : stack.getTypeName());
    } catch (...) {}
    return r;
}

LL_TYPE_INSTANCE_HOOK(
    PonderSlotUnhoverHook,
    ll::memory::HookPriority::Normal,
    ContainerScreenController,
    &ContainerScreenController::$_onContainerSlotUnhovered,
    ui::ViewRequest,
    std::string const& collectionName,
    int                index
) {
    setHovered({});
    return origin(collectionName, index);
}

LL_TYPE_INSTANCE_HOOK(PonderContainerLeaveHook, ll::memory::HookPriority::Normal, ContainerScreenController, &ContainerScreenController::$onLeave, void) {
    setHovered({});
    origin();
}

// ---- module ------------------------------------------------------------------------------------

class PonderModule {
public:
    // input / render listeners at mod load (see screen::ScreenModule), hooks at enable
    void load() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>(
            [](ll::event::KeyInputEvent& ev) {
                auto& s  = state();
                int   vk = ev.keyCode();
                if (vk == kVkW) s.keyDown = ev.isDown();
                if (ev.isDown() && screen::current() && ponderKey(vk)) ev.cancel();
            },
            ll::event::EventPriority::High
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::MouseInputEvent>(
            [](ll::event::MouseInputEvent& ev) {
                if (ev.actionButtonId() == 1) state().leftDown = ev.buttonData() != 0;
            },
            ll::event::EventPriority::High
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::BeforeUIRenderEvent>(
            [](ll::event::BeforeUIRenderEvent&) {
                auto& s = state();
                while (!s.deferred.empty()) {
                    auto fn = std::move(s.deferred.front());
                    s.deferred.pop_front();
                    fn();
                }
            },
            ll::event::EventPriority::High
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            auto& s = state();
            if (screen::current()) {
                s.progress = 0;
                return;
            }
            auto id = hovered();
            if (id.empty() || !library().has(id)) {
                s.progress = 0;
                return;
            }
            // draw once per frame: on the top scene only
            auto* client = currentClient();
            if (!client) return;
            auto stack = client->getClientSceneStack();
            if (ev.screenView().getScreenName() != stack->getScreenName()) return;
            auto now = std::chrono::steady_clock::now();
            float dt = s.last.time_since_epoch().count() == 0 ? 0.f : std::chrono::duration<float>(now - s.last).count();
            s.last   = now;
            s.tickAcc += std::min(dt, 0.25f) * 20.f * config().holdSpeed;
            while (s.tickAcc >= 1.f) {
                s.tickAcc -= 1.f;
                s.progress = cp::holdProgressStep(s.progress, s.keyDown);
            }
            drawHoldHint(ev.uiRenderContext(), id, s.progress);
            if (s.progress >= 1.f) {
                s.progress = 0;
                log().info("ponder: held W over {}", id);
                clearHistory();
                defer([id] { openComponent(id); });
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            setHovered({});
            state().history.clear();
            state().deferred.clear();
            resetRenderCache();
        }));

        namespace net = lv::feature::net;
        // `/create ponder [id]` on the server (feature/ponder/PonderCommand.cpp)
        net::client::on("ponder.open", [](nlohmann::json const& msg) {
            auto id = msg.value("id", std::string{});
            clearHistory();
            defer([id] {
                if (id.empty()) {
                    log().info("ponder: opening the index");
                    openIndex();
                } else if (library().tag(id)) {
                    openTag(id);
                } else if (!openComponent(cp::normaliseId(id))) {
                    log().warn("ponder: nothing to ponder about {}", id);
                }
            });
        });
    }
    void install() {
        PonderSlotHoverHook::hook();
        PonderCraftingHoverHook::hook();
        PonderSlotUnhoverHook::hook();
        PonderContainerLeaveHook::hook();
    }
    void uninstall() {
        PonderSlotHoverHook::unhook();
        PonderCraftingHoverHook::unhook();
        PonderSlotUnhoverHook::unhook();
        PonderContainerLeaveHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::ponder

LV_REGISTER_MODULE(lv::client::ponder::PonderModule);

#endif // LV_CLIENT
