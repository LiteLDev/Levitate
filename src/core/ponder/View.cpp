#include "core/ponder/View.h"

#include <algorithm>
#include <cmath>

namespace lv::core::ponder {

namespace {
constexpr float kPi = 3.14159265358979f;
float rad(float d) { return d * kPi / 180.f; }
} // namespace

View View::fit(Scene const& scene, float cx, float cy, float w, float h) {
    View v;
    v.screenX = cx;
    v.screenY = cy;
    v.center  = {scene.sizeX / 2.f, scene.height / 2.f * 0.6f, scene.sizeZ / 2.f};
    // the base diagonal and the height must fit
    float diag  = std::sqrt(static_cast<float>(scene.sizeX * scene.sizeX + scene.sizeZ * scene.sizeZ));
    float byW   = w / (diag + 1.f);
    float byH   = h / (diag * 0.5f + scene.height * 0.9f + 1.f);
    v.scale     = std::max(4.f, std::min(byW, byH) * scene.scale);
    return v;
}

Projected View::project(Vec3f const& p) const {
    float x = p.x - center.x, y = p.y - center.y, z = p.z - center.z;
    float cy = std::cos(rad(yaw)), sy = std::sin(rad(yaw));
    float x1 = x * cy + z * sy;
    float z1 = -x * sy + z * cy;
    float cp = std::cos(rad(pitch)), sp = std::sin(rad(pitch));
    float y2 = y * cp - z1 * sp;
    float z2 = y * sp + z1 * cp;
    return {screenX + x1 * scale, screenY - y2 * scale, z2};
}

std::array<float, 16> View::matrix(float depthScale) const {
    // M = T(screen) * S(scale, -scale, depthScale) * Rx(pitch) * Ry(yaw) * T(-center)
    float cy = std::cos(rad(yaw)), sy = std::sin(rad(yaw));
    float cp = std::cos(rad(pitch)), sp = std::sin(rad(pitch));
    // rows of R = Rx * Ry
    float r[3][3] = {
        {cy, 0, sy},
        {sp * sy, cp, -sp * cy},
        {-cp * sy, sp, cp * cy},
    };
    float s[3] = {scale, -scale, depthScale};
    float t[3] = {screenX, screenY, 0};
    std::array<float, 16> m{};
    for (int row = 0; row < 3; ++row) {
        float tr = 0;
        for (int col = 0; col < 3; ++col) {
            m[col * 4 + row] = s[row] * r[row][col];
        }
        tr = -(r[row][0] * center.x + r[row][1] * center.y + r[row][2] * center.z) * s[row];
        m[12 + row] = t[row] + tr;
    }
    m[15] = 1;
    return m;
}

void View::drag(float dx, float dy) {
    yaw += dx * 0.6f;
    pitch = std::clamp(pitch + dy * 0.6f, 0.f, 90.f);
}

} // namespace lv::core::ponder
