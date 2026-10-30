// Client mod: the preview of a train being relocated with the wrench (Create's TrainRelocator
// visualise: the train's outline where it would stand).
//
// The server sends "trains.relocate" {boxes: [[8 world corners] per carriage], valid} while the
// player relocates a train (feature/trains/Relocation.cpp) and an empty list when it ends. Each
// carriage is drawn as the 12 edges of its (turned, tilted) bounding box, in Create's green.

#ifdef LV_CLIENT

#include <array>
#include <cmath>
#include <mutex>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
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
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::trains {

namespace {

struct Ghost {
    std::mutex                              mutex;
    std::vector<std::array<Vec3, 8>>        boxes;
    bool                                    logged = false;
};

Ghost& ghost() {
    static Ghost g;
    return g;
}

float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

/// A small cube (the beads that make up an edge).
void cube(Tessellator& t, Vec3 c, float w, std::array<float, 3> const& col) {
    Vec3 lo{c.x - w, c.y - w, c.z - w}, hi{c.x + w, c.y + w, c.z + w};
    auto quad = [&](std::array<Vec3, 4> const& v, float shade) {
        t.color(col[0] * shade, col[1] * shade, col[2] * shade, 1.f);
        for (auto const& p : v) t.vertex(p.x, p.y, p.z);
    };
    quad({Vec3{lo.x, hi.y, lo.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, hi.y, lo.z}}, 1.f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{lo.x, lo.y, hi.z}}, .5f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, hi.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, lo.y, lo.z}}, .8f);
    quad({Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}}, .8f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, lo.y, hi.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{lo.x, hi.y, lo.z}}, .6f);
    quad({Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, lo.y, hi.z}}, .6f);
}

void edge(Tessellator& t, Vec3 a, Vec3 b, std::array<float, 3> const& col) {
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    int   n   = std::max(1, int(len / 0.05f));
    for (int i = 0; i <= n; ++i) {
        float f = float(i) / float(n);
        cube(t, Vec3{a.x + dx * f, a.y + dy * f, a.z + dz * f}, 1.f / 40, col);
    }
}

void renderGhost(BaseActorRenderContext& ctx) {
    auto& g = ghost();
    std::lock_guard lock(g.mutex);
    if (g.boxes.empty() || !ctx.mImpl) return;
    Vec3                 cam = ctx.mImpl->mCameraTargetPosition;
    std::array<float, 3> col{lin(0x95), lin(0xCD), lin(0x41)}; // Create's relocation green
    ScreenContext&       sc   = ctx.mScreenContext;
    Tessellator&         tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
    static int const edges[12][2]{{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto const& b : g.boxes) {
        for (auto const& e : edges) {
            Vec3 a{b[e[0]].x - cam.x, b[e[0]].y - cam.y, b[e[0]].z - cam.z};
            Vec3 c{b[e[1]].x - cam.x, b[e[1]].y - cam.y, b[e[1]].z - cam.z};
            edge(tess, a, c, col);
        }
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!g.logged) {
        g.logged = true;
        log().info("trains client: drawing the relocation preview ({} carriage(s))", g.boxes.size());
    }
}

void onRelocate(nlohmann::json const& msg) {
    std::vector<std::array<Vec3, 8>> boxes;
    for (auto const& b : msg.value("boxes", nlohmann::json::array())) {
        if (!b.is_array() || b.size() != 8) continue;
        std::array<Vec3, 8> corners{};
        for (int i = 0; i < 8; ++i) corners[i] = Vec3{b[i][0].get<float>(), b[i][1].get<float>(), b[i][2].get<float>()};
        boxes.push_back(corners);
    }
    auto& g = ghost();
    std::lock_guard lock(g.mutex);
    g.boxes = std::move(boxes);
    if (g.boxes.empty()) g.logged = false;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    RelocationGhostHook,
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
        renderGhost(renderContext);
    } catch (...) {
    }
}

class RelocationGhostModule {
public:
    void install() {
        feature::net::client::on("trains.relocate", [](nlohmann::json const& msg) {
            try {
                onRelocate(msg);
            } catch (...) {
            }
        });
        RelocationGhostHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto&           g = ghost();
            std::lock_guard lock(g.mutex);
            g.boxes.clear();
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        RelocationGhostHook::unhook();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::trains

LV_REGISTER_MODULE(lv::client::trains::RelocationGhostModule);

#endif // LV_CLIENT
