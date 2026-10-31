// Speedometer and stressometer (U6): Create's SpeedGaugeBlockEntity / StressGaugeBlockEntity.
// The dial target follows the speed (speed levels) or the network's stress / capacity; it drives
// the comparator output, the static needle of vanilla clients (`levitate:dial`, 9 positions) and
// the smooth needle the client mod draws (`transmission.gauge` messages to companions).

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/packet/SpawnParticleEffectPacket.h"
#include "mc/util/MolangScriptArg.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"

#include "core/goggles/Tooltip.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/net/Net.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "mod/Config.h"

// The game doesn't export MolangVariableMap's default constructor; its members (LeviLamina typed
// storage) construct themselves empty (the same inline definition as in feature/kinetics/Feedback.cpp).
inline MolangVariableMap::MolangVariableMap() {}

namespace lv::feature::transmission {

namespace {

constexpr char kChannel[] = "transmission.gauge";

std::unordered_map<GlobalPos, float, ck::GlobalPosHash>& dials() {
    static std::unordered_map<GlobalPos, float, ck::GlobalPosHash> m;
    return m;
}

float computeTarget(std::string const& id, GlobalPos const& pos) {
    auto const& cfg = config::get<kinetics::KineticsConfig>();
    if (id == kSpeedometer) return ct::speedDialTarget(kinetics::speedAt(pos), cfg.mediumSpeed, cfg.fastSpeed, cfg.maxRotationSpeed);
    auto net = kinetics::networkAt(pos);
    if (!net) return 0;
    return ct::stressDialTarget(!cfg.disableStress, kinetics::theoreticalSpeedAt(pos), net->overstressed, net->stress, net->capacity);
}

nlohmann::json entry(GlobalPos const& p, float t) { return nlohmann::json::array({p.x, p.y, p.z, t}); }

void broadcast(GlobalPos const& pos, nlohmann::json const& msg) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) == pos.dim && net::isCompanion(p)) net::send(p, kChannel, msg);
        return true;
    });
}

namespace goggles = feature::goggles;
namespace fmt     = core::goggles::fmt;

/// GaugeBlockEntity.addToGoggleTooltip: the header line.
void gaugeHeader(goggles::InfoContext const& c, goggles::Tooltip& t) { t.push_back(goggles::line(c.tr("create.gui.gauge.info_header", "Gauge Information:"))); }

// SpeedGaugeBlockEntity.addToGoggleTooltip: the speed level and RPM (struck through while
// overstressed, drawn by the client mod)
LV_REGISTER_GOGGLE_INFO(
    kSpeedometer,
    {
        .goggle =
            [](goggles::InfoContext const& c, goggles::Tooltip& t) {
                gaugeHeader(c, t);
                t.push_back(goggles::line(std::string(fmt::kGray) + c.tr("create.gui.speedometer.title", "Rotation Speed")));
                t.push_back(goggles::line(goggles::formattedSpeed(c.player, kinetics::theoreticalSpeedAt(c.pos), kinetics::isOverstressed(c.pos))));
                return true;
            },
        .hover           = goggles::kineticHoverTooltip,
        .replaceDefaults = true,
    }
);

// StressGaugeBlockEntity.addToGoggleTooltip: the stress level and the remaining capacity
LV_REGISTER_GOGGLE_INFO(
    kStressometer,
    {
        .goggle =
            [](goggles::InfoContext const& c, goggles::Tooltip& t) {
                auto const& cfg = config::get<kinetics::KineticsConfig>();
                if (cfg.disableStress) return false;
                gaugeHeader(c, t);
                auto   net      = kinetics::networkAt(c.pos);
                double stress   = net ? net->stress : 0, capacity = net ? net->capacity : 0;
                double fraction = stress / (capacity == 0 ? 1 : capacity);
                t.push_back(goggles::line(std::string(fmt::kGray) + c.tr("create.gui.stressometer.title", "Network Stress")));
                if (kinetics::theoreticalSpeedAt(c.pos) == 0) {
                    t.push_back(goggles::line(std::string(fmt::kDarkGray) + core::goggles::progressBar(3, 0) + c.tr("create.gui.stressometer.no_rotation", "No Rotation")));
                    return true;
                }
                t.push_back(goggles::line(goggles::formattedStress(c.player, fraction)));
                t.push_back(goggles::line(std::string(fmt::kGray) + c.tr("create.gui.stressometer.capacity", "Remaining Capacity")));
                int         level  = fraction > 1 ? 3 : fraction > .75 ? 2 : fraction > .5 ? 1 : 0; // StressImpact.of
                char const* colors[] = {fmt::kGreen, fmt::kYellow, fmt::kGold, fmt::kRed};
                auto        su       = c.tr("create.generic.unit.stress", "su");
                double      remaining = capacity - stress;
                std::string tip       = std::string(colors[level]) + goggles::number(remaining) + su;
                if (remaining != capacity) tip += std::string(fmt::kGray) + " / " + fmt::kDarkGray + goggles::number(capacity) + su;
                t.push_back(goggles::line(tip, 1));
                return true;
            },
        .hover           = goggles::kineticHoverTooltip,
        .replaceDefaults = true,
    }
);

} // namespace

float dialTarget(GlobalPos const& pos) {
    auto it = dials().find(pos);
    return it == dials().end() ? 0.f : it->second;
}

namespace {

constexpr char kDustEffect[] = "levitate:gauge_dust";
std::size_t    gDustSent     = 0;

/// GaugeBlockEntity.color: Create's SpeedGauge (the speed level's colour, a quarter towards
/// white) and StressGauge (green -> yellow -> red with the dial) colours.
unsigned gaugeColour(std::string const& id, GlobalPos const& pos, float dial) {
    auto mix = [](unsigned a, unsigned b, float w) {
        auto ch = [&](int s) {
            float x = static_cast<float>((a >> s) & 0xFF), y = static_cast<float>((b >> s) & 0xFF);
            return static_cast<unsigned>(std::lround(x + (y - x) * std::clamp(w, 0.f, 1.f))) << s;
        };
        return ch(16) | ch(8) | ch(0);
    };
    if (id == kSpeedometer) {
        auto     level = kinetics::speedLevelOf(kinetics::speedAt(pos));
        unsigned base  = level == kinetics::SpeedLevel::Fast ? 0xFF55FF : level == kinetics::SpeedLevel::Medium ? 0x0084FF : level == kinetics::SpeedLevel::Slow ? 0x22FF22 : 0x000000;
        return mix(base, 0xFFFFFF, .25f);
    }
    if (dial < .5f) return mix(0x00FF00, 0xFFFF00, dial * 2);
    if (dial < 1) return mix(0xFFFF00, 0xFF0000, dial * 2 - 1);
    return 0xFF0000;
}

void setVar(MolangVariableMap& map, std::string const& name, float value) {
    auto full = "variable." + name;
    map.setMolangVariable(HashedString::computeHash(full), full.c_str(), MolangScriptArg(value), false);
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
float rnd() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng()); }

/// GaugeBlock.animateTick, run for the players near the gauge: the client calls it for a random
/// block within 16 blocks 667 times a tick (and within 32 another 667 times), so a gauge gets it
/// about once in 23 ticks; then each head face puffs 4 coloured dust particles (dial > 1) or one
/// with a chance of 1/4.
void animateGauge(BlockSource& region, GlobalPos const& pos, std::string const& id, float dial) {
    if (dial == 0) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    auto        facing = static_cast<Face>(std::clamp(stateAt(region, pos, kFacing, 2), 0, 5));
    bool        along  = stateAt(region, pos, kAlongFirst, 0) != 0;
    unsigned colour = gaugeColour(id, pos, dial);
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) != pos.dim) return true;
        auto   e  = p.getPosition();
        double dx = std::fabs(e.x - pos.x - .5), dy = std::fabs(e.y - pos.y - .5), dz = std::fabs(e.z - pos.z - .5);
        double d  = std::max({dx, dy, dz});
        float  chance = (d <= 16 ? 667.f / (32 * 32 * 32) : 0.f) + (d <= 32 ? 667.f / (64 * 64 * 64) : 0.f);
        if (chance <= 0 || rnd() >= chance) return true;
        for (Face face : ck::kFaces) {
            if (!ct::gaugeHeadOnFace(facing, along, face)) continue;
            auto np = stepPos(pos, face);
            if (region.hasChunksAt(game::blockPos(np), 0, false)
                && region.getBlock(game::blockPos(np)).canProvideFullSupport(static_cast<uchar>(ck::opposite(face))))
                continue;
            int count = dial > 1 ? 4 : 1;
            if (count == 1 && rnd() > .25f) continue;
            auto n = ck::offsetOf(face);
            for (int i = 0; i < count; ++i) {
                // a random direction in the face's plane, 0.3 from the face centre, 0.55 out
                float mx = n.x != 0 ? 0 : rnd() * 2 - 1, my = n.y != 0 ? 0 : rnd() * 2 - 1, mz = n.z != 0 ? 0 : rnd() * 2 - 1;
                float len = std::sqrt(mx * mx + my * my + mz * mz);
                if (len < 1e-4f) continue;
                mx = mx / len * .3f, my = my / len * .3f, mz = mz / len * .3f;
                Vec3 at{pos.x + .5f + n.x * .55f + mx, pos.y + .5f + n.y * .55f + my, pos.z + .5f + n.z * .55f + mz};
                MolangVariableMap map;
                setVar(map, "r", ((colour >> 16) & 0xFF) / 255.f);
                setVar(map, "g", ((colour >> 8) & 0xFF) / 255.f);
                setVar(map, "b", (colour & 0xFF) / 255.f);
                setVar(map, "dir_x", mx);
                setVar(map, "dir_y", my);
                setVar(map, "dir_z", mz);
                SpawnParticleEffectPacket pkt(SpawnParticleEffectPacketPayload(at, kDustEffect, static_cast<uchar>(pos.dim), std::optional<MolangVariableMap>(std::move(map))));
                pkt.sendTo(p);
                ++gDustSent;
            }
        }
        return true;
    });
}

} // namespace

std::size_t gaugeDustSent() { return gDustSent; }

void gaugeTick(BlockSource& region, GlobalPos const& pos) {
    auto  id     = idAt(region, pos);
    float target = computeTarget(id, pos);
    animateGauge(region, pos, id, target);
    auto [it, fresh] = dials().try_emplace(pos, -1.f);
    if (!fresh && it->second == target) return;
    it->second = target;
    setStates(region, pos, {{kDial, ct::dialStateOf(target)}});
    broadcast(pos, {{"dim", pos.dim}, {"set", nlohmann::json::array({entry(pos, target)})}});
}

void gaugeForget(GlobalPos const& pos) {
    if (dials().erase(pos)) broadcast(pos, {{"dim", pos.dim}, {"remove", nlohmann::json::array({nlohmann::json::array({pos.x, pos.y, pos.z})})}});
}

void gaugeSyncTo(Player& player) {
    int  dim  = static_cast<int>(player.getDimensionId());
    auto list = nlohmann::json::array();
    for (auto const& [p, t] : dials())
        if (p.dim == dim) list.push_back(entry(p, t));
    net::send(player, kChannel, {{"dim", dim}, {"reset", true}, {"set", list}});
}

void clearGauges() { dials().clear(); }

} // namespace lv::feature::transmission
