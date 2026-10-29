#include "core/ponder/Scene.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>

namespace lv::core::ponder {

bool Box::contains(Vec3i const& p) const {
    return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
}

unsigned paletteColour(Palette p) {
    switch (p) {
    case Palette::White: return 0xFFEEEEEE;
    case Palette::Black: return 0xFF221111;
    case Palette::Red: return 0xFFFF5D6C;
    case Palette::Green: return 0xFF8CBA51;
    case Palette::Blue: return 0xFF5F6CAF;
    case Palette::Slow: return 0xFF22FF22;
    case Palette::Medium: return 0xFF0084FF;
    case Palette::Fast: return 0xFFFF55FF;
    case Palette::Input: return 0xFF7FCDE0;
    case Palette::Output: return 0xFFDDC166;
    }
    return 0xFFEEEEEE;
}

Vec3f slideOffset(Dir d) {
    switch (d) {
    case Dir::Down: return {0, 1, 0}; // moving down: starts above
    case Dir::Up: return {0, -1, 0};
    case Dir::North: return {0, 0, 1}; // moving north (-z): starts south
    case Dir::South: return {0, 0, -1};
    case Dir::West: return {1, 0, 0};
    case Dir::East: return {-1, 0, 0};
    case Dir::None: break;
    }
    return {0, 0, 0};
}

// ---- parsing -------------------------------------------------------------------------------

namespace {

struct Parser {
    std::vector<std::string>& errors;
    std::string               where;

    void err(std::string const& msg) { errors.push_back(where + ": " + msg); }

    std::optional<Vec3i> vec3i(Json const& j) {
        if (!j.is_array() || j.size() != 3 || !j[0].is_number_integer() || !j[1].is_number_integer() || !j[2].is_number_integer()) {
            return std::nullopt;
        }
        return Vec3i{j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
    }

    std::optional<Vec3f> vec3f(Json const& j) {
        if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number()) return std::nullopt;
        return Vec3f{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    }

    std::optional<Area> area(Json const& j) {
        Area a;
        if (j.is_string()) {
            if (j.get<std::string>() == "base") {
                a.base = true;
                return a;
            }
            err("unknown area '" + j.get<std::string>() + "'");
            return std::nullopt;
        }
        if (j.is_object()) {
            auto u = j.find("union");
            if (u == j.end() || !u->is_array() || j.size() != 1) {
                err("an area object needs exactly \"union\": [...]");
                return std::nullopt;
            }
            for (auto const& part : *u) {
                auto sub = area(part);
                if (!sub) return std::nullopt;
                a.base = a.base || sub->base;
                a.boxes.insert(a.boxes.end(), sub->boxes.begin(), sub->boxes.end());
            }
            return a;
        }
        if (auto p = vec3i(j)) {
            a.boxes.push_back({*p, *p});
            return a;
        }
        if (j.is_array() && j.size() == 2) {
            auto p = vec3i(j[0]), q = vec3i(j[1]);
            if (p && q) {
                Box b{{std::min(p->x, q->x), std::min(p->y, q->y), std::min(p->z, q->z)},
                      {std::max(p->x, q->x), std::max(p->y, q->y), std::max(p->z, q->z)}};
                a.boxes.push_back(b);
                return a;
            }
        }
        err("bad area " + j.dump());
        return std::nullopt;
    }

    std::optional<States> states(Json const& j, StringStates* strings = nullptr) {
        States s;
        if (j.is_null()) return s;
        if (!j.is_object()) {
            err("\"states\" must be an object");
            return std::nullopt;
        }
        for (auto const& [k, v] : j.items()) {
            if (v.is_boolean()) s[k] = v.get<bool>() ? 1 : 0;
            else if (v.is_number_integer()) s[k] = v.get<int>();
            else if (v.is_string() && strings) (*strings)[k] = v.get<std::string>();
            else {
                err("state '" + k + "' must be an integer, a boolean or a string");
                return std::nullopt;
            }
        }
        return s;
    }

    bool blockId(std::string const& id) {
        auto colon = id.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= id.size()) {
            err("block / item id '" + id + "' needs a namespace (levitate: or minecraft:)");
            return false;
        }
        for (char c : id) {
            if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '.')) {
                err("bad character in id '" + id + "'");
                return false;
            }
        }
        return true;
    }

    std::optional<Dir> dir(Json const& j, Dir def) {
        if (j.is_null()) return def;
        if (!j.is_string()) {
            err("direction must be a string");
            return std::nullopt;
        }
        static std::map<std::string, Dir> const names{
            {"none", Dir::None},   {"down", Dir::Down}, {"up", Dir::Up},     {"north", Dir::North},
            {"south", Dir::South}, {"west", Dir::West}, {"east", Dir::East},
        };
        auto it = names.find(j.get<std::string>());
        if (it == names.end()) {
            err("unknown direction '" + j.get<std::string>() + "'");
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<Palette> palette(Json const& j) {
        if (j.is_null()) return Palette::White;
        static std::map<std::string, Palette> const names{
            {"white", Palette::White}, {"black", Palette::Black},   {"red", Palette::Red},     {"green", Palette::Green},
            {"blue", Palette::Blue},   {"slow", Palette::Slow},     {"medium", Palette::Medium}, {"fast", Palette::Fast},
            {"input", Palette::Input}, {"output", Palette::Output},
        };
        if (!j.is_string() || !names.contains(j.get<std::string>())) {
            err("unknown colour " + j.dump());
            return std::nullopt;
        }
        return names.at(j.get<std::string>());
    }
};

Json field(Json const& j, char const* name) {
    auto it = j.find(name);
    return it == j.end() ? Json() : *it;
}

int positiveTicks(Parser& p, Json const& j, int def) {
    if (j.is_null()) return def;
    if (!j.is_number_integer() || j.get<int>() <= 0) {
        p.err("\"ticks\" must be a positive integer");
        return def;
    }
    return j.get<int>();
}

bool inside(Scene const& s, Vec3i const& p) { return p.x >= 0 && p.z >= 0 && p.y >= 0 && p.x < s.sizeX && p.z < s.sizeZ && p.y <= s.height + 8; }

void checkArea(Parser& p, Scene const& s, Area const& a) {
    for (auto const& b : a.boxes) {
        if (!inside(s, b.lo) || !inside(s, b.hi)) p.err("area outside the scene (base " + std::to_string(s.sizeX) + "x" + std::to_string(s.sizeZ) + ")");
    }
}

std::set<std::string> const& allowedKeys(Op op) {
    static std::map<Op, std::set<std::string>> const keys{
        {Op::Idle, {"idle"}},
        {Op::Show, {"show", "from", "as"}},
        {Op::Hide, {"hide", "to"}},
        {Op::Set, {"set", "id", "states"}},
        {Op::Destroy, {"destroy"}},
        {Op::Modify, {"modify", "states"}},
        {Op::Text, {"text", "at", "ticks", "color", "keyframe", "near"}},
        {Op::Outline, {"outline", "color", "ticks"}},
        {Op::Input, {"input", "action", "item", "sneak", "ctrl", "ticks", "keyframe"}},
        {Op::Redstone, {"redstone", "ticks"}},
        {Op::Rotate, {"rotate", "rpm"}},
        {Op::Move, {"move", "by", "ticks"}},
        {Op::Turn, {"turn", "axis", "degrees", "origin", "ticks"}},
        {Op::Item, {"item", "at", "to", "ticks"}},
        {Op::Keyframe, {"keyframe"}},
    };
    return keys.at(op);
}

std::optional<Instruction> instruction(Parser& p, Scene const& s, Json const& j, std::set<std::string>& sections) {
    if (!j.is_object()) {
        p.err("an instruction must be an object");
        return std::nullopt;
    }
    static std::vector<std::pair<char const*, Op>> const ops{
        {"idle", Op::Idle},       {"show", Op::Show},     {"hide", Op::Hide},       {"set", Op::Set},
        {"destroy", Op::Destroy}, {"modify", Op::Modify}, {"text", Op::Text},       {"outline", Op::Outline},
        {"input", Op::Input},     {"redstone", Op::Redstone}, {"rotate", Op::Rotate}, {"move", Op::Move},
        {"turn", Op::Turn},       {"item", Op::Item},
    };
    Instruction in;
    char const* opName = nullptr;
    for (auto const& [name, op] : ops) {
        if (j.contains(name) && !(std::string_view(name) == "item" && j.contains("input"))) {
            if (opName) {
                p.err(std::string("an instruction has both \"") + opName + "\" and \"" + name + "\"");
                return std::nullopt;
            }
            opName = name;
            in.op  = op;
        }
    }
    if (!opName) {
        if (j.contains("keyframe") && j.size() == 1) {
            in.op       = Op::Keyframe;
            in.keyframe = true;
            return in;
        }
        p.err("unknown instruction " + j.dump());
        return std::nullopt;
    }
    for (auto const& [k, v] : j.items()) {
        if (!allowedKeys(in.op).contains(k)) p.err(std::string("\"") + opName + "\" does not take \"" + k + "\"");
    }
    auto const& main = j.at(opName);
    auto        needArea = [&]() -> bool {
        auto a = p.area(main);
        if (!a) return false;
        checkArea(p, s, *a);
        in.area = *a;
        return true;
    };
    auto point = [&](char const* name, Vec3f& out) -> bool {
        auto v = field(j, name);
        if (v.is_null()) return false;
        auto f = p.vec3f(v);
        if (!f) {
            p.err(std::string("\"") + name + "\" must be [x, y, z]");
            return false;
        }
        out = *f;
        return true;
    };
    auto flag = [&](char const* name, bool def) {
        auto v = field(j, name);
        if (v.is_null()) return def;
        if (!v.is_boolean()) {
            p.err(std::string("\"") + name + "\" must be a boolean");
            return def;
        }
        return v.get<bool>();
    };

    switch (in.op) {
    case Op::Idle:
        if (!main.is_number_integer() || main.get<int>() <= 0) p.err("\"idle\" needs a positive tick count");
        else in.ticks = main.get<int>();
        break;
    case Op::Show: {
        if (!needArea()) return std::nullopt;
        if (auto d = p.dir(field(j, "from"), Dir::Down)) in.dir = *d;
        auto as = field(j, "as");
        if (as.is_string()) {
            in.section = as.get<std::string>();
            sections.insert(in.section);
        } else if (!as.is_null()) p.err("\"as\" must be a section name");
        break;
    }
    case Op::Hide:
        if (!needArea()) return std::nullopt;
        if (auto d = p.dir(field(j, "to"), Dir::Up)) in.dir = *d;
        break;
    case Op::Set: {
        if (!needArea()) return std::nullopt;
        auto id = field(j, "id");
        if (!id.is_string()) {
            p.err("\"set\" needs an \"id\"");
            return std::nullopt;
        }
        in.block.id = id.get<std::string>();
        p.blockId(in.block.id);
        if (auto st = p.states(field(j, "states"), &in.block.strings)) in.block.states = *st;
        break;
    }
    case Op::Destroy:
        if (!needArea()) return std::nullopt;
        break;
    case Op::Modify: {
        if (!needArea()) return std::nullopt;
        auto st = p.states(field(j, "states"), &in.strings);
        if (!st || (st->empty() && in.strings.empty())) p.err("\"modify\" needs \"states\"");
        else in.states = *st;
        break;
    }
    case Op::Text: {
        if (!main.is_number_integer() || main.get<int>() <= 0) {
            p.err("\"text\" needs a text number (1, 2, ...)");
            return std::nullopt;
        }
        in.text     = main.get<int>();
        in.hasPoint = point("at", in.point);
        in.ticks    = positiveTicks(p, field(j, "ticks"), 60);
        if (auto c = p.palette(field(j, "color"))) in.color = *c;
        in.keyframe = flag("keyframe", false);
        in.near     = flag("near", true);
        break;
    }
    case Op::Outline:
        if (!needArea()) return std::nullopt;
        if (auto c = p.palette(field(j, "color"))) in.color = *c;
        in.ticks = positiveTicks(p, field(j, "ticks"), 60);
        break;
    case Op::Input: {
        auto f = p.vec3f(main);
        if (!f) {
            p.err("\"input\" needs a point [x, y, z]");
            return std::nullopt;
        }
        in.point    = *f;
        in.hasPoint = true;
        auto action = field(j, "action");
        in.action   = action.is_string() ? action.get<std::string>() : "right";
        if (in.action != "right" && in.action != "left" && in.action != "scroll" && in.action != "none") {
            p.err("unknown input action '" + in.action + "'");
        }
        auto item = field(j, "item");
        if (item.is_string()) {
            in.item = item.get<std::string>();
            p.blockId(in.item);
        }
        in.sneak    = flag("sneak", false);
        in.ctrl     = flag("ctrl", false);
        in.keyframe = flag("keyframe", false);
        in.ticks    = positiveTicks(p, field(j, "ticks"), 40);
        break;
    }
    case Op::Redstone:
        if (!needArea()) return std::nullopt;
        in.ticks = positiveTicks(p, field(j, "ticks"), 20);
        break;
    case Op::Rotate: {
        if (!needArea()) return std::nullopt;
        auto rpm = field(j, "rpm");
        if (!rpm.is_number()) p.err("\"rotate\" needs \"rpm\"");
        else in.rpm = rpm.get<float>();
        break;
    }
    case Op::Move: {
        if (!main.is_string()) {
            p.err("\"move\" needs a section name");
            return std::nullopt;
        }
        in.section = main.get<std::string>();
        if (!sections.contains(in.section)) p.err("\"move\" of unknown section '" + in.section + "' (show it with \"as\" first)");
        if (!point("by", in.by)) p.err("\"move\" needs \"by\"");
        in.ticks = positiveTicks(p, field(j, "ticks"), 20);
        break;
    }
    case Op::Turn: {
        if (!main.is_string()) {
            p.err("\"turn\" needs a section name");
            return std::nullopt;
        }
        in.section = main.get<std::string>();
        if (!sections.contains(in.section)) p.err("\"turn\" of unknown section '" + in.section + "' (show it with \"as\" first)");
        auto axis = field(j, "axis");
        if (!axis.is_string() || (axis != "x" && axis != "y" && axis != "z")) p.err("\"turn\" needs \"axis\": x, y or z");
        else in.axis = axis.get<std::string>()[0];
        auto deg = field(j, "degrees");
        if (!deg.is_number()) p.err("\"turn\" needs \"degrees\"");
        else in.degrees = deg.get<float>();
        if (!point("origin", in.point)) p.err("\"turn\" needs \"origin\"");
        in.ticks = positiveTicks(p, field(j, "ticks"), 20);
        break;
    }
    case Op::Item: {
        if (!main.is_string()) {
            p.err("\"item\" needs an item id");
            return std::nullopt;
        }
        in.item = main.get<std::string>();
        p.blockId(in.item);
        if (!point("at", in.point)) p.err("\"item\" needs \"at\"");
        in.to = in.point;
        point("to", in.to);
        in.ticks = positiveTicks(p, field(j, "ticks"), 20);
        break;
    }
    case Op::Keyframe: break;
    }
    return in;
}

} // namespace

ParseResult parseScene(Json const& j) {
    ParseResult r;
    Parser      p{r.errors, "scene"};
    if (!j.is_object()) {
        p.err("a scene must be an object");
        return r;
    }
    static std::set<std::string> const topKeys{"id", "size", "height", "scale", "blocks", "timeline"};
    for (auto const& [k, v] : j.items()) {
        if (!topKeys.contains(k)) p.err("unknown key \"" + k + "\"");
    }
    Scene s;
    auto  id = field(j, "id");
    if (!id.is_string() || id.get<std::string>().empty()) p.err("missing \"id\"");
    else {
        s.id    = id.get<std::string>();
        p.where = s.id;
        for (char c : s.id) {
            if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) {
                p.err("scene ids use lowercase letters, digits and '_'");
                break;
            }
        }
    }
    if (auto sz = field(j, "size"); !sz.is_null()) {
        if (!sz.is_array() || sz.size() != 2 || !sz[0].is_number_integer() || !sz[1].is_number_integer() || sz[0].get<int>() < 1
            || sz[1].get<int>() < 1 || sz[0].get<int>() > 32 || sz[1].get<int>() > 32) {
            p.err("\"size\" must be [x, z] with 1..32");
        } else {
            s.sizeX = sz[0].get<int>();
            s.sizeZ = sz[1].get<int>();
        }
    }
    if (auto h = field(j, "height"); h.is_number_integer() && h.get<int>() >= 1 && h.get<int>() <= 32) s.height = h.get<int>();
    else if (!h.is_null()) p.err("\"height\" must be 1..32");
    if (auto sc = field(j, "scale"); sc.is_number() && sc.get<float>() > 0.1f && sc.get<float>() <= 4.f) s.scale = sc.get<float>();
    else if (!sc.is_null()) p.err("\"scale\" must be 0.1..4");

    auto blocks = field(j, "blocks");
    if (!blocks.is_null() && !blocks.is_array()) p.err("\"blocks\" must be an array");
    else if (blocks.is_array()) {
        for (auto const& b : blocks) {
            if (!b.is_object()) {
                p.err("a block entry must be an object");
                continue;
            }
            for (auto const& [k, v] : b.items()) {
                if (k != "at" && k != "area" && k != "id" && k != "states") p.err("block entries do not take \"" + k + "\"");
            }
            auto bid = field(b, "id");
            if (!bid.is_string()) {
                p.err("a block entry needs \"id\"");
                continue;
            }
            BlockDef def{bid.get<std::string>(), {}};
            p.blockId(def.id);
            if (auto st = p.states(field(b, "states"), &def.strings)) def.states = *st;
            std::optional<Area> a;
            if (b.contains("at")) a = p.area(b.at("at"));
            else if (b.contains("area")) a = p.area(b.at("area"));
            else p.err("a block entry needs \"at\" or \"area\"");
            if (!a) continue;
            auto before = r.errors.size();
            checkArea(p, s, *a);
            if (a->base) p.err("blocks can't use the \"base\" area");
            if (r.errors.size() != before) continue; // never expand a box outside the scene
            for (auto const& box : a->boxes) {
                for (int x = box.lo.x; x <= box.hi.x; ++x)
                    for (int y = box.lo.y; y <= box.hi.y; ++y)
                        for (int z = box.lo.z; z <= box.hi.z; ++z) s.blocks[{x, y, z}] = def;
            }
        }
    }

    auto timeline = field(j, "timeline");
    if (!timeline.is_array() || timeline.empty()) p.err("\"timeline\" must be a non-empty array");
    else {
        std::set<std::string> sections;
        for (auto const& ij : timeline) {
            if (auto in = instruction(p, s, ij, sections)) s.timeline.push_back(std::move(*in));
        }
        if (s.length() <= 0) p.err("the timeline has no \"idle\": the scene would have no length");
    }
    if (r.errors.empty()) r.scene = std::move(s);
    return r;
}

int Scene::length() const {
    int t = 0;
    for (auto const& in : timeline)
        if (in.op == Op::Idle) t += in.ticks;
    return t;
}

std::vector<int> Scene::keyframes() const {
    std::set<int> out;
    int           t = 0;
    for (auto const& in : timeline) {
        if (in.op == Op::Idle) t += in.ticks;
        else if (in.keyframe) out.insert(t);
    }
    return {out.begin(), out.end()};
}

std::vector<int> Scene::textNumbers() const {
    std::set<int> out;
    for (auto const& in : timeline)
        if (in.op == Op::Text) out.insert(in.text);
    return {out.begin(), out.end()};
}

std::vector<std::string> Scene::blockIds() const {
    std::set<std::string> out;
    for (auto const& [p, b] : blocks) out.insert(b.id);
    for (auto const& in : timeline)
        if (in.op == Op::Set) out.insert(in.block.id);
    return {out.begin(), out.end()};
}

// ---- evaluation ----------------------------------------------------------------------------

namespace {

struct Cell {
    BlockDef    block;
    bool        exists  = true;
    bool        shown   = false;
    float       shownAt = -1e9f; ///< tick the fade-in started
    Dir         showDir = Dir::None;
    bool        hiding  = false;
    float       hiddenAt = 0;
    Dir         hideDir  = Dir::Up;
    std::string section;
    float       rpm = 0, angleBase = 0, rpmSince = 0;
};

bool inArea(Area const& a, Vec3i const& p, bool /*floorTile*/) {
    if (a.base && p.y == 0) return true;
    for (auto const& b : a.boxes)
        if (b.contains(p)) return true;
    return false;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

/// Smooth 0..1 ease (Create eases its section movement).
float ease(float t) {
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}

constexpr float kDegPerTickPerRpm = 0.3f; ///< 360 degrees / 1200 ticks

} // namespace

SceneState evaluate(Scene const& scene, float time) {
    SceneState st;
    st.time = time;
    std::map<Vec3i, Cell> cells;
    std::set<Vec3i>       baseCells;
    for (int x = 0; x < scene.sizeX; ++x)
        for (int z = 0; z < scene.sizeZ; ++z) {
            Vec3i p{x, 0, z};
            if (scene.blocks.contains(p)) continue;
            Cell c;
            c.block = {(x + z) % 2 == 0 ? "minecraft:snow" : "minecraft:white_concrete", {}};
            cells[p] = c;
            baseCells.insert(p);
        }
    for (auto const& [p, b] : scene.blocks) {
        Cell c;
        c.block  = b;
        cells[p] = c;
    }
    auto isBase = [&](Vec3i const& p) { return baseCells.contains(p); };

    struct Anim {
        float t0, ticks;
        Vec3f by;
        char  axis;
        float degrees;
        Vec3f origin;
        bool  turn;
    };
    std::map<std::string, std::vector<Anim>> anims;

    float t = 0;
    for (auto const& in : scene.timeline) {
        if (t > time) break;
        switch (in.op) {
        case Op::Idle: t += static_cast<float>(in.ticks); break;
        case Op::Show:
            for (auto& [p, c] : cells) {
                if (!c.exists || !inArea(in.area, p, isBase(p))) continue;
                if (!in.section.empty()) c.section = in.section;
                if (c.shown && !c.hiding) continue; // already visible: stays in place
                c.shown   = true;
                c.hiding  = false;
                c.shownAt = t;
                c.showDir = in.dir;
            }
            break;
        case Op::Hide:
            for (auto& [p, c] : cells) {
                if (!c.shown || c.hiding || !inArea(in.area, p, isBase(p))) continue;
                c.hiding   = true;
                c.hiddenAt = t;
                c.hideDir  = in.dir;
            }
            break;
        case Op::Set:
            for (auto const& b : in.area.boxes)
                for (int x = b.lo.x; x <= b.hi.x; ++x)
                    for (int y = b.lo.y; y <= b.hi.y; ++y)
                        for (int z = b.lo.z; z <= b.hi.z; ++z) {
                            auto& c = cells[{x, y, z}];
                            if (!c.exists) c.section.clear(); // a new block, not the destroyed one
                            c.block    = in.block;
                            c.exists   = true;
                            c.rpm      = 0; // placed blocks start still
                            c.angleBase = 0;
                            c.rpmSince = t;
                            if (!c.shown || c.hiding) {
                                c.shown   = true;
                                c.hiding  = false;
                                c.shownAt = -1e9f; // appears at once
                            }
                            baseCells.erase({x, y, z});
                        }
            break;
        case Op::Destroy:
            for (auto& [p, c] : cells)
                if (inArea(in.area, p, isBase(p))) c.exists = false;
            break;
        case Op::Modify:
            for (auto& [p, c] : cells) {
                if (!c.exists || !inArea(in.area, p, isBase(p))) continue;
                for (auto const& [k, v] : in.states) c.block.states[k] = v;
                for (auto const& [k, v] : in.strings) c.block.strings[k] = v;
            }
            break;
        case Op::Text:
            if (time < t + static_cast<float>(in.ticks)) {
                TextView v;
                v.text     = in.text;
                v.hasPoint = in.hasPoint;
                v.point    = in.point;
                v.color    = in.color;
                v.near     = in.near;
                v.age      = time - t;
                v.left     = t + static_cast<float>(in.ticks) - time;
                st.texts.push_back(v);
            }
            break;
        case Op::Outline:
            if (time < t + static_cast<float>(in.ticks)) st.outlines.push_back({in.area, in.color, time - t, t + in.ticks - time});
            break;
        case Op::Input:
            if (time < t + static_cast<float>(in.ticks)) {
                st.inputs.push_back({in.point, in.action, in.item, in.sneak, in.ctrl, time - t, t + in.ticks - time});
            }
            break;
        case Op::Redstone:
            if (time < t + static_cast<float>(in.ticks)) st.redstone.push_back({in.area, time - t, t + in.ticks - time});
            break;
        case Op::Rotate:
            for (auto& [p, c] : cells) {
                if (!c.exists || !inArea(in.area, p, isBase(p))) continue;
                c.angleBase += c.rpm * kDegPerTickPerRpm * (t - c.rpmSince);
                c.rpm      = in.rpm;
                c.rpmSince = t;
            }
            break;
        case Op::Move: anims[in.section].push_back({t, static_cast<float>(in.ticks), in.by, 'y', 0, {}, false}); break;
        case Op::Turn: anims[in.section].push_back({t, static_cast<float>(in.ticks), {}, in.axis, in.degrees, in.point, true}); break;
        case Op::Item:
            if (time < t + static_cast<float>(in.ticks)) {
                float k = clamp01((time - t) / static_cast<float>(in.ticks));
                st.items.push_back({in.item,
                                    {in.point.x + (in.to.x - in.point.x) * k, in.point.y + (in.to.y - in.point.y) * k,
                                     in.point.z + (in.to.z - in.point.z) * k}});
            }
            break;
        case Op::Keyframe: break;
        }
    }

    // sections: accumulated movement / rotation
    for (auto const& [name, list] : anims) {
        SectionView sv;
        for (auto const& a : list) {
            float k = a.ticks > 0 ? ease((time - a.t0) / a.ticks) : 1.f;
            if (a.turn) {
                sv.turns.push_back({a.axis, a.degrees * k, a.origin});
            } else {
                sv.offset.x += a.by.x * k;
                sv.offset.y += a.by.y * k;
                sv.offset.z += a.by.z * k;
            }
        }
        st.sections[name] = sv;
    }

    for (auto const& [p, c] : cells) {
        if (!c.exists || !c.shown) continue;
        BlockView v;
        v.pos     = p;
        v.block   = c.block;
        v.section = c.section;
        v.base    = isBase(p);
        float in  = clamp01((time - c.shownAt) / kFadeTicks);
        float out = c.hiding ? 1.f - clamp01((time - c.hiddenAt) / kFadeTicks) : 1.f;
        v.alpha   = std::min(in, out);
        if (v.alpha <= 0.f) continue;
        Vec3f off{};
        if (in < 1.f) {
            auto s = slideOffset(c.showDir);
            float k = 1.f - ease(in);
            off = {s.x * k, s.y * k, s.z * k};
        }
        if (out < 1.f) {
            auto s = slideOffset(c.hideDir);
            float k = 1.f - ease(out);
            off.x -= s.x * k, off.y -= s.y * k, off.z -= s.z * k;
        }
        if (auto it = st.sections.find(c.section); !c.section.empty() && it != st.sections.end()) {
            off.x += it->second.offset.x, off.y += it->second.offset.y, off.z += it->second.offset.z;
        }
        v.offset = off;
        v.rpm    = c.rpm;
        v.angle  = std::fmod(c.angleBase + c.rpm * kDegPerTickPerRpm * (time - c.rpmSince), 360.f);
        st.blocks.push_back(std::move(v));
    }
    std::sort(st.texts.begin(), st.texts.end(), [](TextView const& a, TextView const& b) { return a.age > b.age; });
    for (std::size_t i = 0; i < st.texts.size(); ++i) st.texts[i].index = static_cast<int>(i);
    return st;
}

// ---- library -------------------------------------------------------------------------------

void Library::add(Scene scene) {
    auto id     = scene.id;
    mScenes[id] = std::move(scene);
}

std::vector<std::string> Library::setIndex(Json const& j) {
    std::vector<std::string> errors;
    mTags.clear();
    mComponents.clear();
    if (!j.is_object()) {
        errors.push_back("index.json: must be an object");
        return errors;
    }
    auto comps = field(j, "components");
    if (!comps.is_array()) errors.push_back("index.json: needs \"components\": [...]");
    else {
        std::set<std::string> seen;
        for (auto const& c : comps) {
            if (!c.is_object() || !field(c, "id").is_string() || !field(c, "scenes").is_array()) {
                errors.push_back("index.json: each component needs \"id\" and \"scenes\"");
                continue;
            }
            Component comp{c.at("id").get<std::string>(), {}};
            if (!seen.insert(comp.id).second) errors.push_back("index.json: duplicate component " + comp.id);
            for (auto const& s : c.at("scenes")) {
                if (!s.is_string()) errors.push_back("index.json: " + comp.id + ": scenes are ids");
                else comp.scenes.push_back(s.get<std::string>());
            }
            mComponents.push_back(std::move(comp));
        }
    }
    auto tags = field(j, "tags");
    if (!tags.is_array()) {
        errors.push_back("index.json: needs \"tags\": [...]");
        return errors;
    }
    std::set<std::string> seen;
    for (auto const& t : tags) {
        Tag tag;
        if (!t.is_object() || !field(t, "id").is_string() || !field(t, "icon").is_string() || !field(t, "items").is_array()) {
            errors.push_back("index.json: each tag needs \"id\", \"icon\" and \"items\"");
            continue;
        }
        tag.id    = t.at("id").get<std::string>();
        tag.icon  = t.at("icon").get<std::string>();
        tag.index = field(t, "index").is_boolean() ? t.at("index").get<bool>() : true;
        if (!seen.insert(tag.id).second) errors.push_back("index.json: duplicate tag " + tag.id);
        for (auto const& i : t.at("items")) {
            if (!i.is_string()) errors.push_back("index.json: " + tag.id + ": items are ids");
            else tag.items.push_back(i.get<std::string>());
        }
        mTags.push_back(std::move(tag));
    }
    return errors;
}

std::vector<std::string> Library::missingScenes() const {
    std::vector<std::string> out;
    for (auto const& c : mComponents)
        for (auto const& s : c.scenes)
            if (!mScenes.contains(s)) out.push_back(c.id + " -> " + s);
    return out;
}

Library::Component const* Library::component(std::string const& id) const {
    for (auto const& c : mComponents)
        if (c.id == id) return &c;
    return nullptr;
}

bool Library::has(std::string const& component) const { return !scenesOf(component).empty(); }

std::vector<Scene const*> Library::scenesOf(std::string const& id) const {
    std::vector<Scene const*> out;
    if (auto const* c = component(id)) {
        for (auto const& s : c->scenes)
            if (auto it = mScenes.find(s); it != mScenes.end()) out.push_back(&it->second);
    }
    return out;
}

Scene const* Library::scene(std::string const& id) const {
    auto it = mScenes.find(id);
    return it == mScenes.end() ? nullptr : &it->second;
}

std::vector<Tag const*> Library::tagsOf(std::string const& component) const {
    std::vector<Tag const*> out;
    for (auto const& t : mTags)
        if (std::find(t.items.begin(), t.items.end(), component) != t.items.end()) out.push_back(&t);
    return out;
}

Tag const* Library::tag(std::string const& id) const {
    for (auto const& t : mTags)
        if (t.id == id) return &t;
    return nullptr;
}

std::vector<std::string> Library::components() const {
    std::vector<std::string> out;
    for (auto const& c : mComponents)
        if (has(c.id)) out.push_back(c.id);
    return out;
}

std::string normaliseId(std::string const& id) {
    if (id.empty() || id.find(':') != std::string::npos) return id;
    return "levitate:" + id;
}

float holdProgressStep(float progress, bool held) {
    if (held) return std::min(1.0f, progress + std::max(0.25f, progress) * 0.25f);
    return std::max(0.0f, progress - 0.05f);
}

std::vector<std::size_t> utf8Boundaries(std::string const& s) {
    std::vector<std::size_t> out{0};
    for (std::size_t i = 1; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) out.push_back(i);
    }
    if (!s.empty()) out.push_back(s.size());
    return out;
}

} // namespace lv::core::ponder
