// Client mod: super glue outlines (Create's SuperGlueSelectionHandler).
//
// The server sends the glue regions near the player and the pending first click on channel
// "contraption.glue" {d: dim, r: [[x0,y0,z0,x1,y1,z1], ...], s: [x,y,z] | null, m: max range}
// (feature/contraption/Glue.cpp). While the local player holds super glue, every frame:
// - each region gets a thin outline, Create's passive green (0x4D9162); the one under the
//   crosshair (the player's look ray within reach) the highlight green (0x68C586);
// - after the first click, the selection box from that block to the looked-at block, in the
//   highlight green when it can be glued (every side at most the max range, the corners connected
//   through blocks inside the box, as the server checks) and Create's fail yellow (0xC5B548)
//   otherwise.
// Players without the client mod get particles along the edges from the server instead.

#ifdef LV_CLIENT

#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "core/contraption/Glue.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::contraption {

namespace {

namespace cc = core::contraption;

constexpr char   kSuperGlue[] = "levitate:super_glue";
constexpr double kReach       = 5.0;

struct GlueState {
    std::mutex                 mutex;
    int                        dim = 0;
    std::vector<cc::GlueBox>   regions;
    std::optional<cc::Pos>     first;
    int                        maxRange = 24;
    // cached validity of the selection box (first, second)
    std::optional<std::pair<cc::Pos, cc::Pos>> checked;
    bool                       valid = false;
    bool                       logged = false;
};

GlueState& gs() {
    static GlueState s;
    return s;
}

/// sRGB -> the linear vertex colours of the colour material.
float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }
std::array<float, 3> rgb(int hex) { return {lin(hex >> 16 & 0xff), lin(hex >> 8 & 0xff), lin(hex & 0xff)}; }

constexpr int kPassive   = 0x4D9162;
constexpr int kHighlight = 0x68C586;
constexpr int kFail      = 0xC5B548;

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

void box(Tessellator& t, Vec3 lo, Vec3 hi, std::array<float, 3> const& c) {
    auto quad = [&](std::array<Vec3, 4> const& v, float shade) {
        t.color(c[0] * shade, c[1] * shade, c[2] * shade, 1.f);
        for (auto const& p : v) t.vertex(p.x, p.y, p.z);
    };
    quad({Vec3{lo.x, hi.y, lo.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, hi.y, lo.z}}, 1.f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{lo.x, lo.y, hi.z}}, 0.5f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, hi.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, lo.y, lo.z}}, 0.8f);
    quad({Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}}, 0.8f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, lo.y, hi.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{lo.x, hi.y, lo.z}}, 0.6f);
    quad({Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, lo.y, hi.z}}, 0.6f);
}

/// The 12 edges of a box as thin bars (relative to the camera).
void outline(Tessellator& t, Vec3 lo, Vec3 hi, std::array<float, 3> const& c, float w) {
    for (float y : {lo.y, hi.y}) {
        box(t, Vec3{lo.x - w, y - w, lo.z - w}, Vec3{hi.x + w, y + w, lo.z + w}, c);
        box(t, Vec3{lo.x - w, y - w, hi.z - w}, Vec3{hi.x + w, y + w, hi.z + w}, c);
        box(t, Vec3{lo.x - w, y - w, lo.z - w}, Vec3{lo.x + w, y + w, hi.z + w}, c);
        box(t, Vec3{hi.x - w, y - w, lo.z - w}, Vec3{hi.x + w, y + w, hi.z + w}, c);
    }
    for (float x : {lo.x, hi.x})
        for (float z : {lo.z, hi.z}) box(t, Vec3{x - w, lo.y, z - w}, Vec3{x + w, hi.y, z + w}, c);
}

/// Distance along the ray to a block box [lo, hi) (nullopt: missed).
std::optional<double> rayBox(Vec3 const& o, Vec3 const& d, cc::GlueBox const& b) {
    double lo[3]{double(b.min.x), double(b.min.y), double(b.min.z)};
    double hi[3]{double(b.max.x + 1), double(b.max.y + 1), double(b.max.z + 1)};
    double org[3]{o.x, o.y, o.z}, dir[3]{d.x, d.y, d.z};
    double tmin = -1e18, tmax = 1e18;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-9) {
            if (org[i] < lo[i] || org[i] > hi[i]) return std::nullopt;
            continue;
        }
        double t1 = (lo[i] - org[i]) / dir[i], t2 = (hi[i] - org[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return std::nullopt;
    }
    if (tmax < 0) return std::nullopt;
    return std::max(0.0, tmin);
}

void renderGlue(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    if (feature::common::items::idOf(player->getSelectedItem()) != kSuperGlue) return;
    auto const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3 cam = impl->mCameraTargetPosition;
    int  dim = static_cast<int>(player->getDimensionId());

    auto& s = gs();
    std::lock_guard lock(s.mutex);
    if (s.dim != dim) return;

    // the looked-at block and the glue region under the crosshair
    std::optional<cc::Pos> hovered;
    auto const&            hit = player->getLevel().getHitResult();
    if (hit.mType == HitResultType::Tile) hovered = cc::Pos{hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    Vec3   eye  = player->getEyePos();
    Vec3   look = player->getViewVector();
    int    highlighted = -1;
    double best        = kReach;
    for (int i = 0; i < static_cast<int>(s.regions.size()); ++i) {
        if (auto d = rayBox(eye, look, s.regions[i]); d && *d <= best) {
            best        = *d;
            highlighted = i;
        }
    }

    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
    int  drawn = 0;
    auto draw  = [&](cc::GlueBox const& b, int colour, float w) {
        Vec3 lo{float(b.min.x - cam.x), float(b.min.y - cam.y), float(b.min.z - cam.z)};
        Vec3 hi{float(b.max.x + 1 - cam.x), float(b.max.y + 1 - cam.y), float(b.max.z + 1 - cam.z)};
        outline(tess, lo, hi, rgb(colour), w);
        ++drawn;
    };
    for (int i = 0; i < static_cast<int>(s.regions.size()); ++i) {
        if (s.first && i == highlighted) continue; // Create: no glue highlight while selecting
        draw(s.regions[i], i == highlighted ? kHighlight : kPassive, 1.f / 64);
    }
    if (s.first) {
        cc::Pos second = hovered.value_or(*s.first);
        if (!s.checked || s.checked->first != *s.first || s.checked->second != second) {
            auto& region = player->getDimensionBlockSource();
            auto  check  = cc::checkSelection(*s.first, second, s.maxRange, [&](cc::Pos const& p) {
                BlockPos bp{p.x, p.y, p.z};
                return region.hasChunksAt(bp, 0, false) && !region.getBlock(bp).isAir();
            });
            s.checked = std::pair{*s.first, second};
            s.valid   = check == cc::GlueCheck::Ok;
        }
        draw(cc::GlueBox::of(*s.first, second), s.valid ? kHighlight : kFail, 1.f / 32);
    }
    if (drawn == 0) return;
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!s.logged) {
        s.logged = true;
        log().info("contraption client: drawing {} super glue outline(s)", drawn);
    }
}

void onGlue(nlohmann::json const& msg) {
    std::vector<cc::GlueBox> list;
    for (auto const& r : msg.value("r", nlohmann::json::array())) {
        if (!r.is_array() || r.size() != 6) continue;
        list.push_back(cc::GlueBox::of({r[0].get<int>(), r[1].get<int>(), r[2].get<int>()}, {r[3].get<int>(), r[4].get<int>(), r[5].get<int>()}));
    }
    std::optional<cc::Pos> first;
    if (auto const& sel = msg.value("s", nlohmann::json()); sel.is_array() && sel.size() == 3)
        first = cc::Pos{sel[0].get<int>(), sel[1].get<int>(), sel[2].get<int>()};
    auto& s = gs();
    std::lock_guard lock(s.mutex);
    s.dim      = msg.value("d", 0);
    s.regions  = std::move(list);
    s.first    = first;
    s.maxRange = msg.value("m", 24);
    s.checked.reset();
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    GlueRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);
    if (renderAlphaLayer) return;
    try {
        renderGlue(renderContext);
    } catch (...) {
    }
}

class GlueClientModule {
public:
    void install() {
        feature::net::client::on("contraption.glue", [](nlohmann::json const& msg) {
            try {
                onGlue(msg);
            } catch (...) {
            }
        });
        GlueRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto&           s = gs();
            std::lock_guard lock(s.mutex);
            s.regions.clear();
            s.first.reset();
            s.checked.reset();
            s.logged = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        GlueRenderHook::unhook();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::contraption

LV_REGISTER_MODULE(lv::client::contraption::GlueClientModule);

#endif // LV_CLIENT
