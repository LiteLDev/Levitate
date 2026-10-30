// Client mod part of the bearings unit (U21): minecart couplings are drawn between the coupled
// carts (Create's CouplingRenderer: a ring on the end of each cart facing the other one and a
// connector between the rings, following both carts' interpolated positions every frame). The
// server sends the coupled pairs on "bearings.couplings" (Carts.cpp).

#ifdef LV_CLIENT

#include <array>
#include <cmath>
#include <mutex>
#include <utility>
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
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::bearings {

namespace {

std::mutex                                         gMutex;
std::vector<std::pair<std::int64_t, std::int64_t>> gPairs;
bool                                               gLogged = false;

void onMessage(nlohmann::json const& msg) {
    std::vector<std::pair<std::int64_t, std::int64_t>> pairs;
    for (auto const& e : msg.value("couplings", nlohmann::json::array()))
        if (e.is_array() && e.size() == 2) pairs.emplace_back(e[0].get<std::int64_t>(), e[1].get<std::int64_t>());
    std::lock_guard lock(gMutex);
    gPairs  = std::move(pairs);
    gLogged = false;
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

struct Colour {
    float r, g, b;
};

struct V {
    double x, y, z;
    V operator+(V o) const { return {x + o.x, y + o.y, z + o.z}; }
    V operator-(V o) const { return {x - o.x, y - o.y, z - o.z}; }
    V operator*(double k) const { return {x * k, y * k, z * k}; }
    [[nodiscard]] double length() const { return std::sqrt(x * x + y * y + z * z); }
};

/// A box centred on `c` with half extents along the orthonormal axes a, b, d (camera relative).
void orientedBox(Tessellator& t, V c, V a, V b, V d, Colour col) {
    std::array<V, 8> p;
    for (int i = 0; i < 8; ++i) p[i] = c + a * (i & 1 ? 1 : -1) + b * (i & 2 ? 1 : -1) + d * (i & 4 ? 1 : -1);
    auto quad = [&](int i0, int i1, int i2, int i3, float shade) {
        t.color(col.r * shade, col.g * shade, col.b * shade, 1.f);
        // both windings: the axes may form a mirrored frame
        for (int i : {i0, i1, i2, i3}) t.vertex(float(p[i].x), float(p[i].y), float(p[i].z));
        for (int i : {i3, i2, i1, i0}) t.vertex(float(p[i].x), float(p[i].y), float(p[i].z));
    };
    quad(2, 6, 7, 3, 1.f);
    quad(0, 1, 5, 4, .5f);
    quad(0, 2, 3, 1, .8f);
    quad(4, 5, 7, 6, .8f);
    quad(0, 4, 6, 2, .6f);
    quad(1, 3, 7, 5, .6f);
}

void renderCouplings(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    auto const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3  o     = impl->mCameraTargetPosition;
    float alpha = ctx.mFrameAlpha;

    std::lock_guard lock(gMutex);
    if (gPairs.empty()) return;
    auto& level = player->getLevel();
    struct Ends {
        V a, b;
    };
    std::vector<Ends> visible;
    for (auto const& [ua, ub] : gPairs) {
        ActorUniqueID ia, ib;
        ia.rawID = ua;
        ib.rawID = ub;
        auto* a  = level.fetchEntity(ia, false);
        auto* b  = level.fetchEntity(ib, false);
        if (!a || !b || a->getDimensionId() != player->getDimensionId()) continue;
        auto pa = a->getInterpolatedPosition(alpha), pb = b->getInterpolatedPosition(alpha);
        visible.push_back({{pa.x - o.x, pa.y - o.y, pa.z - o.z}, {pb.x - o.x, pb.y - o.y, pb.z - o.z}});
    }
    if (visible.empty()) return;

    Colour ring{lin(0xa4), lin(0x9e), lin(0x8a)}, connector{lin(0x6e), lin(0x6a), lin(0x60)};
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(visible.size() * 3 * 48), false);
    for (auto const& e : visible) {
        // the coupling height on the cart body; the ends sit on the carts' facing ends
        V      lift{0, 0.45, 0};
        V      from = e.a + lift, to = e.b + lift;
        V      dir  = to - from;
        double len  = dir.length();
        if (len < 1e-3 || len > 16) continue;
        V f = dir * (1 / len);
        // a horizontal side vector and the up vector of the connector's frame
        V side{-f.z, 0, f.x};
        if (side.length() < 1e-3) side = {1, 0, 0};
        side = side * (1 / side.length());
        V up{side.y * f.z - side.z * f.y, side.z * f.x - side.x * f.z, side.x * f.y - side.y * f.x};
        double reach = std::min(0.55, len / 2);
        V      ra = from + f * reach, rb = to - f * reach;
        // rings (Create's coupling attachment) on both carts
        orientedBox(tess, ra, f * (1.5 / 16), side * (2.5 / 16), up * (2.5 / 16), ring);
        orientedBox(tess, rb, f * (1.5 / 16), side * (2.5 / 16), up * (2.5 / 16), ring);
        // the connector between them
        V      mid = (ra + rb) * 0.5;
        double half = (rb - ra).length() / 2;
        if (half > 1e-3) orientedBox(tess, mid, f * half, side * (1.0 / 16), up * (1.0 / 16), connector);
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLogged) {
        gLogged = true;
        log().info("bearings client: drawing {} minecart coupling(s)", visible.size());
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    CouplingRenderHook,
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
        renderCouplings(renderContext);
    } catch (...) {
    }
}

class CouplingRendererModule {
public:
    void install() {
        feature::net::client::on("bearings.couplings", [](nlohmann::json const& msg) { onMessage(msg); });
        CouplingRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gPairs.clear();
            gLogged = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        CouplingRenderHook::unhook();
        std::lock_guard lock(gMutex);
        gPairs.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::bearings

LV_REGISTER_MODULE(lv::client::bearings::CouplingRendererModule);

#endif // LV_CLIENT
