// Cuckoo clocks (U49): Create's CuckooClockBlockEntity on the server. While the clock turns its
// hands follow the time of day (block states `levitate:hour` / `levitate:minute`, the static hands
// of vanilla clients; the client mod chases the exact angles itself), it ticks (note block hat),
// and in the overworld the pig comes out at noon and the creeper at dusk (sounds, the doors and
// figure as `levitate:cuckoo` for vanilla clients, `transmission.cuckoo` messages for the client
// mod's animation). The mysterious cuckoo clock turns either into the surprise: smoke, the fuse,
// and at progress 50 it removes itself and explodes (power 3, no fire, blocks break) with Create's
// death message. Elsewhere (not the overworld) the hands spin 24 times faster and it ticks every
// 3 ticks. Sounds play on the server for every client (Create plays them client-side).

#include <algorithm>
#include <cfloat>
#include <map>
#include <random>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/dimension/Dimension.h"

#include "core/transmission/CuckooClock.h"
#include "feature/advancements/Advancements.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::transmission {

namespace {

using ct::CuckooAnimation;

constexpr char kChannel[]      = "transmission.cuckoo";
constexpr char kDeathMessage[] = "death.attack.create.cuckoo_surprise";

struct ClockState {
    CuckooAnimation animation = CuckooAnimation::None;
    int             progress  = 0;
};

std::map<GlobalPos, ClockState>& clocks() {
    static std::map<GlobalPos, ClockState> m;
    return m;
}

std::optional<std::int64_t>& dayTimeOverride() {
    static std::optional<std::int64_t> t;
    return t;
}

/// Server tick of the last surprise explosion + 1 (0: none): deaths by explosions in that tick
/// and the next get Create's death message.
std::uint64_t gSurpriseTick = 0;

std::uint64_t serverTick() {
    auto level = ll::service::getLevel();
    return level ? level->getCurrentServerTick().tickID : 0;
}

nlohmann::json entry(GlobalPos const& p, ClockState const& s) {
    return nlohmann::json::array({p.x, p.y, p.z, static_cast<int>(s.animation), s.progress});
}

void broadcast(int dim, nlohmann::json const& msg) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) == dim && net::isCompanion(p)) net::send(p, kChannel, msg);
        return true;
    });
}

char const* animationName(CuckooAnimation a) {
    switch (a) {
    case CuckooAnimation::Pig: return "pig";
    case CuckooAnimation::Creeper: return "creeper";
    case CuckooAnimation::Surprise: return "surprise";
    default: return "none";
    }
}

void start(BlockSource& region, GlobalPos const& pos, CuckooAnimation requested) {
    bool  mysterious = idAt(region, pos) == kMysteriousCuckooClock;
    auto& s          = clocks()[pos];
    s.animation      = ct::cuckooStarted(requested, mysterious);
    s.progress       = 0;
    broadcast(pos.dim, {{"dim", pos.dim}, {"set", nlohmann::json::array({entry(pos, s)})}});
    log().info("transmission: cuckoo clock at {} starts the {} animation", pos.key(), animationName(s.animation));
    // Create's AllAdvancements.CUCKOO_CLOCK: awarded when the creeper (bedtime) comes out
    if (requested == CuckooAnimation::Creeper) advancements::awardOwnerIfNear(pos, "cuckoo_clock", 32);
}

void surprise(BlockSource& region, GlobalPos const& pos) {
    auto level = ll::service::getLevel();
    if (!level) return;
    log().info("transmission: mysterious cuckoo clock at {} explodes", pos.key());
    clocks().erase(pos);
    broadcast(pos.dim, {{"dim", pos.dim}, {"remove", nlohmann::json::array({nlohmann::json::array({pos.x, pos.y, pos.z})})}});
    level->destroyBlock(region, game::blockPos(pos), false, BlockChangeContext{});
    gSurpriseTick = serverTick() + 1;
    level->explode(region, nullptr, Vec3{pos.x + .5f, pos.y + .5f, pos.z + .5f}, ct::kCuckooSurprisePower, false, true, FLT_MAX, false);
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

void smoke(GlobalPos const& pos) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(pos.dim).lock();
    if (!dimension) return;
    // VecHelper.offsetRandomly(centre, .5): up to half a block off the centre on each axis
    std::uniform_real_distribution<float> d(-.5f, .5f);
    Vec3 at{pos.x + .5f + d(rng()), pos.y + .5f + d(rng()), pos.z + .5f + d(rng())};
    level->spawnParticleEffect("minecraft:basic_smoke_particle", at, dimension.get());
}

} // namespace

std::int64_t clockDayTime() {
    if (dayTimeOverride()) return *dayTimeOverride();
    auto level = ll::service::getLevel();
    return level ? level->getTime() : 0;
}

void setClockDayTimeOverride(std::optional<std::int64_t> dayTime) { dayTimeOverride() = dayTime; }

void clockTick(BlockSource& region, GlobalPos const& pos) {
    if (kinetics::speedAt(pos) == 0) return; // Create: nothing happens while the clock stands still
    bool natural = pos.dim == 0;
    auto time    = ct::clockTime(clockDayTime(), natural);
    auto& s      = clocks()[pos];

    // the hands (vanilla clients' static model; the client mod chases the angles itself)
    int hour = ct::hourHandState(time), minute = ct::minuteHandState(time);
    if (stateAt(region, pos, kHour, -1) != hour || stateAt(region, pos, kMinute, -1) != minute) {
        setStates(region, pos, {{kHour, hour}, {kMinute, minute}});
    }
    auto sound = [&](ct::ClockSound const& cs) { playSound(pos.dim, pos, cs.name, cs.volume, cs.pitch); };
    if (s.animation == CuckooAnimation::None || !natural) {
        if (auto tick = ct::cuckooTickSound(serverTick(), natural)) sound(*tick);
    }
    if (!natural) return;

    if (s.animation == CuckooAnimation::None) {
        auto trigger = ct::cuckooTrigger(time);
        if (trigger != CuckooAnimation::None) start(region, pos, trigger);
    } else {
        int  value = s.progress;
        auto type  = s.animation;
        s.progress = value + 1;
        for (auto const& cs : ct::cuckooAnimationSounds(type, value)) sound(cs);
        if (ct::cuckooSmokes(type, value)) smoke(pos);
        if (value > ct::kCuckooAnimationEnd) s.animation = CuckooAnimation::None;
        if (s.animation == CuckooAnimation::Surprise && s.progress == ct::kCuckooSurpriseTick) {
            surprise(region, pos);
            return;
        }
    }
    int model = ct::cuckooModelState(s.animation, static_cast<float>(s.progress));
    if (stateAt(region, pos, kCuckoo, -1) != model) setStates(region, pos, {{kCuckoo, model}});
}

std::pair<int, int> clockAnimation(GlobalPos const& pos) {
    auto it = clocks().find(pos);
    if (it == clocks().end()) return {0, 0};
    return {static_cast<int>(it->second.animation), it->second.progress};
}

void startClockAnimation(BlockSource& region, GlobalPos const& pos, int animation) {
    start(region, pos, static_cast<CuckooAnimation>(std::clamp(animation, 0, 2)));
}

void clockForget(GlobalPos const& pos) {
    if (clocks().erase(pos)) {
        broadcast(pos.dim, {{"dim", pos.dim}, {"remove", nlohmann::json::array({nlohmann::json::array({pos.x, pos.y, pos.z})})}});
    }
}

void clockSyncTo(Player& player) {
    int  dim  = static_cast<int>(player.getDimensionId());
    auto list = nlohmann::json::array();
    for (auto const& [p, s] : clocks())
        if (p.dim == dim && s.animation != CuckooAnimation::None) list.push_back(entry(p, s));
    net::send(player, kChannel, {{"dim", dim}, {"reset", true}, {"set", list}});
}

void clearClocks() {
    clocks().clear();
    dayTimeOverride().reset();
}

} // namespace lv::feature::transmission

// CreateDamageSources.cuckooSurprise: deaths by the surprise's explosion get Create's message
// ("death.attack.create.cuckoo_surprise", translated by the resource pack).
using LvCuckooDeathMessage = std::pair<std::string, std::vector<std::string>>;
LL_TYPE_INSTANCE_HOOK(
    CuckooSurpriseDeathMessageHook,
    ll::memory::HookPriority::Normal,
    ActorDamageSource,
    &ActorDamageSource::getDeathMessage,
    LvCuckooDeathMessage,
    std::string const& deadName,
    Actor*             dead
) {
    using lv::feature::transmission::gSurpriseTick;
    if (gSurpriseTick != 0) {
        SharedTypes::Legacy::ActorDamageCause cause = mCause;
        if (cause == SharedTypes::Legacy::ActorDamageCause::BlockExplosion || cause == SharedTypes::Legacy::ActorDamageCause::EntityExplosion) {
            auto now = lv::feature::transmission::serverTick();
            if (now + 1 >= gSurpriseTick && now <= gSurpriseTick) {
                return {lv::feature::transmission::kDeathMessage, {deadName}};
            }
        }
    }
    return origin(deadName, dead);
}

namespace lv::feature::transmission {

void installClockHooks() { CuckooSurpriseDeathMessageHook::hook(); }
void uninstallClockHooks() { CuckooSurpriseDeathMessageHook::unhook(); }

/// The death message the surprise gives (selftests).
std::string surpriseDeathMessage(std::string const& name) {
    ActorDamageSource src(SharedTypes::Legacy::ActorDamageCause::BlockExplosion, "");
    return src.getDeathMessage(name, nullptr).first;
}

/// Marks a surprise explosion now (selftests of the death message).
void markSurpriseForTest() { gSurpriseTick = serverTick() + 1; }

} // namespace lv::feature::transmission
