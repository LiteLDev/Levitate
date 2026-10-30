// Client (steam unit, U18): draws the fluid inside fluid tanks with windows (Create's
// FluidTankRenderer) and the moving piston / connecting rod of running steam engines (Create's
// SteamEngineRenderer). The server sends tank contents ("steam.tanks") and engines
// ("steam.engines") to companion players (src/feature/steam/SteamModule.cpp).

#ifdef LV_CLIENT

#include <array>
#include <bit>
#include <cstring>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/sound/SoundEngine.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/deps/audio/ServerSoundHandle.h"
#include "mc/network/packet/PlaySoundPacket.h"
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
    double      gauge = 0, gaugeShown = 0; ///< boiler dial: target and displayed (Create's LerpedFloat)
    int         occluded = 0;              ///< sides without a gauge (south, west, north, east bits)
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

/// Create's boiler gauges (FluidTankRenderer.renderAsBoiler): on every side of the structure not
/// hidden by a block in front, a brass gauge in the middle of the side's bottom layer whose needle
/// turns `-145 * gauge + 90` degrees around its pivot (6/16 up). The needle chases the server's
/// target like Create's `gauge.chase(target, .125, EXP)`. Returns true when a gauge was drawn.
bool drawGauges(std::vector<Quad>& out, Tank& t, V3 const& eye, double dt) {
    V3 centre{t.x + t.w / 2.0, t.y + .5, t.z + t.w / 2.0};
    if ((centre - eye).length() > 64) return false;
    t.gaugeShown += (t.gauge - t.gaugeShown) * std::min(1.0, .125 * dt * 20);
    double const px    = 1 / 16.0;
    double const pivot = t.y + 6 * px;
    // Create's 2D order: south, west, north, east
    static V3 const normals[4]{{0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0}};
    V3 const        up{0, 1, 0};
    bool            drawn = false;
    for (int i = 0; i < 4; ++i) {
        if (t.occluded & (1 << i)) continue;
        V3 n     = normals[i];
        V3 right = V3{n.z, 0, -n.x}; // the viewer's right, looking at the side
        V3 face  = V3{centre.x, pivot, centre.z} + n * (t.w / 2.0);
        // a box on the side: `hw` / `hh` half sizes along the side, from `a` to `b` out of it
        auto slab = [&](V3 c, double a, double b, double hw, double hh, float r, float g, float bl) {
            V3 p0 = c + n * a - right * hw - up * hh, p1 = c + n * b + right * hw + up * hh;
            box(out, {std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::min(p0.z, p1.z)},
                {std::max(p0.x, p1.x), std::max(p0.y, p1.y), std::max(p0.z, p1.z)}, r, g, bl);
        };
        slab(face, 0, 1.5 * px, 4 * px, 4 * px, .62f, .42f, .14f);          // brass housing
        slab(face, 1.5 * px, 1.75 * px, 3 * px, 3 * px, .86f, .84f, .76f);  // dial
        // tick marks at the ends of the scale
        for (double deg : {0.0, 145.0}) {
            double a  = deg * 3.14159265358979 / 180.0;
            V3     d  = right * -std::cos(a) + up * std::sin(a);
            slab(face + d * (2.4 * px), 1.75 * px, 1.9 * px, .3 * px, .3 * px, .2f, .2f, .2f);
        }
        // the needle: from the left (0) clockwise over the top to 145 degrees (1)
        double a      = std::clamp(t.gaugeShown, 0.0, 1.2) * 145.0 * 3.14159265358979 / 180.0;
        V3     dir    = right * -std::cos(a) + up * std::sin(a);
        V3     base   = face + n * (1.95 * px);
        bar(out, base - dir * (.5 * px), base + dir * (2.6 * px), .3 * px, .55f, .06f, .04f);
        slab(face, 1.75 * px, 2.2 * px, .6 * px, .6 * px, .25f, .22f, .2f); // hub
        drawn = true;
    }
    return drawn;
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
            if (auto it = tanks.find(key); it != tanks.end()) {
                t.shown      = it->second.shown;
                t.gaugeShown = it->second.gaugeShown;
            }
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
            t.gauge    = s.value("g", 0.0);
            t.occluded = s.value("occ", 0);
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

/// The first whistle tone this client received (handle, time): the server plays it once as a
/// looping instance, so a tone still playing 3 s later (the sample is 0.6 s long) proves the loop.
/// One log line per process (the e2e scenario waits for it).
std::mutex                                           gToneMutex;
std::optional<std::uint64_t>                         gToneHandle;
std::chrono::steady_clock::time_point                gToneAt;
bool                                                 gToneChecked = false;

/// SoundEngine::isPlayingSound(ServerSoundHandle): the handle is an opaque 64-bit id, passed in a
/// register like the game's own callers do (its header declares a copy constructor it doesn't
/// define, so the member can't be called with one directly).
bool isPlayingHandle(SoundEngine const& engine, std::uint64_t handle) {
    auto member = static_cast<bool (SoundEngine::*)(ServerSoundHandle) const>(&SoundEngine::$isPlayingSound);
    static_assert(sizeof(member) == sizeof(void*), "single inheritance member pointer");
    auto fn = reinterpret_cast<bool (*)(SoundEngine const*, std::uint64_t)>(std::bit_cast<void*>(member));
    return fn(&engine, handle);
}

void probeWhistleLoop() {
    std::uint64_t handle = 0;
    {
        std::lock_guard lock(gToneMutex);
        if (gToneChecked || !gToneHandle || std::chrono::steady_clock::now() - gToneAt < std::chrono::seconds(3)) return;
        gToneChecked = true;
        handle       = *gToneHandle;
    }
    auto* ci = currentClient();
    if (!ci) return;
    auto* engine = ci->getSoundEngine().get().get();
    if (!engine) return;
    // a handle that was never played must read as not playing, or the probe proves nothing
    if (isPlayingHandle(*engine, handle ^ 0xFFFF'0000ull)) log().warn("steam client: the sound engine reports unknown handles as playing");
    else if (isPlayingHandle(*engine, handle)) log().info("steam client: whistle loop still playing after 3 s");
    else log().warn("steam client: the whistle tone stopped before 3 s");
}

void drawSteam(BaseActorRenderContext& ctx) {
    try {
        probeWhistleLoop();
    } catch (...) {
    }
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
    int               tankCount = 0, engineCount = 0, gaugeCount = 0;
    {
        std::lock_guard lock(gMutex);
        // fluids (Create's FluidTankRenderer: 1/4 caps, a 1/16 puddle, inset by the hull)
        if (auto it = gTanks.find(dim); it != gTanks.end()) {
            for (auto& [key, t] : it->second) {
                if (t.boiler) {
                    if (drawGauges(quads, t, o, dt)) ++gaugeCount;
                    continue;
                }
                if (!t.window || t.fluid.empty() || t.amount <= 0) continue;
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
    static bool loggedGauges = false;
    if (!loggedGauges && gaugeCount > 0) {
        loggedGauges = true;
        log().info("steam client: drawing the gauges of {} boiler(s)", gaugeCount);
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

// The whistle tone as the client receives it (loop count, handle): remembered for the probe.
LL_TYPE_INSTANCE_HOOK(
    WhistleToneHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const& source,
    PlaySoundPacket const&   packet
) {
    try {
        std::string const& name = packet.mName;
        if (name.starts_with("levitate.steam.whistle_") && name != "levitate.steam.whistle_chiff") {
            auto const* raw     = reinterpret_cast<unsigned char const*>(&packet.mServerSoundHandle);
            bool        engaged = raw[sizeof(std::uint64_t)] != 0;
            std::uint64_t handle = 0;
            std::memcpy(&handle, raw, sizeof(handle));
            std::lock_guard lock(gToneMutex);
            if (!gToneHandle && engaged) {
                gToneHandle = handle;
                gToneAt     = std::chrono::steady_clock::now();
                log().info("steam client: received the whistle tone {} (loop count {}, handle {:#x})", name, int(packet.mLoopCount), handle);
            }
        }
    } catch (...) {
    }
    origin(source, packet);
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
        WhistleToneHook::hook();
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
        WhistleToneHook::unhook();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::steam

LV_REGISTER_MODULE(lv::client::steam::SteamRenderModule);

#endif // LV_CLIENT
