// Client mod part of the trains unit (U27): the station flag (Create's StationRenderer flag and
// StationBlockEntity#flag). A small flag on a pole on top of every station is raised while a
// train stands at the station or while the station is in assembly mode, and lowered otherwise:
// the value chases its target linearly by 0.1 per tick, the pole is up once the value passes 0.2
// and wiggles while it settles (Create's progress curve). The cloth is red, green in assembly
// mode (the station texture's flag colours). The server sends "trains.flags" (Stations.cpp);
// vanilla clients see the flag painted on the station's sides.

#ifdef LV_CLIENT

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
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

namespace lv::client::trains {

namespace {

using Clock = std::chrono::steady_clock;

struct Flag {
    int               fx = 1, fz = 0;
    bool              target     = false;
    bool              assembling = false;
    float             value      = 0;   ///< Create's flag LerpedFloat value at `at`
    Clock::time_point at;               ///< when `value` was taken
    bool              known = false;
};

struct Key {
    int dim, x, y, z;
    friend auto operator<=>(Key const&, Key const&) = default;
};

std::mutex          gMutex;
std::map<Key, Flag> gFlags;
bool                gLogged = false;

/// The flag value now: chases its target by 0.1 per tick (20 ticks per second).
float valueNow(Flag const& f, Clock::time_point now) {
    float ticks = std::chrono::duration<float>(now - f.at).count() * 20.f;
    float v     = f.target ? f.value + 0.1f * ticks : f.value - 0.1f * ticks;
    return std::clamp(v, 0.f, 1.f);
}

void onMessage(nlohmann::json const& msg) {
    std::lock_guard lock(gMutex);
    auto            now = Clock::now();
    std::map<Key, Flag> next;
    for (auto const& e : msg.value("flags", nlohmann::json::array())) {
        if (!e.is_array() || e.size() < 7) continue;
        Key  k{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()};
        Flag f;
        if (auto it = gFlags.find(k); it != gFlags.end()) f = it->second;
        bool target = e[4].get<int>() != 0;
        if (!f.known) {
            // a flag seen for the first time shows its state at once
            f.value = target ? 1.f : 0.f;
            f.at    = now;
        } else if (target != f.target) {
            f.value     = valueNow(f, now);
            f.at        = now;
        }
        f.known      = true;
        f.target     = target;
        f.fx         = e[5].get<int>();
        f.fz         = e[6].get<int>();
        f.assembling = e.size() > 7 && e[7].get<int>() != 0;
        next[k]      = f;
    }
    gFlags = std::move(next);
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

struct Colour {
    float r, g, b;
};

/// A box given by its 8 corners (index bits: x, y, z), already transformed and camera relative.
void box(Tessellator& t, std::array<Vec3, 8> const& c, Colour col) {
    auto quad = [&](int a, int b, int d, int e, float shade) {
        t.color(col.r * shade, col.g * shade, col.b * shade, 1.f);
        // both windings: the fold frame may be mirrored, and the material culls back faces
        for (int i : {a, b, d, e}) t.vertex(c[i].x, c[i].y, c[i].z);
        for (int i : {e, d, b, a}) t.vertex(c[i].x, c[i].y, c[i].z);
    };
    quad(2, 6, 7, 3, 1.f);   // top (y max)
    quad(0, 1, 5, 4, 0.5f);  // bottom
    quad(0, 2, 3, 1, 0.8f);  // z min
    quad(4, 5, 7, 6, 0.8f);  // z max
    quad(0, 4, 6, 2, 0.6f);  // x min
    quad(1, 3, 7, 5, 0.6f);  // x max
}

/// Create's flag progress: up once the value passes 0.2 (squared ease), wiggling while it settles.
float progressOf(Flag const& f, float value) {
    float progress = std::pow(std::min(value * 5.f, 1.f), 2.f);
    bool  settled  = f.target ? value >= 1.f : value <= 0.f;
    if (f.target && !settled && progress == 1.f) {
        float wiggle = (value - .2f) / .8f;
        progress += std::sin(wiggle * 2.f * std::numbers::pi_v<float> * 4.f) / 8.f / std::max(1.f, 8.f * wiggle);
    }
    return progress;
}

void renderFlags(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int         dim  = static_cast<int>(player->getDimensionId());
    auto const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3 o = impl->mCameraTargetPosition;

    std::lock_guard lock(gMutex);
    if (gFlags.empty()) return;
    auto now = Clock::now();
    std::vector<std::pair<Key, Flag>> visible;
    for (auto const& [k, f] : gFlags) {
        if (k.dim != dim) continue;
        double dx = k.x + 0.5 - o.x, dz = k.z + 0.5 - o.z;
        if (dx * dx + dz * dz > 96.0 * 96.0) continue;
        visible.emplace_back(k, f);
    }
    if (visible.empty()) return;

    Colour pole{lin(0x5b), lin(0x54), lin(0x50)}, brass{lin(0xc4), lin(0x9a), lin(0x4a)};
    Colour red{lin(0xb8), lin(0x40), lin(0x3a)}, green{lin(0x58), lin(0xa0), lin(0x4a)};
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(visible.size() * 3 * 48), false);
    for (auto const& [k, f] : visible) {
        float progress = progressOf(f, valueNow(f, now));
        // the pole stands on the top face, at the corner beside the track (the arrival side's
        // right), and folds down towards the track when lowered: turned about the horizontal
        // axis along the arrival direction
        double px = k.x + 0.5 + f.fz * 0.3125, pz = k.z + 0.5 - f.fx * 0.3125, py = k.y + 1.0;
        double angle = (1.0 - progress) * std::numbers::pi / 2.0; // 0 up, pi/2 lying
        // local frame: u = along the arrival direction, w = sideways (fold direction), y up
        double ux = f.fx, uz = f.fz, wx = -f.fz, wz = f.fx;
        auto at = [&](double u, double h, double w) {
            // fold: rotate (h, w) by `angle` about the u axis (h goes towards -w as it falls)
            double h2 = h * std::cos(angle) + w * std::sin(angle);
            double w2 = -h * std::sin(angle) + w * std::cos(angle);
            return Vec3{float(px + ux * u + wx * w2 - o.x), float(py + h2 - o.y), float(pz + uz * u + wz * w2 - o.z)};
        };
        auto cube = [&](double u0, double h0, double w0, double u1, double h1, double w1, Colour c) {
            std::array<Vec3, 8> corners;
            for (int i = 0; i < 8; ++i) corners[i] = at(i & 1 ? u1 : u0, i & 2 ? h1 : h0, i & 4 ? w1 : w0);
            box(tess, corners, c);
        };
        constexpr double px1 = 1.0 / 16;
        cube(-px1 / 2, 0, -px1 / 2, px1 / 2, 11 * px1, px1 / 2, pole);           // pole
        cube(-px1, 11 * px1, -px1, px1, 12 * px1, px1, brass);                  // knob
        cube(px1 / 2, 6 * px1, -px1 / 4, 6.5 * px1, 10.5 * px1, px1 / 4, f.assembling ? green : red); // cloth
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLogged) {
        gLogged = true;
        log().info("trains client: drawing {} station flag(s)", visible.size());
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    StationFlagRenderHook,
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
        renderFlags(renderContext);
    } catch (...) {
    }
}

class StationFlagModule {
public:
    void install() {
        feature::net::client::on("trains.flags", [](nlohmann::json const& msg) { onMessage(msg); });
        StationFlagRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gFlags.clear();
            gLogged = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        StationFlagRenderHook::unhook();
        std::lock_guard lock(gMutex);
        gFlags.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::trains

LV_REGISTER_MODULE(lv::client::trains::StationFlagModule);

#endif // LV_CLIENT
