#include "core/assembly/Sequenced.h"

#include <algorithm>

namespace lv::core::assembly {

namespace {
using json = nlohmann::json;

std::string itemOf(json const& e) {
    if (e.is_string()) return common::mapId(e.get<std::string>());
    if (!e.is_object()) return {};
    for (auto const* k : {"item", "id"}) {
        if (auto it = e.find(k); it != e.end() && it->is_string()) return common::mapId(it->get<std::string>());
    }
    return {};
}

bool addAlternative(json const& e, common::ItemIngredient& out) {
    if (!e.is_object()) return false;
    if (auto item = itemOf(e); !item.empty()) {
        out.items.push_back(item);
        return true;
    }
    if (auto it = e.find("tag"); it != e.end() && it->is_string()) {
        out.tags.push_back(common::mapId(it->get<std::string>()));
        return true;
    }
    if (auto it = e.find("children"); it != e.end() && it->is_array()) {
        bool any = false;
        for (auto const& c : *it) any |= addAlternative(c, out);
        return any;
    }
    return false;
}
} // namespace

std::string stepTypeName(std::string_view typeId) {
    if (auto colon = typeId.find(':'); colon != std::string_view::npos) typeId = typeId.substr(colon + 1);
    for (auto t : {kDeploying, kPressing, kCutting, kFilling})
        if (typeId == t) return std::string(t);
    return {};
}

std::optional<SequencedRecipe> parseSequenced(json const& doc, std::string const& id, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<SequencedRecipe> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    if (!doc.is_object()) return fail("not a JSON object");
    SequencedRecipe r;
    r.id = id;

    auto ing = doc.find("ingredient");
    if (ing == doc.end()) return fail("missing 'ingredient'");
    if (ing->is_array()) {
        for (auto const& alt : *ing) addAlternative(alt, r.ingredient);
    } else {
        addAlternative(*ing, r.ingredient);
    }
    if (r.ingredient.empty()) return fail("bad 'ingredient'");

    auto trans = doc.find("transitional_item");
    if (trans == doc.end()) trans = doc.find("transitionalItem");
    if (trans == doc.end() || (r.transitionalItem = itemOf(*trans)).empty()) return fail("missing 'transitional_item'");

    if (auto it = doc.find("loops"); it != doc.end() && it->is_number_integer()) r.loops = std::max(1, it->get<int>());

    auto seq = doc.find("sequence");
    if (seq == doc.end() || !seq->is_array() || seq->empty()) return fail("missing 'sequence'");
    int index = 0;
    for (auto const& step : *seq) {
        if (!step.is_object()) return fail("sequence step is not an object");
        auto type = stepTypeName(step.value("type", std::string{}));
        if (type.empty()) return fail("unsupported sequence step type '" + step.value("type", std::string{}) + "'");
        std::string err;
        auto sub = common::parseRecipe(step, type, id + "/step_" + std::to_string(index++), &err);
        if (!sub) return fail("sequence step: " + err);
        if (sub->ingredients.empty()) return fail("sequence step without ingredients");
        if (type == kDeploying && sub->ingredients.size() < 2) return fail("deploying step needs the applied item");
        if (type == kFilling && sub->fluidIngredients.empty()) return fail("filling step needs a fluid");
        SequenceStep s;
        s.type         = type;
        s.keepHeldItem = step.value("keep_held_item", step.value("keepHeldItem", false));
        s.recipe       = std::move(*sub);
        r.sequence.push_back(std::move(s));
    }

    auto results = doc.find("results");
    if (results == doc.end() || !results->is_array() || results->empty()) return fail("missing 'results'");
    for (auto const& e : *results) {
        common::ItemResult out;
        out.id = itemOf(e);
        if (out.id.empty()) return fail("bad result " + e.dump());
        out.count = std::max(1, e.is_object() ? e.value("count", 1) : 1);
        if (e.is_object()) {
            if (auto c = e.find("chance"); c != e.end() && c->is_number()) out.chance = c->get<float>();
        }
        r.results.push_back(std::move(out));
    }

    if (auto tags = doc.find("tags"); tags != doc.end() && tags->is_object()) {
        for (auto const& [tag, values] : tags->items()) {
            if (!values.is_array()) continue;
            auto& list = r.inlineTags[common::mapId(tag)];
            for (auto const& v : values)
                if (v.is_string()) list.push_back(common::mapId(v.get<std::string>()));
        }
    }
    return r;
}

bool completesWith(SequencedRecipe const& recipe, int finished) {
    int len = static_cast<int>(recipe.sequence.size());
    if (len == 0) return true;
    return (finished + 1) / len >= recipe.loops;
}

float progressAt(SequencedRecipe const& recipe, int finishedAfter) {
    int total = recipe.totalSteps();
    if (total <= 0) return 1.f;
    return std::clamp(static_cast<float>(finishedAfter) / static_cast<float>(total), 0.f, 1.f);
}

int pickWeighted(std::vector<common::ItemResult> const& results, float random01) {
    float total = 0;
    for (auto const& r : results) total += std::max(0.f, r.chance);
    if (total <= 0) return -1;
    float number = random01 * total;
    for (std::size_t i = 0; i < results.size(); ++i) {
        number -= std::max(0.f, results[i].chance);
        if (number < 0) return static_cast<int>(i);
    }
    return static_cast<int>(results.size()) - 1;
}

std::vector<float> resultShares(std::vector<common::ItemResult> const& results) {
    float total = 0;
    for (auto const& r : results) total += std::max(0.f, r.chance);
    std::vector<float> out;
    for (auto const& r : results) out.push_back(total > 0 ? std::max(0.f, r.chance) / total : 0.f);
    return out;
}

std::vector<TooltipLine> tooltipLines(SequencedRecipe const& recipe, int finished, std::vector<std::string> const& stepNames) {
    std::vector<TooltipLine> out;
    int len   = static_cast<int>(recipe.sequence.size());
    int total = recipe.totalSteps();
    out.push_back({"create.recipe.sequenced_assembly", "Recipe Sequence", {}, '7'});
    out.push_back({"create.recipe.assembly.progress", "Progress: {0}/{1}", {std::to_string(finished), std::to_string(total)}, '8'});
    int remaining = total - finished;
    for (int i = 0; i < len && i < remaining; ++i) {
        auto idx  = static_cast<std::size_t>((i + finished) % len);
        auto name = idx < stepNames.size() ? stepNames[idx] : recipe.sequence[idx].type;
        if (i == 0) out.push_back({"create.recipe.assembly.next", "Next: {0}", {name}, 'b'});
        else out.push_back({"", "-> {0}", {name}, '3'});
    }
    return out;
}

} // namespace lv::core::assembly
