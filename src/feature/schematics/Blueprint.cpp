// Crafting blueprint (Create's BlueprintEntity / BlueprintItem / BlueprintMenu). Create's
// blueprint is a hanging entity of 1x1, 2x2 or 3x3 sections; here each section is a thin block
// (`levitate:placed_crafting_blueprint`, SPEC §8) and the group is placed and removed together.
// Each section holds a 3x3 recipe (ghost items, filters allowed) plus a display item (inferred
// from the recipe or set by hand) and a secondary display item. Right-click on a configured
// section crafts the recipe from the player's inventory (sneaking: up to a stack); right-click
// on an empty one (or with a wrench) opens the recipe screen; hitting a configured section clears it.

#include <algorithm>
#include <set>

#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/crafting/Recipe.h"
#include "mc/world/item/crafting/RecipeIngredient.h"
#include "mc/world/item/crafting/Recipes.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/schematics/Schematics.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::schematics {

namespace items   = common::items;
namespace screens = common::screens;

namespace {

constexpr char const* kScreen = "schematics.blueprint";
constexpr std::array<cs::Pos, 6> kDirs{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

struct Section {
    cs::Pos                   anchor; ///< position of section 0
    int                       size = 1, index = 0, face = 3, horiz = 2;
    std::array<ItemStack, 11> items;  ///< 0..8 recipe, 9 display, 10 secondary display
    bool                      inferred = false;
};

std::optional<Section> sectionAt(GlobalPos const& p) {
    auto const* d = dataIf(p);
    if (!d || !d->contains("blueprint")) return std::nullopt;
    auto const& b = (*d)["blueprint"];
    Section     s;
    auto        a = b.value("anchor", Json::array());
    if (a.is_array() && a.size() == 3) s.anchor = {a[0].get<int>(), a[1].get<int>(), a[2].get<int>()};
    s.size     = std::clamp(b.value("size", 1), 1, 3);
    s.index    = b.value("index", 0);
    s.face     = std::clamp(b.value("face", 3), 0, 5);
    s.horiz    = std::clamp(b.value("horiz", 2), 2, 5);
    s.inferred = b.value("inferred", false);
    auto it    = b.value("items", Json::array());
    for (std::size_t i = 0; i < 11 && i < it.size(); ++i) s.items[i] = items::fromJson(it[i]);
    return s;
}

void storeSection(GlobalPos const& p, Section const& s) {
    Json it = Json::array();
    for (auto const& st : s.items) it.push_back(items::toJson(st));
    data(p)["blueprint"] = Json{
        {"anchor",   {s.anchor.x, s.anchor.y, s.anchor.z}},
        {"size",     s.size                              },
        {"index",    s.index                             },
        {"face",     s.face                              },
        {"horiz",    s.horiz                             },
        {"inferred", s.inferred                          },
        {"items",    it                                  },
    };
}

std::vector<GlobalPos> groupOf(GlobalPos const& p, Section const& s) {
    std::vector<GlobalPos> out;
    // section 0 sits at `anchor`; cells are relative to the clicked position used at placement,
    // which is the centre for 3x3 and section 0 otherwise
    cs::Pos origin = s.anchor;
    if (s.size == 3) {
        auto cells0 = cs::blueprintCells({0, 0, 0}, s.face, s.horiz, 3);
        origin      = s.anchor - cells0[0];
    }
    for (auto const& c : cs::blueprintCells(origin, s.face, s.horiz, s.size)) out.push_back(gp(p.dim, c));
    return out;
}

// ---- recipes ------------------------------------------------------------------------------------------

struct Match {
    ItemStack                result;
    std::vector<std::string> remainders;
};

bool ingredientMatches(Recipe const& r, RecipeIngredient const& ing, ItemStack const& s) {
    bool ingEmpty = ing.isNull();
    if (items::isEmpty(s)) return ingEmpty;
    if (ingEmpty) return false;
    return r.itemValidForRecipe(ing, s);
}

std::optional<Match> findRecipe(std::array<ItemStack, 9> const& grid) {
    auto level = ll::service::getLevel();
    if (!level) return std::nullopt;
    // bounding box of the used cells
    int minX = 3, minY = 3, maxX = -1, maxY = -1, used = 0;
    for (int i = 0; i < 9; ++i) {
        if (items::isEmpty(grid[static_cast<std::size_t>(i)])) continue;
        ++used;
        minX = std::min(minX, i % 3), maxX = std::max(maxX, i % 3);
        minY = std::min(minY, i / 3), maxY = std::max(maxY, i / 3);
    }
    if (!used) return std::nullopt;
    int                  w = maxX - minX + 1, h = maxY - minY + 1;
    std::optional<Match> found;
    level->getRecipes().forEachRecipeFor(HashedString("crafting_table"), [&](Recipe const& r) {
        if (found || r.isMultiRecipe()) return;
        auto const& results = r.getResultItems();
        if (results.empty() || results.front().isNull()) return;
        bool ok = false;
        if (r.isShapeless()) {
            auto const& ings = *r.mMyIngredients;
            if (static_cast<int>(ings.size()) != used) return;
            std::vector<bool> taken(ings.size(), false);
            ok = true;
            for (int i = 0; i < 9 && ok; ++i) {
                auto const& s = grid[static_cast<std::size_t>(i)];
                if (items::isEmpty(s)) continue;
                bool hit = false;
                for (std::size_t k = 0; k < ings.size() && !hit; ++k) {
                    if (taken[k] || !ingredientMatches(r, ings[k], s)) continue;
                    taken[k] = hit = true;
                }
                ok = hit;
            }
        } else {
            int rw = r.mWidth, rh = r.mHeight;
            if (rw != w || rh != h) return;
            for (bool mirror : {false, true}) {
                bool all = true;
                for (int y = 0; y < h && all; ++y) {
                    for (int x = 0; x < w && all; ++x) {
                        auto const& s = grid[static_cast<std::size_t>((minY + y) * 3 + minX + x)];
                        all           = ingredientMatches(r, r.getIngredient(mirror ? w - 1 - x : x, y), s);
                    }
                }
                if (all) {
                    ok = true;
                    break;
                }
            }
        }
        if (!ok) return;
        auto const& res = results.front();
        auto        st  = items::make(res.getTypeName(), res.mCount, res.getAuxValue());
        if (!st) return;
        Match m;
        m.result = *st;
        for (auto const& s : grid) {
            auto id = items::idOf(s);
            if (id.ends_with("_bucket") && id != "minecraft:bucket") m.remainders.push_back("minecraft:bucket");
            else if (id == "minecraft:honey_bottle" || id == "minecraft:potion") m.remainders.push_back("minecraft:glass_bottle");
        }
        found = std::move(m);
    });
    return found;
}

void inferDisplay(Section& s) {
    std::array<ItemStack, 9> grid;
    for (int i = 0; i < 9; ++i) grid[static_cast<std::size_t>(i)] = s.items[static_cast<std::size_t>(i)];
    // filters in the grid: no recipe inference (Create infers only from plain items)
    for (auto const& g : grid)
        if (common::filter::isFilterItem(g)) return;
    auto m = findRecipe(grid);
    if (m) {
        if (items::isEmpty(s.items[9]) || s.inferred) {
            s.items[9] = m->result;
            s.inferred = true;
        }
    } else if (s.inferred) {
        s.items[9] = items::empty();
        s.inferred = false;
    }
}

bool isBlueprint(GlobalPos const& p) { return idAt(p) == kBlueprintBlock; }

std::optional<GlobalPos> sectionOf(Player& player, Json const& ctx) {
    auto p = posFrom(ctx.value("pos", Json()));
    if (!p || !isBlueprint(*p) || !inReach(player, *p)) return std::nullopt;
    return p;
}

Json screenData(Section const& s) {
    Json it = Json::array();
    for (auto const& st : s.items) it.push_back(items::toJson(st));
    return {{"items", it}, {"inferred", s.inferred}};
}

void blueprintForm(Player& player, Json const& ctx) {
    auto p = sectionOf(player, ctx);
    if (!p) return;
    auto s = sectionAt(*p);
    if (!s) return;
    std::vector<ItemStack> inv;
    auto&                  container = player.getInventory();
    for (int i = 0; i < container.getContainerSize(); ++i) {
        auto const& st = container.getItem(i);
        if (items::isEmpty(st)) continue;
        if (std::none_of(inv.begin(), inv.end(), [&](ItemStack const& o) { return items::sameItemSameData(o, st); })) inv.push_back(items::withCount(st, 1));
    }
    auto keep = trFor(player, "lv.common.form.keep", "(keep)");
    auto none = trFor(player, "lv.common.form.empty", "(empty)");
    ll::form::CustomForm form(trFor(player, "item.create.crafting_blueprint", "Crafting Blueprint"));
    form.appendLabel(trFor(player, "lv.schematics.blueprint.form.hint", "Pick an ingredient for each slot of the 3x3 grid (row by row). Items in your inventory are listed."));
    for (int i = 0; i < 11; ++i) {
        auto const&              cur = s->items[static_cast<std::size_t>(i)];
        std::vector<std::string> options{keep + " " + (items::isEmpty(cur) ? none : items::displayName(cur)), none};
        for (auto const& st : inv) options.push_back(items::displayName(st));
        std::string label = i < 9 ? trFor(player, "create.crafting_blueprint.crafting_slot", "Ingredient Slot") + " " + std::to_string(i + 1)
                          : i == 9 ? trFor(player, "create.crafting_blueprint.display_slot", "Display Slot")
                                   : trFor(player, "create.crafting_blueprint.secondary_display_slot", "Secondary Display Slot");
        form.appendDropdown("slot" + std::to_string(i), label, options, 0);
    }
    auto pos = *p;
    form.sendTo(player, [pos, inv](Player& pl, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r || !isBlueprint(pos) || !inReach(pl, pos)) return;
        for (int i = 0; i < 11; ++i) {
            auto it = r->find("slot" + std::to_string(i));
            if (it == r->end()) continue;
            auto const* v = std::get_if<uint64>(&it->second);
            if (!v || *v == 0) continue;
            setBlueprintSlot(pos, i, *v == 1 ? items::empty() : inv[static_cast<std::size_t>(*v - 2)]);
        }
        log().info("crafting blueprint at {} {} {}: {} set the recipe (form)", pos.x, pos.y, pos.z, pl.getRealName());
    });
}

bool gSilentRemoval = false;

} // namespace

bool blueprintSilent() { return gSilentRemoval; }

std::string debugRecipes(std::string const& result) {
    auto level = ll::service::getLevel();
    if (!level) return "no level";
    int         total = 0;
    std::string out;
    level->getRecipes().forEachRecipeFor(HashedString("crafting_table"), [&](Recipe const& r) {
        ++total;
        auto const& res = r.getResultItems();
        if (res.empty() || res.front().getTypeName() != result) return;
        out += " [" + std::to_string(r.mWidth) + "x" + std::to_string(r.mHeight) + (r.isShapeless() ? " shapeless" : "") + (r.isMultiRecipe() ? " multi" : "")
             + (r.hasDataDrivenResult() ? " dd" : "") + ":";
        for (auto const& ing : *r.mMyIngredients) out += " " + ing.getSerializedNameAndAux(false);
        out += "]";
    });
    return std::to_string(total) + " recipes;" + out;
}

void setBlueprintSlot(GlobalPos const& section, int slot, ItemStack const& stack) {
    auto s = sectionAt(section);
    if (!s || slot < 0 || slot > 10) return;
    s->items[static_cast<std::size_t>(slot)] = items::withCount(stack, items::isEmpty(stack) ? 0 : 1);
    if (slot < 9) inferDisplay(*s);
    else if (slot == 9) s->inferred = false;
    storeSection(section, *s);
}

bool placeBlueprint(Player& player, GlobalPos const& support, int face) {
    if (face < 0 || face > 5) return false;
    cs::Pos at    = cp(support) + kDirs[static_cast<std::size_t>(face)];
    int     horiz = game::playerFacing(player); // Java 2D facing: 0 S, 1 W, 2 N, 3 E
    static constexpr std::array<int, 4> kToBedrock{3, 4, 2, 5};
    horiz = kToBedrock[static_cast<std::size_t>(horiz & 3)];
    for (int size = 3; size > 0; --size) {
        auto cells = cs::blueprintCells(at, face, horiz, size);
        bool fits  = true;
        for (auto const& c : cells) {
            GlobalPos g = gp(support.dim, c);
            GlobalPos behind = gp(support.dim, c - kDirs[static_cast<std::size_t>(face)]);
            auto const* b    = blockAt(behind);
            if (!replaceable(g) || !b || !solid(*b)) {
                fits = false;
                break;
            }
        }
        if (!fits) continue;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            GlobalPos g = gp(support.dim, cells[i]);
            placeId(g, kBlueprintBlock, {{kFacing, face}});
            Section s;
            s.anchor = cells[0];
            s.size   = size;
            s.index  = static_cast<int>(i);
            s.face   = face;
            s.horiz  = horiz;
            storeSection(g, s);
        }
        sound(gp(support.dim, at), "hit.wood", 1.f, 1.f);
        log().info("crafting blueprint placed at {} {} {} ({}x{}) by {}", at.x, at.y, at.z, size, size, player.getRealName());
        return true;
    }
    return false;
}

void blueprintRemoved(GlobalPos const& p, Json const& doc, bool drop) {
    if (gSilentRemoval || !doc.contains("blueprint")) return;
    auto const& b = doc["blueprint"];
    Section     s;
    auto        a = b.value("anchor", Json::array());
    if (a.is_array() && a.size() == 3) s.anchor = {a[0].get<int>(), a[1].get<int>(), a[2].get<int>()};
    s.size  = std::clamp(b.value("size", 1), 1, 3);
    s.face  = std::clamp(b.value("face", 3), 0, 5);
    s.horiz = std::clamp(b.value("horiz", 2), 2, 5);
    gSilentRemoval = true;
    for (auto const& g : groupOf(p, s)) {
        if (g == p) continue;
        if (isBlueprint(g)) clearAt(g);
    }
    gSilentRemoval = false;
    if (drop)
        if (auto item = makeItem(kBlueprintItem)) dropAt(p, *item);
    log().info("crafting blueprint at {} {} {} removed", p.x, p.y, p.z);
}

int blueprintCraft(Player& player, GlobalPos const& p, bool sneaking) {
    auto s = sectionAt(p);
    if (!s || items::isEmpty(s->items[9])) return 0;
    auto& inv     = player.getInventory();
    int   crafted = 0;
    bool  first   = true;
    std::optional<std::string> recipeResult;
    do {
        std::array<ItemStack, 9> grid;
        bool                     success = true;
        for (int i = 0; i < 9 && success; ++i) {
            auto const& want = s->items[static_cast<std::size_t>(i)];
            if (items::isEmpty(want)) continue;
            bool hit = false;
            for (int slot = 0; slot < inv.getContainerSize() && !hit; ++slot) {
                auto st = inv.getItem(slot);
                if (items::isEmpty(st) || !common::filter::matches(want, st)) continue;
                grid[static_cast<std::size_t>(i)] = items::withCount(st, 1);
                inv.setItem(slot, items::withCount(st, items::countOf(st) - 1));
                hit = true;
            }
            success = hit;
        }
        std::optional<Match> m;
        if (success) {
            m = findRecipe(grid);
            if (!m || items::isEmpty(m->result) || items::countOf(m->result) + crafted > 64) success = false;
            else if (recipeResult && *recipeResult != items::idOf(m->result)) success = false;
        }
        if (!success) {
            // put the taken items back
            for (int i = 0; i < 9; ++i) {
                auto const& g = grid[static_cast<std::size_t>(i)];
                if (items::isEmpty(g)) continue;
                ItemStack back = g;
                if (!player.add(back) && !items::isEmpty(back)) player.drop(back, false);
            }
            break;
        }
        recipeResult = items::idOf(m->result);
        crafted += items::countOf(m->result);
        if (first) sound(gp(p.dim, cp(p)), "random.pop", 0.2f, 1.f);
        give(player, m->result);
        for (auto const& r : m->remainders)
            if (auto st = makeItem(r)) give(player, *st);
        first = false;
    } while (sneaking);
    player.sendInventory(false);
    if (crafted > 0) log().info("crafting blueprint at {} {} {}: {} crafted {} {}", p.x, p.y, p.z, player.getRealName(), crafted, recipeResult.value_or(""));
    return crafted;
}

void blueprintUse(Player& player, GlobalPos const& p, bool holdingWrench) {
    auto s = sectionAt(p);
    if (!s) return;
    if (!holdingWrench && !items::isEmpty(s->items[9])) {
        blueprintCraft(player, p, player.isSneaking());
        return;
    }
    screens::open(player, kScreen, {{"pos", posJson(p)}});
}

bool blueprintAttack(Player& player, GlobalPos const& p) {
    auto s = sectionAt(p);
    if (!s || items::isEmpty(s->items[9])) return false;
    for (auto& st : s->items) st = items::empty();
    s->inferred = false;
    storeSection(p, *s);
    log().info("crafting blueprint at {} {} {}: {} cleared a section", p.x, p.y, p.z, player.getRealName());
    return true;
}

namespace detail {

void registerBlueprint() {
    screens::registerType({
        .id   = kScreen,
        .data = [](Player& pl, Json const& ctx) -> std::optional<Json> {
            auto p = sectionOf(pl, ctx);
            if (!p) return std::nullopt;
            auto s = sectionAt(*p);
            if (!s) return std::nullopt;
            return screenData(*s);
        },
        .action =
            [](Player& pl, Json const& ctx, Json const& action) {
                auto p = sectionOf(pl, ctx);
                if (!p) return;
                auto op   = action.value("op", std::string{});
                int  slot = action.value("index", -1);
                if (op == "set") setBlueprintSlot(*p, slot, items::fromJson(action.value("item", Json())));
                else if (op == "clear") setBlueprintSlot(*p, slot, items::empty());
            },
        .form = blueprintForm,
    });
}

} // namespace detail

} // namespace lv::feature::schematics
