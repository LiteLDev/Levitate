#pragma once

// Outlines and flat boxes in the world for client units (Create's Outliner: value box frames,
// selection boxes, the red "missing room" box). Client build only; draw from a
// `LevelRendererCamera::$renderBlockEntities` hook (the opaque pass, `renderAlphaLayer == false`):
//
//     render::Lines lines(ctx);                                     // begins a batch
//     lines.outline({x, y, z}, {x + 1, y + 1, z + 1}, render::kWhite);
//     lines.plate(face, centre, size, render::kWhite);              // a value box frame on a face
//     // drawn when `lines` goes out of scope
//
// Coordinates are absolute world coordinates (doubles); the batch makes them camera-relative.
// Lines are thin bars (`width` in blocks, Create's outlines are 1/32 wide, value boxes 1/64)
// drawn unlit with the game's "wireframe" material, so they show in every light.

#ifdef LV_CLIENT

#include <algorithm>
#include <array>
#include <cmath>

#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"

namespace lv::client::render {

struct Rgb {
    float r = 1, g = 1, b = 1;
    [[nodiscard]] static constexpr Rgb hex(unsigned v) {
        return {static_cast<float>((v >> 16) & 0xff) / 255.f, static_cast<float>((v >> 8) & 0xff) / 255.f, static_cast<float>(v & 0xff) / 255.f};
    }
};
inline constexpr Rgb kWhite = Rgb::hex(0xffffff);
inline constexpr Rgb kRed   = Rgb::hex(0xff5d6c); // Create's Outliner red (placement failures)

/// The material the batches draw with.
inline mce::MaterialPtr& lineMaterial() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

using P3 = std::array<double, 3>;

/// A batch of lines and boxes, drawn at the end of its scope (or `flush()`).
class Lines {
public:
    explicit Lines(BaseActorRenderContext& ctx) : mCtx(ctx), mTess(ctx.mScreenContext.tessellator) {
        Vec3 cam = ctx.mImpl.get()->mCameraTargetPosition;
        mCam            = {cam.x, cam.y, cam.z};
    }
    Lines(Lines const&)            = delete;
    Lines& operator=(Lines const&) = delete;
    ~Lines() { flush(); }

    /// A solid axis-aligned box (faces shaded as the game shades blocks).
    void box(P3 lo, P3 hi, Rgb c) {
        if (!mAny) mTess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
        mAny   = true;
        Vec3 l = rel(lo), h = rel(hi);
        auto quad = [&](std::array<Vec3, 4> const& v, float shade) {
            mTess.color(c.r * shade, c.g * shade, c.b * shade, 1.f);
            for (auto const& p : v) mTess.vertex(p.x, p.y, p.z);
        };
        quad({Vec3{l.x, h.y, l.z}, Vec3{l.x, h.y, h.z}, Vec3{h.x, h.y, h.z}, Vec3{h.x, h.y, l.z}}, 1.f);
        quad({Vec3{l.x, l.y, l.z}, Vec3{h.x, l.y, l.z}, Vec3{h.x, l.y, h.z}, Vec3{l.x, l.y, h.z}}, .5f);
        quad({Vec3{l.x, l.y, l.z}, Vec3{l.x, h.y, l.z}, Vec3{h.x, h.y, l.z}, Vec3{h.x, l.y, l.z}}, .8f);
        quad({Vec3{l.x, l.y, h.z}, Vec3{h.x, l.y, h.z}, Vec3{h.x, h.y, h.z}, Vec3{l.x, h.y, h.z}}, .8f);
        quad({Vec3{l.x, l.y, l.z}, Vec3{l.x, l.y, h.z}, Vec3{l.x, h.y, h.z}, Vec3{l.x, h.y, l.z}}, .6f);
        quad({Vec3{h.x, l.y, l.z}, Vec3{h.x, h.y, l.z}, Vec3{h.x, h.y, h.z}, Vec3{h.x, l.y, h.z}}, .6f);
    }

    /// A straight line from `a` to `b` (a square bar `width` thick, in any direction).
    void line(P3 a, P3 b, Rgb c, double width = 1.0 / 32) {
        double w = width / 2;
        P3     d{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        int    nonZero = (std::abs(d[0]) > 1e-9) + (std::abs(d[1]) > 1e-9) + (std::abs(d[2]) > 1e-9);
        if (nonZero <= 1) {
            // along an axis: an axis-aligned box
            box({std::min(a[0], b[0]) - w, std::min(a[1], b[1]) - w, std::min(a[2], b[2]) - w},
                {std::max(a[0], b[0]) + w, std::max(a[1], b[1]) + w, std::max(a[2], b[2]) + w}, c);
            return;
        }
        // any other direction: a prism around the segment, sides along two perpendiculars
        double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        P3     u{d[0] / len, d[1] / len, d[2] / len};
        P3     ref = std::abs(u[1]) < .9 ? P3{0, 1, 0} : P3{1, 0, 0};
        P3     p{u[1] * ref[2] - u[2] * ref[1], u[2] * ref[0] - u[0] * ref[2], u[0] * ref[1] - u[1] * ref[0]};
        double pl = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (auto& v : p) v = v / pl * w;
        P3 q{u[1] * p[2] - u[2] * p[1], u[2] * p[0] - u[0] * p[2], u[0] * p[1] - u[1] * p[0]};
        auto at = [&](P3 const& o, double sp, double sq) { return rel({o[0] + p[0] * sp + q[0] * sq, o[1] + p[1] * sp + q[1] * sq, o[2] + p[2] * sp + q[2] * sq}); };
        if (!mAny) mTess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
        mAny = true;
        static constexpr double kCorners[5][2]{{1, 1}, {-1, 1}, {-1, -1}, {1, -1}, {1, 1}};
        for (int s = 0; s < 4; ++s) {
            float shade = s % 2 ? .8f : 1.f;
            mTess.color(c.r * shade, c.g * shade, c.b * shade, 1.f);
            for (auto const& v : {at(a, kCorners[s][0], kCorners[s][1]), at(b, kCorners[s][0], kCorners[s][1]), at(b, kCorners[s + 1][0], kCorners[s + 1][1]),
                                  at(a, kCorners[s + 1][0], kCorners[s + 1][1])})
                mTess.vertex(v.x, v.y, v.z);
        }
    }

    /// The 12 edges of the box [lo, hi].
    void outline(P3 lo, P3 hi, Rgb c, double width = 1.0 / 32) {
        for (double y : {lo[1], hi[1]}) {
            line({lo[0], y, lo[2]}, {hi[0], y, lo[2]}, c, width);
            line({lo[0], y, hi[2]}, {hi[0], y, hi[2]}, c, width);
            line({lo[0], y, lo[2]}, {lo[0], y, hi[2]}, c, width);
            line({hi[0], y, lo[2]}, {hi[0], y, hi[2]}, c, width);
        }
        for (double x : {lo[0], hi[0]})
            for (double z : {lo[2], hi[2]}) line({x, lo[1], z}, {x, hi[1], z}, c, width);
    }

    /// The frame of a square `size` wide centred on `centre`, lying in the plane of block face
    /// `face` (0 down, 1 up, 2 north, 3 south, 4 west, 5 east): Create's value box outline.
    void plate(int face, P3 centre, double size, Rgb c, double width = 1.0 / 64) {
        int    axis = face < 2 ? 1 : face < 4 ? 2 : 0;
        double h    = size / 2;
        P3     lo = centre, hi = centre;
        for (int i = 0; i < 3; ++i) {
            if (i == axis) continue;
            lo[i] -= h;
            hi[i] += h;
        }
        // a hair in front of the face, so the frame is not hidden in the block model
        double out = (face % 2 == 1 ? 1 : -1) * 0.002;
        lo[axis] += out;
        hi[axis] += out;
        int u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
        P3  a = lo, b = lo, d = lo;
        b[u] = hi[u];
        d[v] = hi[v];
        P3 e = hi;
        e[axis] = lo[axis];
        line(a, b, c, width);
        line(a, d, c, width);
        line(b, e, c, width);
        line(d, e, c, width);
    }

    /// Draws what was added so far (the tessellator is only begun while something is queued).
    void flush() {
        if (mAny) MeshHelpers::renderMeshImmediately(mCtx.mScreenContext, mTess, lineMaterial(), OffscreenCaptureDescription{});
        mAny = false;
    }

private:
    [[nodiscard]] Vec3 rel(P3 const& p) const {
        return Vec3{static_cast<float>(p[0] - mCam[0]), static_cast<float>(p[1] - mCam[1]), static_cast<float>(p[2] - mCam[2])};
    }

    BaseActorRenderContext& mCtx;
    Tessellator&            mTess;
    P3                      mCam{};
    bool                    mAny = false;
};

} // namespace lv::client::render

#endif // LV_CLIENT
