// Fluid particles for every client (Create's FluidFX); see FluidFX.h.

#include <algorithm>
#include <cmath>
#include <random>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/packet/SpawnParticleEffectPacket.h"
#include "mc/util/MolangScriptArg.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "feature/common/FluidRegistry.h"
#include "feature/pipes/FluidFX.h"

// The game doesn't export MolangVariableMap's default constructor; its members (LeviLamina typed
// storage) construct themselves empty (as in feature/kinetics/Feedback.cpp).
inline MolangVariableMap::MolangVariableMap() {}

namespace lv::feature::pipes::fx {

namespace {

constexpr char kFluidParticle[] = "levitate:fluid_particle";
constexpr char kDripParticle[]  = "levitate:fluid_drip";

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

Stats& mutableStats() {
    static Stats s;
    return s;
}

void setVar(MolangVariableMap& map, std::string const& name, float value) {
    auto full = "variable." + name;
    map.setMolangVariable(HashedString::computeHash(full), full.c_str(), MolangScriptArg(value), false);
}

V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
V3 mul(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
double length(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

/// VecHelper.offsetRandomly(Vec3.ZERO, r, radius).
V3 offsetRandomly(double radius) {
    return {(random01() - .5) * 2 * radius, (random01() - .5) * 2 * radius, (random01() - .5) * 2 * radius};
}
/// VecHelper.axisAlingedPlaneOf.
V3 planeOf(V3 d) { return {1 - std::abs(d.x), 1 - std::abs(d.y), 1 - std::abs(d.z)}; }
V3 normalOf(core::pipes::Face f) {
    auto o = core::pipes::offsetOf(f);
    return {double(o.x), double(o.y), double(o.z)};
}
V3 centreOf(core::GlobalPos const& p) { return {p.x + .5, p.y + .5, p.z + .5}; }

} // namespace

double random01() { return std::uniform_real_distribution<double>(0., 1.)(rng()); }

Stats const& stats() { return mutableStats(); }
void         resetStats() { mutableStats() = {}; }

int spawn(int dim, V3 at, V3 motion, std::string const& fluid, Kind kind, double range) {
    auto level = ll::service::getLevel();
    if (!level || fluid.empty()) return 0;
    auto&         st    = mutableStats();
    std::uint32_t c     = common::fluidtypes::colorOf(fluid);
    double        speed = length(motion);
    V3            dir   = speed > 1e-9 ? mul(motion, 1 / speed) : V3{0, -1, 0};
    Vec3          pos{float(at.x), float(at.y), float(at.z)};
    int           sent = 0;
    std::optional<MolangVariableMap> vars;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) != dim) return true;
        auto   e  = p.getPosition();
        double dx = e.x - at.x, dy = e.y - at.y, dz = e.z - at.z;
        if (dx * dx + dy * dy + dz * dz > range * range) return true;
        if (!vars) {
            vars.emplace();
            setVar(*vars, "dir_x", float(dir.x));
            setVar(*vars, "dir_y", float(dir.y));
            setVar(*vars, "dir_z", float(dir.z));
            setVar(*vars, "speed", float(speed * 20)); // blocks per tick -> per second
            setVar(*vars, "r", ((c >> 16) & 0xFF) / 255.f);
            setVar(*vars, "g", ((c >> 8) & 0xFF) / 255.f);
            setVar(*vars, "b", (c & 0xFF) / 255.f);
        }
        SpawnParticleEffectPacket pkt(SpawnParticleEffectPacketPayload(
            pos, kind == Kind::Drip ? kDripParticle : kFluidParticle, static_cast<uchar>(dim), std::optional<MolangVariableMap>(*vars)));
        pkt.sendTo(p);
        ++sent;
        return true;
    });
    (kind == Kind::Drip ? st.drip : st.fluid) += 1;
    st.sent += static_cast<std::uint64_t>(sent);
    return sent;
}

void rimParticles(core::GlobalPos const& pos, core::pipes::Face side, int amount, std::string const& fluid) {
    V3 d = normalOf(side);
    for (int i = 0; i < amount; ++i) {
        V3 v = offsetRandomly(1);
        if (double l = length(v); l > 1e-9) v = mul(v, 1 / l);
        v       = {std::clamp(v.x, -kRimRadius, kRimRadius), std::clamp(v.y, -kRimRadius, kRimRadius), std::clamp(v.z, -kRimRadius, kRimRadius)};
        v       = add(mul(v, planeOf(d)), mul(d, .45 + random01() / 16));
        V3 m    = mul(v, .05);
        V3 at   = add(v, centreOf(pos));
        at.y   -= 1 / 16.;
        spawn(pos.dim, at, m, fluid, Kind::Drip);
    }
}

void pouringLiquid(core::GlobalPos const& pos, core::pipes::Face side, int amount, std::string const& fluid, bool inbound) {
    V3 d = normalOf(side);
    for (int i = 0; i < amount; ++i) {
        V3 v      = offsetRandomly(kRimRadius * .75);
        v         = add(mul(v, planeOf(d)), mul(d, .5 + random01() / 4));
        V3 m      = mul(v, 1 / 4.);
        V3 centre = centreOf(pos);
        v         = add(v, centre);
        if (inbound) {
            v = add(v, m);
            m = mul(sub(add(centre, mul(d, .5)), v), 1 / 16.);
        }
        v.y -= 1 / 16.;
        spawn(pos.dim, v, m, fluid, Kind::Fluid);
    }
}

} // namespace lv::feature::pipes::fx
