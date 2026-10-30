#include <gtest/gtest.h>

#include <set>
#include <string>

#include "core/advancements/Advancements.h"

using namespace lv::core::advancements;

TEST(Advancements, TableMatchesCreate) {
    auto defs = all();
    EXPECT_EQ(defs.size(), 102u); // 102 entries in Create 6.0.10 (204 lang keys)
    EXPECT_EQ(defs[0].id, "root");
    EXPECT_TRUE(defs[0].free);
    std::set<std::string_view> ids;
    for (auto const& d : defs) {
        EXPECT_TRUE(ids.insert(d.id).second) << d.id;
        if (d.id != "root") EXPECT_NE(find(d.parent), nullptr) << d.id;
        // parents come before their children (Create builds them in order)
        if (d.id != "root") EXPECT_LT(indexOf(d.parent), indexOf(d.id)) << d.id;
    }
    EXPECT_EQ(find("hand_crank_000")->type, Type::Secret);
    EXPECT_EQ(find("wrench_goggles")->collect[1], "levitate:goggles");
    EXPECT_EQ(find("copper")->type, Type::Silent);
    EXPECT_EQ(find("nope"), nullptr);
}

TEST(Advancements, TypeFlags) {
    EXPECT_FALSE(toast(Type::Silent));
    EXPECT_TRUE(toast(Type::Normal));
    EXPECT_FALSE(announce(Type::Normal));
    EXPECT_TRUE(announce(Type::Noisy));
    EXPECT_TRUE(announce(Type::Secret));
    EXPECT_TRUE(hidden(Type::Secret));
    EXPECT_FALSE(hidden(Type::Expert));
    EXPECT_TRUE(goal(Type::Expert));
    EXPECT_FALSE(goal(Type::Noisy));
}

TEST(Advancements, VisibilityFollowsVanilla) {
    std::set<std::string> done = {"root"};
    auto                  isDone = [&](std::string_view id) { return done.contains(std::string(id)); };
    auto                  vis    = visibility(isDone);
    auto                  v      = [&](char const* id) { return vis[static_cast<std::size_t>(indexOf(id))]; };
    EXPECT_TRUE(v("root"));
    EXPECT_TRUE(v("andesite_alloy"));    // parent done
    EXPECT_TRUE(v("andesite_casing"));   // grandparent done
    EXPECT_FALSE(v("mechanical_press")); // three levels down
    EXPECT_FALSE(v("hand_crank_000"));   // hidden

    done.insert("andesite_alloy");
    done.insert("andesite_casing");
    vis = visibility(isDone);
    EXPECT_TRUE(v("mechanical_press"));
    EXPECT_TRUE(v("encased_fan"));
    EXPECT_FALSE(v("fan_processing"));

    // a done hidden entry shows itself and its path to the root
    done = {"root", "hand_crank_000"};
    vis  = visibility(isDone);
    EXPECT_TRUE(v("hand_crank_000"));
    EXPECT_TRUE(v("mechanical_mixer")); // ancestor of a done entry
    EXPECT_TRUE(v("andesite_alloy"));
    EXPECT_FALSE(v("lava_wheel_00000"));
}

TEST(Advancements, LayoutPlacesChildrenRight) {
    auto const& l = layout();
    ASSERT_EQ(l.size(), all().size());
    for (std::size_t i = 1; i < l.size(); ++i) {
        int p = parentOf(i);
        ASSERT_GE(p, 0);
        EXPECT_FLOAT_EQ(l[i].x, l[static_cast<std::size_t>(p)].x + 1) << all()[i].id;
    }
    // no two entries share a cell
    std::set<std::pair<int, int>> cells;
    for (auto const& pl : l) EXPECT_TRUE(cells.insert({static_cast<int>(pl.x * 2), static_cast<int>(pl.y * 2)}).second);
}
