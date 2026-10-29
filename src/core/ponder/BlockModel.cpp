#include "core/ponder/BlockModel.h"

#include <cctype>
#include <cmath>
#include <variant>

namespace lv::core::ponder {

using Json = nlohmann::json;

// ---- Molang ----------------------------------------------------------------------------------------

namespace {

using Value = std::variant<float, std::string>;

struct MolangParser {
    std::string const&                        s;
    std::map<std::string, int> const&         states;
    std::map<std::string, std::string> const& strings;
    std::size_t                               i  = 0;
    bool                                      ok = true;

    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool eat(std::string_view tok) {
        ws();
        if (s.compare(i, tok.size(), tok) == 0) {
            i += tok.size();
            return true;
        }
        return false;
    }
    static float num(Value const& v) { return std::holds_alternative<float>(v) ? std::get<float>(v) : 0.f; }
    static bool  truthy(Value const& v) { return std::holds_alternative<float>(v) ? std::get<float>(v) != 0.f : !std::get<std::string>(v).empty(); }

    Value orExpr() {
        Value l = andExpr();
        while (ok && eat("||")) {
            Value r = andExpr();
            l       = (truthy(l) || truthy(r)) ? 1.f : 0.f;
        }
        return l;
    }
    Value andExpr() {
        Value l = cmpExpr();
        while (ok && eat("&&")) {
            Value r = cmpExpr();
            l       = (truthy(l) && truthy(r)) ? 1.f : 0.f;
        }
        return l;
    }
    Value cmpExpr() {
        Value l = unary();
        for (auto op : {"==", "!=", "<=", ">=", "<", ">"}) {
            if (!eat(op)) continue;
            Value r = unary();
            std::string o(op);
            if (std::holds_alternative<std::string>(l) || std::holds_alternative<std::string>(r)) {
                bool eq = l == r;
                if (o == "==") return eq ? 1.f : 0.f;
                if (o == "!=") return eq ? 0.f : 1.f;
                return 0.f;
            }
            float a = num(l), b = num(r);
            if (o == "==") return a == b ? 1.f : 0.f;
            if (o == "!=") return a != b ? 1.f : 0.f;
            if (o == "<=") return a <= b ? 1.f : 0.f;
            if (o == ">=") return a >= b ? 1.f : 0.f;
            if (o == "<") return a < b ? 1.f : 0.f;
            return a > b ? 1.f : 0.f;
        }
        return l;
    }
    Value unary() {
        ws();
        if (eat("!")) return truthy(unary()) ? 0.f : 1.f;
        if (eat("(")) {
            Value v = orExpr();
            if (!eat(")")) ok = false;
            return v;
        }
        return primary();
    }
    std::string str() {
        ws();
        if (i >= s.size() || (s[i] != '\'' && s[i] != '"')) {
            ok = false;
            return {};
        }
        char q = s[i++];
        auto e = s.find(q, i);
        if (e == std::string::npos) {
            ok = false;
            return {};
        }
        auto out = s.substr(i, e - i);
        i        = e + 1;
        return out;
    }
    Value primary() {
        ws();
        if (i >= s.size()) {
            ok = false;
            return 0.f;
        }
        char c = s[i];
        if (c == '\'' || c == '"') return str();
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.') {
            std::size_t start = i++;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
            try {
                return std::stof(s.substr(start, i - start));
            } catch (...) {
                ok = false;
                return 0.f;
            }
        }
        std::size_t start = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.')) ++i;
        std::string ident = s.substr(start, i - start);
        for (auto& ch : ident) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ident == "true") return 1.f;
        if (ident == "false") return 0.f;
        if (ident == "q.block_state" || ident == "query.block_state") {
            if (!eat("(")) {
                ok = false;
                return 0.f;
            }
            auto name = str();
            if (!eat(")")) ok = false;
            if (auto it = states.find(name); it != states.end()) return static_cast<float>(it->second);
            if (auto it = strings.find(name); it != strings.end()) return it->second;
            return 0.f;
        }
        ok = false;
        return 0.f;
    }
};

V3 vec3(Json const& j, V3 fallback = {0, 0, 0}) {
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number()) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

/// Bedrock's rotation of geometry (cube / bone): Z, then Y, then X about `pivot` (degrees).
V3 rotateZYX(V3 p, V3 const& pivot, V3 const& deg) {
    constexpr float kRad = 3.14159265358979f / 180.f;
    for (int k = 0; k < 3; ++k) p[k] -= pivot[k];
    if (deg[2] != 0) {
        float c = std::cos(deg[2] * kRad), s = std::sin(deg[2] * kRad);
        p = {p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2]};
    }
    if (deg[1] != 0) {
        float c = std::cos(deg[1] * kRad), s = std::sin(deg[1] * kRad);
        p = {p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c};
    }
    if (deg[0] != 0) {
        float c = std::cos(deg[0] * kRad), s = std::sin(deg[0] * kRad);
        p = {p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c};
    }
    for (int k = 0; k < 3; ++k) p[k] += pivot[k];
    return p;
}

/// The block transformation: right-handed rotation X, then Y, then Z about the block centre.
V3 rotateXYZ(V3 p, V3 const& deg) {
    constexpr float kRad = 3.14159265358979f / 180.f;
    for (int k = 0; k < 3; ++k) p[k] -= 0.5f;
    if (deg[0] != 0) {
        float c = std::cos(deg[0] * kRad), s = std::sin(deg[0] * kRad);
        p = {p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c};
    }
    if (deg[1] != 0) {
        float c = std::cos(deg[1] * kRad), s = std::sin(deg[1] * kRad);
        p = {p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c};
    }
    if (deg[2] != 0) {
        float c = std::cos(deg[2] * kRad), s = std::sin(deg[2] * kRad);
        p = {p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2]};
    }
    for (int k = 0; k < 3; ++k) p[k] += 0.5f;
    return p;
}

struct FaceUv {
    float       u = 0, v = 0, w = 0, h = 0;
    std::string material;
};

} // namespace

std::optional<float> evalMolang(std::string const& expr, std::map<std::string, int> const& states, std::map<std::string, std::string> const& strings) {
    MolangParser p{expr, states, strings};
    Value        v = p.orExpr();
    p.ws();
    if (!p.ok || p.i != expr.size()) return std::nullopt;
    return std::holds_alternative<float>(v) ? std::get<float>(v) : (std::get<std::string>(v).empty() ? 0.f : 1.f);
}

// ---- geometry ----------------------------------------------------------------------------------------

std::map<std::string, Geometry> parseGeometries(Json const& file) {
    std::map<std::string, Geometry> out;
    if (!file.is_object()) return out;
    auto list = file.find("minecraft:geometry");
    if (list == file.end() || !list->is_array()) return out;
    for (auto const& geo : *list) {
        if (!geo.is_object()) continue;
        auto const& desc = geo.value("description", Json::object());
        auto        id   = desc.value("identifier", std::string());
        if (id.empty()) continue;
        float tw = desc.value("texture_width", 16.f), th = desc.value("texture_height", 16.f);
        auto  bones = geo.value("bones", Json::array());
        // bone transforms (pivot, rotation, parent) for the parent chain
        struct BoneT {
            V3          pivot{0, 0, 0}, rot{0, 0, 0};
            std::string parent;
        };
        std::map<std::string, BoneT> bt;
        for (auto const& b : bones) {
            if (!b.is_object()) continue;
            bt[b.value("name", std::string())] = {vec3(b.value("pivot", Json())), vec3(b.value("rotation", Json())), b.value("parent", std::string())};
        }
        auto& g = out[id];
        for (auto const& bone : bones) {
            if (!bone.is_object()) continue;
            auto name = bone.value("name", std::string());
            for (auto const& cube : bone.value("cubes", Json::array())) {
                V3    o  = vec3(cube.value("origin", Json()));
                V3    s  = vec3(cube.value("size", Json()));
                float in = cube.value("inflate", 0.f);
                V3    pivot = vec3(cube.value("pivot", Json()), {o[0] + s[0] / 2, o[1] + s[1] / 2, o[2] + s[2] / 2});
                V3    rot   = vec3(cube.value("rotation", Json()));
                float x0 = o[0] - in, y0 = o[1] - in, z0 = o[2] - in, x1 = o[0] + s[0] + in, y1 = o[1] + s[1] + in, z1 = o[2] + s[2] + in;

                std::map<std::string, FaceUv> uvs;
                auto                          uvj = cube.value("uv", Json());
                if (uvj.is_object()) {
                    for (auto const& [face, f] : uvj.items()) {
                        if (!f.is_object()) continue;
                        auto uv = f.value("uv", Json::array({0, 0}));
                        auto sz = f.value("uv_size", Json::array({0, 0}));
                        if (!uv.is_array() || uv.size() != 2 || !sz.is_array() || sz.size() != 2) continue;
                        uvs[face] = FaceUv{uv[0].get<float>(), uv[1].get<float>(), sz[0].get<float>(), sz[1].get<float>(), f.value("material_instance", face)};
                    }
                } else if (uvj.is_array() && uvj.size() == 2) { // box UV
                    float u = uvj[0].get<float>(), v = uvj[1].get<float>();
                    float sx = s[0], sy = s[1], sz = s[2];
                    uvs["up"]    = {u + sz, v, sx, sz, "up"};
                    uvs["down"]  = {u + sz + sx, v, sx, sz, "down"};
                    uvs["west"]  = {u, v + sz, sz, sy, "west"};
                    uvs["north"] = {u + sz, v + sz, sx, sy, "north"};
                    uvs["east"]  = {u + sz + sx, v + sz, sz, sy, "east"};
                    uvs["south"] = {u + 2 * sz + sx, v + sz, sx, sy, "south"};
                }
                std::map<std::string, std::array<V3, 4>> faces = {
                    {"north", {V3{x1, y1, z0}, V3{x0, y1, z0}, V3{x0, y0, z0}, V3{x1, y0, z0}}},
                    {"south", {V3{x0, y1, z1}, V3{x1, y1, z1}, V3{x1, y0, z1}, V3{x0, y0, z1}}},
                    {"east", {V3{x1, y1, z1}, V3{x1, y1, z0}, V3{x1, y0, z0}, V3{x1, y0, z1}}},
                    {"west", {V3{x0, y1, z0}, V3{x0, y1, z1}, V3{x0, y0, z1}, V3{x0, y0, z0}}},
                    {"up", {V3{x0, y1, z0}, V3{x1, y1, z0}, V3{x1, y1, z1}, V3{x0, y1, z1}}},
                    {"down", {V3{x0, y0, z1}, V3{x1, y0, z1}, V3{x1, y0, z0}, V3{x0, y0, z0}}},
                };
                for (auto const& [fname, corners] : faces) {
                    auto it = uvs.find(fname);
                    if (it == uvs.end()) continue;
                    auto const& f = it->second;
                    ModelFace   mf;
                    mf.bone     = name;
                    mf.material = f.material;
                    mf.face     = fname;
                    std::array<V2, 4> uv{{{f.u / tw, f.v / th}, {(f.u + f.w) / tw, f.v / th}, {(f.u + f.w) / tw, (f.v + f.h) / th}, {f.u / tw, (f.v + f.h) / th}}};
                    for (int k = 0; k < 4; ++k) {
                        V3 p = rotateZYX(corners[static_cast<std::size_t>(k)], pivot, rot);
                        // the bone and its parents
                        std::string cur = name;
                        for (int depth = 0; depth < 16 && !cur.empty(); ++depth) {
                            auto b = bt.find(cur);
                            if (b == bt.end()) break;
                            p   = rotateZYX(p, b->second.pivot, b->second.rot);
                            cur = b->second.parent;
                        }
                        mf.pos[static_cast<std::size_t>(k)] = {-p[0] / 16.f + .5f, p[1] / 16.f, p[2] / 16.f + .5f};
                        mf.uv[static_cast<std::size_t>(k)]  = uv[static_cast<std::size_t>(k)];
                    }
                    g.faces.push_back(std::move(mf));
                }
            }
        }
    }
    return out;
}

Geometry const& fullBlock() {
    static Geometry g = [] {
        Geometry out;
        auto     file = Json::parse(R"({"minecraft:geometry": [{"description": {"identifier": "full", "texture_width": 16, "texture_height": 16},
            "bones": [{"name": "block", "cubes": [{"origin": [-8, 0, -8], "size": [16, 16, 16], "uv": {
            "north": {"uv": [0, 0], "uv_size": [16, 16]}, "south": {"uv": [0, 0], "uv_size": [16, 16]},
            "east": {"uv": [0, 0], "uv_size": [16, 16]}, "west": {"uv": [0, 0], "uv_size": [16, 16]},
            "up": {"uv": [0, 0], "uv_size": [16, 16]}, "down": {"uv": [0, 0], "uv_size": [16, 16]}}}]}]}]})");
        out = parseGeometries(file)["full"];
        return out;
    }();
    return g;
}

// ---- block definitions ----------------------------------------------------------------------------

std::optional<BlockFile> parseBlockFile(Json const& file) {
    if (!file.is_object()) return std::nullopt;
    auto b = file.find("minecraft:block");
    if (b == file.end() || !b->is_object()) return std::nullopt;
    BlockFile out;
    auto      desc = b->value("description", Json::object());
    out.id         = desc.value("identifier", std::string());
    if (out.id.empty()) return std::nullopt;
    auto states = desc.value("states", Json::object()); // keep it alive: items() only refers to it
    for (auto const& [k, v] : states.items()) {
        if (v.is_array() && !v.empty() && v[0].is_number_integer()) out.defaults[k] = v[0].get<int>();
        else if (v.is_array() && !v.empty() && v[0].is_boolean()) out.defaults[k] = v[0].get<bool>() ? 1 : 0;
        else if (v.is_object() && v.contains("values") && v["values"].is_object()) out.defaults[k] = v["values"].value("min", 0);
    }
    out.components = b->value("components", Json::object());
    for (auto const& p : b->value("permutations", Json::array())) {
        if (!p.is_object()) continue;
        out.permutations.emplace_back(p.value("condition", std::string("false")), p.value("components", Json::object()));
    }
    return out;
}

namespace {
void apply(ResolvedBlock& r, Json const& comps) {
    if (!comps.is_object()) return;
    if (auto g = comps.find("minecraft:geometry"); g != comps.end()) {
        if (g->is_string()) {
            r.geometry = g->get<std::string>();
            r.boneVisibility.clear();
        } else if (g->is_object()) {
            r.geometry = g->value("identifier", std::string());
            r.boneVisibility.clear();
            auto vis = g->value("bone_visibility", Json::object());
            for (auto const& [bone, v] : vis.items()) {
                r.boneVisibility[bone] = v.is_boolean() ? (v.get<bool>() ? "true" : "false") : v.is_string() ? v.get<std::string>() : "true";
            }
        }
    }
    if (auto m = comps.find("minecraft:material_instances"); m != comps.end() && m->is_object()) {
        r.materials.clear();
        std::map<std::string, Json> raw;
        for (auto const& [k, v] : m->items()) raw[k] = v;
        for (auto const& [k, v] : raw) {
            Json cur = v;
            for (int depth = 0; depth < 8 && cur.is_string(); ++depth) { // alias to another instance
                auto it = raw.find(cur.get<std::string>());
                if (it == raw.end()) break;
                cur = it->second;
            }
            if (cur.is_object()) r.materials[k] = cur.value("texture", std::string());
        }
    }
    if (auto t = comps.find("minecraft:transformation"); t != comps.end() && t->is_object()) {
        r.rotation    = vec3(t->value("rotation", Json()));
        r.translation = vec3(t->value("translation", Json()));
        r.scale       = vec3(t->value("scale", Json()), {1, 1, 1});
    }
}
} // namespace

ResolvedBlock resolve(BlockFile const& file, std::map<std::string, int> const& given) {
    auto states = file.defaults;
    for (auto const& [k, v] : given) states[k] = v;
    ResolvedBlock r;
    apply(r, file.components);
    for (auto const& [cond, comps] : file.permutations) {
        auto v = evalMolang(cond, states);
        if (v && *v != 0.f) apply(r, comps);
    }
    return r;
}

std::string textureOf(ResolvedBlock const& block, ModelFace const& face) {
    auto find = [&](std::string const& k) -> std::string const* {
        auto it = block.materials.find(k);
        return it == block.materials.end() || it->second.empty() ? nullptr : &it->second;
    };
    if (auto const* t = find(face.material)) return *t;
    // model x is mirrored: the model's east face is the block's west face
    std::string dir = face.face == "east" ? "west" : face.face == "west" ? "east" : face.face;
    if (auto const* t = find(dir)) return *t;
    if (dir != "up" && dir != "down")
        if (auto const* t = find("side")) return *t;
    if (auto const* t = find("*")) return *t;
    return {};
}

std::vector<Quad> buildQuads(ResolvedBlock const& block, Geometry const& geometry, std::map<std::string, int> const& states, std::vector<std::string> const& hideBones) {
    std::vector<Quad> out;
    std::map<std::string, bool> visible;
    for (auto const& f : geometry.faces) {
        auto [it, fresh] = visible.try_emplace(f.bone, true);
        if (fresh) {
            if (auto bv = block.boneVisibility.find(f.bone); bv != block.boneVisibility.end()) {
                auto v    = evalMolang(bv->second, states);
                it->second = !v || *v != 0.f;
            }
            for (auto const& h : hideBones)
                if (h == f.bone) it->second = false;
        }
        if (!it->second) continue;
        Quad q;
        q.texture = textureOf(block, f);
        q.uv      = f.uv;
        for (int k = 0; k < 4; ++k) {
            V3 p = f.pos[static_cast<std::size_t>(k)];
            for (int a = 0; a < 3; ++a) p[a] = (p[a] - 0.5f) * block.scale[a] + 0.5f;
            p = rotateXYZ(p, block.rotation);
            for (int a = 0; a < 3; ++a) p[a] += block.translation[a];
            q.pos[static_cast<std::size_t>(k)] = p;
        }
        // directional shading from the face normal
        V3 e1{q.pos[1][0] - q.pos[0][0], q.pos[1][1] - q.pos[0][1], q.pos[1][2] - q.pos[0][2]};
        V3 e2{q.pos[3][0] - q.pos[0][0], q.pos[3][1] - q.pos[0][1], q.pos[3][2] - q.pos[0][2]};
        V3 n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0) {
            for (auto& c : n) c /= len;
            // geometry corners are TL, TR, BR, BL seen from outside; with the mirrored model x,
            // (TR - TL) x (BL - TL) points outwards
            float ny = n[1], ax = std::abs(n[0]), az = std::abs(n[2]);
            q.shade = ny > 0.7f ? 1.f : ny < -0.7f ? 0.5f : az >= ax ? 0.8f : 0.6f;
        }
        out.push_back(std::move(q));
    }
    return out;
}

} // namespace lv::core::ponder
