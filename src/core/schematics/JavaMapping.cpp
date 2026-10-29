#include "core/schematics/JavaMapping.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <unordered_map>

namespace lv::core::schematics {

using nbt::Tag;

std::string JavaState::get(std::string_view key, std::string_view fallback) const {
    for (auto const& [k, v] : props)
        if (k == key) return v;
    return std::string(fallback);
}

bool JavaState::has(std::string_view key) const {
    for (auto const& [k, v] : props)
        if (k == key) return true;
    return false;
}

JavaState parseJavaState(std::string_view text) {
    JavaState s;
    auto      open = text.find('[');
    s.name         = std::string(text.substr(0, open));
    if (open == std::string_view::npos) return s;
    auto body = text.substr(open + 1);
    if (auto close = body.rfind(']'); close != std::string_view::npos) body = body.substr(0, close);
    while (!body.empty()) {
        auto comma = body.find(',');
        auto part  = body.substr(0, comma);
        auto eq    = part.find('=');
        if (eq != std::string_view::npos) s.props.emplace_back(std::string(part.substr(0, eq)), std::string(part.substr(eq + 1)));
        if (comma == std::string_view::npos) break;
        body = body.substr(comma + 1);
    }
    return s;
}

namespace {

int toInt(std::string const& s, int fallback = 0) {
    int  v = fallback;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} ? v : fallback;
}

/// Bedrock facing order: down, up, north, south, west, east.
int facingIndex(std::string const& f) {
    static constexpr std::array<std::string_view, 6> k{"down", "up", "north", "south", "west", "east"};
    for (int i = 0; i < 6; ++i)
        if (k[static_cast<std::size_t>(i)] == f) return i;
    return -1;
}

/// Legacy horizontal `direction`: south, west, north, east.
int swne(std::string const& f) {
    if (f == "south") return 0;
    if (f == "west") return 1;
    if (f == "north") return 2;
    if (f == "east") return 3;
    return 0;
}

bool isHorizontal(std::string const& f) { return f == "north" || f == "south" || f == "west" || f == "east"; }

std::string opposite(std::string const& f) {
    if (f == "north") return "south";
    if (f == "south") return "north";
    if (f == "west") return "east";
    if (f == "east") return "west";
    if (f == "up") return "down";
    if (f == "down") return "up";
    return f;
}

struct Out {
    BlockState st;
    void       name(std::string n) { st.name = n.find(':') == std::string::npos ? "minecraft:" + n : n; }
    void       alt(std::string n) { st.alternatives.push_back(n.find(':') == std::string::npos ? "minecraft:" + n : n); }
    void       s(std::string_view key, std::string v) { st.states.set(key, Tag(std::move(v))); }
    void       i(std::string_view key, int v) { st.states.set(key, Tag(static_cast<std::int32_t>(v))); }
    void       b(std::string_view key, bool v) { st.states.set(key, Tag(static_cast<std::int8_t>(v ? 1 : 0))); }
};

bool endsWith(std::string_view s, std::string_view suffix) { return s.ends_with(suffix); }
std::string strip(std::string_view s, std::string_view suffix) { return std::string(s.substr(0, s.size() - suffix.size())); }

/// Plain renames (Java path -> Bedrock path), states handled by the generic rules.
std::unordered_map<std::string_view, std::string_view> const& renames() {
    static std::unordered_map<std::string_view, std::string_view> const m{
        {"dirt_path", "grass_path"},
        {"snow_block", "snow"},
        {"cobweb", "web"},
        {"note_block", "noteblock"},
        {"spawner", "mob_spawner"},
        {"terracotta", "hardened_clay"},
        {"magma_block", "magma"},
        {"red_nether_bricks", "red_nether_brick"},
        {"nether_bricks", "nether_brick"},
        {"slime_block", "slime"},
        {"sugar_cane", "reeds"},
        {"bricks", "brick_block"},
        {"dead_bush", "deadbush"},
        {"lily_pad", "waterlily"},
        {"melon", "melon_block"},
        {"nether_quartz_ore", "quartz_ore"},
        {"end_stone_bricks", "end_bricks"},
        {"shulker_box", "undyed_shulker_box"},
        {"frogspawn", "frog_spawn"},
        {"flowering_azalea_leaves", "azalea_leaves_flowered"},
        {"light_gray_glazed_terracotta", "silver_glazed_terracotta"},
        {"tripwire", "trip_wire"},
        {"kelp_plant", "kelp"},
        {"twisting_vines_plant", "twisting_vines"},
        {"weeping_vines_plant", "weeping_vines"},
        {"cave_vines_plant", "cave_vines"},
        {"tall_seagrass", "seagrass"},
        {"beetroots", "beetroot"},
        {"stonecutter", "stonecutter_block"},
        {"smooth_quartz", "smooth_quartz"},
        {"nether_portal", "portal"},
        {"moss_carpet", "moss_carpet"},
        {"powder_snow_cauldron", "cauldron"},
        {"water_cauldron", "cauldron"},
        {"lava_cauldron", "cauldron"},
        {"attached_pumpkin_stem", "pumpkin_stem"},
        {"attached_melon_stem", "melon_stem"},
        {"rooted_dirt", "dirt_with_roots"},
        {"oak_door", "wooden_door"},
        {"oak_trapdoor", "trapdoor"},
        {"oak_fence_gate", "fence_gate"},
        {"oak_button", "wooden_button"},
        {"oak_pressure_plate", "wooden_pressure_plate"},
        {"powered_rail", "golden_rail"},
        {"jack_o_lantern", "lit_pumpkin"},
        {"chain", "iron_chain"},
        {"grass", "short_grass"},
        {"cobblestone_stairs", "stone_stairs"},
        {"stone_stairs", "normal_stone_stairs"},
        {"prismarine_brick_stairs", "prismarine_bricks_stairs"},
        {"end_stone_brick_stairs", "end_brick_stairs"},
        {"stone_slab", "normal_stone_slab"},
    };
    return m;
}

/// Alternative Bedrock names for some renames (older / newer spellings).
std::unordered_map<std::string_view, std::string_view> const& alternatives() {
    static std::unordered_map<std::string_view, std::string_view> const m{
        {"iron_chain", "chain"},
        {"short_grass", "tallgrass"},
        {"dirt_with_roots", "rooted_dirt"},
        {"stonecutter_block", "stonecutter"},
        {"undyed_shulker_box", "shulker_box"},
    };
    return m;
}

void facingStates(Out& o, std::string const& f) {
    int idx = facingIndex(f);
    if (idx < 0) return;
    o.i("facing_direction", idx);
    o.s("minecraft:facing_direction", f);
    if (isHorizontal(f)) {
        o.s("minecraft:cardinal_direction", f);
        o.i("direction", swne(f));
    }
}

void mapCreate(JavaState const& js, std::string const& path, Out& o) {
    o.name("levitate:" + path);
    if (path == "chute") {
        // chute states run 0..n: facing 0 down, 1..4 north / south / west / east; shape 0 normal,
        // 1 window, 2 intersection, 3 encased
        int f = facingIndex(js.get("facing"));
        o.i("levitate:facing", f >= 2 ? f - 1 : 0);
        auto s = js.get("shape");
        o.i("levitate:shape", s == "window" ? 1 : s == "intersection" ? 2 : s == "encased" ? 3 : 0);
        return;
    }
    for (auto const& [k, v] : js.props) {
        if (k == "waterlogged") continue;
        std::string key = "levitate:" + k;
        if (k == "axis") {
            o.i("levitate:axis", v == "x" ? 0 : v == "y" ? 1 : 2);
        } else if (k == "facing" || k == "horizontal_facing") {
            int f = facingIndex(v);
            if (f >= 0) o.i("levitate:facing", f);
        } else if (v == "true" || v == "false") {
            o.b(key, v == "true");
        } else if (!v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-')) {
            o.i(key, toInt(v));
        } else {
            o.s(key, v);
        }
    }
}

} // namespace

std::optional<BlockState> mapJavaState(JavaState const& js) {
    std::string ns   = "minecraft";
    std::string path = js.name;
    if (auto colon = js.name.find(':'); colon != std::string::npos) {
        ns   = js.name.substr(0, colon);
        path = js.name.substr(colon + 1);
    }
    Out o;
    if (ns == "create") {
        mapCreate(js, path, o);
        return o.st;
    }
    if (ns != "minecraft") {
        o.st.name = js.name; // unknown mod: reported as unmapped by the game side
        return o.st;
    }
    if (path == "structure_void") return std::nullopt;
    if (path == "air" || path == "cave_air" || path == "void_air" || path == "moving_piston" || path == "piston_head"
        || path == "bubble_column") {
        o.name("air");
        return o.st;
    }

    auto p     = [&](std::string_view k) { return js.get(k); };
    auto truth = [&](std::string_view k) { return js.get(k) == "true"; };
    std::string const facing = p("facing");

    // ---- families -------------------------------------------------------------------------------
    if (endsWith(path, "_stairs")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        static constexpr std::array<std::string_view, 4> kWeirdo{"east", "west", "south", "north"};
        for (int i = 0; i < 4; ++i)
            if (kWeirdo[static_cast<std::size_t>(i)] == facing) o.i("weirdo_direction", i);
        o.b("upside_down_bit", p("half") == "top");
        return o.st;
    }
    if (endsWith(path, "_slab")) {
        auto        it   = renames().find(path);
        std::string base = it != renames().end() ? std::string(it->second) : path;
        if (p("type") == "double") {
            std::string stem = strip(base, "_slab");
            o.name(stem + "_double_slab");
            o.alt("double_" + base);
            o.alt(stem + "_slab");
            o.s("minecraft:vertical_half", "bottom");
        } else {
            o.name(base);
            o.s("minecraft:vertical_half", p("type") == "top" ? "top" : "bottom");
        }
        return o.st;
    }
    if (endsWith(path, "_door")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        o.b("upper_block_bit", p("half") == "upper");
        o.b("door_hinge_bit", p("hinge") == "right");
        o.b("open_bit", truth("open"));
        if (isHorizontal(facing)) {
            o.s("minecraft:cardinal_direction", facing);
            // legacy order east, south, west, north
            static constexpr std::array<std::string_view, 4> kDoor{"east", "south", "west", "north"};
            for (int i = 0; i < 4; ++i)
                if (kDoor[static_cast<std::size_t>(i)] == facing) o.i("direction", i);
        }
        return o.st;
    }
    if (endsWith(path, "_trapdoor")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        static constexpr std::array<std::string_view, 4> kTrap{"east", "west", "south", "north"};
        for (int i = 0; i < 4; ++i)
            if (kTrap[static_cast<std::size_t>(i)] == facing) o.i("direction", i);
        if (isHorizontal(facing)) o.s("minecraft:cardinal_direction", facing);
        o.b("open_bit", truth("open"));
        o.b("upside_down_bit", p("half") == "top");
        return o.st;
    }
    if (endsWith(path, "_fence_gate")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        if (isHorizontal(facing)) {
            o.i("direction", swne(facing));
            o.s("minecraft:cardinal_direction", facing);
        }
        o.b("open_bit", truth("open"));
        o.b("in_wall_bit", truth("in_wall"));
        return o.st;
    }
    if (endsWith(path, "_button")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        auto face = p("face");
        o.i("facing_direction", face == "floor" ? 1 : face == "ceiling" ? 0 : std::max(0, facingIndex(facing)));
        o.b("button_pressed_bit", truth("powered"));
        return o.st;
    }
    if (endsWith(path, "_pressure_plate")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        o.i("redstone_signal", js.has("power") ? toInt(p("power")) : truth("powered") ? 15 : 0);
        return o.st;
    }
    if (path == "lever") {
        o.name("lever");
        auto face = p("face");
        bool northSouth = facing == "north" || facing == "south";
        if (face == "floor") o.s("lever_direction", northSouth ? "up_north_south" : "up_east_west");
        else if (face == "ceiling") o.s("lever_direction", northSouth ? "down_north_south" : "down_east_west");
        else o.s("lever_direction", facing);
        o.b("open_bit", truth("powered"));
        return o.st;
    }
    if (path == "torch" || path == "soul_torch" || path == "copper_torch") {
        o.name(path);
        o.s("torch_facing_direction", "top");
        return o.st;
    }
    if (path == "wall_torch" || path == "soul_wall_torch" || path == "copper_wall_torch") {
        o.name(path == "wall_torch" ? "torch" : path == "soul_wall_torch" ? "soul_torch" : "copper_torch");
        o.s("torch_facing_direction", facing);
        return o.st;
    }
    if (path == "redstone_torch" || path == "redstone_wall_torch") {
        bool lit = !js.has("lit") || truth("lit");
        o.name(lit ? "redstone_torch" : "unlit_redstone_torch");
        o.s("torch_facing_direction", path == "redstone_torch" ? std::string("top") : facing);
        return o.st;
    }
    if (endsWith(path, "_wall_hanging_sign")) {
        o.name(strip(path, "_wall_hanging_sign") + "_hanging_sign");
        o.b("hanging", false);
        o.b("attached_bit", false);
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        return o.st;
    }
    if (endsWith(path, "_hanging_sign")) {
        o.name(path);
        o.b("hanging", true);
        o.b("attached_bit", truth("attached"));
        o.i("ground_sign_direction", toInt(p("rotation")));
        return o.st;
    }
    if (endsWith(path, "_wall_sign")) {
        auto wood = strip(path, "_wall_sign");
        o.name(wood == "oak" ? std::string("wall_sign") : wood == "dark_oak" ? std::string("darkoak_wall_sign") : path);
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        return o.st;
    }
    if (endsWith(path, "_sign")) {
        auto wood = strip(path, "_sign");
        o.name(wood == "oak" ? std::string("standing_sign") : wood == "dark_oak" ? std::string("darkoak_standing_sign") : wood + "_standing_sign");
        o.i("ground_sign_direction", toInt(p("rotation")));
        return o.st;
    }
    if (endsWith(path, "_wall_banner")) {
        o.name("wall_banner");
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        return o.st;
    }
    if (endsWith(path, "_banner")) {
        o.name("standing_banner");
        o.i("ground_sign_direction", toInt(p("rotation")));
        return o.st;
    }
    if (endsWith(path, "_bed")) {
        o.name("bed");
        o.i("direction", swne(facing));
        o.b("head_piece_bit", p("part") == "head");
        o.b("occupied_bit", false);
        return o.st;
    }
    if (endsWith(path, "_wall_skull") || endsWith(path, "_wall_head")) {
        o.name(endsWith(path, "_wall_skull") ? strip(path, "_wall_skull") + "_skull" : strip(path, "_wall_head") + "_head");
        o.alt("skull");
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        return o.st;
    }
    if (endsWith(path, "_skull") || endsWith(path, "_head")) {
        o.name(path);
        o.alt("skull");
        o.i("facing_direction", 1);
        return o.st;
    }
    if (endsWith(path, "_wall")) {
        o.name(path);
        o.b("wall_post_bit", truth("up"));
        for (auto side : {"north", "south", "east", "west"}) {
            auto v = p(side);
            o.s(std::string("wall_connection_type_") + side, v == "low" || v == "true" ? "short" : v == "tall" ? "tall" : "none");
        }
        return o.st;
    }
    if (endsWith(path, "_candle") || path == "candle") {
        o.name(path);
        o.i("candles", std::max(0, toInt(p("candles"), 1) - 1));
        o.b("lit", truth("lit"));
        return o.st;
    }
    if (path.starts_with("potted_")) {
        o.name("flower_pot");
        return o.st;
    }
    if (endsWith(path, "_glazed_terracotta")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        o.i("facing_direction", std::max(2, facingIndex(facing)));
        return o.st;
    }
    if (path == "furnace" || path == "blast_furnace" || path == "smoker") {
        o.name(truth("lit") ? "lit_" + path : path);
        facingStates(o, facing);
        return o.st;
    }
    if (path == "redstone_lamp" || path == "redstone_ore" || path == "deepslate_redstone_ore") {
        o.name(truth("lit") ? "lit_" + path : path);
        return o.st;
    }
    if (path == "repeater") {
        o.name(truth("powered") ? "powered_repeater" : "unpowered_repeater");
        o.i("direction", swne(facing));
        o.s("minecraft:cardinal_direction", facing);
        o.i("repeater_delay", std::max(0, toInt(p("delay"), 1) - 1));
        return o.st;
    }
    if (path == "comparator") {
        o.name(truth("powered") ? "powered_comparator" : "unpowered_comparator");
        o.i("direction", swne(facing));
        o.s("minecraft:cardinal_direction", facing);
        o.b("output_subtract_bit", p("mode") == "subtract");
        o.b("output_lit_bit", truth("powered"));
        return o.st;
    }
    if (path == "redstone_wire") {
        o.name("redstone_wire");
        o.i("redstone_signal", toInt(p("power")));
        return o.st;
    }
    if (path == "daylight_detector") {
        o.name(truth("inverted") ? "daylight_detector_inverted" : "daylight_detector");
        o.i("redstone_signal", toInt(p("power")));
        return o.st;
    }
    if (path == "rail" || path == "powered_rail" || path == "detector_rail" || path == "activator_rail") {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        static constexpr std::array<std::string_view, 10> kShapes{
            "north_south", "east_west", "ascending_east", "ascending_west", "ascending_north", "ascending_south", "south_east", "south_west", "north_west", "north_east"
        };
        auto shape = p("shape");
        for (int i = 0; i < 10; ++i)
            if (kShapes[static_cast<std::size_t>(i)] == shape) o.i("rail_direction", i);
        if (path != "rail") o.b("rail_data_bit", truth("powered"));
        return o.st;
    }
    if (path == "water" || path == "lava") {
        int level = toInt(p("level"));
        o.name(level == 0 ? path : "flowing_" + path);
        o.i("liquid_depth", level);
        return o.st;
    }
    if (path == "snow") {
        o.name("snow_layer");
        o.i("height", std::max(0, toInt(p("layers"), 1) - 1));
        o.b("covered_bit", false);
        return o.st;
    }
    if (path == "farmland") {
        o.name("farmland");
        o.i("moisturized_amount", toInt(p("moisture")));
        return o.st;
    }
    if (path == "cake") {
        o.name("cake");
        o.i("bite_counter", toInt(p("bites")));
        return o.st;
    }
    if (path == "sea_pickle") {
        o.name("sea_pickle");
        o.i("cluster_count", std::max(0, toInt(p("pickles"), 1) - 1));
        o.b("dead_bit", !truth("waterlogged"));
        return o.st;
    }
    if (path == "wheat" || path == "carrots" || path == "potatoes" || path == "melon_stem" || path == "pumpkin_stem" || path == "sweet_berry_bush") {
        o.name(path);
        o.i("growth", toInt(p("age")));
        return o.st;
    }
    if (path == "beetroots") {
        o.name("beetroot");
        o.i("growth", std::min(7, toInt(p("age")) * 2 + (toInt(p("age")) == 3 ? 1 : 0)));
        return o.st;
    }
    if (path == "attached_pumpkin_stem" || path == "attached_melon_stem") {
        o.name(path == "attached_pumpkin_stem" ? "pumpkin_stem" : "melon_stem");
        o.i("growth", 7);
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        return o.st;
    }
    if (path == "cocoa") {
        o.name("cocoa");
        o.i("age", toInt(p("age")));
        o.i("direction", swne(opposite(facing)));
        return o.st;
    }
    if (path == "cauldron" || path == "water_cauldron" || path == "lava_cauldron" || path == "powder_snow_cauldron") {
        o.name("cauldron");
        o.i("fill_level", path == "cauldron" ? 0 : path == "lava_cauldron" ? 6 : std::min(6, toInt(p("level"), 3) * 2));
        o.s("cauldron_liquid", path == "lava_cauldron" ? "lava" : path == "powder_snow_cauldron" ? "powder_snow" : "water");
        return o.st;
    }
    if (path == "composter") {
        o.name("composter");
        o.i("composter_fill_level", toInt(p("level")));
        return o.st;
    }
    if (path == "respawn_anchor") {
        o.name("respawn_anchor");
        o.i("respawn_anchor_charge", toInt(p("charges")));
        return o.st;
    }
    if (path == "observer") {
        o.name("observer");
        o.s("minecraft:facing_direction", facing);
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        o.b("powered_bit", truth("powered"));
        return o.st;
    }
    if (path == "hopper") {
        o.name("hopper");
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        o.b("toggle_bit", js.get("enabled", "true") != "true");
        return o.st;
    }
    if (path == "dispenser" || path == "dropper") {
        o.name(path);
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        o.b("triggered_bit", truth("triggered"));
        return o.st;
    }
    if (path == "lantern" || path == "soul_lantern") {
        o.name(path);
        o.b("hanging", truth("hanging"));
        return o.st;
    }
    if (path == "campfire" || path == "soul_campfire") {
        o.name(path);
        o.b("extinguished", !truth("lit"));
        if (isHorizontal(facing)) {
            o.s("minecraft:cardinal_direction", facing);
            o.i("direction", swne(facing));
        }
        return o.st;
    }
    if (path == "bell" || path == "grindstone") {
        o.name(path);
        auto a = p(path == "bell" ? "attachment" : "face");
        std::string att = a == "floor" ? "standing" : a == "ceiling" ? "hanging" : a == "double_wall" ? "multiple" : "side";
        o.s("attachment", att);
        o.i("direction", swne(facing));
        o.s("minecraft:cardinal_direction", facing);
        return o.st;
    }
    if (path == "vine") {
        o.name("vine");
        int bits = (truth("south") ? 1 : 0) | (truth("west") ? 2 : 0) | (truth("north") ? 4 : 0) | (truth("east") ? 8 : 0);
        o.i("vine_direction_bits", bits);
        return o.st;
    }
    if (path == "tall_grass" || path == "large_fern" || path == "sunflower" || path == "lilac" || path == "rose_bush" || path == "peony"
        || path == "pitcher_plant" || path == "small_dripleaf") {
        o.name(path);
        o.b("upper_block_bit", p("half") == "upper");
        if (path == "small_dripleaf" && isHorizontal(facing)) o.s("minecraft:cardinal_direction", facing);
        return o.st;
    }
    if (path == "seagrass" || path == "tall_seagrass") {
        o.name("seagrass");
        o.s("sea_grass_type", path == "seagrass" ? "default" : p("half") == "upper" ? "double_top" : "double_bot");
        return o.st;
    }
    if (path == "kelp" || path == "kelp_plant") {
        o.name("kelp");
        o.i("kelp_age", toInt(p("age")));
        return o.st;
    }
    if (path == "nether_portal") {
        o.name("portal");
        o.s("portal_axis", p("axis") == "z" ? "z" : "x");
        return o.st;
    }
    if (path == "end_portal_frame") {
        o.name("end_portal_frame");
        o.i("direction", swne(facing));
        o.s("minecraft:cardinal_direction", facing);
        o.b("end_portal_eye_bit", truth("eye"));
        return o.st;
    }
    if (path == "tripwire_hook") {
        o.name("tripwire_hook");
        o.i("direction", swne(facing));
        o.b("attached_bit", truth("attached"));
        o.b("powered_bit", truth("powered"));
        return o.st;
    }
    if (path == "light") {
        int level = toInt(p("level"), 15);
        o.name("light_block_" + std::to_string(level));
        o.alt("light_block");
        o.i("block_light_level", level);
        return o.st;
    }
    if (path == "scaffolding") {
        o.name("scaffolding");
        o.i("stability", toInt(p("distance")));
        o.b("stability_check", false);
        return o.st;
    }
    if (path == "pointed_dripstone") {
        o.name("pointed_dripstone");
        o.b("hanging", p("vertical_direction") == "down");
        auto th = p("thickness");
        o.s("dripstone_thickness", th == "tip_merge" ? "merge" : th.empty() ? "tip" : th);
        return o.st;
    }
    if (path == "brown_mushroom_block" || path == "red_mushroom_block" || path == "mushroom_stem") {
        o.name(path);
        bool all = truth("north") && truth("south") && truth("east") && truth("west") && truth("up") && truth("down");
        o.i("huge_mushroom_bits", path == "mushroom_stem" ? 15 : all ? 14 : 0);
        return o.st;
    }
    if (path == "piston" || path == "sticky_piston") {
        o.name(path);
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        return o.st;
    }
    if (path == "end_rod" || path == "lightning_rod") {
        o.name(path);
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        o.s("minecraft:block_face", facing);
        return o.st;
    }
    if (endsWith(path, "_bud") || path == "amethyst_cluster") {
        o.name(path);
        o.s("minecraft:block_face", facing);
        return o.st;
    }
    if (path == "barrel") {
        o.name("barrel");
        o.i("facing_direction", std::max(0, facingIndex(facing)));
        o.b("open_bit", truth("open"));
        return o.st;
    }
    if (path == "beehive" || path == "bee_nest") {
        o.name(path);
        o.i("direction", swne(facing));
        o.i("honey_level", toInt(p("honey_level")));
        return o.st;
    }
    if (endsWith(path, "_leaves")) {
        auto it = renames().find(path);
        o.name(it != renames().end() ? std::string(it->second) : path);
        o.b("persistent_bit", truth("persistent"));
        o.b("update_bit", false);
        return o.st;
    }

    // ---- renames and generic properties ----------------------------------------------------------
    auto it = renames().find(path);
    std::string target = it != renames().end() ? std::string(it->second) : path;
    o.name(target);
    if (auto a = alternatives().find(target); a != alternatives().end()) o.alt(std::string(a->second));
    if (js.has("axis")) o.s("pillar_axis", p("axis"));
    if (!facing.empty()) facingStates(o, facing);
    if (js.has("powered")) o.b("powered_bit", truth("powered"));
    if (js.has("open")) o.b("open_bit", truth("open"));
    if (js.has("lit")) o.b("lit", truth("lit"));
    if (js.has("hanging")) o.b("hanging", truth("hanging"));
    if (js.has("age")) {
        o.i("age", toInt(p("age")));
        o.i("growth", toInt(p("age")));
    }
    if (js.has("half")) {
        o.b("upside_down_bit", p("half") == "top");
        o.b("upper_block_bit", p("half") == "upper");
    }
    if (js.has("power")) o.i("redstone_signal", toInt(p("power")));
    return o.st;
}

} // namespace lv::core::schematics
