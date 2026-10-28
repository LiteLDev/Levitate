// Native filter screens (SPEC §4): the list filter (18 ghost slots, allow/deny list, respect /
// ignore data, reset) and the attribute filter (reference item, attribute selector, add /
// add inverted, three modes, reset). Both edit the filter in the player's hand through
// "screen.action" messages; the server answers with fresh data.

#ifdef LV_CLIENT

#include <algorithm>

#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/common/FilterLogic.h"
#include "feature/common/Items.h"
#include "mod/Module.h"

namespace lv::client::screen {

namespace {
namespace items = lv::feature::common::items;
using core::common::kListFilterSlots;

std::shared_ptr<ItemStack> stackOf(Json const& j) {
    auto s = items::fromJson(j);
    if (items::isEmpty(s)) return nullptr;
    return std::make_shared<ItemStack>(s);
}

std::string nameOf(std::shared_ptr<ItemStack> const& s) { return s ? s->getName() : std::string{}; }

// ---- list filter -------------------------------------------------------------------------------

class ListFilterScreen : public Screen {
public:
    ListFilterScreen(Json const& data, SendFn send)
    : Screen(ctr("lv.common.filter.title", "List Filter"), 176, 92),
      mSend(std::move(send)),
      mData(data) {
        showInventory = true;
    }

    void init() override {
        for (int i = 0; i < kListFilterSlots; ++i) {
            auto r = core::common::gridCell(7, 18, 9, 18, i);
            mSlots.push_back(&add<Slot>(r, [this, i](Button b, bool) { clickSlot(i, b); }));
        }
        auto& allow = add<IconButton>(Rect{7, 62, 18, 18}, "A", [this] { mSend({{"op", "mode"}, {"blacklist", false}}); });
        allow.tooltip = {ctr("create.gui.filter.allow_list", "Allow-List"),
                         ctr("create.gui.filter.allow_list.description", "Items pass if they match any of the above. An empty Allow-List rejects everything.")};
        auto& deny = add<IconButton>(Rect{27, 62, 18, 18}, "D", [this] { mSend({{"op", "mode"}, {"blacklist", true}}); });
        deny.tooltip   = {ctr("create.gui.filter.deny_list", "Deny-List"),
                          ctr("create.gui.filter.deny_list.description", "Items pass if they do NOT match any of the above. An empty Deny-List accepts everything.")};
        deny.litColour = color::kButtonOff;
        auto& respect = add<IconButton>(Rect{53, 62, 18, 18}, "=", [this] { mSend({{"op", "respect_data"}, {"value", true}}); });
        respect.tooltip = {ctr("create.gui.filter.respect_data", "Respect Data"),
                           ctr("create.gui.filter.respect_data.description", "Items only match if their durability, enchantments, and other attributes match as well.")};
        auto& ignore = add<IconButton>(Rect{73, 62, 18, 18}, "~", [this] { mSend({{"op", "respect_data"}, {"value", false}}); });
        ignore.tooltip = {ctr("create.gui.filter.ignore_data", "Ignore Data"),
                          ctr("create.gui.filter.ignore_data.description", "Items match regardless of their attributes.")};
        auto& reset = add<IconButton>(Rect{151, 62, 18, 18}, "x", [this] { mSend({{"op", "reset"}}); });
        reset.tooltip = {ctr("lv.common.filter.reset", "Reset")};
        mAllow = &allow, mDeny = &deny, mRespect = &respect, mIgnore = &ignore;
        mMode  = &add<Label>(Rect{96, 67, 50, 9}, "", color::kTextDark);
        onData(mData);
    }

    void onData(Json const& data) override {
        mData   = data;
        auto it = data.find("items");
        for (int i = 0; i < kListFilterSlots; ++i) {
            auto& slot = *mSlots[static_cast<std::size_t>(i)];
            slot.stack = (it != data.end() && it->is_array() && i < static_cast<int>(it->size())) ? stackOf((*it)[static_cast<std::size_t>(i)]) : nullptr;
            slot.tooltip = slot.stack ? std::vector<std::string>{nameOf(slot.stack)} : std::vector<std::string>{};
        }
        bool black       = data.value("blacklist", false);
        bool respect     = data.value("respect_data", false);
        mAllow->toggled  = !black;
        mDeny->toggled   = black;
        mRespect->toggled = respect;
        mIgnore->toggled  = !respect;
        mMode->text = black ? ctr("create.gui.filter.deny_list", "Deny-List") : ctr("create.gui.filter.allow_list", "Allow-List");
    }

    void clickInventory(int, ItemStack const& stack, Button b, bool shift) override {
        if (items::isEmpty(stack)) {
            cursor.reset();
            return;
        }
        if (shift) {
            // straight into the first free ghost slot
            for (int i = 0; i < kListFilterSlots; ++i) {
                if (!mSlots[static_cast<std::size_t>(i)]->stack) {
                    mSend({{"op", "set"}, {"index", i}, {"item", items::toJson(items::withCount(stack, 1))}});
                    return;
                }
            }
            return;
        }
        if (b == Button::Left) cursor = std::make_shared<ItemStack>(items::withCount(stack, 1));
    }

private:
    void clickSlot(int i, Button b) {
        if (b == Button::Left && cursor && !cursor->isNull()) {
            mSend({{"op", "set"}, {"index", i}, {"item", items::toJson(*cursor)}});
        } else if (mSlots[static_cast<std::size_t>(i)]->stack) {
            mSend({{"op", "clear"}, {"index", i}});
        }
    }

    SendFn             mSend;
    Json               mData;
    std::vector<Slot*> mSlots;
    IconButton *       mAllow{}, *mDeny{}, *mRespect{}, *mIgnore{};
    Label*             mMode{};
};

// ---- attribute filter --------------------------------------------------------------------------

class AttributeFilterScreen : public Screen {
public:
    AttributeFilterScreen(Json const& data, SendFn send)
    : Screen(ctr("lv.common.attribute_filter.title", "Attribute Filter"), 214, 120),
      mSend(std::move(send)),
      mData(data) {
        showInventory = true;
    }

    void init() override {
        mRef = &add<Slot>(Rect{8, 20, 18, 18}, [this](Button b, bool) {
            if (b == Button::Left && cursor && !cursor->isNull()) mSend({{"op", "reference"}, {"item", items::toJson(*cursor)}});
            else mSend({{"op", "reference"}, {"item", nullptr}});
        });
        mRef->tooltip = {ctr("create.gui.attribute_filter.add_reference_item", "Add Reference Item")};
        mSelector     = &add<ScrollInput>(Rect{30, 20, 136, 18}, "", 0, 0, 0, [](int) {});
        mSelector->format = [this](int v) {
            if (mOptions.empty()) return ctr("create.gui.attribute_filter.no_selected_attributes", "No attributes selected");
            auto const& o = mOptions[static_cast<std::size_t>(std::clamp<int>(v, 0, static_cast<int>(mOptions.size()) - 1))];
            return o.value("text", std::string{});
        };
        auto& addBtn = add<IconButton>(Rect{170, 20, 18, 18}, "+", [this] { addSelected(false); });
        addBtn.tooltip = {ctr("create.gui.attribute_filter.add_attribute", "Add attribute to List")};
        auto& addInv = add<IconButton>(Rect{190, 20, 18, 18}, "-", [this] { addSelected(true); });
        addInv.tooltip = {ctr("create.gui.attribute_filter.add_inverted_attribute", "Add opposite attribute to List")};
        addInv.litColour = color::kButtonOff;

        static char const* modes[]   = {"whitelist_disj", "whitelist_conj", "blacklist"};
        static char const* symbols[] = {"|", "&", "!"};
        for (int i = 0; i < 3; ++i) {
            auto& b = add<IconButton>(Rect{8.0f + 20.0f * static_cast<float>(i), 94, 18, 18}, symbols[i], [this, i] {
                mSend({{"op", "mode"}, {"mode", modes[i]}});
            });
            mModes[i] = &b;
        }
        mModes[0]->tooltip = {ctr("create.gui.attribute_filter.allow_list_disjunctive", "Allow-List (Any)"),
                              ctr("create.gui.attribute_filter.allow_list_disjunctive.description", "Items pass if they have any of the selected attributes.")};
        mModes[1]->tooltip = {ctr("create.gui.attribute_filter.allow_list_conjunctive", "Allow-List (All)"),
                              ctr("create.gui.attribute_filter.allow_list_conjunctive.description", "Items pass only if they have ALL of the selected attributes.")};
        mModes[2]->tooltip = {ctr("create.gui.attribute_filter.deny_list", "Deny-List"),
                              ctr("create.gui.attribute_filter.deny_list.description", "Items pass if they do NOT have any of the selected attributes.")};
        mModes[2]->litColour = color::kButtonOff;
        auto& reset = add<IconButton>(Rect{188, 94, 18, 18}, "x", [this] { mSend({{"op", "reset"}}); });
        reset.tooltip = {ctr("lv.common.filter.reset", "Reset")};
        onData(mData);
    }

    void onData(Json const& data) override {
        mData       = data;
        mRef->stack = stackOf(data.value("reference", Json()));
        mOptions.clear();
        if (auto it = data.find("options"); it != data.end() && it->is_array())
            for (auto const& o : *it) mOptions.push_back(o);
        mSelector->max   = std::max(0, static_cast<int>(mOptions.size()) - 1);
        mSelector->value = std::clamp(mSelector->value, 0, mSelector->max);
        auto mode        = data.value("mode", std::string("whitelist_disj"));
        mModes[0]->toggled = mode == "whitelist_disj";
        mModes[1]->toggled = mode == "whitelist_conj";
        mModes[2]->toggled = mode == "blacklist";
        mLines.clear();
        if (auto it = data.find("attributes"); it != data.end() && it->is_array())
            for (auto const& a : *it) mLines.push_back(a.value("text", std::string{}));
    }

    void renderExtra(Canvas& c, float, float) override {
        Rect box{8, 42, width - 16, 48};
        c.bevel(box, color::kSlot, color::kSlotDark, color::kSlotLight, true);
        if (mLines.empty()) {
            c.text(box.x + 4, box.y + 4, ctr("create.gui.attribute_filter.no_selected_attributes", "No attributes selected"), color::kTextGray);
        }
        for (std::size_t i = 0; i < mLines.size() && i < 4; ++i)
            c.text(box.x + 4, box.y + 3 + 11.0f * static_cast<float>(i), "- " + mLines[i], color::kText);
        if (mLines.size() > 4) c.text(box.x + box.w - 4, box.y + box.h - 11, "+" + std::to_string(mLines.size() - 4), color::kTextGold, Align::Right);
    }

    void clickInventory(int, ItemStack const& stack, Button b, bool shift) override {
        if (items::isEmpty(stack)) {
            cursor.reset();
            return;
        }
        if (shift) mSend({{"op", "reference"}, {"item", items::toJson(items::withCount(stack, 1))}});
        else if (b == Button::Left) cursor = std::make_shared<ItemStack>(items::withCount(stack, 1));
    }

private:
    void addSelected(bool inverted) {
        if (mOptions.empty()) return;
        auto const& o = mOptions[static_cast<std::size_t>(std::clamp(mSelector->value, 0, static_cast<int>(mOptions.size()) - 1))];
        mSend({{"op", "add"}, {"type", o.value("type", std::string{})}, {"value", o.value("value", std::string{})}, {"inverted", inverted}});
    }

    SendFn                   mSend;
    Json                     mData;
    Slot*                    mRef{};
    ScrollInput*             mSelector{};
    IconButton*              mModes[3]{};
    std::vector<Json>        mOptions;
    std::vector<std::string> mLines;
};
} // namespace

class FilterScreensModule {
public:
    void install() {
        registerServerScreen("common.filter", [](Json const& data, SendFn send) {
            return std::make_unique<ListFilterScreen>(data, std::move(send));
        });
        registerServerScreen("common.attribute_filter", [](Json const& data, SendFn send) {
            return std::make_unique<AttributeFilterScreen>(data, std::move(send));
        });
    }
    void uninstall() {}
};

} // namespace lv::client::screen

LV_REGISTER_MODULE(lv::client::screen::FilterScreensModule);

#endif // LV_CLIENT
