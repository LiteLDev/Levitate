#include "feature/common/Recipes.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemTag.h"

#include "feature/common/Items.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Main.h"

namespace lv::feature::common::recipes {

namespace fs = std::filesystem;

namespace {
struct Cache {
    std::map<std::string, std::vector<ProcessingRecipe>, std::less<>>          types;
    std::map<std::string, std::vector<std::string>, std::less<>>               tags; ///< raw entries
    bool                                                                       tagsLoaded = false;
};
Cache& cache() {
    static Cache c;
    static bool  hooked = [] {
        config::onReload([] { reload(); });
        return true;
    }();
    (void)hooked;
    return c;
}

fs::path recipeDir() { return Mod::getInstance().getSelf().getModDir() / "recipes"; }

std::optional<nlohmann::json> readJson(fs::path const& p) {
    std::ifstream in(p);
    if (!in) return std::nullopt;
    auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

void loadTags() {
    auto& c = cache();
    if (c.tagsLoaded) return;
    c.tagsLoaded = true;
    std::error_code ec;
    for (auto const& e : fs::directory_iterator(recipeDir() / "tags", ec)) {
        if (e.path().extension() != ".json") continue;
        auto j = readJson(e.path());
        if (!j || !j->is_object()) {
            log().warn("recipe tags {} are unreadable", e.path().string());
            continue;
        }
        for (auto& [tag, values] : j->items()) {
            if (!values.is_array()) continue;
            auto& list = c.tags[core::common::mapId(tag)];
            for (auto const& v : values)
                if (v.is_string()) list.push_back(core::common::mapId(v.get<std::string>()));
        }
    }
}

void collectTag(std::string const& tag, std::set<std::string>& out, std::set<std::string>& seen) {
    if (!seen.insert(tag).second) return;
    auto& c  = cache();
    auto  it = c.tags.find(tag);
    if (it == c.tags.end()) return;
    for (auto const& v : it->second) {
        if (v.starts_with('#')) collectTag(v.substr(1), out, seen);
        else out.insert(v);
    }
}

std::vector<ProcessingRecipe> loadDir(std::string const& type) {
    std::vector<ProcessingRecipe> out;
    std::error_code               ec;
    auto                          dir = recipeDir() / type;
    for (auto const& e : fs::recursive_directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        auto rel = fs::relative(e.path(), dir, ec).replace_extension().generic_string();
        auto id  = "levitate:" + type + "/" + rel;
        auto doc = readJson(e.path());
        if (!doc) {
            log().warn("recipe {} is not valid JSON (skipped)", id);
            continue;
        }
        std::string err;
        if (auto r = core::common::parseRecipe(*doc, type, id, &err)) out.push_back(std::move(*r));
        else log().warn("recipe {} skipped: {}", id, err);
    }
    std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.id < b.id; });
    return out;
}
} // namespace

std::vector<ProcessingRecipe> const& ofType(std::string const& type) {
    auto& c = cache();
    if (auto it = c.types.find(type); it != c.types.end()) return it->second;
    auto list = loadDir(type);
    log().debug("loaded {} {} recipes", list.size(), type);
    return c.types.emplace(type, std::move(list)).first->second;
}

std::vector<ProcessingRecipe> const& loadType(std::string const& type) { return ofType(type); }

std::vector<ProcessingRecipe const*> withFirstIngredient(std::string const& type, ItemStack const& stack) {
    std::vector<ProcessingRecipe const*> out;
    for (auto const& r : ofType(type)) {
        if (!r.ingredients.empty() && matches(r.ingredients.front(), stack)) out.push_back(&r);
    }
    return out;
}

ProcessingRecipe const* findFirst(std::string const& type, ItemStack const& stack) {
    for (auto const& r : ofType(type)) {
        if (!r.ingredients.empty() && matches(r.ingredients.front(), stack)) return &r;
    }
    return nullptr;
}

ProcessingRecipe const* byId(std::string const& type, std::string const& id) {
    for (auto const& r : ofType(type))
        if (r.id == id) return &r;
    return nullptr;
}

bool inTag(std::string const& tag, ItemStack const& stack) {
    if (items::isEmpty(stack)) return false;
    loadTags();
    std::set<std::string> ids, seen;
    collectTag(tag, ids, seen);
    if (ids.contains(items::idOf(stack))) return true;
    return stack.hasTag(ItemTag{HashedString(tag)});
}

std::vector<std::string> tagItems(std::string const& tag) {
    loadTags();
    std::set<std::string> ids, seen;
    collectTag(tag, ids, seen);
    return {ids.begin(), ids.end()};
}

bool matches(ItemIngredient const& ingredient, ItemStack const& stack) {
    if (items::isEmpty(stack)) return false;
    auto id = items::idOf(stack);
    for (auto const& i : ingredient.items)
        if (i == id) return true;
    for (auto const& t : ingredient.tags)
        if (inTag(t, stack)) return true;
    return false;
}

bool matchesFluid(FluidIngredient const& ingredient, std::string const& fluid) {
    if (!ingredient.fluid.empty()) return ingredient.fluid == fluid;
    loadTags();
    std::set<std::string> ids, seen;
    collectTag(ingredient.tag, ids, seen);
    return ids.contains(fluid);
}

std::vector<ItemStack> rollResults(ProcessingRecipe const& recipe) {
    static std::mt19937                          rng{std::random_device{}()};
    std::uniform_real_distribution<float>        dist(0.0f, 1.0f);
    std::vector<ItemStack>                       out;
    for (auto const& r : recipe.results) {
        int n = core::common::rollResult(r, [&] { return dist(rng); });
        if (n <= 0) continue;
        if (auto s = items::make(r.id, n)) out.push_back(std::move(*s));
        else log().warn("recipe {}: unknown result item {}", recipe.id, r.id);
    }
    return out;
}

void reload() {
    auto& c = cache();
    c.types.clear();
    c.tags.clear();
    c.tagsLoaded = false;
}

} // namespace lv::feature::common::recipes
