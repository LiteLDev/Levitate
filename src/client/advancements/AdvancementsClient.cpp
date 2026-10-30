// Client side of the advancements unit (U45): Create's advancement toast (top right, 5 s) and
// the advancement tree screen (`/create advancements`), drawn natively by the client mod.

#ifdef LV_CLIENT

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <set>
#include <sstream>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"

#include "mc/client/gui/screens/ScreenView.h"
#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/advancements/Advancements.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::advancements {

namespace {

namespace net   = lv::feature::net;
namespace items = lv::feature::common::items;
namespace ca    = lv::core::advancements;
using namespace lv::client::screen;
using Clock = std::chrono::steady_clock;

std::shared_ptr<ItemStack> iconOf(std::string_view id) {
    auto s = items::make(id, 1);
    if (!s || items::isEmpty(*s)) return nullptr;
    return std::make_shared<ItemStack>(*s);
}

std::string titleOf(std::string_view id) { return ctr("advancement.create." + std::string(id), id); }

std::vector<std::string> lines(std::string const& s) {
    std::vector<std::string> out;
    std::stringstream        ss(s);
    std::string              l;
    while (std::getline(ss, l)) out.push_back(l);
    return out;
}

// ---- toast -----------------------------------------------------------------------------------------------------------

struct Toast {
    int                        index = 0;
    Clock::time_point          start;
    std::shared_ptr<ItemStack> icon;
};
std::deque<Toast>& toasts() {
    static std::deque<Toast> t;
    return t;
}

constexpr float kToastW = 160, kToastH = 32;
constexpr int   kToastMs = 5000; ///< vanilla AdvancementToast display time

void drawToasts(MinecraftUIRenderContext& ctx) {
    auto& q = toasts();
    if (q.empty()) return;
    auto now = Clock::now();
    while (!q.empty() && now - q.front().start > std::chrono::milliseconds(kToastMs + 600)) q.pop_front();
    auto [sw, sh] = guiSize();
    Canvas c(ctx);
    float  y = 0;
    for (std::size_t i = 0; i < q.size() && i < 5; ++i) {
        auto&  t  = q[i];
        double ms = std::chrono::duration<double, std::milli>(now - t.start).count();
        // vanilla: slides in over 600 ms, stays, slides out
        double vis = ms < 600 ? ms / 600.0 : (ms > kToastMs ? std::max(0.0, 1.0 - (ms - kToastMs) / 600.0) : 1.0);
        float  x   = sw - kToastW * static_cast<float>(vis);
        auto const& d = ca::all()[static_cast<std::size_t>(t.index)];
        c.bevel({x, y, kToastW, kToastH}, 0xF0202020, 0xFF5A5A5A, 0xFF0A0A0A, false);
        c.frame({x + 1, y + 1, kToastW - 2, kToastH - 2}, ca::goal(d.type) ? 0xFFC6A23A : 0xFF8B8B8B);
        if (t.icon) c.item(*t.icon, x + 8, y + 8);
        bool        goal = ca::goal(d.type);
        std::string head = goal ? ctr("lv.advancements.toast.goal", "Goal Reached!") : ctr("lv.advancements.toast.task", "Advancement Made!");
        c.text(x + 30, y + 7, head, 0xFFFFFF00);
        c.text(x + 30, y + 18, titleOf(d.id), color::kText);
        y += kToastH;
    }
}

// ---- tree screen -------------------------------------------------------------------------------------------------------

constexpr float kCell = 28; ///< vanilla: 28 px per column, 27 per row

/// The scrollable tree (Create's tab of the vanilla advancement screen).
class TreeView : public Widget {
public:
    TreeView(Rect r, std::set<std::string> done) : Widget(r), mDone(std::move(done)) {
        mVisible = ca::visibility([this](std::string_view id) { return mDone.contains(std::string(id)); });
        auto const& l = ca::layout();
        for (std::size_t i = 0; i < l.size(); ++i) {
            if (!mVisible[i]) continue;
            mMaxX = std::max(mMaxX, l[i].x * kCell + 26);
            mMaxY = std::max(mMaxY, l[i].y * kCell + 26);
        }
        // start with the root on the left, centred vertically on it
        mScrollX = 0;
        mScrollY = std::clamp(l[0].y * kCell + 13 - rect.h / 2, 0.f, std::max(0.f, mMaxY + 8 - rect.h));
        mIcons.resize(l.size());
    }

    int hovered = -1;

    void render(Canvas& c, float mx, float my) override {
        c.bevel(rect, 0xFF3B3B3B, 0xFF151515, 0xFF6A6A6A, true);
        // a subtle grid like Create's background
        for (float gx = rect.x + 8 - std::fmod(mScrollX, 16.f); gx < rect.x + rect.w - 2; gx += 16) clipped(c, {gx, rect.y + 2, 1, rect.h - 4}, 0x18FFFFFF);
        for (float gy = rect.y + 8 - std::fmod(mScrollY, 16.f); gy < rect.y + rect.h - 2; gy += 16) clipped(c, {rect.x + 2, gy, rect.w - 4, 1}, 0x18FFFFFF);
        auto const& l   = ca::layout();
        auto        org = [&](std::size_t i) { return std::pair{rect.x + 6 + l[i].x * kCell - mScrollX, rect.y + 6 + l[i].y * kCell - mScrollY}; };
        // connections: vanilla draws a line from the parent's right edge to the child's left edge
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 1; i < l.size(); ++i) {
                int p = ca::parentOf(i);
                if (p < 0 || !mVisible[i] || !mVisible[static_cast<std::size_t>(p)]) continue;
                auto [cx, cy] = org(i);
                auto [px, py] = org(static_cast<std::size_t>(p));
                bool     done = mDone.contains(std::string(ca::all()[i].id));
                unsigned col  = pass == 0 ? 0xFF000000 : (done ? 0xFFFFFFFF : 0xFFA0A0A0);
                float    t    = pass == 0 ? 3.f : 1.f;
                float    o    = pass == 0 ? -1.f : 0.f;
                float    midX = px + 26 + (cx - px - 26) / 2;
                clipped(c, {px + 26, py + 13 + o, midX - px - 26, t}, col);
                clipped(c, {midX + o, std::min(py, cy) + 13 + o, t, std::abs(cy - py) + t}, col);
                clipped(c, {midX, cy + 13 + o, cx - midX, t}, col);
            }
        }
        hovered = -1;
        for (std::size_t i = 0; i < l.size(); ++i) {
            if (!mVisible[i]) continue;
            auto [x, y] = org(i);
            if (x < rect.x + 2 || y < rect.y + 2 || x + 26 > rect.x + rect.w - 2 || y + 26 > rect.y + rect.h - 2) continue;
            auto const& d    = ca::all()[i];
            bool        done = mDone.contains(std::string(d.id));
            unsigned    fill = done ? 0xFFD8A933 : 0xFF6E6E6E;
            unsigned    edge = done ? 0xFF7A5A12 : 0xFF2C2C2C;
            if (ca::goal(d.type)) {
                // Create's goal frame: rounded
                c.fill({x + 3, y, 20, 26}, edge);
                c.fill({x, y + 3, 26, 20}, edge);
                c.fill({x + 3, y + 1, 20, 24}, fill);
                c.fill({x + 1, y + 3, 24, 20}, fill);
            } else {
                c.fill({x, y, 26, 26}, edge);
                c.fill({x + 1, y + 1, 24, 24}, fill);
            }
            c.fill({x + 4, y + 4, 18, 18}, done ? 0xFF8F6A1C : 0xFF4A4A4A);
            if (!mIcons[i]) mIcons[i] = iconOf(d.icon);
            if (mIcons[i]) c.item(*mIcons[i], x + 5, y + 5);
            if (mx >= x && mx < x + 26 && my >= y && my < y + 26) {
                hovered = static_cast<int>(i);
                c.fill({x + 1, y + 1, 24, 24}, color::kHover);
            }
        }
    }

    bool scroll(float mx, float my, int delta, bool shift) override {
        if (mx < rect.x || my < rect.y || mx > rect.x + rect.w || my > rect.y + rect.h) return false;
        float step = static_cast<float>(-delta) * 20;
        if (shift) mScrollX = std::clamp(mScrollX + step, 0.f, std::max(0.f, mMaxX + 12 - rect.w));
        else mScrollY = std::clamp(mScrollY + step, 0.f, std::max(0.f, mMaxY + 12 - rect.h));
        return true;
    }

    [[nodiscard]] bool isDone(std::size_t i) const { return mDone.contains(std::string(ca::all()[i].id)); }
    [[nodiscard]] std::size_t doneCount() const { return mDone.size(); }

private:
    void clipped(Canvas& c, Rect r, unsigned col) const {
        float x0 = std::max(r.x, rect.x + 2), y0 = std::max(r.y, rect.y + 2);
        float x1 = std::min(r.x + r.w, rect.x + rect.w - 2), y1 = std::min(r.y + r.h, rect.y + rect.h - 2);
        if (x1 > x0 && y1 > y0) c.fill({x0, y0, x1 - x0, y1 - y0}, col);
    }

    std::set<std::string>                   mDone;
    std::vector<bool>                       mVisible;
    std::vector<std::shared_ptr<ItemStack>> mIcons;
    float mScrollX = 0, mScrollY = 0, mMaxX = 0, mMaxY = 0;
};

class TreeScreen : public Screen {
public:
    explicit TreeScreen(Json const& data) : Screen(ctr("advancement.create.root", "Welcome to Create"), 300, 196) { mDone = doneOf(data); }

    void init() override { mTree = &add<TreeView>(Rect{8, 18, width - 16, height - 38}, mDone); }

    void onData(Json const& data) override {
        mDone = doneOf(data);
        widgets.clear();
        init();
    }

    void renderExtra(Canvas& c, float mx, float my) override {
        if (!mTree) return;
        c.text(8, height - 15,
               std::to_string(mTree->doneCount()) + " / " + std::to_string(ca::all().size()) + "  " + ctr("lv.advancements.screen.hint", "Scroll to move, Shift-scroll to move sideways"),
               color::kTextDark, Align::Left, false);
        if (mTree->hovered < 0) return;
        auto const&              d = ca::all()[static_cast<std::size_t>(mTree->hovered)];
        std::vector<std::string> tip{(mTree->isDone(static_cast<std::size_t>(mTree->hovered)) ? "§e" : "§f") + titleOf(d.id)};
        for (auto const& l : lines(ctr("advancement.create." + std::string(d.id) + ".desc", ""))) tip.push_back(l.rfind("§", 0) == 0 ? l : "§6" + l);
        c.tooltip(mx, my, tip);
    }

private:
    static std::set<std::string> doneOf(Json const& data) {
        std::set<std::string> s;
        for (auto const& id : data.value("done", Json::array()))
            if (id.is_string()) s.insert(id.get<std::string>());
        return s;
    }

    std::set<std::string> mDone;
    TreeView*             mTree = nullptr;
};

} // namespace

class AdvancementsClientModule {
public:
    void install() {}
    void load() {
        registerServerScreen("advancements.tree", [](Json const& data, SendFn) {
            log().info("advancements: tree screen opened ({} done)", data.value("done", Json::array()).size());
            return std::make_unique<TreeScreen>(data);
        });
        net::client::on("advancements.toast", [](Json const& msg) {
            int i = ca::indexOf(msg.value("id", std::string()));
            if (i < 0) return;
            toasts().push_back({i, Clock::now(), iconOf(ca::all()[static_cast<std::size_t>(i)].icon)});
            log().info("advancements: toast {}", msg.value("id", std::string()));
        });
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { toasts().clear(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            if (ev.screenView().getScreenName() != "hud_screen") return;
            drawToasts(ev.uiRenderContext());
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

} // namespace lv::client::advancements

LV_REGISTER_MODULE(lv::client::advancements::AdvancementsClientModule);

#endif // LV_CLIENT
