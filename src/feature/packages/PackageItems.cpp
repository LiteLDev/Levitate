// Cardboard packages and the package filter (SPEC §2.21, §3.6.25): item data, lore, opening,
// throwing and the address form.

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>

#include "ll/api/form/CustomForm.h"

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"

#include "core/common/Utf8.h"
#include "core/packages/Address.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/packages/PackagesInternal.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::packages {

namespace items   = common::items;
namespace screens = common::screens;

namespace {
constexpr char const* kDataKey  = "package";
constexpr int         kRareOdds = 7500; // Create's PackageStyles.RARE_CHANCE

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

Json dataOf(ItemStack const& s) {
    auto j = items::getData(s, kDataKey);
    return j.is_object() ? j : Json::object();
}

void storeData(ItemStack& s, Json const& j) { items::setData(s, kDataKey, j.empty() ? Json(nullptr) : j); }

bool endsWith(std::string_view s, std::string_view suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

/// Same item and data, ignoring the count (stacking rule of an item handler).
bool stackable(ItemStack const& a, ItemStack const& b) { return items::sameItemSameData(a, b); }

int maxStack(ItemStack const& s) { return std::max(1, int(s.getMaxStackSize())); }

} // namespace

ItemStack insertStackedInto(std::vector<ItemStack>& slots, ItemStack const& stack) {
    // NeoForge's insertItemStacked (slot limit 64), the same rule as core::packages::insertStacked
    int count = items::countOf(stack);
    int limit = std::min(64, maxStack(stack));
    for (int pass = 0; pass < 2 && count > 0; ++pass) {
        for (auto& slot : slots) {
            bool empty = items::isEmpty(slot);
            if (pass == 0 ? (empty || !stackable(slot, stack)) : !empty) continue;
            int have  = items::countOf(slot);
            int moved = std::min(limit - have, count);
            if (moved <= 0) continue;
            slot = items::withCount(stack, have + moved);
            count -= moved;
            if (count <= 0) break;
        }
    }
    return items::withCount(stack, count);
}

// ---- ids ---------------------------------------------------------------------------------------

std::vector<std::string> const& packageIds() {
    static std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (char const* size : {"12x12", "10x12", "10x8", "12x10"}) out.push_back(std::string("levitate:cardboard_package_") + size);
        for (char const* rare : {"creeper", "darcy", "evan", "jinx", "kryppers", "simi", "starlotte", "thunder", "up", "vector"})
            out.push_back(std::string("levitate:rare_") + rare + "_package");
        return out;
    }();
    return ids;
}

std::vector<std::string> const& postboxIds() {
    static std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (char const* c :
             {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"})
            out.push_back(std::string("levitate:") + c + "_postbox");
        return out;
    }();
    return ids;
}

bool isPostboxId(std::string_view id) {
    if (!id.starts_with("levitate:") || !endsWith(id, "_postbox")) return false;
    auto const& ids = postboxIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// ---- packages ------------------------------------------------------------------------------------

bool isPackage(ItemStack const& stack) {
    auto id = items::idOf(stack);
    if (!id.starts_with("levitate:") || !(endsWith(id, "_package") || id.starts_with("levitate:cardboard_package_"))) return false;
    auto const& ids = packageIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool isPackageFilter(ItemStack const& stack) { return items::idOf(stack) == kPackageFilterId; }

std::string addressOf(ItemStack const& stack) {
    if (items::isEmpty(stack)) return {};
    return dataOf(stack).value("address", std::string{});
}

void setAddress(ItemStack& stack, std::string const& address) {
    auto j       = dataOf(stack);
    j["address"] = address;
    storeData(stack, j);
    updateLore(stack);
}

void clearAddress(ItemStack& stack) {
    auto j = dataOf(stack);
    j.erase("address");
    storeData(stack, j);
    updateLore(stack);
}

std::vector<ItemStack> contentsOf(ItemStack const& box) {
    std::vector<ItemStack> out(kPackageSlots);
    auto                   j = dataOf(box);
    auto                   c = j.find("contents");
    if (c == j.end() || !c->is_array()) return out;
    for (std::size_t i = 0; i < c->size() && i < out.size(); ++i) out[i] = items::fromJson((*c)[i]);
    return out;
}

ItemStack defaultBox() { return items::make(packageIds().front()).value_or(ItemStack::EMPTY_ITEM()); }

ItemStack containing(std::vector<ItemStack> const& stacks) {
    auto const& ids  = packageIds();
    bool        rare = std::uniform_int_distribution<int>(0, kRareOdds - 1)(rng()) == 0;
    int         lo = rare ? 4 : 0, hi = rare ? int(ids.size()) - 1 : 3;
    auto        id  = ids[std::size_t(std::uniform_int_distribution<int>(lo, hi)(rng()))];
    ItemStack   box = items::make(id).value_or(ItemStack::EMPTY_ITEM());
    if (items::isEmpty(box)) return box;
    std::vector<ItemStack> slots(kPackageSlots);
    for (auto const& s : stacks)
        if (!items::isEmpty(s)) (void)insertStackedInto(slots, s);
    Json contents = Json::array();
    for (auto const& s : slots) contents.push_back(items::toJson(s));
    auto j        = dataOf(box);
    j["contents"] = contents;
    storeData(box, j);
    updateLore(box);
    return box;
}

bool fitsInPackage(ItemStack const& stack) {
    if (isPackage(stack)) return false;
    auto id = items::idOf(stack);
    return !endsWith(id, "shulker_box") && !endsWith(id, "bundle");
}

std::optional<OrderData> orderOf(ItemStack const& box) {
    auto j = dataOf(box);
    auto o = j.find("order");
    if (o == j.end() || !o->is_object()) return std::nullopt;
    OrderData d;
    d.orderId       = o->value("order_id", -1);
    d.linkIndex     = o->value("link_index", 0);
    d.isFinalLink   = o->value("is_final_link", false);
    d.fragmentIndex = o->value("fragment_index", 0);
    d.isFinal       = o->value("is_final", false);
    d.context       = o->value("order_context", Json());
    return d;
}

void setOrder(ItemStack& box, OrderData const& order) {
    auto j     = dataOf(box);
    j["order"] = {
        {"order_id",       order.orderId      },
        {"link_index",     order.linkIndex    },
        {"is_final_link",  order.isFinalLink  },
        {"fragment_index", order.fragmentIndex},
        {"is_final",       order.isFinal      },
    };
    if (!order.context.is_null()) j["order"]["order_context"] = order.context;
    storeData(box, j);
}

Json orderContextOf(ItemStack const& box) {
    if (auto o = orderOf(box)) return o->context;
    return dataOf(box).value("order_context", Json());
}

void setOrderContext(ItemStack& box, Json const& context) {
    auto j = dataOf(box);
    if (context.is_null()) j.erase("order_context");
    else j["order_context"] = context;
    storeData(box, j);
}

void updateLore(ItemStack& box) {
    std::vector<std::string> lines;
    auto                     address = addressOf(box);
    if (isPackageFilter(box)) {
        if (!cp::isBlank(address)) lines.push_back("§7-> §6" + address);
    } else {
        if (dataOf(box).contains("address")) lines.push_back("§6→ " + address);
        int visible = 0, skipped = 0;
        for (auto const& s : contentsOf(box)) {
            if (items::isEmpty(s) || endsWith(items::idOf(s), "_spawn_egg")) continue;
            if (visible > 2) {
                ++skipped;
                continue;
            }
            ++visible;
            lines.push_back("§7" + items::displayName(s) + " x" + std::to_string(items::countOf(s)));
        }
        if (skipped > 0) lines.push_back("§o" + tr("lv.packages.more_items", "and {} more...", skipped));
    }
    if (lines.empty()) box.clearCustomLore();
    else box.setCustomLore(lines);
}

bool filterMatches(ItemStack const& filter, ItemStack const& stack) {
    auto address = addressOf(filter);
    if (cp::isBlank(address)) return items::sameItem(filter, stack); // a plain filter item
    return isPackage(stack) && matchAddress(addressOf(stack), address);
}

// ---- player actions ----------------------------------------------------------------------------------

namespace {
/// The mob of a spawn egg (Create's SpawnEggItem.getType): the egg item's own actor identifier
/// (villager eggs spawn `villager_v2`, ...); nullopt for other items.
std::optional<ActorDefinitionIdentifier> spawnEggMob(ItemStack const& stack) {
    if (items::isEmpty(stack) || !endsWith(items::idOf(stack), "_spawn_egg")) return std::nullopt;
    auto const* item = stack.mItem.get();
    if (!item) return std::nullopt;
    auto id = item->getActorIdentifier(stack);
    if (id.mIdentifier->empty()) return std::nullopt;
    return id;
}

bool spawnFromEgg(BlockSource& region, ItemStack const& egg, Actor* spawner, cp::V3 const& at) {
    auto mob   = spawnEggMob(egg);
    auto level = ll::service::getLevel();
    if (!mob || !level) return false;
    auto* spawned = level->getSpawner().spawnMob(region, *mob, spawner, Vec3{float(at.x), float(at.y), float(at.z)}, false, false, false);
    if (spawned) log().info("packages: a {} spawned from a package's spawn egg", *mob->mFullName);
    return spawned != nullptr;
}

} // namespace

void openPackage(Player& player) {
    ItemStack box = player.getSelectedItem();
    if (!isPackage(box)) return;
    auto contents = contentsOf(box);
    player.setSelectedItem(items::withCount(box, items::countOf(box) - 1));
    player.sendInventory(false);
    // Create's PackageItem.open: a spawn egg spawns its mob one block in front of the player
    // (BlockPos.containing(position + horizontal look)); the egg is used up when the mob spawned
    auto   feet = player.getFeetPos();
    cp::V3 look = lookVec(player);
    look.y      = 0;
    if (look.length() > 1e-6) look = look * (1.0 / look.length());
    cp::V3 front{std::floor(feet.x + look.x) + 0.5, std::floor(feet.y + look.y), std::floor(feet.z + look.z) + 0.5};
    int stacks = 0;
    for (auto s : contents) {
        if (items::isEmpty(s)) continue;
        if (spawnEggMob(s) && spawnFromEgg(player.getDimensionBlockSource(), s, &player, front)) s = items::withCount(s, items::countOf(s) - 1);
        if (items::isEmpty(s)) continue;
        ++stacks;
        giveOrDrop(player, s);
    }
    auto p = player.getFeetPos();
    playSound(player.getDimensionId(), {p.x, p.y, p.z}, "levitate.packages.package_pop");
    log().info("packages: {} opened a package ({} stacks)", player.getRealName(), stacks);
}

void throwPackage(Player& player, int ticksHeld) {
    // Create's getPackageVelocity (the bow curve)
    float f = float(ticksHeld) / 20.f;
    f       = (f * f + f * 2.f) / 3.f;
    if (f > 1.f) f = 1.f;
    if (f < 0.1f) return;
    ItemStack held = player.getSelectedItem();
    if (!isPackage(held)) return;
    auto feet = player.getFeetPos();
    playSound(player.getDimensionId(), {feet.x, feet.y, feet.z}, "random.bow", 0.5f, 0.5f);
    ItemStack copy = items::withCount(held, 1);
    if (!player.isCreative()) {
        player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
        player.sendInventory(false);
    }
    cp::V3 motion = lookVec(player) * (f * 2.0);
    cp::V3 at     = cp::V3{feet.x, feet.y + 0.9, feet.z} + motion;
    dropItem(player.getDimensionId(), at, copy, motion);
    log().info("packages: {} threw a package (power {:.2f})", player.getRealName(), f);
}

namespace {
constexpr char kFilterScreen[] = "packages.package_filter";

using core::utf8Prefix;

/// Writes the address of the package filter in the player's hand (Create: 50 characters at most).
void setFilterAddress(Player& p, std::string const& address) {
    if (!isPackageFilter(p.getSelectedItem())) return;
    std::string text   = utf8Prefix(address, 50);
    ItemStack   filter = p.getSelectedItem();
    if (cp::isBlank(text)) clearAddress(filter);
    else setAddress(filter, text);
    p.setSelectedItem(filter);
    p.sendInventory(false);
    log().info("packages: package filter of {} set to '{}'", p.getRealName(), text);
}

bool filterInHand(Player& p, nlohmann::json const& ctx) {
    return ctx.value("slot", -1) == p.getSelectedItemSlot() && isPackageFilter(p.getSelectedItem());
}
} // namespace

void openPackageFilterForm(Player& player) {
    int slot = player.getSelectedItemSlot();
    if (!isPackageFilter(player.getSelectedItem())) return;
    ll::form::CustomForm form(trFor(player, "lv.packages.package_filter", "Package Filter"));
    form.appendLabel(trFor(player, "create.gui.package_port.catch_packages_wildcard", "Use * as a text wildcard"));
    form.appendInput(
        "address",
        trFor(player, "lv.packages.form.address", "Address"),
        "",
        addressOf(player.getSelectedItem())
    );
    form.sendTo(player, [slot](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result || p.getSelectedItemSlot() != slot || !isPackageFilter(p.getSelectedItem())) return;
        auto it = result->find("address");
        if (it == result->end()) return;
        auto const* text = std::get_if<std::string>(&it->second);
        if (!text) return;
        setFilterAddress(p, *text);
    });
}

void openPackageFilterScreen(Player& player) {
    if (!isPackageFilter(player.getSelectedItem())) return;
    screens::open(player, kFilterScreen, {{"slot", player.getSelectedItemSlot()}});
}

void installItemScreens() {
    // Create's PackageFilterScreen: one address box (native for companions, the form otherwise)
    screens::registerType({
        .id   = kFilterScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            if (!filterInHand(p, ctx)) return std::nullopt;
            return Json{{"address", addressOf(p.getSelectedItem())}};
        },
        .action = [](Player& p, Json const& ctx, Json const& action) {
            if (!filterInHand(p, ctx) || action.value("op", std::string{}) != "address") return;
            setFilterAddress(p, action.value("text", std::string{}));
        },
        .form = [](Player& p, Json const&) { openPackageFilterForm(p); },
    });
}

} // namespace lv::feature::packages
