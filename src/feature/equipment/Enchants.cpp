// Create's enchantments (AllEnchantments): Capacity (backtanks: + enchantedBacktankCapacity air per
// level, also on a placed backtank) and Potato Recovery (potato cannon: 12.5 % + 12.5 % per level).
//
// Bedrock's enchantment registry is a closed enum (Enchant::Type, vanilla ids 0..41) that clients
// know by id, so the levels live in the item's user data (`lv:enchants`) and are shown as lore lines
// named like enchantments. The enchanting table offers them as Java does: when Bedrock computed the
// table's options, each option rolls Java's EnchantmentHelper#selectEnchantment over the item's
// vanilla candidates plus Create's enchantment (weight 10, costs 15 + 15·(L-1) .. 45 + 15·(L-1)),
// seeded with the player's enchantment seed + the option; when that option is applied (the craft
// request of the table), the item gets the rolled level. Anvils merge the levels as Java's anvil
// (equal levels + 1, else the higher) and grindstones remove them.

#include <algorithm>
#include <map>
#include <utility>

#include "ll/api/memory/Hook.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/managers/models/EnchantingContainerManagerModel.h"
#include "mc/world/inventory/network/ItemStackNetResult.h"
#include "mc/world/inventory/network/crafting/CraftHandlerEnchant.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/item/enchanting/EnchantmentInstance.h"
#include "mc/world/item/enchanting/ItemEnchantOption.h"
#include "mc/world/item/enchanting/ItemEnchants.h"

#include "core/equipment/Gear.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;

namespace {

constexpr char kEnchantLore[] = "lv:enchant_lore"; ///< the lore lines this unit wrote (to replace them)
constexpr std::string_view kKeys[] = {kCapacity, kPotatoRecovery};

std::string enchantName(std::string_view key, Player const* viewer) {
    std::string k   = "enchantment.create." + std::string(key);
    std::string def = key == kCapacity ? "Capacity" : "Potato Recovery";
    return viewer ? trFor(*viewer, k, def) : tr(k, def);
}

/// Rewrites the lore lines of Create's enchantments (gray, as Java lists enchantments).
void refreshLore(ItemStack& stack, Player const* viewer) {
    auto old  = items::getData(stack, kEnchantLore);
    auto lore = stack.getCustomLore();
    if (old.is_array())
        for (auto const& o : old)
            if (o.is_string()) std::erase(lore, o.get<std::string>());
    std::vector<std::string> mine;
    for (auto key : kKeys)
        if (int l = enchantLevel(stack, key); l > 0) mine.push_back("§r§7" + enchantName(key, viewer) + " " + ce::enchantLevelName(l));
    lore.insert(lore.begin(), mine.begin(), mine.end());
    if (lore.empty()) stack.clearCustomLore();
    else stack.setCustomLore(lore);
    items::setData(stack, kEnchantLore, mine.empty() ? Json() : Json(mine));
}

using VanillaSet = std::vector<std::pair<int, int>>; ///< (type, level), sorted

VanillaSet vanillaOf(ItemEnchants const& e) {
    VanillaSet out;
    for (auto const& inst : e.getAllEnchants()) out.emplace_back(static_cast<int>(inst.mEnchantType), inst.mLevel);
    std::sort(out.begin(), out.end());
    return out;
}

/// What the table rolled for a player's current item: per option, its vanilla enchantments and the
/// level of Create's enchantment it adds.
struct RolledOption {
    VanillaSet  vanilla;
    std::string key;
    int         level = 0;
};
struct Rolled {
    std::string               itemId;
    std::vector<RolledOption> options;
};
std::map<std::int64_t, Rolled>& rolled() {
    static std::map<std::int64_t, Rolled> m;
    return m;
}

/// The player applying a table option (set around CraftHandlerEnchant::_handleCraftAction).
thread_local Player* gEnchanting = nullptr;
thread_local bool    gWriting    = false;

int javaWeight(Enchant::Frequency f) {
    switch (f) {
    case Enchant::Frequency::Common: return 10;
    case Enchant::Frequency::Uncommon: return 5;
    case Enchant::Frequency::Rare: return 2;
    default: return 1;
    }
}

bool isBook(std::string_view id) { return id == "minecraft:book" || id == "minecraft:enchanted_book"; }

/// The keys of Create's enchantments the item can get at the table.
std::vector<std::string_view> tableKeys(std::string_view itemId) {
    std::vector<std::string_view> out;
    for (auto key : kKeys)
        if (enchantApplies(itemId, key)) out.push_back(key);
    return out;
}

/// Java's selection for one option. `vanilla(cost)` gives the vanilla candidates.
std::pair<std::string, int> roll(
    std::string const&                                                     itemId,
    int                                                                    enchantValue,
    int                                                                    cost,
    int                                                                    seed,
    int                                                                    slot,
    std::function<std::vector<ce::EnchantCandidate>(int)> const&          vanilla,
    std::function<bool(ce::EnchantCandidate const&, ce::EnchantCandidate const&)> const& compatible
) {
    auto keys = tableKeys(itemId);
    if (keys.empty() || cost <= 0) return {};
    ce::JavaRandom random(static_cast<std::int64_t>(seed) + slot);
    auto           available = [&](int c) {
        auto list = vanilla ? vanilla(c) : std::vector<ce::EnchantCandidate>{};
        if (int l = ce::enchantLevelForCost(c))
            for (auto key : keys) list.push_back({std::string(key), l, ce::kEnchantWeight});
        return list;
    };
    auto picks = ce::selectEnchantments(random, enchantValue, cost, available, compatible);
    // Java's EnchantmentMenu#getEnchantmentList: a book keeps all but one random pick
    if (isBook(itemId) && picks.size() > 1) picks.erase(picks.begin() + random.nextInt(static_cast<int>(picks.size())));
    for (auto const& p : picks)
        for (auto key : keys)
            if (p.key == key) return {p.key, p.level};
    return {};
}

/// The vanilla enchantments the table offers for an item at a cost (Java's getAvailableEnchantmentResults
/// over Bedrock's registry: primary slots, not treasure, the highest level whose cost range holds `cost`).
std::vector<ce::EnchantCandidate> vanillaCandidates(Item const* item, bool book, int cost) {
    std::vector<ce::EnchantCandidate> out;
    int slot = item ? item->getEnchantSlot() : 0;
    for (auto const& e : Enchant::mEnchants()) {
        if (!e || e->mIsDisabled || e->isTreasureOnly() || !e->isDiscoverable()) continue;
        if (!book && (e->mPrimarySlots & slot) == 0) continue;
        for (int l = e->getMaxLevel(); l >= e->getMinLevel(); --l) {
            if (cost >= e->getMinCost(l) && cost <= e->getMaxCost(l)) {
                out.push_back({std::to_string(static_cast<int>(e->mEnchantType)), l, javaWeight(e->mFrequency)});
                break;
            }
        }
    }
    return out;
}

bool vanillaCompatible(ce::EnchantCandidate const& a, ce::EnchantCandidate const& b) {
    // Create's enchantments are compatible with everything
    auto num = [](std::string const& k) { return !k.empty() && std::all_of(k.begin(), k.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    if (!num(a.key) || !num(b.key)) return true;
    auto const& all = Enchant::mEnchants();
    auto        ia  = static_cast<std::size_t>(std::stoi(a.key));
    if (ia >= all.size() || !all[ia]) return true;
    return all[ia]->isCompatibleWith(static_cast<Enchant::Type>(std::stoi(b.key)));
}

/// After Bedrock computed the table's options: roll Create's enchantment for each.
void onOptions(EnchantingContainerManagerModel& model) {
    Player& player = model.mPlayer;
    if (player.isClientSide()) return;
    auto        uid  = uidOf(player);
    ItemStack   input;
    for (auto const& s : model.getItemCopies()) {
        if (items::isEmpty(s) || items::idOf(s) == "minecraft:lapis_lazuli") continue;
        input = s;
        break;
    }
    auto& r = rolled()[uid];
    r       = {};
    if (items::isEmpty(input)) return;
    r.itemId     = items::idOf(input);
    Item const* item = input.mItem.get();
    bool        book = isBook(r.itemId);
    int         value = item ? item->getEnchantValue() : 0;
    if (book && value <= 0) value = 1; // Java: a book's enchantability is 1
    auto const& options = model.mEnchantOptions.get();
    for (std::size_t i = 0; i < options.size(); ++i) {
        RolledOption o;
        o.vanilla = vanillaOf(options[i].mEnchants.get());
        auto [key, level] = roll(
            r.itemId,
            value,
            options[i].mCost,
            player.mEnchantmentSeed,
            static_cast<int>(i),
            [&](int c) { return vanillaCandidates(item, book, c); },
            vanillaCompatible
        );
        o.key   = key;
        o.level = level;
        r.options.push_back(std::move(o));
    }
}

/// An item got the enchantments of a table option: add the rolled level of Create's enchantment.
void onEnchanted(ItemStackBase& stack, ItemEnchants const& enchants) {
    if (!gEnchanting || gWriting) return;
    auto it = rolled().find(uidOf(*gEnchanting));
    if (it == rolled().end()) return;
    auto set = vanillaOf(enchants);
    for (auto const& o : it->second.options) {
        if (o.level <= 0 || o.vanilla != set) continue;
        gWriting = true;
        auto& s  = static_cast<ItemStack&>(stack);
        setEnchantLevel(s, o.key, std::max(o.level, enchantLevel(s, o.key)), gEnchanting);
        gWriting = false;
        log().info("equipment: {} enchanted {} with {} {}", gEnchanting->getRealName(), items::idOf(s), o.key, o.level);
        return;
    }
}

} // namespace

// ---- public ---------------------------------------------------------------------------------------------

int enchantLevel(ItemStack const& stack, std::string_view key) {
    if (items::isEmpty(stack)) return 0;
    auto j = items::getData(stack, kEnchantsData);
    if (!j.is_object()) return 0;
    auto it = j.find(std::string(key));
    return it != j.end() && it->is_number_integer() ? std::clamp(it->get<int>(), 0, 255) : 0;
}

void setEnchantLevel(ItemStack& stack, std::string_view key, int level, Player const* viewer) {
    auto j = items::getData(stack, kEnchantsData);
    if (!j.is_object()) j = Json::object();
    if (level > 0) j[std::string(key)] = level;
    else j.erase(std::string(key));
    items::setData(stack, kEnchantsData, j.empty() ? Json() : j);
    refreshLore(stack, viewer);
}

bool enchantApplies(std::string_view itemId, std::string_view key) {
    if (isBook(itemId)) return true;
    if (key == kCapacity) return isBacktank(itemId);
    if (key == kPotatoRecovery) return itemId == kPotatoCannon;
    return false;
}

std::pair<std::string, int> rollTableEnchant(std::string const& itemId, int enchantValue, int cost, int seed, int slot) {
    // without vanilla candidates (selftests): only Create's enchantment
    return roll(itemId, enchantValue, cost, seed, slot, {}, [](auto const&, auto const&) { return true; });
}

std::vector<std::pair<std::string, int>> tableRolls(Player const& player) {
    std::vector<std::pair<std::string, int>> out;
    auto                                     it = rolled().find(uidOf(player));
    if (it != rolled().end())
        for (auto const& o : it->second.options) out.emplace_back(o.key, o.level);
    return out;
}

void asTableEnchant(Player& player, std::function<void()> const& fn) {
    Player* prev = gEnchanting;
    gEnchanting  = &player;
    try {
        fn();
    } catch (...) {
        gEnchanting = prev;
        throw;
    }
    gEnchanting = prev;
}

} // namespace lv::feature::equipment

namespace eq    = lv::feature::equipment;
namespace items = lv::feature::common::items;

LL_TYPE_INSTANCE_HOOK(
    EquipmentEnchantOptionsHook,
    ll::memory::HookPriority::Normal,
    EnchantingContainerManagerModel,
    &EnchantingContainerManagerModel::recalculateOptions,
    void
) {
    origin();
    try {
        eq::onOptions(*this);
    } catch (std::exception const& e) {
        lv::log().error("equipment: enchanting options failed: {}", e.what());
    }
}

LL_TYPE_INSTANCE_HOOK(
    EquipmentEnchantCraftHook,
    ll::memory::HookPriority::Normal,
    CraftHandlerEnchant,
    &CraftHandlerEnchant::$_handleCraftAction,
    ItemStackNetResult,
    ItemStackRequestActionCraftBase const& action
) {
    Player* prev     = eq::gEnchanting;
    eq::gEnchanting  = &static_cast<Player&>(mPlayer);
    auto result      = origin(action);
    eq::gEnchanting  = prev;
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    EquipmentSaveEnchantsHook,
    ll::memory::HookPriority::Normal,
    ItemStackBase,
    &ItemStackBase::saveEnchantsToUserData,
    void,
    ItemEnchants const& enchants
) {
    origin(enchants);
    try {
        eq::onEnchanted(*this, enchants);
    } catch (std::exception const& e) {
        lv::log().error("equipment: enchanting failed: {}", e.what());
    }
}

// Java's anvil: Create's enchantments of the second item join the first (equal levels + 1, else the
// higher; a book's enchantments go on anything they apply to), at Create's anvil cost of 1 per level.
LL_STATIC_HOOK(
    EquipmentAnvilHook,
    ll::memory::HookPriority::Normal,
    &EnchantUtils::combineEnchantedItems,
    int,
    ItemStack const& first,
    ItemStack const& second,
    ItemStack&       out,
    bool             bookEnchant
) {
    int cost = origin(first, second, out, bookEnchant);
    try {
        if (items::isEmpty(out)) return cost;
        std::string outId = items::idOf(out);
        int         extra = 0;
        for (auto key : {eq::kCapacity, eq::kPotatoRecovery}) {
            int a = eq::enchantLevel(first, key), b = eq::enchantLevel(second, key);
            if (b <= 0 || !eq::enchantApplies(outId, key)) {
                if (a > 0 && eq::enchantLevel(out, key) != a) eq::setEnchantLevel(out, key, a);
                continue;
            }
            int level = a > 0 ? lv::core::equipment::mergeEnchantLevel(a, b) : b;
            eq::setEnchantLevel(out, key, level);
            extra += bookEnchant ? std::max(1, level / 2) : level;
        }
        return cost + extra;
    } catch (std::exception const& e) {
        lv::log().error("equipment: anvil merge failed: {}", e.what());
        return cost;
    }
}

// Java's grindstone removes every non-curse enchantment, Create's included.
LL_STATIC_HOOK(EquipmentGrindstoneHook, ll::memory::HookPriority::Normal, &EnchantUtils::removeEnchants, void, ItemStack& out) {
    origin(out);
    try {
        if (items::isEmpty(out) || !items::getData(out, eq::kEnchantsData).is_object()) return;
        for (auto key : {eq::kCapacity, eq::kPotatoRecovery}) eq::setEnchantLevel(out, key, 0);
    } catch (std::exception const& e) {
        lv::log().error("equipment: grindstone failed: {}", e.what());
    }
}

namespace lv::feature::equipment {
namespace {
class EnchantsModule {
public:
    void install() {
        EquipmentEnchantOptionsHook::hook();
        EquipmentEnchantCraftHook::hook();
        EquipmentSaveEnchantsHook::hook();
        EquipmentAnvilHook::hook();
        EquipmentGrindstoneHook::hook();
    }
    void uninstall() {
        EquipmentGrindstoneHook::unhook();
        EquipmentAnvilHook::unhook();
        EquipmentSaveEnchantsHook::unhook();
        EquipmentEnchantCraftHook::unhook();
        EquipmentEnchantOptionsHook::unhook();
        rolled().clear();
    }
};
} // namespace
} // namespace lv::feature::equipment

LV_REGISTER_MODULE(lv::feature::equipment::EnchantsModule);
