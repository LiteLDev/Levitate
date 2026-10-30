// What pipes show besides their blocks (pipes unit, U17):
//
// - Particles (Create's PipeConnection.tickFlowProgress -> spawnParticles / spawnSplashOnRim), for
//   every client: each tick, every face with a flow pours a particle out of (or into) an open end,
//   or with a 1/1000 chance drips at its rim; a flow that just appeared splashes once. Only near
//   players (20 blocks, Create's MAX_PARTICLE_RENDER_DISTANCE).
// - Glass pipe flows for the client mod (Create's TransparentStraightPipeRenderer): the flows of
//   every glass pipe (fluid, direction, progress, speed) go to companions over "pipes.glass" when
//   they change; the client advances the fronts itself (src/client/pipes/PipesRender.cpp).

#include <map>
#include <set>
#include <utility>
#include <vector>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/FluidRegistry.h"
#include "feature/net/Net.h"
#include "feature/pipes/FluidFX.h"
#include "feature/pipes/PipeEffects.h"
#include "feature/pipes/Pipes.h"
#include "mod/Log.h"

namespace lv::feature::pipes {

namespace {

using Json = nlohmann::json;
using Key  = std::pair<GlobalPos, int>;

constexpr char kGlassChannel[] = "pipes.glass";

struct State {
    std::set<Key>             flows;     ///< faces with a flow last tick
    std::map<GlobalPos, Json> glassSent; ///< what companions were told about each glass pipe
    std::uint64_t             splashes = 0, pours = 0, drips = 0;
    bool                      loggedGlass = false;
};

State& st() {
    static State s;
    return s;
}

Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }

int renderSlot(std::string const& fluid) {
    auto const* t = common::fluidtypes::find(fluid);
    int         s = t ? t->renderSlot : 0;
    return s <= 0 || s > 7 ? 6 : s;
}

/// The flows of a glass pipe as the client draws them: [face, slot, inbound, progress, speed].
Json glassFlows(cp::Node const& n) {
    Json out = Json::array();
    for (auto const& c : n.connections) {
        if (!c || !c->flow || c->flow->fluid.empty()) continue;
        auto const& f = *c->flow;
        float       speed = f.complete ? 0.f : cp::flowSpeed(c->pressure(f.inbound));
        out.push_back(Json::array({static_cast<int>(c->side), renderSlot(f.fluid.id), f.inbound ? 1 : 0, f.complete ? 1.f : f.progress, speed}));
    }
    return out;
}

/// Same flows (fluid, direction, completion, speed): the client keeps advancing them itself.
bool sameShape(Json const& a, Json const& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto const& x = a[i];
        auto const& y = b[i];
        if (x[0] != y[0] || x[1] != y[1] || x[2] != y[2] || x[4] != y[4]) return false;
        if ((x[3].get<float>() >= 1.f) != (y[3].get<float>() >= 1.f)) return false;
    }
    return true;
}

void broadcastGlass(GlobalPos const& p, Json const& flows) {
    auto level = ll::service::getLevel();
    if (!level) return;
    Json msg{{"op", "glass"}, {"dim", p.dim}, {"pos", posJson(p)}, {"flows", flows}};
    level->forEachPlayer([&](Player& pl) {
        if (static_cast<int>(pl.getDimensionId()) == p.dim) net::send(pl, kGlassChannel, msg);
        return true;
    });
}

struct Viewer {
    int    dim;
    double x, y, z;
};

bool viewerNear(std::vector<Viewer> const& viewers, GlobalPos const& p) {
    constexpr double r = fx::kParticleRange + 1;
    for (auto const& v : viewers) {
        if (v.dim != p.dim) continue;
        double dx = v.x - (p.x + .5), dy = v.y - (p.y + .5), dz = v.z - (p.z + .5);
        if (dx * dx + dy * dy + dz * dz <= r * r) return true;
    }
    return false;
}

} // namespace

void tickPipeEffects(cp::Model& model) {
    auto& s     = st();
    auto  level = ll::service::getLevel();
    if (!level) return;
    std::vector<Viewer> viewers;
    level->forEachPlayer([&](Player& p) {
        auto e = p.getPosition();
        viewers.push_back({static_cast<int>(p.getDimensionId()), e.x, e.y, e.z});
        return true;
    });

    std::set<Key>       flowsNow;
    std::set<GlobalPos> glassSeen;
    for (auto const& [pos, node] : model.nodes()) {
        bool any = false;
        for (auto const& c : node.connections)
            if (c && c->flow && !c->flow->fluid.empty()) any = true;
        bool glass = false;
        if (any || s.glassSent.contains(pos)) {
            auto* region = regionOf(pos.dim);
            glass        = region && region->hasChunksAt(toBlockPos(pos), 0, false) && kindOf(region->getBlock(toBlockPos(pos))) == BlockKind::Glass;
        }
        if (glass) {
            glassSeen.insert(pos);
            Json flows = glassFlows(node);
            auto it    = s.glassSent.find(pos);
            if (it == s.glassSent.end() ? !flows.empty() : !sameShape(it->second, flows)) {
                broadcastGlass(pos, flows);
                if (flows.empty()) s.glassSent.erase(pos);
                else s.glassSent[pos] = flows;
                if (!s.loggedGlass && !flows.empty()) {
                    s.loggedGlass = true;
                    log().info("pipes: glass pipe at {} {} {} shows {} flows to the client mod", pos.x, pos.y, pos.z, flows.size());
                }
            }
        }
        if (!any) continue;
        bool near = viewerNear(viewers, pos);
        for (auto const& c : node.connections) {
            if (!c || !c->flow || c->flow->fluid.empty()) continue;
            Key  key{pos, static_cast<int>(c->side)};
            bool fresh = !s.flows.contains(key);
            flowsNow.insert(key);
            if (!near) continue;
            bool        openEnd = c->source && c->source->kind == cp::SourceKind::OpenEnd;
            auto const& fluid   = c->flow->fluid.id;
            if (openEnd) {
                // Create: open ends pour (or suck in) one particle every tick, the splash too
                fx::pouringLiquid(pos, c->side, 1, fluid, c->flow->inbound);
                ++s.pours;
                if (fresh) ++s.splashes;
            } else if (fresh) {
                fx::rimParticles(pos, c->side, 1, fluid); // spawnSplashOnRim (SPLASH_PARTICLE_AMOUNT 1)
                ++s.splashes;
            } else if (fx::random01() < fx::kIdleParticleChance) {
                fx::rimParticles(pos, c->side, 1, fluid);
                ++s.drips;
            }
        }
    }
    s.flows = std::move(flowsNow);
    // glass pipes that are gone (or no longer glass)
    for (auto it = s.glassSent.begin(); it != s.glassSent.end();) {
        if (glassSeen.contains(it->first)) {
            ++it;
            continue;
        }
        broadcastGlass(it->first, Json::array());
        it = s.glassSent.erase(it);
    }
}

void syncGlassTo(Player& player) {
    int dim = static_cast<int>(player.getDimensionId());
    for (auto const& [p, flows] : st().glassSent)
        if (p.dim == dim) net::send(player, kGlassChannel, Json{{"op", "glass"}, {"dim", p.dim}, {"pos", posJson(p)}, {"flows", flows}});
}

void clearPipeEffects() { st() = {}; }

EffectCounts pipeEffectCounts() {
    auto const& s = st();
    return {s.splashes, s.pours, s.drips, s.glassSent.size()};
}

std::optional<nlohmann::json> glassState(GlobalPos const& p) {
    auto const& m  = st().glassSent;
    auto        it = m.find(p);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

} // namespace lv::feature::pipes
