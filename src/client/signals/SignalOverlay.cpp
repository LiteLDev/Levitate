// Client: track markers of signals and observers (Create's TrackTargetingBehaviour overlay).
//
// The server sends "signals.overlay" {markers: [{d: dim, p: [x,y,z], v: [dx,dz], k: 0 signal /
// 1 observer, s: lamp state (Invalid 3 / Red 0 / Yellow 1 / Green 2) or observer powered}]} to
// companions whenever the markers change. Every frame, after the opaque block entities, a flat
// arrow in the signal's colour is drawn on the track where each signal side is bound (pointing
// the way the trains it controls travel) and a square where an observer is bound. Vanilla
// clients see the signal state on the block itself (block state `levitate:signal_state`).
//
// While the player holds a signal / observer item that carries a track selection (item data
// `lv:track_target`, Create's TrackTargetingBlockItem), the selected track is outlined (Create's
// TrackTargetingClient). Create also makes the item glint (isFoil); Bedrock draws no glint on items
// shown with their block's model, as ours are, even when isGlint says so (tried: SPEC §8).

#ifdef LV_CLIENT

#include <array>
#include <cmath>
#include <mutex>
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

#include "client/Client.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "feature/common/Items.h"
#include "mc/world/item/ItemStack.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::signals {

namespace {

struct Marker {
    int                   dim = 0;
    std::array<double, 3> p{};
    double                dx = 1, dz = 0;
    int                   kind  = 0;
    int                   state = 3;
};

std::mutex          gMutex;
std::vector<Marker> gMarkers;
bool                gLogged = false;

/// sRGB -> the linear vertex colours of the colour material.
float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

std::array<float, 3> colourOf(Marker const& m) {
    if (m.kind == 1) return m.state ? std::array{lin(0xd0), lin(0x40), lin(0x2f)} : std::array{lin(0x4a), lin(0x2c), lin(0x2a)};
    switch (m.state) {
    case 0: return {lin(0xe2), lin(0x38), lin(0x2a)}; // red
    case 1: return {lin(0xf2), lin(0xc3), lin(0x30)}; // yellow
    case 2: return {lin(0x3f), lin(0xd2), lin(0x5a)}; // green
    default: return {lin(0x55), lin(0x55), lin(0x55)};
    }
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

/// Whether a stack carries a track selection (item data lv:track_target).
bool hasTrackTarget(ItemStack const& stack) {
    auto const* tag = stack.mUserData.get();
    return tag && tag->contains("lv:track_target", Tag::Type::String);
}

/// Create's TrackTargetingClient: the outline of the track the held item has selected.
void renderSelection(BaseActorRenderContext& ctx, LocalPlayer& player, Vec3 const& o) {
    auto const& held = player.getSelectedItem();
    if (!hasTrackTarget(held)) return;
    auto j = feature::common::items::getData(held, "track_target");
    double lo[3], hi[3];
    try {
        if (j.at("d").get<int>() != static_cast<int>(player.getDimensionId())) return;
        if (j.value("c", false)) {
            auto a = j.at("a");
            for (int i = 0; i < 3; ++i) lo[i] = a.at(i).get<double>() - .5, hi[i] = a.at(i).get<double>() + .5;
            lo[1] = a.at(1).get<double>() - .05, hi[1] = lo[1] + .25;
        } else {
            auto t = j.at("t");
            for (int i = 0; i < 3; ++i) lo[i] = t.at(i).get<int>(), hi[i] = lo[i] + 1;
            hi[1] = lo[1] + .25; // the track's own shape is flat
        }
    } catch (...) {
        return;
    }
    for (int i = 0; i < 3; ++i) lo[i] -= (&o.x)[i] + .002, hi[i] -= (&o.x)[i] - .002;
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, 12 * 8, false);
    tess.color(lin(0x68), lin(0xc5), lin(0x86), 1.0f);
    constexpr double w = 1.0 / 32; // half the line width
    // each of the 12 edges as two crossed thin quads, both windings
    auto edge = [&](std::array<double, 3> a, std::array<double, 3> b) {
        int axis = a[0] != b[0] ? 0 : a[1] != b[1] ? 1 : 2;
        for (int k = 0; k < 3; ++k) {
            if (k == axis) continue;
            auto p = [&](std::array<double, 3> v, double off) {
                v[k] += off;
                tess.vertex(float(v[0]), float(v[1]), float(v[2]));
            };
            p(a, -w), p(a, w), p(b, w), p(b, -w);
            p(b, -w), p(b, w), p(a, w), p(a, -w);
        }
    };
    for (int i = 0; i < 4; ++i) {
        double x0 = (i & 1) ? hi[0] : lo[0], y0 = (i & 2) ? hi[1] : lo[1];
        edge({x0, y0, lo[2]}, {x0, y0, hi[2]});
        double z0 = (i & 1) ? hi[2] : lo[2];
        edge({lo[0], y0, z0}, {hi[0], y0, z0});
        double xz = (i & 2) ? hi[0] : lo[0];
        edge({xz, lo[1], z0}, {xz, hi[1], z0});
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    static bool logged = false;
    if (!logged) {
        logged = true;
        log().info("signals: outlining the selected track");
    }
}

void renderMarkers(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int  dim = static_cast<int>(player->getDimensionId());
    auto const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3 o = impl->mCameraTargetPosition;
    renderSelection(ctx, *player, o);

    std::lock_guard lock(gMutex);

    std::vector<Marker const*> visible;
    for (auto const& m : gMarkers) {
        if (m.dim != dim) continue;
        double ex = m.p[0] - o.x, ey = m.p[1] - o.y, ez = m.p[2] - o.z;
        if (ex * ex + ey * ey + ez * ez > 96 * 96) continue;
        visible.push_back(&m);
    }
    if (visible.empty()) return;
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(visible.size() * 8), false);
    for (auto const* m : visible) {
        auto   c  = colourOf(*m);
        double y  = m->p[1] + 0.27 - o.y; // just above the rails
        double fx = m->dx, fz = m->dz;    // forward
        double sx = -fz, sz = fx;          // side
        double cx = m->p[0] - o.x, cz = m->p[2] - o.z;
        auto   v  = [&](double f, double s) { tess.vertex(float(cx + fx * f + sx * s), float(y), float(cz + fz * f + sz * s)); };
        tess.color(c[0], c[1], c[2], 1.0f);
        if (m->kind == 1) {
            // observer: a square plate
            v(-.3, -.3), v(-.3, .3), v(.3, .3), v(.3, -.3);
            v(.3, -.3), v(.3, .3), v(-.3, .3), v(-.3, -.3);
            continue;
        }
        // signal: an arrow (shaft and head), both windings so it shows from any side
        v(-.45, -.1), v(-.45, .1), v(.05, .1), v(.05, -.1);
        v(.05, -.3), v(.05, .3), v(.45, 0), v(.45, 0);
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLogged) {
        gLogged = true;
        log().info("signals: rendering {} track marker(s)", visible.size());
    }
}

void onOverlay(nlohmann::json const& msg) {
    std::vector<Marker> list;
    for (auto const& j : msg.value("markers", nlohmann::json::array())) {
        try {
            Marker m;
            m.dim   = j.at("d").get<int>();
            m.p     = {j.at("p").at(0).get<double>(), j.at("p").at(1).get<double>(), j.at("p").at(2).get<double>()};
            m.dx    = j.at("v").at(0).get<double>();
            m.dz    = j.at("v").at(1).get<double>();
            m.kind  = j.value("k", 0);
            m.state = j.value("s", 3);
            double l = std::sqrt(m.dx * m.dx + m.dz * m.dz);
            if (l < 1e-6) m.dx = 1, m.dz = 0;
            else m.dx /= l, m.dz /= l;
            list.push_back(m);
        } catch (...) {
        }
    }
    std::lock_guard lock(gMutex);
    gMarkers = std::move(list);
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    SignalsRenderHook,
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
        renderMarkers(renderContext);
    } catch (...) {
    }
}

class SignalOverlayModule {
public:
    void install() {
        feature::net::client::on("signals.overlay", [](nlohmann::json const& msg) { onOverlay(msg); });
        SignalsRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gMarkers.clear();
            gLogged = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        SignalsRenderHook::unhook();
        std::lock_guard lock(gMutex);
        gMarkers.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::signals

LV_REGISTER_MODULE(lv::client::signals::SignalOverlayModule);

#endif // LV_CLIENT
