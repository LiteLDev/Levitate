#include "core/common/Recipe.h"

#include <algorithm>

namespace lv::core::common {

namespace {
using json = nlohmann::json;

std::string str(json const& j, std::initializer_list<char const*> keys) {
    for (auto const* k : keys) {
        auto it = j.find(k);
        if (it != j.end() && it->is_string()) return it->get<std::string>();
    }
    return {};
}

int integer(json const& j, std::initializer_list<char const*> keys, int fallback) {
    for (auto const* k : keys) {
        auto it = j.find(k);
        if (it != j.end() && it->is_number()) return it->get<int>();
    }
    return fallback;
}

bool isFluidEntry(json const& e) {
    if (!e.is_object()) return false;
    if (e.contains("fluid") || e.contains("fluidTag") || e.contains("fluid_tag")) return true;
    auto type = str(e, {"type"});
    return type.find("fluid") != std::string::npos;
}

bool addItemAlternative(json const& e, ItemIngredient& out) {
    if (!e.is_object()) return false;
    if (auto item = str(e, {"item", "id"}); !item.empty()) {
        out.items.push_back(mapId(item));
        return true;
    }
    if (auto tag = str(e, {"tag"}); !tag.empty()) {
        out.tags.push_back(mapId(tag));
        return true;
    }
    // neoforge compound ingredient: {"type": "neoforge:compound", "children": [...]}
    if (auto it = e.find("children"); it != e.end() && it->is_array()) {
        bool any = false;
        for (auto const& c : *it) any |= addItemAlternative(c, out);
        return any;
    }
    return false;
}

FluidIngredient parseFluidIngredient(json const& e) {
    FluidIngredient f;
    f.amount = integer(e, {"amount"}, 0);
    f.fluid  = mapId(str(e, {"fluid"}));
    f.tag    = mapId(str(e, {"fluidTag", "fluid_tag", "tag"}));
    return f;
}
} // namespace

std::string_view heatRequirementName(HeatRequirement h) {
    switch (h) {
    case HeatRequirement::Heated:
        return "heated";
    case HeatRequirement::Superheated:
        return "superheated";
    default:
        return "none";
    }
}

std::string_view heatLevelName(HeatLevel h) {
    switch (h) {
    case HeatLevel::Smouldering:
        return "smouldering";
    case HeatLevel::Fading:
        return "fading";
    case HeatLevel::Kindled:
        return "kindled";
    case HeatLevel::Seething:
        return "seething";
    default:
        return "none";
    }
}

bool heatSatisfies(HeatRequirement required, HeatLevel level) {
    switch (required) {
    case HeatRequirement::Superheated:
        return level == HeatLevel::Seething;
    case HeatRequirement::Heated:
        return level != HeatLevel::None && level != HeatLevel::Smouldering;
    default:
        return true;
    }
}

std::string mapId(std::string_view id) {
    if (id.starts_with("create:")) return "levitate:" + std::string(id.substr(7));
    return std::string(id);
}

std::optional<ProcessingRecipe>
parseRecipe(json const& doc, std::string const& type, std::string const& id, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<ProcessingRecipe> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    if (!doc.is_object()) return fail("not a JSON object");
    ProcessingRecipe r;
    r.id   = id;
    r.type = type;
    r.raw  = doc;

    auto ingredients = doc.find("ingredients");
    if (ingredients != doc.end() && !ingredients->is_array()) return fail("'ingredients' is not an array");
    if (ingredients != doc.end()) {
        for (auto const& e : *ingredients) {
            if (isFluidEntry(e)) {
                auto f = parseFluidIngredient(e);
                if ((f.fluid.empty() && f.tag.empty()) || f.amount <= 0) return fail("bad fluid ingredient");
                r.fluidIngredients.push_back(std::move(f));
                continue;
            }
            ItemIngredient ing;
            if (e.is_array()) {
                for (auto const& alt : e) addItemAlternative(alt, ing);
            } else {
                addItemAlternative(e, ing);
            }
            if (ing.empty()) return fail("bad item ingredient " + e.dump());
            int count = e.is_object() ? std::max(1, integer(e, {"count"}, 1)) : 1;
            for (int i = 0; i < count; ++i) r.ingredients.push_back(ing);
        }
    }

    auto results = doc.find("results");
    if (results == doc.end()) results = doc.find("result");
    if (results != doc.end()) {
        auto addResult = [&](json const& e) -> bool {
            if (!e.is_object()) return false;
            if (e.contains("amount")) {
                FluidResult f{mapId(str(e, {"fluid", "id"})), integer(e, {"amount"}, 0)};
                if (f.fluid.empty() || f.amount <= 0) return false;
                r.fluidResults.push_back(std::move(f));
                return true;
            }
            ItemResult out;
            out.id    = mapId(str(e, {"id", "item"}));
            out.count = std::max(1, integer(e, {"count"}, 1));
            if (auto c = e.find("chance"); c != e.end() && c->is_number()) out.chance = c->get<float>();
            if (out.id.empty()) return false;
            r.results.push_back(std::move(out));
            return true;
        };
        if (results->is_array()) {
            for (auto const& e : *results)
                if (!addResult(e)) return fail("bad result " + e.dump());
        } else if (!addResult(*results)) {
            return fail("bad result");
        }
    }
    if (r.results.empty() && r.fluidResults.empty()) return fail("recipe has no results");

    auto heat = str(doc, {"heat_requirement", "heatRequirement"});
    if (heat == "heated") r.heat = HeatRequirement::Heated;
    else if (heat == "superheated") r.heat = HeatRequirement::Superheated;
    else if (!heat.empty() && heat != "none") return fail("unknown heat requirement '" + heat + "'");
    r.processingTime = integer(doc, {"processing_time", "processingTime"}, 0);
    return r;
}

int rollResult(ItemResult const& result, std::function<float()> const& random01) {
    int out = result.count;
    for (int i = 0; i < result.count; ++i) {
        if (random01() > result.chance) --out;
    }
    return out;
}

} // namespace lv::core::common
