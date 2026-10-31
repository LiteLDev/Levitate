// Every recipe file the build ships must be readable (U54): merge conflict markers in three
// mechanical crafting recipes once made the loader skip them silently (it parses without
// exceptions and drops what it can't read), and a 2-wide row in a 3-wide crafting pattern moved an
// ingredient to another column. These tests read the files in the repository.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "core/common/Recipe.h"

namespace fs = std::filesystem;
using Json   = nlohmann::json;

namespace {

fs::path repoRoot() {
    // tests/core/RecipeFilesTest.cpp -> repo
    fs::path p = fs::path(__FILE__).parent_path().parent_path().parent_path();
    if (fs::exists(p / "assets" / "recipes")) return p;
    for (fs::path c = fs::current_path(); !c.empty() && c != c.root_path(); c = c.parent_path()) {
        if (fs::exists(c / "assets" / "recipes")) return c;
    }
    return p;
}

std::string readText(fs::path const& p) {
    std::ifstream      in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// A line starting with a merge conflict marker.
bool hasConflictMarker(std::string const& text) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with("<<<<<<<") || line.starts_with(">>>>>>>") || line.starts_with("=======") || line.starts_with("|||||||")) return true;
    }
    return false;
}

std::vector<fs::path> jsonFiles(fs::path const& dir) {
    std::vector<fs::path> out;
    if (!fs::exists(dir)) return out;
    for (auto const& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".json") out.push_back(e.path());
    return out;
}

} // namespace

// assets/recipes/<type>/**.json: Create's processing recipe types, read by the U3 loader.
TEST(RecipeFiles, ProcessingRecipesParse) {
    auto dir   = repoRoot() / "assets" / "recipes";
    auto files = jsonFiles(dir);
    ASSERT_GT(files.size(), 500u) << "recipe folder not found at " << dir.string();
    for (auto const& p : files) {
        auto rel  = fs::relative(p, dir).generic_string();
        auto text = readText(p);
        EXPECT_FALSE(hasConflictMarker(text)) << rel << " contains a merge conflict marker";
        // the loader allows comments (nlohmann ignore_comments)
        auto doc = Json::parse(text, nullptr, false, true);
        ASSERT_FALSE(doc.is_discarded()) << rel << " is not valid JSON";
        auto type = rel.substr(0, rel.find('/'));
        if (type == "tags") {
            ASSERT_TRUE(doc.is_object()) << rel;
            for (auto const& [tag, values] : doc.items()) {
                EXPECT_TRUE(values.is_array()) << rel << " tag " << tag;
                for (auto const& v : values) EXPECT_TRUE(v.is_string()) << rel << " tag " << tag;
            }
            continue;
        }
        std::string err;
        auto        id = "levitate:" + fs::path(rel).replace_extension().generic_string();
        auto        r  = lv::core::common::parseRecipe(doc, type, id, &err);
        EXPECT_TRUE(r.has_value()) << rel << ": " << err;
        if (r) EXPECT_FALSE(r->results.empty() && r->fluidResults.empty() && type != "sequenced_assembly") << rel << " has no result";
    }
}

// assets/pack/bp/recipes/**.json: Bedrock crafting / furnace / stonecutter recipes.
TEST(RecipeFiles, BehaviorPackRecipesAreWellFormed) {
    auto dir   = repoRoot() / "assets" / "pack" / "bp" / "recipes";
    auto files = jsonFiles(dir);
    ASSERT_GT(files.size(), 1000u) << "behavior pack recipes not found at " << dir.string();
    std::map<std::string, std::string> ids; // identifier -> file
    for (auto const& p : files) {
        auto rel  = fs::relative(p, dir).generic_string();
        auto text = readText(p);
        EXPECT_FALSE(hasConflictMarker(text)) << rel << " contains a merge conflict marker";
        auto doc = Json::parse(text, nullptr, false);
        ASSERT_FALSE(doc.is_discarded()) << rel << " is not valid JSON";
        int recipes = 0;
        for (auto const& [key, body] : doc.items()) {
            if (!key.starts_with("minecraft:recipe_")) continue;
            ++recipes;
            ASSERT_TRUE(body.is_object()) << rel;
            auto identifier = body.value(Json::json_pointer("/description/identifier"), std::string());
            EXPECT_FALSE(identifier.empty()) << rel << " has no identifier";
            auto [it, fresh] = ids.emplace(identifier, rel);
            EXPECT_TRUE(fresh) << rel << " repeats the identifier " << identifier << " of " << it->second;
            if (key != "minecraft:recipe_shaped") continue;
            // Bedrock pads short rows on the right: rows of one pattern must have the same width
            auto const& pattern = body.at("pattern");
            ASSERT_TRUE(pattern.is_array() && !pattern.empty()) << rel;
            auto                  width = pattern[0].get<std::string>().size();
            std::set<std::string> used;
            for (auto const& row : pattern) {
                auto r = row.get<std::string>();
                EXPECT_EQ(r.size(), width) << rel << " has rows of different widths";
                for (char c : r)
                    if (c != ' ') used.insert(std::string(1, c));
            }
            auto const& key2 = body.at("key");
            for (auto const& c : used) EXPECT_TRUE(key2.contains(c)) << rel << " uses '" << c << "' without a key";
            for (auto const& [c, v] : key2.items()) EXPECT_TRUE(used.contains(c)) << rel << " defines an unused key '" << c << "'";
        }
        EXPECT_EQ(recipes, 1) << rel << " should hold exactly one recipe";
    }
}
