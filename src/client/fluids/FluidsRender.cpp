// Client: fluid machine rendering (client build only).
//
// - Hose pulleys: the hose hanging from the pulley down to its end piece, extrapolated between
//   the server's updates ("fluids.sync" op hose / hoses / hose_remove).
// - Spouts: the fluid stream from the nozzle down to the item while it is filled (op spout), as
//   Create's SpoutRenderer: its width follows ((2p - 1)^2 - 1) / 32 over the 20 filling ticks.
// Both are drawn as coloured boxes after the block entities of the opaque pass.

#ifdef LV_CLIENT

#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
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

namespace lv::client::fluids {

namespace {

using Clock = std::chrono::steady_clock;
using Key   = std::tuple<int, int, int, int>; // dim, x, y, z

struct Hose {
    float             offset = 0;
    float             speed  = 0; ///< blocks per tick
    Clock::time_point at;
};
struct Spout {
    std::uint32_t     color = 0;
    Clock::time_point at;
};

std::mutex           gMutex;
std::map<Key, Hose>  gHoses;
std::map<Key, Spout> gSpouts;
bool                 gLoggedHose = false;

constexpr double kPx = 1 / 16.0;

Key keyOf(int dim, nlohmann::json const& pos) { return {dim, pos.at(0).get<int>(), pos.at(1).get<int>(), pos.at(2).get<int>()}; }

void onSync(nlohmann::json const& msg) {
    auto op  = msg.value("op", "");
    int  dim = msg.value("dim", 0);
    std::lock_guard lock(gMutex);
    try {
        if (op == "hose") {
            gHoses[keyOf(dim, msg.at("pos"))] = {msg.value("offset", 0.f), msg.value("speed", 0.f), Clock::now()};
        } else if (op == "hose_remove") {
            gHoses.erase(keyOf(dim, msg.at("pos")));
        } else if (op == "hoses") {
            for (auto it = gHoses.begin(); it != gHoses.end();) it = std::get<0>(it->first) == dim ? gHoses.erase(it) : std::next(it);
            for (auto const& h : msg.value("hoses", nlohmann::json::array()))
                gHoses[keyOf(dim, h.at("pos"))] = {h.value("offset", 0.f), h.value("speed", 0.f), Clock::now()};
        } else if (op == "spout") {
            gSpouts[keyOf(dim, msg.at("pos"))] = {msg.value("color", 0u), Clock::now()};
        }
    } catch (...) {
    }
}

struct Quad {
    std::array<Vec3, 4> v;
    float               r, g, b;
};

/// The six faces of an axis-aligned box.
void box(std::vector<Quad>& out, Vec3 lo, Vec3 hi, float r, float g, float b) {
    auto add = [&](Vec3 a, Vec3 bb, Vec3 c, Vec3 d, float shade) { out.push_back({{a, bb, c, d}, r * shade, g * shade, b * shade}); };
    add({lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, 1.f);   // up
    add({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, .5f);   // down
    add({lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z}, .8f);   // north
    add({lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, .8f);   // south
    add({lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, .6f);   // west
    add({hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}, .6f);   // east
}

float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

void drawFluids(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player || !ctx.mImpl) return;
    int  dim    = static_cast<int>(player->getDimensionId());
    Vec3 origin = ctx.mImpl->mCameraTargetPosition;
    auto now    = Clock::now();

    std::vector<Quad> quads;
    {
        std::lock_guard lock(gMutex);
        if (gHoses.empty() && gSpouts.empty()) return;
        for (auto const& [k, h] : gHoses) {
            auto [d, x, y, z] = k;
            if (d != dim) continue;
            float ticks  = std::chrono::duration<float>(now - h.at).count() * 20.f;
            float offset = std::max(0.f, h.offset + h.speed * std::min(ticks, 10.f));
            if (offset <= 0.01f) continue;
            double cx = x + .5, cz = z + .5, end = y - offset;
            // the hose (dark rubber) and its copper end piece
            box(quads, {float(cx - 2 * kPx), float(end + 4 * kPx), float(cz - 2 * kPx)}, {float(cx + 2 * kPx), float(y), float(cz + 2 * kPx)},
                lin(0x3a), lin(0x30), lin(0x2c));
            box(quads, {float(cx - 5 * kPx), float(end), float(cz - 5 * kPx)}, {float(cx + 5 * kPx), float(end + 4 * kPx), float(cz + 5 * kPx)},
                lin(0xc4), lin(0x6e), lin(0x43));
        }
        for (auto it = gSpouts.begin(); it != gSpouts.end();) {
            float ticks = std::chrono::duration<float>(now - it->second.at).count() * 20.f;
            if (ticks > 20) {
                it = gSpouts.erase(it);
                continue;
            }
            auto [d, x, y, z] = it->first;
            if (d == dim) {
                // Create's SpoutRenderer: processingPT counts down from 20
                float pt       = 20 - ticks;
                float progress = std::clamp(1 - (pt - 5) / 10, 0.f, 1.f);
                float radius   = std::abs(float(std::pow(2 * progress - 1, 2) - 1)) / 32.f;
                if (radius > 0.002f) {
                    auto   c  = it->second.color;
                    double cx = x + .5, cz = z + .5;
                    box(quads, {float(cx - radius), float(y - 1.2), float(cz - radius)}, {float(cx + radius), float(y + 0.5), float(cz + radius)},
                        lin(int(c >> 16 & 0xff)), lin(int(c >> 8 & 0xff)), lin(int(c & 0xff)));
                }
            }
            ++it;
        }
    }
    if (quads.empty()) return;
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(quads.size() * 4), false);
    for (auto const& q : quads) {
        tess.color(q.r, q.g, q.b, 1.0f);
        for (auto const& v : q.v) tess.vertex(v.x - origin.x, v.y - origin.y, v.z - origin.z);
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLoggedHose) {
        gLoggedHose = true;
        log().info("fluids: rendering {} quads (hoses / spout streams)", quads.size());
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    FluidsRenderHook,
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
        drawFluids(renderContext);
    } catch (...) {
    }
}

class FluidsRenderModule {
public:
    void install() {
        feature::net::client::on("fluids.sync", [](nlohmann::json const& msg) { onSync(msg); });
        FluidsRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gHoses.clear();
            gSpouts.clear();
            gLoggedHose = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        FluidsRenderHook::unhook();
        std::lock_guard lock(gMutex);
        gHoses.clear();
        gSpouts.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::fluids

LV_REGISTER_MODULE(lv::client::fluids::FluidsRenderModule);

#endif // LV_CLIENT
