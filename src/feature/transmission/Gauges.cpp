// Speedometer and stressometer (U6): Create's SpeedGaugeBlockEntity / StressGaugeBlockEntity.
// The dial target follows the speed (speed levels) or the network's stress / capacity; it drives
// the comparator output, the static needle of vanilla clients (`levitate:dial`, 9 positions) and
// the smooth needle the client mod draws (`transmission.gauge` messages to companions).

#include <cmath>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"

#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/net/Net.h"
#include "feature/transmission/TransmissionInternal.h"
#include "mod/Config.h"

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

} // namespace

float dialTarget(GlobalPos const& pos) {
    auto it = dials().find(pos);
    return it == dials().end() ? 0.f : it->second;
}

void gaugeTick(BlockSource& region, GlobalPos const& pos) {
    auto  id     = idAt(region, pos);
    float target = computeTarget(id, pos);
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
