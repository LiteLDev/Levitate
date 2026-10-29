// Client side of the redstone unit (U23, client mod only): the threshold switch screen (Create's
// ThresholdSwitchScreen) and the hover panel showing a redstone link's frequency items and mode
// (and, with goggles, an analog lever's strength) next to the crosshair.

#ifdef LV_CLIENT

#include <algorithm>
#include <array>
#include <chrono>
#include <tuple>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/GlobalPos.h"
#include "core/redstone/Diodes.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::redstone {

namespace {
namespace net   = lv::feature::net;
namespace items = lv::feature::common::items;
namespace cr    = lv::core::redstone;
using namespace lv::client::screen;

constexpr char kLinkId[]  = "levitate:redstone_link";
constexpr char kLeverId[] = "levitate:analog_lever";

// ---- threshold switch screen -------------------------------------------------------------------

class ThresholdScreen : public Screen {
public:
    ThresholdScreen(Json const& data, SendFn send)
    : Screen(ctr("create.gui.threshold_switch.title", "Threshold Switch"), 200, 116),
      mSend(std::move(send)),
      mData(data) {}

    void init() override {
        add<Label>(Rect{8, 24, 30, 9}, ctr("create.gui.threshold_switch.power_on_when", "Power on when..."), color::kTextDark);
        add<Label>(Rect{8, 48, 30, 9}, ctr("create.gui.threshold_switch.power_off_when", "Power off when..."), color::kTextDark);
        mUpper = &add<ScrollInput>(Rect{8, 34, 120, 12}, "", 0, 1, 0, [this](int) { changed(true); });
        mLower = &add<ScrollInput>(Rect{8, 58, 120, 12}, "", 0, 1, 0, [this](int) { changed(false); });
        mUpper->tooltip = {ctr("create.gui.threshold_switch.upper_threshold", "...at or above:")};
        mLower->tooltip = {ctr("create.gui.threshold_switch.lower_threshold", "...at or below:")};
        mMeasure = &add<ScrollInput>(Rect{132, 34, 60, 12}, "", 0, 1, 0, [this](int) {
            // the stored thresholds are raw amounts: keep them, only the unit shown changes
            mStacks = mMeasure->value == 1;
            int s   = step();
            if (mMax > mMin) std::tie(mOff, mOn) = cr::clampThresholds(mOff / s * s, mOn / s * s, mMin, mMax, s, true);
            refreshInputs();
            mDirty    = true;
            mLastEdit = std::chrono::steady_clock::now();
        });
        mMeasure->options = {mData.value("unitItems", std::string("Items")), mData.value("unitStacks", std::string("Stacks"))};
        mMeasure->tooltip = {ctr("create.schedule.condition.threshold.item_measure", "Item Measure")};
        mInvert = &add<IconButton>(Rect{148, 90, 18, 18}, "~", [this] {
            mInverted = !mInverted;
            send();
        });
        mInvert->tooltip = {ctr("create.gui.threshold_switch.invert_signal", "Invert Signal")};
        auto& ok = add<IconButton>(Rect{172, 90, 18, 18}, "v", [] { close(); });
        ok.tooltip = {"OK"};
        mTarget = &add<Slot>(Rect{8, 90, 18, 18}, [](Button, bool) {});
        mState  = &add<Label>(Rect{32, 95, 100, 9}, "", color::kTextDark);
        mStacks   = mData.value("inStacks", false);
        mInverted = mData.value("inverted", false);
        apply(mData, true);
    }

    void onData(Json const& data) override { apply(data, false); }

    void tick() override {
        if (mDirty && std::chrono::steady_clock::now() - mLastEdit > std::chrono::seconds(1)) send();
    }

    void onClose() override {
        if (mDirty) send();
    }

private:
    int step() const { return cr::thresholdStep(mType == "fluid", mType == "item" && mStacks); }

    std::string unit() const {
        if (mType == "item") return mStacks ? mData.value("unitStacks", std::string("Stacks")) : mData.value("unitItems", std::string("Items"));
        return mData.value("unitBuckets", std::string("Buckets"));
    }

    std::string label(char const* key, int raw) const {
        if (mType == "custom") {
            auto l = mData.value("labels", Json::object());
            if (l.contains(key)) return l[key].get<std::string>();
        }
        return std::to_string(raw / step()) + " " + unit();
    }

    void apply(Json const& data, bool first) {
        mData   = data;
        mType   = data.value("type", std::string("none"));
        mMin    = std::max(0, data.value("min", 0));
        mMax    = data.value("max", 0);
        if (first || !mDirty) {
            mOn  = data.value("onAbove", 128);
            mOff = data.value("offBelow", 64);
            mInverted = data.value("inverted", false);
            mStacks   = data.value("inStacks", false);
        }
        bool supported      = mType != "none";
        mUpper->visible     = supported;
        mLower->visible     = supported;
        mMeasure->visible   = mType == "item";
        mMeasure->value     = mStacks ? 1 : 0;
        refreshInputs();
        mInvert->toggled = mInverted;
        auto target      = items::make(data.value("target", std::string{}));
        mTarget->stack   = target ? std::make_shared<ItemStack>(*target) : nullptr;
        std::vector<std::string> tip;
        if (!supported) {
            tip.push_back(ctr("create.gui.threshold_switch.not_attached", "Not attached to a block"));
        } else {
            tip.push_back(ctr("create.gui.threshold_switch.currently", "Currently {0}", label("current", data.value("current", 0))));
            if (mMin / step() == 0) tip.push_back(ctr("create.gui.threshold_switch.range_max", "(max. {0})", label("max", mMax)));
            else tip.push_back(ctr("create.gui.threshold_switch.range", "({0} to {1})", mMin / step(), label("max", mMax)));
        }
        mTarget->tooltip = tip;
        bool powered     = data.value("powered", false);
        mState->text     = powered ? "ON" : "OFF";
        mState->argb     = powered ? 0xFFC0281C : color::kTextGray;
    }

    void refreshInputs() {
        int s = step();
        // Create's ScrollInputs: the upper one in [min + step, max], the lower one in [min, max - step]
        mUpper->min   = (mMin + s) / s;
        mUpper->max   = std::max(mUpper->min, mMax / s);
        mLower->min   = mMin / s;
        mLower->max   = std::max(mLower->min, (mMax - s) / s);
        mUpper->value = std::clamp(mOn / s, mUpper->min, mUpper->max);
        mLower->value = std::clamp(mOff / s, mLower->min, mLower->max);
        mUpper->format = [this](int v) { return "≥ " + (mType == "custom" ? std::to_string(v) : std::to_string(v) + " " + unit()); };
        mLower->format = [this](int v) { return "≤ " + (mType == "custom" ? std::to_string(v) : std::to_string(v) + " " + unit()); };
    }

    void changed(bool upper) {
        int s = step();
        mOn   = mUpper->value * s;
        mOff  = mLower->value * s;
        if (mMax > mMin) std::tie(mOff, mOn) = cr::clampThresholds(mOff, mOn, mMin, mMax, s, upper);
        refreshInputs();
        mDirty    = true;
        mLastEdit = std::chrono::steady_clock::now();
    }

    void send() {
        mDirty = false;
        mSend({{"offBelow", mOff}, {"onAbove", mOn}, {"inverted", mInverted}, {"inStacks", mStacks}});
    }

    SendFn       mSend;
    Json         mData;
    std::string  mType = "none";
    int          mMin = 0, mMax = 0, mOn = 128, mOff = 64;
    bool         mStacks = false, mInverted = false, mDirty = false;
    ScrollInput *mUpper{}, *mLower{}, *mMeasure{};
    IconButton*  mInvert{};
    Slot*        mTarget{};
    Label*       mState{};
    std::chrono::steady_clock::time_point mLastEdit{};
};

// ---- hover panel -------------------------------------------------------------------------------

struct Hover {
    bool                     has = false;
    core::GlobalPos          pos;
    std::array<int, 4>       key{};
    std::vector<std::string> lines;
    std::vector<std::shared_ptr<ItemStack>> items;
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
    auto const& block = player->getDimensionBlockSource().getBlock(hit.mBlock);
    auto const& id    = block.getTypeName();
    if (id != kLinkId && id != kLeverId) {
        h.has = false;
        return;
    }
    double lx = hit.mPos.x - hit.mBlock.x, ly = hit.mPos.y - hit.mBlock.y, lz = hit.mPos.z - hit.mBlock.z;
    core::GlobalPos   pos{static_cast<int>(player->getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    std::array<int, 4> key{static_cast<int>(lx * 8), static_cast<int>(ly * 8), static_cast<int>(lz * 8), static_cast<int>(hit.mFacing)};
    if (h.pos == pos && h.key == key && h.cooldown > 0) return;
    h.pos = pos, h.key = key, h.cooldown = 10;
    net::client::send(
        "redstone.hover",
        {{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}, {"face", static_cast<int>(hit.mFacing)}, {"hit", Json::array({lx, ly, lz})}}
    );
}

} // namespace

class RedstoneClientModule {
public:
    // Client listeners are added at mod load (hooking them while the game runs can deadlock).
    void install() {
        registerServerScreen("redstone.threshold_switch", [](Json const& data, SendFn send) {
            return std::make_unique<ThresholdScreen>(data, std::move(send));
        });
    }
    void load() {
        net::client::on("redstone.hover", [](Json const& msg) {
            auto& h = hover();
            h.lines.clear();
            h.items.clear();
            for (auto const& l : msg.value("lines", Json::array())) h.lines.push_back(l.get<std::string>());
            for (auto const& i : msg.value("items", Json::array())) {
                auto s = items::fromJson(i);
                h.items.push_back(items::isEmpty(s) ? nullptr : std::make_shared<ItemStack>(s));
            }
            bool was = h.has;
            h.has    = !h.lines.empty();
            if (h.has && !was) lv::log().info("redstone client: hover panel shown ({} lines)", h.lines.size());
        });
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { pollHover(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { hover() = Hover{}; }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            auto& h = hover();
            if (!h.has || current() || ev.screenView().getScreenName() != "hud_screen") return;
            auto [sw, sh] = guiSize();
            Canvas c(ev.uiRenderContext());
            float  x = sw / 2 + 4, y = sh / 2 + 14;
            c.tooltip(x, y, h.lines);
            // the frequency items, as Create shows them on the block
            float w = 0;
            for (auto const& l : h.lines) w = std::max(w, c.textWidth(l));
            float ix = std::min(x + 12 + w + 12, sw - 20), iy = y - 12;
            for (auto const& s : h.items) {
                if (s) c.item(*s, ix, iy);
                iy += 18;
            }
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

} // namespace lv::client::redstone

LV_REGISTER_MODULE(lv::client::redstone::RedstoneClientModule);

#endif // LV_CLIENT
