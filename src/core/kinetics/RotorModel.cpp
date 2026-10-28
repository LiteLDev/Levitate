#include "core/kinetics/RotorModel.h"

#include <cmath>

#include "nlohmann/json.hpp"

namespace lv::core::kinetics {

namespace {

using V3 = std::array<float, 3>;

V3 rotate(V3 p, V3 const& pivot, V3 const& degrees) {
    constexpr float kRad = 3.14159265358979f / 180.f;
    for (int i = 0; i < 3; ++i) p[i] -= pivot[i];
    // Bedrock applies Z, then Y, then X.
    if (degrees[2] != 0) {
        float c = std::cos(degrees[2] * kRad), s = std::sin(degrees[2] * kRad);
        p = {p[0] * c - p[1] * s, p[0] * s + p[1] * c, p[2]};
    }
    if (degrees[1] != 0) {
        float c = std::cos(degrees[1] * kRad), s = std::sin(degrees[1] * kRad);
        p = {p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c};
    }
    if (degrees[0] != 0) {
        float c = std::cos(degrees[0] * kRad), s = std::sin(degrees[0] * kRad);
        p = {p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c};
    }
    for (int i = 0; i < 3; ++i) p[i] += pivot[i];
    return p;
}

V3 vec3(nlohmann::json const& j, V3 fallback = {0, 0, 0}) {
    if (!j.is_array() || j.size() != 3) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

struct FaceUv {
    float u, v, w, h;
};

} // namespace

std::optional<std::map<std::string, RotorModel>> parseGeometry(nlohmann::json const& file, std::string* error) {
    auto fail = [&](std::string const& why) -> std::optional<std::map<std::string, RotorModel>> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!file.is_object()) return fail("not an object");
    auto list = file.find("minecraft:geometry");
    if (list == file.end() || !list->is_array()) return fail("no minecraft:geometry");

    std::map<std::string, RotorModel> out;
    for (auto const& geo : *list) {
        auto const& desc = geo.value("description", nlohmann::json::object());
        auto        id   = desc.value("identifier", std::string());
        if (id.empty()) continue;
        float tw = desc.value("texture_width", 16.f), th = desc.value("texture_height", 16.f);
        auto& model = out[id];
        for (auto const& bone : geo.value("bones", nlohmann::json::array())) {
            auto& quads = model.bones[bone.value("name", std::string())];
            for (auto const& cube : bone.value("cubes", nlohmann::json::array())) {
                V3 o = vec3(cube.value("origin", nlohmann::json()));
                V3 s = vec3(cube.value("size", nlohmann::json()));
                V3 pivot = vec3(cube.value("pivot", nlohmann::json()), {o[0] + s[0] / 2, o[1] + s[1] / 2, o[2] + s[2] / 2});
                V3 rot   = vec3(cube.value("rotation", nlohmann::json()));
                float x0 = o[0], y0 = o[1], z0 = o[2], x1 = o[0] + s[0], y1 = o[1] + s[1], z1 = o[2] + s[2];

                std::map<std::string, FaceUv> uvs;
                auto                          uvj = cube.value("uv", nlohmann::json());
                if (uvj.is_object()) {
                    for (auto const& [face, f] : uvj.items()) {
                        auto uv = f.value("uv", nlohmann::json::array({0, 0}));
                        auto sz = f.value("uv_size", nlohmann::json::array({0, 0}));
                        uvs[face] = FaceUv{uv[0].get<float>(), uv[1].get<float>(), sz[0].get<float>(), sz[1].get<float>()};
                    }
                } else if (uvj.is_array() && uvj.size() == 2) { // box UV
                    float u = uvj[0].get<float>(), v = uvj[1].get<float>();
                    float sx = s[0], sy = s[1], sz = s[2];
                    uvs["up"]    = {u + sz, v, sx, sz};
                    uvs["down"]  = {u + sz + sx, v, sx, sz};
                    uvs["west"]  = {u, v + sz, sz, sy};
                    uvs["north"] = {u + sz, v + sz, sx, sy};
                    uvs["east"]  = {u + sz + sx, v + sz, sz, sy};
                    uvs["south"] = {u + 2 * sz + sx, v + sz, sx, sy};
                }

                // corners per face, outside view: TL, TR, BR, BL (model space, before mirroring)
                std::map<std::string, std::array<V3, 4>> faces = {
                    {"north", {V3{x1, y1, z0}, V3{x0, y1, z0}, V3{x0, y0, z0}, V3{x1, y0, z0}}},
                    {"south", {V3{x0, y1, z1}, V3{x1, y1, z1}, V3{x1, y0, z1}, V3{x0, y0, z1}}},
                    {"east",  {V3{x1, y1, z1}, V3{x1, y1, z0}, V3{x1, y0, z0}, V3{x1, y0, z1}}},
                    {"west",  {V3{x0, y1, z0}, V3{x0, y1, z1}, V3{x0, y0, z1}, V3{x0, y0, z0}}},
                    {"up",    {V3{x0, y1, z0}, V3{x1, y1, z0}, V3{x1, y1, z1}, V3{x0, y1, z1}}},
                    {"down",  {V3{x0, y0, z1}, V3{x1, y0, z1}, V3{x1, y0, z0}, V3{x0, y0, z0}}},
                };
                for (auto const& [name, corners] : faces) {
                    auto it = uvs.find(name);
                    if (it == uvs.end()) continue;
                    FaceUv const& f = it->second;
                    ModelQuad     q;
                    std::array<std::array<float, 2>, 4> uv = {{
                        {f.u / tw, f.v / th},
                        {(f.u + f.w) / tw, f.v / th},
                        {(f.u + f.w) / tw, (f.v + f.h) / th},
                        {f.u / tw, (f.v + f.h) / th},
                    }};
                    for (int i = 0; i < 4; ++i) {
                        V3 p     = rotate(corners[i], pivot, rot);
                        q.pos[i] = {-p[0] / 16.f + .5f, p[1] / 16.f, p[2] / 16.f + .5f};
                        q.uv[i]  = uv[i];
                    }
                    quads.push_back(q);
                }
            }
        }
    }
    if (out.empty()) return fail("no geometry with an identifier");
    return out;
}

} // namespace lv::core::kinetics
