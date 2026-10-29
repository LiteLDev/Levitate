#include "feature/common/Filter.h"

#include <algorithm>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemTag.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantmentInstance.h"
#include "mc/world/item/enchanting/ItemEnchants.h"
#include "mc/world/item/crafting/Recipes.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/ComposterBlock.h"
#include "mc/world/level/block/actor/FurnaceBlockActor.h"

#include "core/common/Transfer.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "mod/I18n.h"

namespace lv::feature::common::filter {

using core::common::AttributeMode;
using core::common::kListFilterSlots;

bool isListFilter(ItemStack const& stack) { return items::idOf(stack) == kListFilterId; }
bool isAttributeFilter(ItemStack const& stack) { return items::idOf(stack) == kAttributeFilterId; }
namespace {
std::unordered_map<std::string, Matcher>& matchers() {
    static std::unordered_map<std::string, Matcher> m;
    return m;
}
Matcher const* matcherFor(ItemStack const& filter) {
    if (matchers().empty() || items::isEmpty(filter)) return nullptr;
    auto it = matchers().find(items::idOf(filter));
    return it == matchers().end() ? nullptr : &it->second;
}
} // namespace

void registerMatcher(std::string itemId, Matcher fn) { matchers()[std::move(itemId)] = std::move(fn); }

bool isFilterItem(ItemStack const& stack) {
    return isListFilter(stack) || isAttributeFilter(stack) || matcherFor(stack) != nullptr;
}

// ---- contents ----------------------------------------------------------------------------------

ListFilter readList(ItemStack const& filter) {
    ListFilter out;
    out.items.assign(kListFilterSlots, items::empty());
    auto j = items::getData(filter, "filter");
    if (!j.is_object()) return out;
    out.blacklist   = j.value("blacklist", false);
    out.respectData = j.value("respect_data", false);
    if (auto it = j.find("items"); it != j.end() && it->is_array()) {
        for (std::size_t i = 0; i < it->size() && i < out.items.size(); ++i) out.items[i] = items::fromJson((*it)[i]);
    }
    return out;
}

void writeList(ItemStack& filter, ListFilter const& data) {
    Json list = Json::array();
    bool any  = false;
    for (int i = 0; i < kListFilterSlots; ++i) {
        auto const& s = i < static_cast<int>(data.items.size()) ? data.items[static_cast<std::size_t>(i)] : items::empty();
        list.push_back(items::isEmpty(s) ? Json(nullptr) : items::toJson(items::withCount(s, 1)));
        any |= !items::isEmpty(s);
    }
    if (!any && !data.blacklist && !data.respectData) {
        items::setData(filter, "filter", nullptr);
    } else {
        items::setData(filter, "filter", {{"items", list}, {"blacklist", data.blacklist}, {"respect_data", data.respectData}});
    }
    updateLore(filter);
}

AttributeFilter readAttributes(ItemStack const& filter) {
    AttributeFilter out;
    auto            j = items::getData(filter, "attribute_filter");
    if (!j.is_object()) return out;
    out.mode = core::common::attributeModeFromName(j.value("mode", std::string{}));
    if (auto it = j.find("attributes"); it != j.end() && it->is_array()) {
        for (auto const& a : *it) {
            if (!a.is_object()) continue;
            out.attributes.push_back({a.value("type", std::string{}), a.value("value", std::string{}), a.value("inverted", false)});
        }
    }
    return out;
}

void writeAttributes(ItemStack& filter, AttributeFilter const& data) {
    if (data.attributes.empty() && data.mode == AttributeMode::WhitelistAny) {
        items::setData(filter, "attribute_filter", nullptr);
    } else {
        Json list = Json::array();
        for (auto const& a : data.attributes) list.push_back({{"type", a.type}, {"value", a.value}, {"inverted", a.inverted}});
        items::setData(filter, "attribute_filter", {{"mode", core::common::attributeModeName(data.mode)}, {"attributes", list}});
    }
    updateLore(filter);
}

// ---- attributes --------------------------------------------------------------------------------

namespace {
std::vector<AttributeType>& attributeTypes();

Item const* itemOf(ItemStack const& s) { return s.mItem.get(); }

std::string itemNamespace(ItemStack const& s) {
    auto id  = items::idOf(s);
    auto pos = id.find(':');
    return pos == std::string::npos ? std::string("minecraft") : id.substr(0, pos);
}

bool furnaceResult(ItemStack const& s, char const* tag) {
    auto level = ll::service::getLevel();
    if (!level || items::isEmpty(s)) return false;
    auto out = level->getRecipes().getFurnaceRecipeResult(s, HashedString(tag));
    return !out.isNull();
}

std::vector<std::string> one(bool applies) { return applies ? std::vector<std::string>{""} : std::vector<std::string>{}; }

void simple(std::string id, std::function<bool(ItemStack const&)> fn) {
    AttributeType t;
    t.id        = std::move(id);
    t.valuesFor = [fn](ItemStack const& s) { return one(fn(s)); };
    t.test      = [fn](ItemStack const& s, std::string const&) { return fn(s); };
    attributeTypes().push_back(std::move(t));
}

void valued(std::string id, std::function<std::vector<std::string>(ItemStack const&)> values) {
    AttributeType t;
    t.id        = std::move(id);
    t.valuesFor = values;
    t.test      = [values](ItemStack const& s, std::string const& v) {
        auto all = values(s);
        return std::find(all.begin(), all.end(), v) != all.end();
    };
    attributeTypes().push_back(std::move(t));
}

Enchant const* enchantOf(Enchant::Type type) {
    auto& all = Enchant::mEnchants();
    auto  i   = static_cast<std::size_t>(type);
    return i < all.size() ? all[i].get() : nullptr;
}

bool hasRecipe(char const* type, ItemStack const& s) { return !recipes::withFirstIngredient(type, s).empty(); }

std::string userString(ItemStack const& s, char const* key) {
    auto const* tag = items::userData(s);
    if (!tag || !tag->contains(key, Tag::Type::String)) return {};
    return std::string((*tag)[key].get<StringTag>());
}

/// Create's built-in attribute types (AllItemAttributeTypes), in Create's order.
void registerBuiltins() {
    simple("placeable", [](ItemStack const& s) { return s.mBlock != nullptr || (itemOf(s) && itemOf(s)->isBlockPlanterItem()); });
    simple("consumable", [](ItemStack const& s) { return itemOf(s) && itemOf(s)->isFood(); });
    simple("fluid_container", [](ItemStack const& s) {
        auto id = items::idOf(s);
        return !fluids::fluidInItem(s).empty() || id == "minecraft:bucket" || id == "minecraft:glass_bottle";
    });
    simple("enchanted", [](ItemStack const& s) { return s.isEnchanted(); });
    simple("max_enchanted", [](ItemStack const& s) {
        if (!s.isEnchanted()) return false;
        auto enchants = s.constructItemEnchantsFromUserData();
        bool any      = false;
        for (auto const& e : enchants.getAllEnchants()) {
            any = true;
            auto const* en = enchantOf(e.mEnchantType);
            if (en && e.mLevel < en->getMaxLevel()) return false;
        }
        return any;
    });
    simple("renamed", [](ItemStack const& s) { return !s.getCustomName().empty(); });
    simple("damaged", [](ItemStack const& s) { return s.isDamaged(); });
    simple("badly_damaged", [](ItemStack const& s) {
        auto const* item = itemOf(s);
        int         max  = item ? item->getMaxDamage() : 0;
        return s.isDamaged() && max > 0 && static_cast<float>(s.getDamageValue()) / static_cast<float>(max) > 0.75f;
    });
    simple("not_stackable", [](ItemStack const& s) { return s.getMaxStackSize() <= 1; });
    simple("equipable", [](ItemStack const& s) { return s.isArmorItem() || s.isHumanoidWearableItem(); });
    simple("furnace_fuel", [](ItemStack const& s) { return FurnaceBlockActor::getBurnDuration(s, 200.0f) > 0; });
    simple("washable", [](ItemStack const& s) { return hasRecipe("splashing", s); });
    simple("hauntable", [](ItemStack const& s) { return hasRecipe("haunting", s); });
    simple("crushable", [](ItemStack const& s) { return hasRecipe("crushing", s) || hasRecipe("milling", s); });
    simple("smeltable", [](ItemStack const& s) { return furnaceResult(s, "furnace"); });
    simple("smokable", [](ItemStack const& s) { return furnaceResult(s, "smoker"); });
    simple("blastable", [](ItemStack const& s) { return furnaceResult(s, "blast_furnace"); });
    simple("compostable", [](ItemStack const& s) { return ComposterBlock::getFillChance(s) > 0; });
    valued("in_tag", [](ItemStack const& s) {
        std::vector<std::string> out;
        if (auto const* item = itemOf(s)) {
            for (auto const& t : item->mTags.get()) out.push_back(t.getString());
        }
        return out;
    });
    valued("in_item_group", [](ItemStack const& s) {
        auto const* item = itemOf(s);
        if (!item || item->mCreativeGroup->empty()) return std::vector<std::string>{};
        return std::vector<std::string>{item->mCreativeGroup.get()};
    });
    valued("added_by", [](ItemStack const& s) {
        return items::isEmpty(s) ? std::vector<std::string>{} : std::vector<std::string>{itemNamespace(s)};
    });
    valued("has_enchant", [](ItemStack const& s) {
        std::vector<std::string> out;
        if (!s.isEnchanted()) return out;
        for (auto const& e : s.constructItemEnchantsFromUserData().getAllEnchants()) {
            if (auto const* en = enchantOf(e.mEnchantType)) out.push_back(en->mStringId->getString());
        }
        return out;
    });
    valued("shulker_fill_level", [](ItemStack const& s) {
        if (items::idOf(s).find("shulker_box") == std::string::npos) return std::vector<std::string>{};
        auto const* tag   = items::userData(s);
        int         count = 0, full = 0;
        if (tag && tag->contains("Items", Tag::Type::List)) {
            for (auto const& e : (*tag)["Items"].get<ListTag>()) {
                if (!e.is_object()) continue;
                auto stack = ItemStack::fromTag(e.get<CompoundTag>());
                if (items::isEmpty(stack)) continue;
                ++count;
                if (items::countOf(stack) >= stack.getMaxStackSize()) ++full;
            }
        }
        if (count == 0) return std::vector<std::string>{"empty"};
        return std::vector<std::string>{full >= 27 ? "full" : "partial"};
    });
    valued("has_fluid", [](ItemStack const& s) {
        auto f = fluids::fluidInItem(s);
        return f.empty() ? std::vector<std::string>{} : std::vector<std::string>{f.fluid};
    });
    valued("has_name", [](ItemStack const& s) {
        auto name = s.getCustomName();
        return name.empty() ? std::vector<std::string>{} : std::vector<std::string>{name};
    });
    valued("book_author", [](ItemStack const& s) {
        auto a = userString(s, "author");
        return a.empty() ? std::vector<std::string>{} : std::vector<std::string>{a};
    });
    valued("book_copy", [](ItemStack const& s) {
        if (items::idOf(s) != "minecraft:written_book") return std::vector<std::string>{};
        auto const* tag = items::userData(s);
        int         gen = tag && tag->contains("generation", Tag::Type::Int) ? (*tag)["generation"].get<IntTag>().data : 0;
        static char const* names[] = {"original", "copy_of_original", "copy_of_copy", "tattered"};
        return std::vector<std::string>{names[std::clamp(gen, 0, 3)]};
    });
}

std::vector<AttributeType>& attributeTypes() {
    static std::vector<AttributeType> v;
    return v;
}

std::vector<AttributeType>& types() {
    static bool init = [] {
        registerBuiltins();
        return true;
    }();
    (void)init;
    return attributeTypes();
}

AttributeType const* typeOf(std::string const& id) {
    for (auto const& t : types())
        if (t.id == id) return &t;
    return nullptr;
}
} // namespace

void registerAttribute(AttributeType type) {
    auto& all = types();
    for (auto& t : all) {
        if (t.id == type.id) {
            t = std::move(type);
            return;
        }
    }
    all.push_back(std::move(type));
}

std::vector<AttributeEntry> attributesOf(ItemStack const& stack) {
    std::vector<AttributeEntry> out;
    if (items::isEmpty(stack)) return out;
    for (auto const& t : types()) {
        for (auto& v : t.valuesFor(stack)) out.push_back({t.id, std::move(v), false});
    }
    return out;
}

bool attributeApplies(AttributeEntry const& entry, ItemStack const& stack) {
    auto const* t = typeOf(entry.type);
    return t && !items::isEmpty(stack) && t->test(stack, entry.value);
}

std::string describe(AttributeEntry const& entry, std::string const& locale) {
    static std::unordered_map<std::string, std::pair<char const*, char const*>> const english{
        {"placeable",          {"is placeable", "is not placeable"}                                     },
        {"consumable",         {"can be eaten", "cannot be eaten"}                                      },
        {"fluid_container",    {"can store fluids", "can't store fluids"}                               },
        {"enchanted",          {"is enchanted", "is unenchanted"}                                       },
        {"max_enchanted",      {"is enchanted at max level", "is not enchanted at max level"}           },
        {"renamed",            {"has a custom name", "does not have a custom name"}                     },
        {"damaged",            {"is damaged", "is not damaged"}                                         },
        {"badly_damaged",      {"is heavily damaged", "is not heavily damaged"}                         },
        {"not_stackable",      {"cannot stack", "can stack"}                                            },
        {"equipable",          {"can be equipped", "cannot be equipped"}                                },
        {"furnace_fuel",       {"is furnace fuel", "is not furnace fuel"}                               },
        {"washable",           {"can be Washed", "cannot be Washed"}                                    },
        {"hauntable",          {"can be Haunted", "cannot be Haunted"}                                  },
        {"crushable",          {"can be Crushed", "cannot be Crushed"}                                  },
        {"smeltable",          {"can be Smelted", "cannot be Smelted"}                                  },
        {"smokable",           {"can be Smoked", "cannot be Smoked"}                                    },
        {"blastable",          {"is smeltable in Blast Furnace", "is not smeltable in Blast Furnace"}   },
        {"compostable",        {"can be composted", "cannot be composted"}                              },
        {"in_tag",             {"is tagged {0}", "is not tagged {0}"}                                   },
        {"in_item_group",      {"is in group '{0}'", "is not in group '{0}'"}                           },
        {"added_by",           {"was added by {0}", "was not added by {0}"}                             },
        {"has_enchant",        {"is enchanted with {0}", "is not enchanted with {0}"}                   },
        {"shulker_fill_level", {"is {0} shulker box", "is not {0} shulker box"}                         },
        {"has_fluid",          {"contains {0}", "does not contain {0}"}                                 },
        {"has_name",           {"has custom name {0}", "does not have custom name {0}"}                 },
        {"book_author",        {"was authored by {0}", "was not authored by {0}"}                       },
        {"book_copy",          {"is {0}", "is not {0}"}                                                 },
    };
    std::string key      = "create.item_attributes." + entry.type + (entry.inverted ? ".inverted" : "");
    std::string fallback = entry.type;
    if (auto it = english.find(entry.type); it != english.end()) fallback = entry.inverted ? it->second.second : it->second.first;
    std::string value = entry.value;
    if (entry.type == "shulker_fill_level" || entry.type == "book_copy") {
        value = std::string(translate("create.item_attributes." + entry.type + "." + entry.value, entry.value, locale));
    }
    return core::formatMessageArgs(translate(key, fallback, locale), value);
}

// ---- matching ----------------------------------------------------------------------------------

namespace {
bool testAttributes(AttributeFilter const& data, ItemStack const& stack) {
    std::vector<core::common::AttributeTest> tests;
    tests.reserve(data.attributes.size());
    for (auto const& a : data.attributes) tests.push_back({attributeApplies(a, stack), a.inverted});
    return core::common::combineAttributeTests(tests, data.mode);
}
} // namespace

bool matches(ItemStack const& filter, ItemStack const& stack, bool matchData) {
    if (items::isEmpty(filter)) return true;
    if (auto const* m = matcherFor(filter)) return (*m)(filter, stack, matchData);
    if (isListFilter(filter)) {
        auto data = readList(filter);
        std::vector<ItemStack const*> entries;
        for (auto const& s : data.items)
            if (!items::isEmpty(s)) entries.push_back(&s);
        if (!entries.empty()) {
            return core::common::combineListFilter(
                entries.size(),
                [&](std::size_t i) { return matches(*entries[i], stack, data.respectData); },
                data.blacklist
            );
        }
    } else if (isAttributeFilter(filter)) {
        auto data = readAttributes(filter);
        if (!data.attributes.empty()) return testAttributes(data, stack);
    }
    return matchData ? items::sameItemSameData(filter, stack) : items::sameItem(filter, stack);
}

bool matchesFluid(ItemStack const& filter, fluids::FluidStack const& fluid) {
    if (items::isEmpty(filter)) return true;
    if (fluid.empty()) return false;
    if (isListFilter(filter)) {
        auto data = readList(filter);
        std::vector<ItemStack const*> entries;
        for (auto const& s : data.items)
            if (!items::isEmpty(s)) entries.push_back(&s);
        if (entries.empty()) return false;
        return core::common::combineListFilter(
            entries.size(),
            [&](std::size_t i) { return matchesFluid(*entries[i], fluid); },
            data.blacklist
        );
    }
    auto contained = fluids::fluidInItem(filter);
    return !contained.empty() && contained.fluid == fluid.fluid;
}

items::ItemPredicate predicate(ItemStack const& filter, bool matchData) {
    if (items::isEmpty(filter)) return items::any();
    if (auto const* m = matcherFor(filter)) {
        auto held = std::make_shared<ItemStack>(filter); // server thread
        return [fn = *m, held, matchData](ItemStack const& stack) { return fn(*held, stack, matchData); };
    }
    if (isListFilter(filter)) {
        auto data = std::make_shared<ListFilter>(readList(filter));
        std::erase_if(data->items, [](ItemStack const& s) { return items::isEmpty(s); });
        if (!data->items.empty()) {
            return [data](ItemStack const& stack) {
                return core::common::combineListFilter(
                    data->items.size(),
                    [&](std::size_t i) { return matches(data->items[i], stack, data->respectData); },
                    data->blacklist
                );
            };
        }
    } else if (isAttributeFilter(filter)) {
        auto data = std::make_shared<AttributeFilter>(readAttributes(filter));
        if (!data->attributes.empty()) return [data](ItemStack const& stack) { return testAttributes(*data, stack); };
    }
    auto copy = std::make_shared<ItemStack>(filter);
    return [copy, matchData](ItemStack const& stack) {
        return matchData ? items::sameItemSameData(*copy, stack) : items::sameItem(*copy, stack);
    };
}

// ---- tooltip -----------------------------------------------------------------------------------

void updateLore(ItemStack& filter) {
    std::vector<std::string> lines;
    if (isListFilter(filter)) {
        auto data = readList(filter);
        int  shown = 0, total = 0;
        for (auto const& s : data.items) total += items::isEmpty(s) ? 0 : 1;
        if (total > 0 || data.blacklist) {
            lines.push_back(
                "§6"
                + std::string(data.blacklist ? translate("create.gui.filter.deny_list", "Deny-List") : translate("create.gui.filter.allow_list", "Allow-List"))
            );
            for (auto const& s : data.items) {
                if (items::isEmpty(s)) continue;
                if (shown++ >= 3) {
                    lines.push_back("§7- ...");
                    break;
                }
                lines.push_back("§7- §f" + items::displayName(s));
            }
        }
    } else if (isAttributeFilter(filter)) {
        auto data = readAttributes(filter);
        if (!data.attributes.empty()) {
            char const* key = data.mode == AttributeMode::WhitelistAll ? "create.gui.attribute_filter.allow_list_conjunctive"
                            : data.mode == AttributeMode::Blacklist  ? "create.gui.attribute_filter.deny_list"
                                                                     : "create.gui.attribute_filter.allow_list_disjunctive";
            char const* fb  = data.mode == AttributeMode::WhitelistAll ? "Allow-List (All)"
                            : data.mode == AttributeMode::Blacklist  ? "Deny-List"
                                                                     : "Allow-List (Any)";
            lines.push_back("§6" + std::string(translate(key, fb)));
            int shown = 0;
            for (auto const& a : data.attributes) {
                if (shown++ >= 3) {
                    lines.push_back("§7- ...");
                    break;
                }
                lines.push_back("§7- §f" + describe(a));
            }
        }
    }
    if (lines.empty()) filter.clearCustomLore();
    else filter.setCustomLore(lines);
}

} // namespace lv::feature::common::filter
