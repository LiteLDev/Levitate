#include "core/itemrender/DisplayItem.h"

#include <algorithm>
#include <cmath>

namespace lv::core::itemrender {

namespace {
bool close(double a, double b, double eps = 1e-3) { return std::abs(a - b) < eps; }

template <class T>
bool readArray(Json const& j, char const* key, std::size_t n, T* out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(*it)[i].is_number()) return false;
        out[i] = (*it)[i].get<T>();
    }
    return true;
}
} // namespace

bool nearlySame(Transform const& a, Transform const& b) {
    return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z) && close(a.yaw, b.yaw) && close(a.pitch, b.pitch)
        && close(a.roll, b.roll) && close(a.scale, b.scale);
}

float lerpAngle(float a, float b, float f) {
    float d = std::fmod(b - a, 360.f);
    if (d > 180.f) d -= 360.f;
    if (d < -180.f) d += 360.f;
    return a + d * f;
}

Transform lerp(Transform const& a, Transform const& b, float f) {
    f = std::clamp(f, 0.f, 1.f);
    Transform t;
    t.x     = a.x + (b.x - a.x) * f;
    t.y     = a.y + (b.y - a.y) * f;
    t.z     = a.z + (b.z - a.z) * f;
    t.yaw   = lerpAngle(a.yaw, b.yaw, f);
    t.pitch = lerpAngle(a.pitch, b.pitch, f);
    t.roll  = lerpAngle(a.roll, b.roll, f);
    t.scale = a.scale + (b.scale - a.scale) * f;
    return t;
}

float progress(double now, double start, int ticks) {
    if (ticks <= 0) return 1.f;
    return static_cast<float>(std::clamp((now - start) / ticks, 0.0, 1.0));
}

int pileCopies(int count) {
    if (count <= 1) return 0;
    int log2 = 0;
    while ((count >>= 1) > 0) ++log2;
    return log2 / 2;
}

std::array<float, 4> pileOffset(int i) {
    // a fixed small scatter (Create seeds its random with the item's position on the belt)
    static constexpr std::array<std::array<float, 4>, 4> kOffsets{{
        {.12f, .06f, -.10f, 25.f},
        {-.11f, .12f, .08f, -30.f},
        {.07f, .18f, .12f, 50.f},
        {-.09f, .24f, -.07f, -15.f},
    }};
    return kOffsets[static_cast<std::size_t>(std::max(i - 1, 0)) % kOffsets.size()];
}

Json encode(Entry const& e, bool withItem) {
    Json j{
        {"o", {e.owner[0], e.owner[1], e.owner[2]}},
        {"s", e.slot},
        {"p", {e.t.x, e.t.y, e.t.z}},
        {"r", {e.t.yaw, e.t.pitch, e.t.roll}},
        {"c", e.t.scale},
    };
    if (withItem) j["i"] = e.item;
    if (e.style != Style::Facing) j["st"] = static_cast<int>(e.style);
    if (e.spin != 0) j["sp"] = e.spin;
    if (e.pile) j["pl"] = true;
    if (e.lerp != 0) j["t"] = e.lerp;
    if (e.hide != 0) j["h"] = e.hide;
    return j;
}

std::optional<Entry> decode(Json const& j) {
    if (!j.is_object()) return std::nullopt;
    Entry e;
    if (!readArray(j, "o", 3, e.owner.data())) return std::nullopt;
    auto s = j.find("s");
    if (s == j.end() || !s->is_number_integer()) return std::nullopt;
    e.slot = s->get<std::int64_t>();
    double p[3];
    if (!readArray(j, "p", 3, p)) return std::nullopt;
    e.t.x = p[0], e.t.y = p[1], e.t.z = p[2];
    float r[3]{0, 0, 0};
    if (j.contains("r") && !readArray(j, "r", 3, r)) return std::nullopt;
    e.t.yaw = r[0], e.t.pitch = r[1], e.t.roll = r[2];
    e.t.scale = j.value("c", 1.f);
    if (auto i = j.find("i"); i != j.end()) e.item = *i;
    int style = j.value("st", 0);
    e.style   = style == 1 ? Style::Lying : Style::Facing;
    e.spin    = j.value("sp", 0.f);
    e.pile    = j.value("pl", false);
    e.lerp    = std::clamp(j.value("t", 0), 0, 200);
    e.hide    = j.value("h", std::int64_t{0});
    return e;
}

Json encodeKey(std::array<int, 3> const& owner, std::int64_t slot) { return Json::array({owner[0], owner[1], owner[2], slot}); }

std::optional<std::pair<std::array<int, 3>, std::int64_t>> decodeKey(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return std::pair{std::array<int, 3>{j[0].get<int>(), j[1].get<int>(), j[2].get<int>()}, j[3].get<std::int64_t>()};
}

} // namespace lv::core::itemrender
