#include "core/advancements/Advancements.h"

#include <algorithm>
#include <unordered_map>

namespace lv::core::advancements {

namespace {

using enum Type;

// Create 6.0.10's AllAdvancements, in its order. Icons are Levitate's ids (blocks Levitate has no
// port of show the nearest vanilla item).
constexpr Def kDefs[] = {
    {"root", "", "levitate:brass_hand", Silent, {}, true},

    // Andesite - central branch
    {"andesite_alloy", "root", "levitate:andesite_alloy", Normal, {"levitate:andesite_alloy"}},
    {"andesite_casing", "andesite_alloy", "levitate:andesite_casing", Noisy},
    {"mechanical_press", "andesite_casing", "levitate:mechanical_press", Noisy},
    {"encased_fan", "mechanical_press", "levitate:encased_fan", Normal},
    {"fan_processing", "encased_fan", "levitate:propeller", Normal},
    {"saw_processing", "fan_processing", "levitate:mechanical_saw", Normal},
    {"compacting", "saw_processing", "minecraft:iron_block", Normal},
    {"belt", "compacting", "levitate:belt_connector", Normal},
    {"funnel", "belt", "levitate:andesite_funnel", Normal},
    {"chute", "funnel", "levitate:chute", Normal},
    {"mechanical_mixer", "chute", "levitate:mechanical_mixer", Normal},
    {"burner", "mechanical_mixer", "levitate:blaze_burner", Normal, {"levitate:blaze_burner"}},

    // Andesite - top branch
    {"water_wheel", "andesite_alloy", "levitate:water_wheel", Normal},
    {"windmill", "water_wheel", "levitate:sail_frame", Normal},
    {"shifting_gears", "windmill", "levitate:cogwheel", Normal},
    {"millstone", "shifting_gears", "levitate:millstone", Normal},
    {"super_glue", "millstone", "levitate:super_glue", Normal},
    {"contraption_actors", "super_glue", "levitate:mechanical_harvester", Normal},
    {"portable_storage_interface", "contraption_actors", "levitate:portable_storage_interface", Normal},
    {"wrench_goggles", "portable_storage_interface", "levitate:wrench", Normal, {"levitate:wrench", "levitate:goggles"}},
    {"stressometer", "wrench_goggles", "levitate:stressometer", Normal},
    {"cuckoo_clock", "stressometer", "minecraft:clock", Noisy},

    // Andesite - expert branch
    {"windmill_maxed", "andesite_alloy", "levitate:white_sail", Expert},
    {"ejector_maxed", "windmill_maxed", "levitate:weighted_ejector", Expert},
    {"pulley_maxed", "ejector_maxed", "levitate:rope_pulley", Expert},
    {"cart_pickup", "pulley_maxed", "levitate:minecart_contraption", Expert},
    {"anvil_plough", "cart_pickup", "minecraft:chipped_anvil", Expert},

    // Andesite - hidden
    {"lava_wheel_00000", "mechanical_mixer", "levitate:water_wheel", Secret},
    {"hand_crank_000", "mechanical_mixer", "levitate:hand_crank", Secret},
    {"belt_funnel_kiss", "mechanical_mixer", "levitate:brass_funnel", Secret},
    {"stressometer_maxed", "mechanical_mixer", "levitate:stressometer", Secret},

    // Logistics
    {"cardboard", "mechanical_mixer", "levitate:cardboard", Normal, {"levitate:cardboard"}},
    {"packager", "cardboard", "levitate:packager", Normal},
    {"stock_ticker", "packager", "levitate:stock_ticker", Noisy},
    {"frogport", "stock_ticker", "levitate:package_frogport", Noisy},
    {"table_cloth_shop", "frogport", "levitate:red_table_cloth", Noisy},
    {"factory_gauge", "table_cloth_shop", "levitate:factory_gauge", Noisy},
    {"cardboard_armor", "factory_gauge", "levitate:cardboard", Normal},

    // Logistics - secret
    {"package_chute_throw", "cardboard_armor", "levitate:cardboard_package_12x12", Secret},
    {"cardboard_armor_trim", "cardboard_armor", "levitate:cardboard", Secret},

    // Copper - central branch
    {"copper", "burner", "minecraft:copper_ingot", Silent, {"minecraft:copper_ingot"}},
    {"copper_casing", "copper", "levitate:copper_casing", Normal},
    {"spout", "copper_casing", "levitate:spout", Normal},
    {"drain", "spout", "levitate:item_drain", Normal},
    {"steam_engine", "drain", "levitate:steam_engine", Normal},
    {"steam_whistle", "steam_engine", "levitate:steam_whistle", Normal},
    {"backtank", "steam_whistle", "levitate:copper_backtank", Normal},
    {"diving_suit", "backtank", "levitate:copper_diving_helmet", Normal},

    // Copper - top branch
    {"mechanical_pump_0", "copper", "levitate:mechanical_pump", Normal},
    {"glass_pipe", "mechanical_pump_0", "levitate:fluid_pipe", Normal},
    {"water_supply", "glass_pipe", "minecraft:water_bucket", Normal},
    {"hose_pulley", "water_supply", "levitate:hose_pulley", Normal},
    {"chocolate_bucket", "hose_pulley", "levitate:chocolate_bucket", Normal, {"levitate:chocolate_bucket"}},
    {"honey_drain", "chocolate_bucket", "minecraft:beehive", Normal},

    // Copper - expert branch
    {"hose_pulley_lava", "copper", "levitate:hose_pulley", Expert},
    {"steam_engine_maxed", "hose_pulley_lava", "levitate:steam_engine", Expert},
    {"foods", "steam_engine_maxed", "levitate:chocolate_glazed_berries", Expert},
    {"diving_suit_lava", "foods", "levitate:netherite_diving_helmet", Expert},

    // Copper - hidden
    {"chained_drain", "backtank", "levitate:item_drain", Secret},
    {"cross_streams", "backtank", "minecraft:cobblestone", Secret},
    {"pipe_organ", "backtank", "levitate:steam_whistle", Secret},

    // Brass - central branch
    {"brass", "diving_suit", "levitate:brass_ingot", Normal, {"levitate:brass_ingot"}},
    {"brass_casing", "brass", "levitate:brass_casing", Noisy},
    {"rose_quartz", "brass_casing", "levitate:polished_rose_quartz", Normal, {"levitate:polished_rose_quartz"}},
    {"deployer", "rose_quartz", "levitate:deployer", Normal},
    {"precision_mechanism", "deployer", "levitate:precision_mechanism", Noisy, {"levitate:precision_mechanism"}},
    {"speed_controller", "precision_mechanism", "levitate:rotation_speed_controller", Normal},
    {"mechanical_arm", "speed_controller", "levitate:mechanical_arm", Noisy},
    {"mechanical_crafter", "mechanical_arm", "levitate:mechanical_crafter", Normal},
    {"crushing_wheel", "mechanical_crafter", "levitate:crushing_wheel", Noisy},

    // Brass - top branch
    {"haunted_bell", "brass", "minecraft:bell", Noisy},
    {"clockwork_bearing", "haunted_bell", "levitate:clockwork_bearing", Noisy},
    {"display_link", "clockwork_bearing", "levitate:display_link", Normal},
    {"potato_cannon", "display_link", "levitate:potato_cannon", Noisy},
    {"extendo_grip", "potato_cannon", "levitate:extendo_grip", Normal},
    {"linked_controller", "extendo_grip", "levitate:linked_controller", Normal},
    {"arm_blaze_burner", "linked_controller", "levitate:blaze_burner", Normal},

    // Brass - expert branch
    {"crusher_maxed_0000", "brass", "levitate:crushing_wheel", Expert},
    {"arm_many_targets", "crusher_maxed_0000", "levitate:mechanical_arm", Expert},
    {"potato_cannon_collide", "arm_many_targets", "minecraft:carrot", Expert},
    {"self_deploying", "potato_cannon_collide", "minecraft:rail", Expert},

    // Brass - hidden
    {"fist_bump", "mechanical_crafter", "levitate:deployer", Secret},
    {"crafter_lazy_000", "mechanical_crafter", "levitate:mechanical_crafter", Secret},
    {"extendo_grip_dual", "mechanical_crafter", "levitate:extendo_grip", Secret},
    {"musical_arm", "mechanical_crafter", "minecraft:jukebox", Secret},

    // Trains - central branch
    {"sturdy_sheet", "crushing_wheel", "levitate:sturdy_sheet", Normal, {"levitate:sturdy_sheet"}},
    {"train_casing_00", "sturdy_sheet", "levitate:railway_casing", Normal},
    {"train", "train_casing_00", "levitate:track_station", Noisy},
    {"conductor", "train", "levitate:schedule", Normal},
    {"track_signal", "conductor", "levitate:track_signal", Normal},
    {"display_board_0", "track_signal", "levitate:display_board", Noisy},

    // Trains - top branch
    {"track_0", "sturdy_sheet", "levitate:track", Normal, {"levitate:track"}},
    {"train_whistle", "track_0", "levitate:steam_whistle", Normal},
    {"train_portal", "train_whistle", "minecraft:amethyst_block", Noisy},

    // Trains - expert branch
    {"track_crafting_factory", "sturdy_sheet", "levitate:mechanical_press", Expert},
    {"long_train", "track_crafting_factory", "minecraft:minecart", Expert},
    {"long_travel", "long_train", "levitate:green_seat", Expert},

    // Trains - hidden
    {"train_roadkill", "track_signal", "minecraft:diamond_sword", Secret},
    {"red_signal", "track_signal", "levitate:track_signal", Secret},
    {"train_crash", "track_signal", "levitate:incomplete_track", Secret},
    {"train_crash_backwards", "track_signal", "levitate:incomplete_track", Secret},
};

std::unordered_map<std::string_view, int> const& indexMap() {
    static std::unordered_map<std::string_view, int> const m = [] {
        std::unordered_map<std::string_view, int> r;
        for (int i = 0; i < static_cast<int>(std::size(kDefs)); ++i) r.emplace(kDefs[i].id, i);
        return r;
    }();
    return m;
}

std::vector<std::vector<int>> const& children() {
    static std::vector<std::vector<int>> const c = [] {
        std::vector<std::vector<int>> r(std::size(kDefs));
        for (std::size_t i = 0; i < std::size(kDefs); ++i) {
            int p = parentOf(i);
            if (p >= 0) r[static_cast<std::size_t>(p)].push_back(static_cast<int>(i));
        }
        return r;
    }();
    return c;
}

/// Vanilla's VisibilityRule of one entry.
enum class Rule { Show, Hide, NoChange };

bool evaluate(int i, std::vector<Rule>& stack, std::function<bool(std::string_view)> const& done, std::vector<bool>& out) {
    auto const& d    = kDefs[i];
    bool        isDone = done(d.id);
    Rule        rule   = isDone ? Rule::Show : (hidden(d.type) ? Rule::Hide : Rule::NoChange);
    stack.push_back(rule);
    bool any = isDone;
    for (int c : children()[static_cast<std::size_t>(i)]) any |= evaluate(c, stack, done, out);
    bool visible = any;
    if (!visible) {
        // the nearest of self, parent, grandparent that is not NoChange decides
        for (std::size_t k = 0; k <= 2 && k < stack.size(); ++k) {
            Rule r = stack[stack.size() - 1 - k];
            if (r == Rule::Show) {
                visible = true;
                break;
            }
            if (r == Rule::Hide) break;
        }
    }
    stack.pop_back();
    out[static_cast<std::size_t>(i)] = visible;
    return any;
}

/// Lays out the subtree of `i` from row `next`; returns the entry's row.
float place(int i, int depth, float& next, std::vector<Placement>& out) {
    auto const& kids = children()[static_cast<std::size_t>(i)];
    float       y;
    if (kids.empty()) {
        y = next;
        next += 1;
    } else {
        float first = 0, last = 0;
        for (std::size_t k = 0; k < kids.size(); ++k) {
            float cy = place(kids[k], depth + 1, next, out);
            if (k == 0) first = cy;
            last = cy;
        }
        y = (first + last) / 2;
    }
    out[static_cast<std::size_t>(i)] = Placement{static_cast<float>(depth), y};
    return y;
}

} // namespace

std::span<Def const> all() { return {kDefs, std::size(kDefs)}; }

int indexOf(std::string_view id) {
    auto const& m  = indexMap();
    auto        it = m.find(id);
    return it == m.end() ? -1 : it->second;
}

Def const* find(std::string_view id) {
    int i = indexOf(id);
    return i < 0 ? nullptr : &kDefs[i];
}

int parentOf(std::size_t index) {
    if (index >= std::size(kDefs) || kDefs[index].parent.empty()) return -1;
    return indexOf(kDefs[index].parent);
}

std::vector<bool> visibility(std::function<bool(std::string_view)> const& done) {
    std::vector<bool> out(std::size(kDefs), false);
    std::vector<Rule> stack(3, Rule::NoChange);
    evaluate(0, stack, done, out);
    return out;
}

std::vector<Placement> const& layout() {
    static std::vector<Placement> const l = [] {
        std::vector<Placement> out(std::size(kDefs));
        float next = 0;
        place(0, 0, next, out);
        return out;
    }();
    return l;
}

} // namespace lv::core::advancements
