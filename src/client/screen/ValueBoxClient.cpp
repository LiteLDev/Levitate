// Client side of value boxes and filter slots (SPEC §3.2): Create's value settings board
// (opened by the server on a right-click with the wrench / an empty hand) and the hover label
// shown while looking at a value box or a filter slot.

#ifdef LV_CLIENT

#include <algorithm>
#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/common/ValueSettings.h"
#include "feature/common/ValueBox.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::screen {

namespace {
namespace net = lv::feature::net;
using core::common::BoardSpec;
using core::common::ValueSettings;

/// Create's value settings board: one track per row, columns 0..max with milestone ticks.
class BoardScreen : public Screen {
public:
    BoardScreen(Json msg) : Screen(msg.value("title", std::string{}), 0, 0), mMsg(std::move(msg)) {
        mBoard.maxValue          = mMsg.value("max", 0);
        mBoard.milestoneInterval = std::max(1, mMsg.value("milestone", 1));
        for (auto const& r : mMsg.value("rows", Json::array())) mBoard.rows.push_back(r.get<std::string>());
        if (mBoard.rows.empty()) mBoard.rows.push_back("");
        mSel   = core::common::clampSettings(mBoard, {mMsg.value("row", 0), mMsg.value("value", 0)});
        width  = kTrackX + kTrackW + 12;
        height = 34 + kRowH * static_cast<float>(mBoard.rows.size()) + 14;
    }

    void renderExtra(Canvas& c, float mx, float my) override {
        updateFromPointer(mx, my);
        for (int r = 0; r < static_cast<int>(mBoard.rows.size()); ++r) {
            float y = 22 + kRowH * static_cast<float>(r);
            c.text(kTrackX - 4, y + 4, mBoard.rows[static_cast<std::size_t>(r)], color::kTextDark, Align::Right, false);
            Rect track{kTrackX, y + 2, kTrackW, 12};
            c.bevel(track, color::kSlot, color::kSlotDark, color::kSlotLight, true);
            if (mBoard.maxValue > 0) {
                for (int v = 0; v <= mBoard.maxValue; v += mBoard.milestoneInterval) {
                    float x = kTrackX + kTrackW * static_cast<float>(v) / static_cast<float>(mBoard.maxValue);
                    c.fill({x, y + 4, 1, 8}, color::kSlotLight);
                }
            }
            if (r == mSel.row) {
                float frac = mBoard.maxValue > 0 ? static_cast<float>(mSel.value) / static_cast<float>(mBoard.maxValue) : 0.0f;
                float x    = kTrackX + kTrackW * frac;
                c.fill({x - 2, y, 5, 16}, color::kTextGold);
            }
        }
        float ly = 26 + kRowH * static_cast<float>(mBoard.rows.size());
        c.text(width / 2, ly, label(mSel), color::kText, Align::Centre);
    }

    bool clickBackground(float, float, Button b, bool) override {
        if (b == Button::Left) confirm();
        else close();
        return true;
    }

    void tick() override {}

    bool scrollBoard(int delta, bool shift) {
        mSel.value      = core::common::scrollValue(mBoard, mSel.value, delta, shift);
        mPointerLocked  = true;
        return true;
    }

private:
    static constexpr float kTrackX = 64;
    static constexpr float kTrackW = 160;
    static constexpr float kRowH   = 18;

    void updateFromPointer(float mx, float my) {
        if (mx == mLastX && my == mLastY) return;
        bool first = mLastX < 0;
        mLastX = mx, mLastY = my;
        if (first || mPointerLocked) {
            // the board opens on the server's value: only a real pointer move picks another one
            mPointerLocked = false;
            if (first) return;
        }
        int row = static_cast<int>((my - 22) / kRowH);
        if (row < 0 || row >= static_cast<int>(mBoard.rows.size()) || mx < kTrackX - 6 || mx > kTrackX + kTrackW + 6) return;
        mSel.row   = row;
        mSel.value = core::common::valueAtFraction(mBoard, (mx - kTrackX) / kTrackW);
    }

    std::string label(ValueSettings s) const {
        auto labels = mMsg.find("labels");
        if (labels != mMsg.end() && labels->is_array() && s.row < static_cast<int>(labels->size())) {
            auto const& row = (*labels)[static_cast<std::size_t>(s.row)];
            if (row.is_array() && s.value < static_cast<int>(row.size())) return row[static_cast<std::size_t>(s.value)].get<std::string>();
        }
        return std::to_string(s.value);
    }

    void confirm() {
        net::client::send("valuebox.set", {{"pos", mMsg.value("pos", Json())}, {"box", mMsg.value("box", std::string{})}, {"row", mSel.row}, {"value", mSel.value}});
        close();
    }

    Json          mMsg;
    BoardSpec     mBoard;
    ValueSettings mSel;
    float         mLastX = -1, mLastY = -1;
    bool          mPointerLocked = false;
};

/// A scroll-only widget covering the board, forwarding the wheel to the board.
class BoardScroll : public Widget {
public:
    explicit BoardScroll(BoardScreen& s) : Widget(Rect{0, 0, 10000, 10000}), mScreen(s) {}
    void render(Canvas&, float, float) override {}
    bool scroll(float, float, int delta, bool shift) override { return mScreen.scrollBoard(delta, shift); }

private:
    BoardScreen& mScreen;
};

// ---- hover label -------------------------------------------------------------------------------

struct Hover {
    bool                     has = false;
    core::GlobalPos          pos;
    int                      face = -1;
    std::array<int, 3>       hitKey{};
    std::vector<std::string> lines;
    int                      cooldown = 0;
};
Hover& hover() {
    static Hover h;
    return h;
}

void pollHover() {
    auto& h = hover();
    if (h.cooldown > 0) --h.cooldown;
    auto* player = currentPlayer();
    if (!player || !net::client::connected() || current()) {
        h.has = false;
        return;
    }
    auto const& hit = player->getLevel().getHitResult();
    if (hit.mType != HitResultType::Tile) {
        h.has = false;
        return;
    }
    auto& region = player->getDimensionBlockSource();
    auto const& block = region.getBlock(hit.mBlock);
    auto id = block.getTypeName();
    if (!lv::feature::common::valuebox::boxesOf(id) && !lv::feature::common::filterslot::hasSlots(id)) {
        h.has = false;
        return;
    }
    double lx = hit.mPos.x - hit.mBlock.x, ly = hit.mPos.y - hit.mBlock.y, lz = hit.mPos.z - hit.mBlock.z;
    core::common::Vec local{lx, ly, lz};
    bool onBox = lv::feature::common::valuebox::boxAt(block, hit.mFacing, local) || lv::feature::common::filterslot::slotAt(block, hit.mFacing, local);
    if (!onBox) {
        h.has = false;
        h.face = -1;
        return;
    }
    core::GlobalPos pos{static_cast<int>(player->getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    std::array<int, 3> key{static_cast<int>(lx * 8), static_cast<int>(ly * 8), static_cast<int>(lz * 8)};
    bool same = h.pos == pos && h.face == hit.mFacing && h.hitKey == key;
    if (same && h.cooldown > 0) return; // at most one query per target every 10 ticks
    h.pos = pos, h.face = hit.mFacing, h.hitKey = key, h.cooldown = 10;
    net::client::send(
        "common.hover",
        {{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}, {"face", static_cast<int>(hit.mFacing)}, {"hit", Json::array({lx, ly, lz})}}
    );
}
} // namespace

class ValueBoxClientModule {
public:
    // Client input / render listeners are added at mod load, before the game's threads run:
    // hooking them while the client is running (mod enable) can deadlock the game.
    void install() {}
    void load() {
        net::client::on("valuebox.open", [](Json const& msg) {
            auto screen = std::make_unique<BoardScreen>(msg);
            screen->add<BoardScroll>(*screen);
            open(std::move(screen));
        });
        net::client::on("common.hover", [](Json const& msg) {
            auto& h = hover();
            h.lines.clear();
            for (auto const& l : msg.value("lines", Json::array())) h.lines.push_back(l.get<std::string>());
            h.has = !h.lines.empty();
            // one line per new target and title (live values don't log again; e2e scenarios wait
            // for it); colour codes are drawn, not shown
            static std::string lastTarget;
            auto target = msg.value("pos", Json()).dump() + "|" + std::to_string(h.face) + "|" + (h.has ? h.lines.front() : "");
            if (h.has && target != lastTarget) {
                std::string text;
                for (auto const& l : h.lines) text += (text.empty() ? "" : " | ") + l;
                log().info("valuebox client: hover label {}", text);
            }
            lastTarget = h.has ? target : std::string{};
        });
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { pollHover(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { hover() = Hover{}; }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            auto& h = hover();
            if (!h.has || current() || ev.screenView().getScreenName() != "hud_screen") return;
            auto [sw, sh] = guiSize();
            Canvas c(ev.uiRenderContext());
            c.tooltip(sw / 2 + 4, sh / 2 + 14, h.lines);
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

} // namespace lv::client::screen

LV_REGISTER_MODULE(lv::client::screen::ValueBoxClientModule);

#endif // LV_CLIENT
