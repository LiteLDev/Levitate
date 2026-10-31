// The filter items (SPEC §2.26, §4): right-click opens the filter screen (native for companions,
// forms for vanilla clients); the screen edits the filter in the player's hand.

#include <algorithm>
#include <map>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "core/common/FilterLogic.h"
#include "feature/common/Filter.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::common::filter {

namespace {
using core::common::AttributeMode;
using core::common::kListFilterSlots;
using Json = nlohmann::json;

constexpr char const* kListScreen      = "common.filter";
constexpr char const* kAttributeScreen = "common.attribute_filter";

/// The filter in the hand slot named by the screen context (nullptr when it moved away).
ItemStack const* heldFilter(Player& player, Json const& ctx, bool attribute) {
    int slot = ctx.value("slot", -1);
    if (slot != player.getSelectedItemSlot()) return nullptr;
    auto const& s = player.getSelectedItem();
    return (attribute ? isAttributeFilter(s) : isListFilter(s)) ? &s : nullptr;
}

void storeHeld(Player& player, ItemStack const& stack) {
    player.setSelectedItem(stack);
    player.sendInventory(false);
}

/// Distinct items of the player's inventory (for the form fallback's pickers).
std::vector<ItemStack> inventoryItems(Player& player) {
    std::vector<ItemStack> out;
    auto&                  inv = player.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& s = inv.getItem(i);
        if (items::isEmpty(s)) continue;
        bool seen = std::any_of(out.begin(), out.end(), [&](ItemStack const& o) { return items::sameItemSameData(o, s); });
        if (!seen) out.push_back(items::withCount(s, 1));
    }
    return out;
}

// ---- list filter -------------------------------------------------------------------------------

Json listData(ItemStack const& filter) {
    auto data  = readList(filter);
    Json items = Json::array();
    for (auto const& s : data.items) items.push_back(items::toJson(s));
    return {{"items", items}, {"blacklist", data.blacklist}, {"respect_data", data.respectData}};
}

void listAction(Player& player, Json const& ctx, Json const& action) {
    auto const* held = heldFilter(player, ctx, false);
    if (!held) return;
    ItemStack filter = *held;
    auto      data   = readList(filter);
    auto      op     = action.value("op", std::string{});
    int       index  = action.value("index", -1);
    bool      valid  = index >= 0 && index < kListFilterSlots;
    if (op == "set" && valid) {
        data.items[static_cast<std::size_t>(index)] = items::withCount(items::fromJson(action.value("item", Json())), 1);
        log().info("filter: {} set slot {} to {}", player.getRealName(), index, items::idOf(data.items[static_cast<std::size_t>(index)]));
    } else if (op == "clear" && valid) {
        data.items[static_cast<std::size_t>(index)] = items::empty();
        log().info("filter: {} cleared slot {}", player.getRealName(), index);
    } else if (op == "mode") {
        data.blacklist = action.value("blacklist", false);
        log().info("filter: {} set mode {}", player.getRealName(), data.blacklist ? "deny-list" : "allow-list");
    } else if (op == "respect_data") {
        data.respectData = action.value("value", false);
        log().info("filter: {} set respect data {}", player.getRealName(), data.respectData);
    } else if (op == "reset") {
        // Create's GhostItemMenu.clearContents: the entries go, Deny-List / Respect Data stay
        data.items.assign(kListFilterSlots, items::empty());
        log().info("filter: {} reset the filter", player.getRealName());
    } else {
        return;
    }
    writeList(filter, data);
    storeHeld(player, filter);
}

void listForm(Player& player, Json const& ctx) {
    auto const* held = heldFilter(player, ctx, false);
    if (!held) return;
    auto data = readList(*held);
    auto inv  = inventoryItems(player);
    auto keep = trFor(player, "lv.common.form.keep", "(keep)");
    auto none = trFor(player, "lv.common.form.empty", "(empty)");

    ll::form::CustomForm form(items::displayName(*held));
    form.appendLabel(trFor(player, "lv.common.filter.form.hint", "Pick an item for each slot. Items in your inventory are listed."));
    for (int i = 0; i < kListFilterSlots; ++i) {
        auto const&              cur = data.items[static_cast<std::size_t>(i)];
        std::vector<std::string> options{keep + " " + (items::isEmpty(cur) ? none : items::displayName(cur)), none};
        for (auto const& s : inv) options.push_back(items::displayName(s));
        form.appendDropdown("slot" + std::to_string(i), trFor(player, "lv.common.filter.form.slot", "Slot {}", i + 1), options, 0);
    }
    form.appendDropdown(
        "mode",
        trFor(player, "lv.common.filter.form.mode", "Mode"),
        {trFor(player, "create.gui.filter.allow_list", "Allow-List"), trFor(player, "create.gui.filter.deny_list", "Deny-List")},
        data.blacklist ? 1 : 0
    );
    form.appendDropdown(
        "data",
        trFor(player, "lv.common.filter.form.data", "Data"),
        {trFor(player, "create.gui.filter.ignore_data", "Ignore Data"), trFor(player, "create.gui.filter.respect_data", "Respect Data")},
        data.respectData ? 1 : 0
    );
    form.sendTo(player, [ctx, inv](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto const* held = heldFilter(p, ctx, false);
        if (!held) return;
        ItemStack filter = *held;
        auto      data   = readList(filter);
        auto      pick   = [&](std::string const& key) -> int {
            auto it = result->find(key);
            if (it == result->end()) return 0;
            if (auto const* v = std::get_if<uint64>(&it->second)) return static_cast<int>(*v);
            return 0;
        };
        for (int i = 0; i < kListFilterSlots; ++i) {
            int choice = pick("slot" + std::to_string(i));
            if (choice == 1) data.items[static_cast<std::size_t>(i)] = items::empty();
            else if (choice >= 2 && choice - 2 < static_cast<int>(inv.size())) data.items[static_cast<std::size_t>(i)] = inv[static_cast<std::size_t>(choice - 2)];
        }
        data.blacklist   = pick("mode") == 1;
        data.respectData = pick("data") == 1;
        writeList(filter, data);
        storeHeld(p, filter);
        log().info("filter: {} saved the filter (form)", p.getRealName());
    });
}

// ---- attribute filter --------------------------------------------------------------------------

/// The reference item a player put into the attribute filter screen (server-side, per player).
std::unordered_map<std::int64_t, ItemStack>& references() {
    static std::unordered_map<std::int64_t, ItemStack> m;
    return m;
}

Json attributeData(Player& player, ItemStack const& filter) {
    auto locale = localeOf(player);
    auto data   = readAttributes(filter);
    Json attrs  = Json::array();
    for (auto const& a : data.attributes)
        attrs.push_back({{"type", a.type}, {"value", a.value}, {"inverted", a.inverted}, {"text", describe(a, locale)}});
    Json options = Json::array();
    Json ref     = nullptr;
    if (auto it = references().find(player.getOrCreateUniqueID().rawID); it != references().end()) {
        ref = items::toJson(it->second);
        for (auto const& a : attributesOf(it->second)) {
            AttributeEntry inv = a;
            inv.inverted       = true;
            options.push_back({{"type", a.type}, {"value", a.value}, {"text", describe(a, locale)}, {"text_inverted", describe(inv, locale)}});
        }
    }
    return {
        {"mode",       core::common::attributeModeName(data.mode)},
        {"attributes", attrs                                     },
        {"reference",  ref                                       },
        {"options",    options                                   },
    };
}

bool addAttribute(Player& player, ItemStack& filter, AttributeEntry entry) {
    auto data = readAttributes(filter);
    // Create's AttributeFilterScreen: no limit; an attribute already listed (inverted or not) can't
    // be added again
    bool listed = std::any_of(data.attributes.begin(), data.attributes.end(), [&](AttributeEntry const& a) {
        return a.type == entry.type && a.value == entry.value;
    });
    if (listed) return false;
    data.attributes.push_back(entry);
    writeAttributes(filter, data);
    log().info("attribute filter: {} added {}", player.getRealName(), describe(entry));
    return true;
}

void attributeAction(Player& player, Json const& ctx, Json const& action) {
    auto const* held = heldFilter(player, ctx, true);
    if (!held) return;
    ItemStack filter = *held;
    auto      op     = action.value("op", std::string{});
    auto      key    = player.getOrCreateUniqueID().rawID;
    if (op == "reference") {
        auto ref = items::fromJson(action.value("item", Json()));
        if (items::isEmpty(ref)) references().erase(key);
        else references()[key] = items::withCount(ref, 1);
        log().info("attribute filter: {} set the reference item to {}", player.getRealName(), items::isEmpty(ref) ? "nothing" : items::idOf(ref));
        return; // the filter itself does not change
    }
    if (op == "add") {
        AttributeEntry e{action.value("type", std::string{}), action.value("value", std::string{}), action.value("inverted", false)};
        // only attributes the reference item really has can be added (as Create's screen)
        auto it = references().find(key);
        if (it == references().end()) return;
        auto offered = attributesOf(it->second);
        bool ok      = std::any_of(offered.begin(), offered.end(), [&](AttributeEntry const& a) {
            return a.type == e.type && a.value == e.value;
        });
        if (!ok || !addAttribute(player, filter, e)) return;
    } else if (op == "mode") {
        auto data = readAttributes(filter);
        data.mode = core::common::attributeModeFromName(action.value("mode", std::string{}), data.mode);
        writeAttributes(filter, data);
        log().info("attribute filter: {} set mode {}", player.getRealName(), core::common::attributeModeName(data.mode));
    } else if (op == "reset") {
        // Create's AttributeFilterMenu.clearContents: the attributes go, the mode stays
        auto data = readAttributes(filter);
        data.attributes.clear();
        writeAttributes(filter, data);
        log().info("attribute filter: {} reset the filter", player.getRealName());
    } else {
        return;
    }
    storeHeld(player, filter);
}

void attributeAddForm(Player& player, Json const& ctx, ItemStack const& ref) {
    auto offered = attributesOf(ref);
    if (offered.empty()) {
        player.sendMessage(trFor(player, "create.gui.attribute_filter.no_selected_attributes", "No attributes selected"));
        return;
    }
    auto                     locale = localeOf(player);
    std::vector<std::string> options{trFor(player, "lv.common.form.none", "(none)")};
    for (auto const& a : offered) {
        AttributeEntry inv = a;
        inv.inverted       = true;
        options.push_back("+ " + describe(a, locale));
        options.push_back("- " + describe(inv, locale));
    }
    ll::form::CustomForm form(trFor(player, "create.gui.attribute_filter.add_attribute", "Add attribute to List"));
    form.appendLabel(items::displayName(ref));
    form.appendDropdown("attr", trFor(player, "lv.common.attribute_filter.form.attribute", "Attribute"), options, 0);
    form.sendTo(player, [ctx, offered](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto const* held = heldFilter(p, ctx, true);
        if (!held) return;
        auto it = result->find("attr");
        if (it == result->end()) return;
        auto const* v = std::get_if<uint64>(&it->second);
        if (!v || *v == 0) return;
        auto idx = static_cast<std::size_t>((*v - 1) / 2);
        if (idx >= offered.size()) return;
        AttributeEntry e = offered[idx];
        e.inverted       = ((*v - 1) % 2) == 1;
        ItemStack filter = *held;
        if (addAttribute(p, filter, e)) storeHeld(p, filter);
        else p.sendMessage(trFor(p, "lv.common.attribute_filter.full", "This attribute is already in the list."));
    });
}

void attributeForm(Player& player, Json const& ctx) {
    auto const* held = heldFilter(player, ctx, true);
    if (!held) return;
    auto        data   = readAttributes(*held);
    auto        locale = localeOf(player);
    auto        inv    = inventoryItems(player);
    std::string summary;
    for (auto const& a : data.attributes) summary += "- " + describe(a, locale) + "\n";
    if (summary.empty()) summary = trFor(player, "create.gui.attribute_filter.no_selected_attributes", "No attributes selected");

    ll::form::CustomForm form(items::displayName(*held));
    form.appendLabel(summary);
    form.appendDropdown(
        "mode",
        trFor(player, "lv.common.filter.form.mode", "Mode"),
        {trFor(player, "create.gui.attribute_filter.allow_list_disjunctive", "Allow-List (Any)"),
         trFor(player, "create.gui.attribute_filter.allow_list_conjunctive", "Allow-List (All)"),
         trFor(player, "create.gui.attribute_filter.deny_list", "Deny-List")},
        static_cast<std::size_t>(data.mode)
    );
    std::vector<std::string> refs{trFor(player, "lv.common.form.none", "(none)")};
    for (auto const& s : inv) refs.push_back(items::displayName(s));
    form.appendDropdown("ref", trFor(player, "lv.common.attribute_filter.form.reference", "Add attributes of"), refs, 0);
    form.appendToggle("reset", trFor(player, "lv.common.attribute_filter.form.reset", "Reset (remove every attribute)"), false);
    form.sendTo(player, [ctx, inv](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto const* held = heldFilter(p, ctx, true);
        if (!held) return;
        ItemStack filter = *held;
        auto      data   = readAttributes(filter);
        if (auto it = result->find("reset"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second); v && *v) data.attributes.clear();
        }
        if (auto it = result->find("mode"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second)) data.mode = static_cast<AttributeMode>(std::min<uint64>(*v, 2));
        }
        writeAttributes(filter, data);
        storeHeld(p, filter);
        log().info("attribute filter: {} saved the filter (form)", p.getRealName());
        if (auto it = result->find("ref"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second); v && *v > 0 && *v <= inv.size()) {
                attributeAddForm(p, ctx, inv[static_cast<std::size_t>(*v - 1)]);
            }
        }
    });
}
} // namespace

class FilterItemsModule {
public:
    void install() {
        screens::registerType({
            .id   = kListScreen,
            .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
                auto const* held = heldFilter(p, ctx, false);
                if (!held) return std::nullopt;
                return listData(*held);
            },
            .action = listAction,
            .form   = listForm,
        });
        screens::registerType({
            .id   = kAttributeScreen,
            .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
                auto const* held = heldFilter(p, ctx, true);
                if (!held) return std::nullopt;
                return attributeData(p, *held);
            },
            .action = attributeAction,
            .form   = attributeForm,
        });
        // using a filter (in the air, or on a block that does not take the click) opens its screen
        mHandler = interactions::onUse({}, [](interactions::UseContext const& c) {
            auto& player = c.player;
            bool  list = isListFilter(c.item), attr = isAttributeFilter(c.item);
            if (!list && !attr) return interactions::Result::Pass;
            if (c.sneaking) return interactions::Result::Pass; // Create's FilterItem.use: not while sneaking
            Json ctx{{"slot", player.getSelectedItemSlot()}};
            if (attr) references().erase(player.getOrCreateUniqueID().rawID);
            screens::open(player, list ? kListScreen : kAttributeScreen, ctx);
            return interactions::Result::Consume;
        });
        mLeave = ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerDisconnectEvent>(
            [](ll::event::PlayerDisconnectEvent& ev) { references().erase(ev.self().getOrCreateUniqueID().rawID); }
        );
    }
    void uninstall() {
        interactions::remove(mHandler);
        ll::event::EventBus::getInstance().removeListener(mLeave);
    }

private:
    interactions::HandlerId mHandler   = 0;
    ll::event::ListenerPtr  mLeave;
};

} // namespace lv::feature::common::filter

LV_REGISTER_MODULE(lv::feature::common::filter::FilterItemsModule);
