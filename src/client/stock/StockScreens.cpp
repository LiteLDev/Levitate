// Native screens of the stock unit (SPEC §4, §3.6.26): the stock keeper request screen, the
// stock ticker category editor, the redstone requester, the factory gauge settings and item
// picker; plus the hover label of gauges and tickers. Everything goes through "screen.action";
// the server answers with fresh data. Search, addresses and category names are typed into native
// text fields (vanilla players get the server's forms).

#ifdef LV_CLIENT

#include <algorithm>
#include <array>

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
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::stock {

namespace {
using namespace lv::client::screen;
namespace items = lv::feature::common::items;
namespace net   = lv::feature::net;

/// A render-only icon built from item JSON on the client thread (never a copy of a game-owned
/// stack, as Screen::setCursorItem).
std::shared_ptr<ItemStack> stackOf(Json const& j) {
    if (!j.is_object()) return nullptr;
    auto icon = std::make_shared<ItemStack>();
    *icon     = items::fromJson(j);
    if (icon->isNull()) return nullptr;
    return icon;
}

/// Puts server text into a field unless the player is typing in it.
void syncField(TextField& field, std::string const& text) {
    if (!field.focused()) field.text = text;
}

std::string countText(int n) {
    if (n >= 1'000'000'000) return "∞";
    if (n >= 1'000'000) return std::to_string(n / 1'000'000) + "m";
    if (n >= 10'000) return std::to_string(n / 1000) + "k";
    return std::to_string(n);
}

/// A slot showing a big count at its bottom right.
class CountSlot : public Slot {
public:
    using Slot::Slot;
    int  count = 0;
    void render(Canvas& c, float mx, float my) override {
        Slot::render(c, mx, my);
        if (stack && count > 0) c.text(rect.x + rect.w - 1, rect.y + rect.h - 9, countText(count), color::kText, Align::Right);
    }
};

// ---- stock keeper request screen ---------------------------------------------------------------

class RequestScreen : public Screen {
public:
    static constexpr int kCols = 9, kRows = 5;

    RequestScreen(Json const& data, SendFn send)
    : Screen(ctr("create.gui.stock_keeper.title", "Stock Keeper"), 186, 196),
      mSend(std::move(send)),
      mData(data) {}

    void init() override {
        for (int i = 0; i < kCols * kRows; ++i) {
            auto r = core::common::gridCell(12, 30, kCols, 18, i);
            mGrid.push_back(&add<CountSlot>(r, [this, i](Button b, bool shift) { clickStock(i, b, shift); }));
        }
        for (int i = 0; i < 9; ++i) {
            auto r = core::common::gridCell(12, 128, 9, 18, i);
            mOrder.push_back(&add<CountSlot>(r, [this, i](Button b, bool shift) { clickOrder(i, b, shift); }));
        }
        // Create's search box filters the stock as the player types
        mSearch              = &add<TextField>(Rect{12, 12, 140, 16}, mData.value("search", std::string{}));
        mSearch->placeholder = ctr("create.gui.stock_keeper.search_items", "Search Items");
        mSearch->onChange    = [this](std::string const& text) { mSend({{"op", "search"}, {"text", text}}); };
        mSearch->onSubmit    = [this](std::string const& text) {
            if (!text.empty()) log().info("stock keeper search typed: '{}'", text);
        };
        // the address the order is sent to (kept on the ticker as the last used one)
        mAddress              = &add<TextField>(Rect{12, 150, 162, 16}, mData.value("address", std::string{}), [this](std::string const& text) {
            if (text == mData.value("address", std::string{})) return;
            mData["address"] = text;
            mSend({{"op", "address"}, {"text", text}});
            log().info("stock keeper address typed: '{}'", text);
        });
        mAddress->placeholder = ctr("create.gui.stock_keeper.package_address", "Package Address");
        mAddress->tooltip     = {ctr("create.gui.stock_keeper.package_address", "Package Address")};
        mAddress->maxLength   = 50;
        mSearch->maxLength    = 50;
        mSendBtn      = &add<IconButton>(Rect{126, 170, 48, 18}, ">", [this] { sendOrder(); });
        auto& clear   = add<IconButton>(Rect{104, 170, 18, 18}, "x", [this] { mOrderList.clear(); refreshOrder(); });
        clear.tooltip = {ctr("lv.stock.screen.clear_order", "Clear order")};
        mLock         = &add<IconButton>(Rect{156, 12, 18, 16}, "L", [this] { mSend({{"op", "lock"}}); });
        onData(mData);
    }

    void onData(Json const& data) override {
        mData = data;
        syncField(*mSearch, data.value("search", std::string{}));
        syncField(*mAddress, data.value("address", std::string{}));
        mEntries.clear();
        mHeaders.clear();
        for (auto const& g : data.value("groups", Json::array())) {
            if (g.value("hidden", false)) continue;
            auto list = g.value("items", Json::array());
            if (list.empty()) continue;
            // a category starts on a new row
            while (mEntries.size() % kCols) mEntries.push_back({});
            mHeaders.push_back({static_cast<int>(mEntries.size() / kCols), g.value("name", std::string{}), g.value("index", -1)});
            for (auto const& e : list) mEntries.push_back({stackOf(e.value("item", Json())), e.value("count", 0)});
        }
        bool program        = data.value("program", false);
        mSendBtn->symbol    = program ? "P" : ">";
        mSendBtn->tooltip   = {program ? ctr("create.gui.stock_keeper.program_requester", "Program Requester") : ctr("create.gui.stock_keeper.send", "Send")};
        mLock->visible      = data.value("canLock", false);
        mLock->toggled      = data.value("locked", false);
        mLock->tooltip      = {data.value("locked", false) ? ctr("create.gui.stock_keeper.network_locked", "Network is locked") : ctr("create.gui.stock_keeper.network_open", "Network is open"),
                               ctr("create.gui.stock_keeper.network_lock_tip", "Locking prevents other Players"), ctr("create.gui.stock_keeper.network_lock_tip_1", "from ordering items directly"),
                               ctr("create.gui.stock_keeper.network_lock_tip_2", "Click to toggle")};
        // the screen is rebuilt after a vanilla-style form: the order being put together survives
        auto& stash = orderStash();
        if (mOrderList.empty() && stash.first == data.value("pos", Json())) mOrderList = stash.second;
        // keep the order within the new stock
        for (auto& o : mOrderList) o.second = std::min(o.second, available(o.first));
        std::erase_if(mOrderList, [](auto const& o) { return o.second <= 0; });
        mScroll = std::clamp(mScroll, 0, maxScroll());
        refreshGrid();
        refreshOrder();
    }

    void renderExtra(Canvas& c, float, float) override {
        for (auto const& h : mHeaders) {
            int row = h.row - mScroll;
            if (row < 0 || row >= kRows) continue;
            if (!h.name.empty()) c.text(174, 30 + 18.0f * static_cast<float>(row) + 5, h.name, color::kTextGold, Align::Right);
        }
        if (mData.value("links", 0) == 0) c.text(width / 2, 70, ctr("create.gui.stock_keeper.no_packagers_linked", "No Packagers Linked"), color::kButtonOff, Align::Centre);
        else if (mEntries.empty()) c.text(width / 2, 70, ctr(mData.value("search", std::string{}).empty() ? "create.gui.stock_keeper.inventories_empty" : "create.gui.stock_keeper.no_search_results", "Linked Inventories are Empty"), color::kTextGray, Align::Centre);
        if (mOrderList.size() >= 9) c.text(12, 174, ctr("create.gui.stock_keeper.slots_full", "Order slots already full"), color::kButtonOff);
    }

    bool clickBackground(float, float, Button, bool) override { return false; }

    // the grid scrolls with the wheel (rows)
    void tick() override {}
    bool scrollGrid(int delta) {
        mScroll = std::clamp(mScroll - delta, 0, maxScroll());
        refreshGrid();
        return true;
    }

private:
    struct Entry {
        std::shared_ptr<ItemStack> stack;
        int                        count = 0;
    };
    struct Header {
        int         row = 0;
        std::string name;
        int         index = -1;
    };

    int maxScroll() const { return std::max(0, static_cast<int>((mEntries.size() + kCols - 1) / kCols) - kRows); }

    int available(std::shared_ptr<ItemStack> const& s) const {
        for (auto const& e : mEntries)
            if (e.stack && s && items::sameItemSameData(*e.stack, *s)) return e.count;
        return 0;
    }

    void refreshGrid() {
        for (int i = 0; i < kCols * kRows; ++i) {
            auto  idx  = static_cast<std::size_t>(mScroll * kCols + i);
            auto& slot = *mGrid[static_cast<std::size_t>(i)];
            if (idx < mEntries.size() && mEntries[idx].stack) {
                slot.stack   = mEntries[idx].stack;
                slot.count   = mEntries[idx].count;
                slot.tooltip = {slot.stack->getName() + " x" + countText(slot.count), ctr("lv.stock.screen.click_add", "Click: +1, Shift: +stack, right-click: -1")};
            } else {
                slot.stack = nullptr;
                slot.count = 0;
                slot.tooltip.clear();
            }
        }
    }

    using OrderList = std::vector<std::pair<std::shared_ptr<ItemStack>, int>>;
    static std::pair<Json, OrderList>& orderStash() {
        static std::pair<Json, OrderList> s;
        return s;
    }

    void refreshOrder() {
        orderStash() = {mData.value("pos", Json()), mOrderList};
        for (int i = 0; i < 9; ++i) {
            auto& slot = *mOrder[static_cast<std::size_t>(i)];
            if (i < static_cast<int>(mOrderList.size())) {
                slot.stack   = mOrderList[static_cast<std::size_t>(i)].first;
                slot.count   = mOrderList[static_cast<std::size_t>(i)].second;
                slot.tooltip = {slot.stack->getName() + " x" + std::to_string(slot.count)};
            } else {
                slot.stack = nullptr;
                slot.count = 0;
                slot.tooltip.clear();
            }
        }
        mSendBtn->enabled = !mOrderList.empty();
    }

    /// Create's StockKeeperRequestScreen click: +1, Shift +stack, right-click -1.
    void change(std::shared_ptr<ItemStack> const& s, int max, Button b, bool shift) {
        if (!s) return;
        auto it = std::find_if(mOrderList.begin(), mOrderList.end(), [&](auto const& o) { return items::sameItemSameData(*o.first, *s); });
        int  step = shift ? std::max<int>(1, s->getMaxStackSize()) : 1;
        if (b == Button::Right) {
            if (it == mOrderList.end()) return;
            it->second -= step;
            if (it->second <= 0) mOrderList.erase(it);
        } else {
            if (it == mOrderList.end()) {
                if (mOrderList.size() >= 9) return;
                mOrderList.push_back({s, 0});
                it = mOrderList.end() - 1;
            }
            it->second = std::min(max, it->second + step);
        }
        refreshOrder();
    }

    void clickStock(int i, Button b, bool shift) {
        auto idx = static_cast<std::size_t>(mScroll * kCols + i);
        if (idx >= mEntries.size()) return;
        change(mEntries[idx].stack, mEntries[idx].count, b, shift);
    }

    void clickOrder(int i, Button b, bool shift) {
        if (i >= static_cast<int>(mOrderList.size())) return;
        auto s = mOrderList[static_cast<std::size_t>(i)].first;
        change(s, available(s), b == Button::Left ? Button::Right : Button::Left, shift);
    }

    void sendOrder() {
        if (mOrderList.empty()) return;
        Json list = Json::array();
        for (auto const& [s, n] : mOrderList) list.push_back({{"item", items::toJsonWithCount(*s, 1)}, {"count", n}});
        mSend({{"op", "order"}, {"items", list}, {"address", mAddress->text}, {"encode", mData.value("program", false)}});
        mOrderList.clear();
        refreshOrder();
        close();
    }

    SendFn                                                   mSend;
    Json                                                     mData;
    std::vector<CountSlot*>                                  mGrid, mOrder;
    std::vector<Entry>                                       mEntries;
    std::vector<Header>                                      mHeaders;
    std::vector<std::pair<std::shared_ptr<ItemStack>, int>> mOrderList;
    IconButton *                                             mSendBtn{}, *mLock{};
    TextField *                                              mSearch{}, *mAddress{};
    int                                                      mScroll = 0;
};

/// Forwards the wheel over the item grid to the request screen.
class GridScroll : public Widget {
public:
    GridScroll(Rect r, RequestScreen& s) : Widget(r), mScreen(s) {}
    void render(Canvas&, float, float) override {}
    bool scroll(float mx, float my, int delta, bool) override { return hovered(mx, my) && mScreen.scrollGrid(delta); }

private:
    RequestScreen& mScreen;
};

// ---- category editor ---------------------------------------------------------------------------

class CategoryScreen : public Screen {
public:
    CategoryScreen(Json const& data, SendFn send)
    : Screen(ctr("create.gui.stock_ticker.category_editor", "Category Editor"), 186, 150),
      mSend(std::move(send)),
      mData(data) {
        showInventory = true;
    }

    void init() override {
        for (int i = 0; i < 6; ++i) {
            float y = 24 + 18.0f * static_cast<float>(i);
            Row   r;
            r.slot = &add<Slot>(Rect{10, y, 18, 18}, [](Button, bool) {});
            r.up   = &add<IconButton>(Rect{116, y + 1, 16, 16}, "^", [this, i] { mSend({{"op", "move"}, {"index", i}, {"delta", -1}}); });
            r.down = &add<IconButton>(Rect{133, y + 1, 16, 16}, "v", [this, i] { mSend({{"op", "move"}, {"index", i}, {"delta", 1}}); });
            // Create's category name box, edited in place
            r.name = &add<TextField>(Rect{32, y + 1, 80, 16}, std::string{}, [this, i](std::string const& text) {
                if (static_cast<std::size_t>(i) < mNames.size() && text == mNames[static_cast<std::size_t>(i)]) return;
                mSend({{"op", "rename"}, {"index", i}, {"name", text}});
                log().info("stock ticker category {} renamed to '{}'", i + 1, text);
            });
            r.name->maxLength   = 64;
            r.name->placeholder = ctr("create.gui.stock_ticker.empty_category_name_placeholder", "(Empty)");
            r.del  = &add<IconButton>(Rect{167, y + 1, 16, 16}, "x", [this, i] { mSend({{"op", "remove"}, {"index", i}}); });
            r.up->tooltip   = {ctr("lv.stock.screen.category_up", "Move up")};
            r.down->tooltip = {ctr("lv.stock.screen.category_down", "Move down")};
            r.name->tooltip = {ctr("create.gui.stock_ticker.category_name", "Category Name")};
            r.del->tooltip  = {ctr("create.gui.stock_ticker.delete_category", "Delete Category")};
            r.del->litColour = color::kButtonOff;
            mRows.push_back(r);
        }
        auto& addBtn  = add<IconButton>(Rect{10, 130, 18, 16}, "+", [this] { mSend({{"op", "add"}, {"slot", -1}}); });
        addBtn.tooltip = {ctr("create.gui.stock_ticker.new_category", "New Category"), ctr("lv.stock.screen.add_filter", "Shift-click a filter in your inventory to add a category")};
        onData(mData);
    }

    void onData(Json const& data) override {
        mData     = data;
        auto cats = data.value("categories", Json::array());
        mNames.assign(mRows.size(), {});
        for (std::size_t i = 0; i < mRows.size(); ++i) {
            bool has = i < cats.size();
            for (Widget* w : {static_cast<Widget*>(mRows[i].slot), static_cast<Widget*>(mRows[i].up), static_cast<Widget*>(mRows[i].down), static_cast<Widget*>(mRows[i].name), static_cast<Widget*>(mRows[i].del)})
                w->visible = has;
            if (!has) continue;
            mRows[i].slot->stack = stackOf(cats[i].value("filter", Json()));
            mNames[i]            = cats[i].value("name", std::string{});
            syncField(*mRows[i].name, mNames[i]);
        }
    }

    void renderExtra(Canvas& c, float, float) override {
        c.text(32, 134, ctr("create.gui.stock_ticker.category_filter_tip", "Place a List or Attribute Filter"), color::kTextDark, Align::Left, false);
    }

    void clickInventory(int slot, ItemStack const& stack, Button, bool shift) override {
        if (shift && !items::isEmpty(stack)) mSend({{"op", "add"}, {"slot", slot}});
    }

private:
    struct Row {
        Slot*       slot{};
        IconButton *up{}, *down{}, *del{};
        TextField*  name{};
    };
    SendFn                   mSend;
    Json                     mData;
    std::vector<Row>         mRows;
    std::vector<std::string> mNames;
};

// ---- redstone requester ------------------------------------------------------------------------

class RequesterScreen : public Screen {
public:
    RequesterScreen(Json const& data, SendFn send)
    : Screen(ctr("lv.stock.screen.requester", "Redstone Requester"), 186, 96),
      mSend(std::move(send)),
      mData(data) {}

    void init() override {
        for (int i = 0; i < 9; ++i) mSlots.push_back(&add<CountSlot>(core::common::gridCell(12, 22, 9, 18, i), [this, i](Button b, bool shift) { step(i, b == Button::Left ? 1 : -1, shift); }));
        mAllow = &add<IconButton>(Rect{12, 46, 18, 18}, "+", [this] { mSend({{"op", "set"}, {"partial", true}}); });
        mDeny  = &add<IconButton>(Rect{32, 46, 18, 18}, "=", [this] { mSend({{"op", "set"}, {"partial", false}}); });
        mAllow->tooltip   = {ctr("create.gui.redstone_requester.allow_partial", "Allow partial orders")};
        mDeny->tooltip    = {ctr("create.gui.redstone_requester.dont_allow_partial", "Must send all items")};
        mDeny->litColour  = color::kButtonOff;
        mAddress = &add<TextField>(Rect{12, 70, 162, 16}, mData.value("address", std::string{}), [this](std::string const& text) {
            if (text == mData.value("address", std::string{})) return;
            mData["address"] = text;
            mSend({{"op", "set"}, {"address", text}});
            log().info("redstone requester address typed: '{}'", text);
        });
        mAddress->maxLength   = 50;
        mAddress->placeholder = ctr("create.gui.redstone_requester.requester_address", "Send order to...");
        mAddress->tooltip     = {ctr("create.gui.redstone_requester.requester_address", "Send order to..."), ctr("create.gui.redstone_requester.requester_address_tip", "Enter the address this"),
                                 ctr("create.gui.redstone_requester.requester_address_tip_1", "order should be send to.")};
        onData(mData);
    }

    void onData(Json const& data) override {
        mData   = data;
        if (mAddress) syncField(*mAddress, data.value("address", std::string{}));
        auto ol = data.value("order", Json::array());
        for (std::size_t i = 0; i < mSlots.size(); ++i) {
            auto& s = *mSlots[i];
            s.stack = i < ol.size() ? stackOf(ol[i].value("item", Json())) : nullptr;
            s.count = i < ol.size() ? ol[i].value("count", 1) : 0;
            s.tooltip = s.stack ? std::vector<std::string>{s.stack->getName() + " x" + std::to_string(s.count), ctr("create.gui.factory_panel.scroll_to_change_amount", "Scroll to change amount")}
                                : std::vector<std::string>{};
        }
        bool partial     = data.value("partial", false);
        mAllow->toggled  = partial;
        mDeny->toggled   = !partial;
    }

    void renderExtra(Canvas& c, float, float) override {
        c.text(56, 51, mAllow->toggled ? ctr("create.gui.redstone_requester.allow_partial", "Allow partial orders") : ctr("create.gui.redstone_requester.dont_allow_partial", "Must send all items"), color::kTextDark, Align::Left, false);
    }

    /// Amounts 1..256, Shift ×10 (Create's mouseScrolled); clicks step too.
    void step(int i, int dir, bool shift) {
        auto& s = *mSlots[static_cast<std::size_t>(i)];
        if (!s.stack) return;
        s.count = std::clamp(s.count + dir * (shift ? 10 : 1), 1, 256);
        Json amounts = Json::array();
        for (auto* slot : mSlots)
            if (slot->stack) amounts.push_back(slot->count);
        mSend({{"op", "set"}, {"amounts", amounts}});
    }

private:
    SendFn                  mSend;
    Json                    mData;
    std::vector<CountSlot*> mSlots;
    IconButton *            mAllow{}, *mDeny{};
    TextField*              mAddress{};
};

class SlotScroll : public Widget {
public:
    SlotScroll(Rect r, std::function<void(int, bool)> fn) : Widget(r), mFn(std::move(fn)) {}
    void render(Canvas&, float, float) override {}
    bool scroll(float mx, float my, int delta, bool shift) override {
        if (!hovered(mx, my)) return false;
        mFn(delta > 0 ? 1 : -1, shift);
        return true;
    }

private:
    std::function<void(int, bool)> mFn;
};

// ---- factory gauge -----------------------------------------------------------------------------

class GaugeScreen : public Screen {
public:
    GaugeScreen(Json const& data, SendFn send) : Screen("", 186, 150), mSend(std::move(send)), mData(data) {}

    void init() override {
        bool restocker = mData.value("restocker", false);
        title = restocker ? ctr("create.gui.factory_panel.title_as_restocker", "Restocker Settings") : ctr("create.gui.factory_panel.title_as_recipe", "Recipe Settings");
        mItem = &add<Slot>(Rect{12, 22, 18, 18}, [](Button, bool) {});
        for (int i = 0; i < 9; ++i) {
            auto& s = add<CountSlot>(core::common::gridCell(12, 50, 9, 18, i), [this, i](Button b, bool shift) {
                if (b == Button::Middle) return;
                changeInput(i, b == Button::Left ? 1 : -1, shift && b == Button::Left, false);
            });
            mInputs.push_back(&s);
        }
        mOutput = &add<ScrollInput>(Rect{12, 74, 80, 16}, "", 1, 64, mData.value("output", 1), [this](int v) { mSend({{"op", "set"}, {"output", v}}); });
        mOutput->format = [](int v) { return ctr("create.gui.factory_panel.expected_output", "Expecting {}", "x" + std::to_string(v)); };
        mExpiry = &add<ScrollInput>(Rect{96, 74, 78, 16}, "", -1, 31, mData.value("promiseInterval", -1), [this](int v) { mSend({{"op", "set"}, {"promiseInterval", v}}); });
        mExpiry->format = [](int v) {
            if (v == -1) return ctr("create.gui.factory_panel.promises_do_not_expire", "Promises do not expire");
            return v == 0 ? std::string("30s") : std::to_string(v) + "m";
        };
        mExpiry->tooltip = {ctr("create.gui.factory_panel.promises_expire_title", "Promises expire after"), ctr("create.gui.factory_panel.promise_prevents_oversending", "This prevents over-sending.")};
        auto prompt = restocker ? ctr("create.gui.factory_panel.restocker_address", "Send items to...") : ctr("create.gui.factory_panel.recipe_address", "Send inputs to...");
        mAddress    = &add<TextField>(Rect{12, 96, 162, 16}, mData.value("address", std::string{}), [this](std::string const& text) {
            if (text == mData.value("address", std::string{})) return;
            mData["address"] = text;
            mSend({{"op", "set"}, {"address", text}});
            log().info("factory gauge address typed: '{}'", text);
        });
        mAddress->maxLength   = 50;
        mAddress->placeholder = prompt;
        mAddress->tooltip     = {prompt};
        auto& clear  = add<IconButton>(Rect{12, 120, 18, 18}, "0", [this] { mSend({{"op", "set"}, {"clearPromises", true}}); });
        clear.tooltip = {ctr("create.gui.factory_panel.promised_items", "Promised items"), ctr("create.gui.factory_panel.left_click_reset", "Left-Click to reset")};
        if (!restocker) {
            auto& conn   = add<IconButton>(Rect{134, 120, 18, 18}, "+", [this] { mSend({{"op", "connect"}}); });
            conn.tooltip = {ctr("create.gui.factory_panel.connect_input", "Add new connection")};
        }
        auto& reset   = add<IconButton>(Rect{156, 120, 18, 18}, "x", [this] { mSend({{"op", "set"}, {"reset", true}}); close(); });
        reset.tooltip = {ctr("create.gui.factory_panel.reset", "Reset all settings")};
        reset.litColour = color::kButtonOff;
        for (auto* w : {static_cast<Widget*>(mOutput)}) w->visible = !restocker;
        onData(mData);
    }

    void onData(Json const& data) override {
        mData        = data;
        if (mAddress) {
            syncField(*mAddress, data.value("address", std::string{}));
            // Create: a gauge without an address is inactive (the box's tooltip says so)
            mAddress->tooltip = {mAddress->placeholder};
            if (data.value("address", std::string{}).empty())
                mAddress->tooltip.push_back(ctr("create.gui.factory_panel.address_missing", "Inactive: missing a target address"));
        }
        mItem->stack = stackOf(data.value("item", Json()));
        auto inputs  = data.value("inputs", Json::array());
        for (std::size_t i = 0; i < mInputs.size(); ++i) {
            auto& s   = *mInputs[i];
            bool  has = i < inputs.size() && !mData.value("restocker", false);
            s.visible = has;
            if (!has) continue;
            s.stack   = stackOf(inputs[i].value("item", Json()));
            s.count   = inputs[i].value("amount", 1);
            s.tooltip = {s.stack ? s.stack->getName() + " x" + std::to_string(s.count) : std::string{}, ctr("create.gui.factory_panel.scroll_to_change_amount", "Scroll to change amount"),
                         ctr("create.gui.factory_panel.left_click_disconnect", "Left-Click to disconnect") + " (Shift)"};
        }
        mOutput->value = data.value("output", 1);
        mExpiry->value = data.value("promiseInterval", -1);
    }

    void renderExtra(Canvas& c, float, float) override {
        c.text(34, 24, mData.value("label", std::string{}), color::kTextDark, Align::Left, false);
        c.text(34, 33, ctr("lv.stock.screen.in_storage", "In storage: {}", countText(mData.value("inStorage", 0))) + "  " + ctr("lv.stock.screen.promised", "Promised: {}", mData.value("promised", 0)), color::kTextDark, Align::Left, false);
        if (!mData.value("restocker", false) && mData.value("inputs", Json::array()).empty())
            c.text(12, 54, ctr("create.gui.factory_panel.unconfigured_input_tip", "Ingredients can be added by"), color::kTextGray);
    }

    void changeInput(int i, int dir, bool disconnect, bool shift) {
        auto inputs = mData.value("inputs", Json::array());
        if (i >= static_cast<int>(inputs.size())) return;
        if (disconnect) {
            // Create: left-click an input to disconnect it
            mSend({{"op", "set"}, {"disconnect", inputs[static_cast<std::size_t>(i)].value("pos", Json())}, {"disconnectSlot", inputs[static_cast<std::size_t>(i)].value("slot", 0)}});
            return;
        }
        Json amounts = Json::array();
        for (std::size_t k = 0; k < inputs.size(); ++k) {
            int v = inputs[k].value("amount", 1);
            if (static_cast<int>(k) == i) v = std::clamp(v + dir * (shift ? 10 : 1), 1, 64);
            amounts.push_back(v);
        }
        mSend({{"op", "set"}, {"amounts", amounts}});
    }

private:
    SendFn                  mSend;
    Json                    mData;
    Slot*                   mItem{};
    std::vector<CountSlot*> mInputs;
    ScrollInput *           mOutput{}, *mExpiry{};
    TextField*              mAddress{};
};

class GaugeItemScreen : public Screen {
public:
    GaugeItemScreen(Json const& data, SendFn send)
    : Screen(ctr("create.gui.factory_panel.place_item_to_monitor", "Place Item to Monitor"), 176, 40),
      mSend(std::move(send)) {
        (void)data;
        showInventory = true;
    }
    void renderExtra(Canvas& c, float, float) override { c.text(width / 2, 22, ctr("create.logistics.filter.click_to_set", "Click with item to set"), color::kTextDark, Align::Centre, false); }
    void clickInventory(int, ItemStack const& stack, Button, bool) override {
        // the game's stack is only read (never copied on the client thread)
        if (!items::isEmpty(stack)) mSend({{"op", "set"}, {"item", items::toJsonWithCount(stack, 1)}});
    }

private:
    SendFn mSend;
};

// ---- hover label -------------------------------------------------------------------------------

struct Hover {
    bool                     has = false;
    core::GlobalPos          pos;
    std::array<int, 3>       key{};
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
    auto&       region = player->getDimensionBlockSource();
    auto const& id     = region.getBlock(hit.mBlock).getTypeName();
    if (id != "levitate:factory_gauge" && id != "levitate:stock_ticker") {
        h.has = false;
        return;
    }
    double             lx = hit.mPos.x - hit.mBlock.x, ly = hit.mPos.y - hit.mBlock.y, lz = hit.mPos.z - hit.mBlock.z;
    core::GlobalPos    pos{static_cast<int>(player->getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    std::array<int, 3> key{static_cast<int>(lx * 2), static_cast<int>(ly * 2), static_cast<int>(lz * 2)};
    if (h.pos == pos && h.key == key && h.cooldown > 0) return;
    h.pos = pos, h.key = key, h.cooldown = 10;
    net::client::send("stock.hover", {{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}, {"hit", Json::array({lx, ly, lz})}});
}
} // namespace

class StockScreensModule {
public:
    void install() {
        registerServerScreen("stock.request", [](Json const& data, SendFn send) {
            auto s = std::make_unique<RequestScreen>(data, std::move(send));
            s->add<GridScroll>(Rect{12, 30, 162, 90}, *s);
            return s;
        });
        registerServerScreen("stock.categories", [](Json const& data, SendFn send) { return std::make_unique<CategoryScreen>(data, std::move(send)); });
        registerServerScreen("stock.requester", [](Json const& data, SendFn send) {
            auto  s   = std::make_unique<RequesterScreen>(data, std::move(send));
            auto* raw = s.get();
            for (int i = 0; i < 9; ++i) s->add<SlotScroll>(core::common::gridCell(12, 22, 9, 18, i), [raw, i](int dir, bool shift) { raw->step(i, dir, shift); });
            return s;
        });
        registerServerScreen("stock.gauge", [](Json const& data, SendFn send) {
            auto  s   = std::make_unique<GaugeScreen>(data, std::move(send));
            auto* raw = s.get();
            for (int i = 0; i < 9; ++i) s->add<SlotScroll>(core::common::gridCell(12, 50, 9, 18, i), [raw, i](int dir, bool shift) { raw->changeInput(i, dir, false, shift); });
            return s;
        });
        registerServerScreen("stock.gauge_item", [](Json const& data, SendFn send) { return std::make_unique<GaugeItemScreen>(data, std::move(send)); });
    }
    // client input / render listeners are added at mod load (see ValueBoxClient.cpp)
    void load() {
        net::client::on("stock.hover", [](Json const& msg) {
            auto& h = hover();
            h.lines.clear();
            for (auto const& l : msg.value("lines", Json::array())) h.lines.push_back(l.get<std::string>());
            h.has = !h.lines.empty();
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

} // namespace lv::client::stock

LV_REGISTER_MODULE(lv::client::stock::StockScreensModule);

#endif // LV_CLIENT
