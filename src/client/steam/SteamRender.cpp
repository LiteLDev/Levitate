// Client (steam unit, U18): draws the fluid inside fluid tanks with windows (Create's
// FluidTankRenderer) and the moving piston / connecting rod of running steam engines (Create's
// SteamEngineRenderer). The server sends tank contents ("steam.tanks") and engines
// ("steam.engines") to companion players (src/feature/steam/SteamModule.cpp).

#ifdef LV_CLIENT

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

namespace lv::client::steam {

namespace {

using Json = nlohmann::json;

struct V3 {
    double x = 0, y = 0, z = 0;
    V3     operator+(V3 const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3     operator-(V3 const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3     operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(V3 const& o) const { return x * o.x + y * o.y + z * o.z; }
    V3     cross(V3 const& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(dot(*this)); }
    V3     normalized() const {
        double l = length();
        return l > 1e-9 ? *this * (1 / l) : V3{0, 1, 0};
    }
};

struct Tank {
    int         x = 0, y = 0, z = 0, w = 1, h = 1;
    std::string fluid;
    double      amount = 0, capacity = 1;
    bool        window = true, boiler = false;
    double      shown  = -1; ///< displayed fill (0..1), chases the real one
};
struct Engine {
    int   dim = 0, x = 0, y = 0, z = 0, facing = 1, axis = 0;
    float speed = 0;
};

std::mutex                                        gMutex;
std::map<int, std::map<std::array<int, 3>, Tank>> gTanks; // dim -> controller -> tank
std::vector<Engine>                               gEngines;
bool                                              gLogged = false;
auto const                                        gStart  = std::chrono::steady_clock::now();
auto                                              gLastFrame = std::chrono::steady_clock::now();

V3 dirVec(int d) {
    static V3 const v[]{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    return v[std::clamp(d, 0, 5)];
}

void fluidColour(std::string const& fluid, float& r, float& g, float& b) {
    int c = 0x5a7fbf;
    if (fluid == "minecraft:water") c = 0x3f76e4;
    else if (fluid == "minecraft:lava") c = 0xe0641c;
    else if (fluid == "minecraft:milk" || fluid == "minecraft:powder_snow") c = 0xf2f2f2;
    else if (fluid.ends_with(":honey")) c = 0xf0b030;
    else if (fluid.ends_with(":chocolate")) c = 0x6e4126;
    else if (fluid.ends_with(":tea")) c = 0xa1683a;
    auto lin = [](int v) { return float(std::pow(v / 255.0, 2.2)); };
    r        = lin((c >> 16) & 0xff);
    g        = lin((c >> 8) & 0xff);
    b        = lin(c & 0xff);
    if (fluid == "minecraft:lava") { // glowing: no shading below full brightness
        r = std::min(1.f, r * 1.4f);
        g = std::min(1.f, g * 1.4f);
    }
}

struct Quad {
    std::array<V3, 4> v;
    float             r, g, b;
};

void box(std::vector<Quad>& out, V3 lo, V3 hi, float r, float g, float b) {
    V3 c[8] = {
        {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
        {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
    };
    out.push_back({{c[4], c[7], c[6], c[5]}, r, g, b});                         // top
    out.push_back({{c[0], c[1], c[2], c[3]}, r * .5f, g * .5f, b * .5f});       // bottom
    out.push_back({{c[0], c[4], c[5], c[1]}, r * .8f, g * .8f, b * .8f});       // north
    out.push_back({{c[3], c[2], c[6], c[7]}, r * .8f, g * .8f, b * .8f});       // south
    out.push_back({{c[0], c[3], c[7], c[4]}, r * .65f, g * .65f, b * .65f});    // west
    out.push_back({{c[1], c[5], c[6], c[2]}, r * .65f, g * .65f, b * .65f});    // east
}

/// A square bar of half width `hw` from `a` to `b`.
void bar(std::vector<Quad>& out, V3 a, V3 b, double hw, float r, float g, float bl) {
    V3 d   = (b - a).normalized();
    V3 ref = std::abs(d.y) < .9 ? V3{0, 1, 0} : V3{1, 0, 0};
    V3 p   = d.cross(ref).normalized() * hw;
    V3 q   = d.cross(p).normalized() * hw;
    std::array<V3, 4> ra{a + p + q, a - p + q, a - p - q, a + p - q};
    std::array<V3, 4> rb{b + p + q, b - p + q, b - p - q, b + p - q};
    for (int i = 0; i < 4; ++i) {
        int   j     = (i + 1) & 3;
        float shade = (i & 1) ? .7f : .9f;
        out.push_back({{ra[i], ra[j], rb[j], rb[i]}, r * shade, g * shade, bl * shade});
    }
    out.push_back({{ra[0], ra[1], ra[2], ra[3]}, r * .6f, g * .6f, bl * .6f});
    out.push_back({{rb[3], rb[2], rb[1], rb[0]}, r * .6f, g * .6f, bl * .6f});
}

void onTanks(Json const& msg) {
    std::lock_guard lock(gMutex);
    static bool first = true;
    if (first) {
        first = false;
        log().info("steam client: receiving tank contents");
    }
    if (msg.value("reset", false)) gTanks.clear();
    int   dim  = msg.value("dim", 0);
    auto& tanks = gTanks[dim];
    for (auto const& d : msg.value("del", Json::array())) {
        if (d.is_array() && d.size() == 3) tanks.erase({d[0].get<int>(), d[1].get<int>(), d[2].get<int>()});
    }
    for (auto const& s : msg.value("set", Json::array())) {
        try {
            auto const& p = s.at("p");
            std::array<int, 3> key{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
            Tank t;
            if (auto it = tanks.find(key); it != tanks.end()) t.shown = it->second.shown;
            t.x        = key[0];
            t.y        = key[1];
            t.z        = key[2];
            t.w        = s.value("w", 1);
            t.h        = s.value("h", 1);
            t.fluid    = s.value("f", std::string{});
            t.amount   = s.value("a", 0.0);
            t.capacity = std::max(1.0, s.value("c", 1.0));
            t.window   = s.value("win", true);
            t.boiler   = s.value("boiler", false);
            tanks[key] = t;
        } catch (...) {
        }
    }
}

void onEngines(Json const& msg) {
    std::lock_guard lock(gMutex);
    gEngines.clear();
    for (auto const& e : msg.value("engines", Json::array())) {
        try {
            Engine g;
            g.dim    = e.value("d", 0);
            auto p   = e.at("p");
            g.x      = p[0].get<int>();
            g.y      = p[1].get<int>();
            g.z      = p[2].get<int>();
            g.facing = e.value("f", 1);
            g.axis   = e.value("ax", 0);
            g.speed  = e.value("s", 0.f);
            gEngines.push_back(g);
        } catch (...) {
        }
    }
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

void drawSteam(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    int dim = static_cast<int>(player->getDimensionId());
    auto const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3 origin = impl->mCameraTargetPosition;
    V3   o{origin.x, origin.y, origin.z};

    auto   now     = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(now - gStart).count();
    double dt      = std::min(0.25, std::chrono::duration<double>(now - gLastFrame).count());
    gLastFrame     = now;

    std::vector<Quad> quads;
    int               tankCount = 0, engineCount = 0;
    {
        std::lock_guard lock(gMutex);
        // fluids (Create's FluidTankRenderer: 1/4 caps, a 1/16 puddle, inset by the hull)
        if (auto it = gTanks.find(dim); it != gTanks.end()) {
            for (auto& [key, t] : it->second) {
                if (!t.window || t.boiler || t.fluid.empty() || t.amount <= 0) continue;
                V3 c{t.x + t.w / 2.0, t.y + t.h / 2.0, t.z + t.w / 2.0};
                if ((c - o).length() > 128) continue;
                double fill = std::clamp(t.amount / t.capacity, 0.0, 1.0);
                if (t.shown < 0) t.shown = fill;
                t.shown += (fill - t.shown) * std::min(1.0, dt * 10);
                double const cap = .25, hull = 1 / 16.0 + 1 / 128.0, puddle = 1 / 16.0;
                double       total = t.h - 2 * cap - puddle;
                double       level = std::clamp(t.shown * total, 0.0, total);
                float        r, g, b;
                fluidColour(t.fluid, r, g, b);
                box(quads, {t.x + hull, t.y + cap, t.z + hull}, {t.x + t.w - hull, t.y + cap + puddle + level, t.z + t.w - hull}, r, g, b);
                ++tankCount;
            }
        }
        // engines: piston up and down with the shaft (Create's SteamEngineRenderer)
        for (auto const& e : gEngines) {
            if (e.dim != dim) continue;
            V3 centre{e.x + .5, e.y + .5, e.z + .5};
            if ((centre - o).length() > 96) continue;
            V3     u     = dirVec(e.facing);
            double angle = std::fmod(seconds * 20 * e.speed * 0.3, 360.0) * 3.14159265358979 / 180.0;
            double piston = ((1 - std::sin(angle)) / 4) * 24 / 16.0;
            V3     head   = centre + u * (.5 + .1 + piston * .5);
            // the crank pin turns with the shaft, two blocks in front of the engine
            V3 shaftCentre = centre + u * 2;
            V3 axisV       = e.axis == 0 ? V3{1, 0, 0} : e.axis == 1 ? V3{0, 1, 0} : V3{0, 0, 1};
            V3 side        = axisV.cross(u).normalized();
            V3 pin         = shaftCentre + (u * -std::sin(angle) + side * std::cos(angle)) * (4 / 16.0);
            bar(quads, centre + u * .5, head, 1.5 / 16, .66f, .66f, .68f);        // piston rod
            box(quads, head - V3{.12, .12, .12}, head + V3{.12, .12, .12}, .45f, .45f, .48f); // crosshead
            bar(quads, head, pin, 1 / 16.0, .77f, .48f, .3f);                     // connecting rod
            ++engineCount;
        }
    }
    if (quads.empty()) return;
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(quads.size() * 4), false);
    for (auto const& q : quads) {
        tess.color(q.r, q.g, q.b, 1.0f);
        for (auto const& v : q.v) tess.vertex(float(v.x - o.x), float(v.y - o.y), float(v.z - o.z));
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    static bool loggedEngines = false;
    if (!gLogged && tankCount > 0) {
        gLogged = true;
        log().info("steam client: drawing {} tank fluid(s)", tankCount);
    }
    if (!loggedEngines && engineCount > 0) {
        loggedEngines = true;
        log().info("steam client: drawing {} engine(s)", engineCount);
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    SteamRenderHook,
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
        drawSteam(renderContext);
    } catch (...) {
    }
}

class SteamRenderModule {
public:
    void install() {
        static bool once = false;
        if (!once) {
            once = true;
            feature::net::client::on("steam.tanks", [](Json const& msg) { onTanks(msg); });
            feature::net::client::on("steam.engines", [](Json const& msg) { onEngines(msg); });
        }
        SteamRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gTanks.clear();
            gEngines.clear();
            gLogged = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        SteamRenderHook::unhook();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::steam

LV_REGISTER_MODULE(lv::client::steam::SteamRenderModule);

#endif // LV_CLIENT
