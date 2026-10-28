// Client mod: ropes of running rope pulleys and belts of running elevator pulleys, with their
// magnet, drawn from the pulley down to the moving structure (Create's AbstractPulleyRenderer).
// The server sends "pistons.rope" (src/feature/pistons/Sync.cpp); between two updates (3 ticks)
// the offset is extrapolated with the synced speed.

#ifdef LV_CLIENT

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
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
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::pistons {

namespace {

using Clock = std::chrono::steady_clock;

struct Rope {
    float             offset   = 0;
    float             speed    = 0;
    bool              elevator = false;
    Clock::time_point at;
};

struct Key {
    int dim, x, y, z;
    friend auto operator<=>(Key const&, Key const&) = default;
};

std::mutex          gMutex;
std::map<Key, Rope> gRopes;
bool                gLogged = false;

void onMessage(nlohmann::json const& msg) {
    std::string op  = msg.value("op", "");
    int         dim = msg.value("dim", 0);
    std::lock_guard lock(gMutex);
    if (op == "reset") {
        std::erase_if(gRopes, [&](auto const& e) { return e.first.dim == dim; });
        return;
    }
    for (auto const& r : msg.value("ropes", nlohmann::json::array())) {
        if (!r.is_array() || r.size() < 3) continue;
        Key k{dim, r[0].get<int>(), r[1].get<int>(), r[2].get<int>()};
        if (op == "remove") {
            gRopes.erase(k);
        } else if (op == "set" && r.size() >= 6) {
            gRopes[k] = Rope{r[3].get<float>(), r[4].get<float>(), r[5].get<bool>(), Clock::now()};
        }
    }
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

struct Colour {
    float r, g, b;
};

/// A box's 6 faces (min/max corners relative to the camera).
void box(Tessellator& t, Vec3 lo, Vec3 hi, Colour c) {
    auto quad = [&](std::array<Vec3, 4> const& v, float shade) {
        t.color(c.r * shade, c.g * shade, c.b * shade, 1.f);
        for (auto const& p : v) t.vertex(p.x, p.y, p.z);
    };
    quad({Vec3{lo.x, hi.y, lo.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, hi.y, lo.z}}, 1.f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{lo.x, lo.y, hi.z}}, 0.5f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, hi.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, lo.y, lo.z}}, 0.8f);
    quad({Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}}, 0.8f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, lo.y, hi.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{lo.x, hi.y, lo.z}}, 0.6f);
    quad({Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, lo.y, hi.z}}, 0.6f);
}

void renderRopes(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int  dim = static_cast<int>(player->getDimensionId());
    auto const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3 o = impl->mCameraTargetPosition;

    std::lock_guard lock(gMutex);
    if (gRopes.empty()) return;
    std::vector<std::pair<Key, Rope>> visible;
    for (auto const& [k, r] : gRopes) {
        if (k.dim != dim) continue;
        double dx = k.x + 0.5 - o.x, dz = k.z + 0.5 - o.z;
        if (dx * dx + dz * dz > 160.0 * 160.0) continue;
        visible.emplace_back(k, r);
    }
    if (visible.empty()) return;

    // colours of our textures (pistons.ps1), linear vertex colours
    Colour rope{lin(0x8a), lin(0x6a), lin(0x45)}, belt{lin(0x4a), lin(0x3c), lin(0x30)};
    Colour magnet{lin(0x9a), lin(0x9d), lin(0xa3)}, magnetTip{lin(0xb0), lin(0x3a), lin(0x2e)};
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(visible.size() * 3 * 24), false);
    auto now = Clock::now();
    for (auto const& [k, r] : visible) {
        float ticks  = std::min(3.f, std::chrono::duration<float>(now - r.at).count() * 20.f);
        float offset = std::max(0.f, r.offset + r.speed * ticks);
        float cx = float(k.x + 0.5 - o.x), cz = float(k.z + 0.5 - o.z);
        float top    = float(k.y - o.y);              // the pulley's bottom face
        float bottom = float(k.y - offset + 1 - o.y); // the magnet's top face
        if (bottom < top) {
            if (r.elevator) {
                box(tess, Vec3{cx - 5 / 16.f, bottom, cz - 1 / 16.f}, Vec3{cx + 5 / 16.f, top, cz + 1 / 16.f}, belt);
            } else {
                box(tess, Vec3{cx - 2 / 16.f, bottom, cz - 2 / 16.f}, Vec3{cx + 2 / 16.f, top, cz + 2 / 16.f}, rope);
            }
        }
        if (offset > 0.01f) {
            // the magnet: a plate under a short rope piece
            float mt = float(k.y - offset + 1 - o.y);
            box(tess, Vec3{cx - 2 / 16.f, mt - 10 / 16.f, cz - 2 / 16.f}, Vec3{cx + 2 / 16.f, mt, cz + 2 / 16.f}, r.elevator ? belt : rope);
            box(tess, Vec3{cx - 5 / 16.f, mt - 1.f, cz - 5 / 16.f}, Vec3{cx + 5 / 16.f, mt - 10 / 16.f, cz + 5 / 16.f}, magnet);
            box(tess, Vec3{cx - 4 / 16.f, mt - 1.f - 0.001f, cz - 4 / 16.f}, Vec3{cx + 4 / 16.f, mt - 14 / 16.f, cz + 4 / 16.f}, magnetTip);
        }
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLogged) {
        gLogged = true;
        log().info("pistons: rendering {} rope(s)", visible.size());
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    PistonsRopeRenderHook,
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
        renderRopes(renderContext);
    } catch (...) {
    }
}

class RopeRendererModule {
public:
    void install() {
        feature::net::client::on("pistons.rope", [](nlohmann::json const& msg) { onMessage(msg); });
        PistonsRopeRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gRopes.clear();
            gLogged = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        PistonsRopeRenderHook::unhook();
        std::lock_guard lock(gMutex);
        gRopes.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

/// Ropes the client knows (selftests / debugging).
size_t knownRopes() {
    std::lock_guard lock(gMutex);
    return gRopes.size();
}

} // namespace lv::client::pistons

LV_REGISTER_MODULE(lv::client::pistons::RopeRendererModule);

#endif // LV_CLIENT
