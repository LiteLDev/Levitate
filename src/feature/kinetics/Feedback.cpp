// Kinetic feedback (U37 `gaps`, filling U2's "not yet"): Create's rotation indicator particles
// and ambient kinetic soundscapes, sent from the server to every client (vanilla and client mod
// alike: the particle and the sounds are part of Levitate's resource pack, so there is one path
// and no doubled effects). Rules in core/kinetics/Feedback.h, docs/SPEC.md §3.1.
// Selftest positions: testPos dx 3780..3799 (U37).

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>

#include "ll/api/coro/CoroTask.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/SpawnParticleEffectPacket.h"
#include "mc/util/MolangScriptArg.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"

#include "core/kinetics/Feedback.h"
#include "feature/common/Items.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/kinetics/KineticsInternal.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "selftest/SelfTest.h"

// The game doesn't export MolangVariableMap's default constructor; its members (LeviLamina typed
// storage) construct themselves empty (the same inline definition as in feature/bulk/World.cpp).
inline MolangVariableMap::MolangVariableMap() {}

namespace lv::feature::kinetics {

/// `config/gaps.json` (U37): Create's CClient ambience keys, applied by the server for everyone.
struct FeedbackConfig {
    int version = 1;
    /// Create's enableAmbientSounds: the cog / kinetic soundscapes near rotating blocks.
    bool enableAmbientSounds = true;
    /// Create's ambientVolumeCap: the loudest a soundscape gets (before its relative volume).
    float ambientVolumeCap = .1f;
    /// Rotation indicator particles for players wearing Engineer's Goggles (Create: always on).
    bool rotationIndicators = true;
};

} // namespace lv::feature::kinetics

LV_REGISTER_CONFIG("gaps", lv::feature::kinetics::FeedbackConfig);

namespace lv::feature::kinetics {

namespace fb = core::kinetics::feedback;

namespace {

constexpr char kIndicatorEffect[] = "levitate:rotation_indicator";
constexpr char kCogsSound[]       = "levitate.kinetics.cogs";
constexpr char kRumbleSound[]     = "levitate.kinetics.rumble";
constexpr char kGearbox[]         = "levitate:gearbox";

struct Burst {
    GlobalPos     pos;
    int           players = 0;
    int           speed   = 0;
    unsigned      colour  = 0;
    std::uint64_t tick    = 0;
};

struct PlayerScapes {
    std::map<std::pair<fb::Ambience, fb::PitchGroup>, float> pitches; ///< kept while the scape lives
};

struct State {
    std::uint64_t                                       tick = 0;
    std::map<GlobalPos, std::uint64_t>                  queued; ///< indicator bursts due at tick
    std::deque<Burst>                                   bursts; ///< last bursts (selftests)
    std::unordered_map<std::int64_t, PlayerScapes>      players;
    std::uint64_t                                       ambientPlays = 0;
};
State& st() {
    static State s;
    return s;
}

std::pair<float, float> thresholds() {
    auto const& c = config::get<KineticsConfig>();
    return {c.mediumSpeed, c.fastSpeed};
}

std::optional<core::kinetics::Axis> axisOf(KineticSpec const& spec, BlockState const& state) {
    return spec.rotationAxis ? spec.rotationAxis(state) : std::nullopt;
}

core::kinetics::CogType cogOf(KineticSpec const& spec, BlockState const& state) {
    return spec.cog ? spec.cog(state) : core::kinetics::CogType::None;
}

void setVar(MolangVariableMap& map, std::string const& name, float value) {
    auto full = "variable." + name;
    map.setMolangVariable(HashedString::computeHash(full), full.c_str(), MolangScriptArg(value), false);
}

// ---- rotation indicators ----------------------------------------------------------------------

/// KineticEffectHandler.spawnRotationIndicators: 20 particles from the block's centre, sent to
/// the players within 32 blocks; the particle is only visible with goggles on
/// (RotationIndicatorParticle.isVisible), so only those players get it.
void spawnIndicators(GlobalPos const& pos) {
    auto node = nodeAt(pos);
    if (!node) return;
    auto const* spec = specOf(node->state.id);
    if (!spec) return;
    auto [medium, fast] = thresholds();
    auto ind            = fb::indicatorFor(node->speed, axisOf(*spec, node->state), cogOf(*spec, node->state), medium, fast);
    if (!ind) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    double base = std::fmod(static_cast<double>(level->getTime()) * ind->speed, 360.0);
    Vec3   at{static_cast<float>(pos.x) + .5f, static_cast<float>(pos.y) + .5f, static_cast<float>(pos.z) + .5f};
    int    sent = 0;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) != pos.dim) return true;
        auto   e  = p.getPosition();
        double dx = e.x - at.x, dy = e.y - at.y, dz = e.z - at.z;
        if (dx * dx + dy * dy + dz * dz > fb::kIndicatorRange * fb::kIndicatorRange) return true;
        if (!goggles::isWearingGoggles(p)) return true;
        MolangVariableMap map;
        setVar(map, "count", static_cast<float>(ind->count));
        setVar(map, "life", static_cast<float>(ind->lifeSpan));
        setVar(map, "base", static_cast<float>(base));
        setVar(map, "spd", static_cast<float>(ind->speed));
        setVar(map, "ax", static_cast<float>(static_cast<int>(ind->axis)));
        setVar(map, "r1", ind->radius1);
        setVar(map, "r2", ind->radius2);
        auto rgb = [&](char const* prefix, unsigned c) {
            setVar(map, std::string(prefix) + "r", ((c >> 16) & 0xFF) / 255.f);
            setVar(map, std::string(prefix) + "g", ((c >> 8) & 0xFF) / 255.f);
            setVar(map, std::string(prefix) + "b", (c & 0xFF) / 255.f);
        };
        rgb("c", ind->colour);
        rgb("f", ind->fade);
        SpawnParticleEffectPacket pkt(SpawnParticleEffectPacketPayload(at, kIndicatorEffect, static_cast<uchar>(pos.dim), std::optional<MolangVariableMap>(std::move(map))));
        pkt.sendTo(p);
        ++sent;
        return true;
    });
    auto& s = st();
    s.bursts.push_back({pos, sent, ind->speed, ind->colour, s.tick});
    if (s.bursts.size() > 64) s.bursts.pop_front();
    if (sent > 0) log().info("kinetics: rotation indicators at {} {} {} ({} RPM) for {} players", pos.x, pos.y, pos.z, node->speed, sent);
}

void onChange(SpeedChange const& c) {
    if (c.removed || !config::get<FeedbackConfig>().rotationIndicators) return;
    auto const* spec = specOf(c.blockId);
    if (!spec) return;
    bool hasSource      = c.source && *c.source != c.pos;
    auto [medium, fast] = thresholds();
    if (!fb::queuesIndicator(spec->generator, hasSource, c.prevTheoretical, c.theoretical, medium, fast)) return;
    st().queued[c.pos] = st().tick + fb::kIndicatorDelay; // queueRotationIndicators (countdown 2)
}

// ---- soundscapes ------------------------------------------------------------------------------

struct Listener {
    Player*   player = nullptr;
    GlobalPos block;  ///< blockPosition
    Vec3      ears;
    std::vector<fb::Source> sources;
};

/// KineticBlockEntity.tickAudio for every rotating block near a listener: the kinetic hum for
/// every block, the cog sound for cogwheels (small / large / encased) and gearboxes.
void collectSources(std::vector<Listener>& listeners) {
    detail::solver().forEachNode([&](core::kinetics::NodeInfo const& n) {
        if (n.speed == 0) return;
        KineticSpec const* spec = nullptr;
        float              pitch = 0;
        for (auto& l : listeners) {
            if (!fb::inAmbientRange(n.pos, l.block)) continue;
            if (!spec) {
                spec  = specOf(n.state.id);
                pitch = fb::ambientPitch(n.speed);
            }
            l.sources.push_back({fb::Ambience::Kinetic, n.pos, pitch});
            bool cog = spec && cogOf(*spec, n.state) != core::kinetics::CogType::None;
            if (cog || n.state.id == kGearbox) l.sources.push_back({fb::Ambience::Cog, n.pos, pitch});
        }
    });
}

struct Play {
    fb::Scape scape;
    float     volume = 0;
    float     pitch  = 1;
};

/// The soundscapes a listener hears now (SoundScapes.tick + SoundScape.getVolume).
std::vector<Play> scapesFor(Listener const& l, PlayerScapes& kept, float cap) {
    std::vector<Play> out;
    auto              groups = fb::groupSources(l.sources);
    std::map<std::pair<fb::Ambience, fb::PitchGroup>, float> alive;
    for (auto const& g : groups) {
        auto  key = std::make_pair(g.group, g.pitchGroup);
        auto  it  = kept.pitches.find(key);
        float pitch = it != kept.pitches.end() ? it->second : g.pitch;
        alive[key]  = pitch;
        double dx = g.x - l.ears.x, dy = g.y - l.ears.y, dz = g.z - l.ears.z;
        float  v  = fb::scapeVolume(g.count, std::sqrt(dx * dx + dy * dy + dz * dz), cap) * fb::relativeVolume(g.group);
        if (v > 0) out.push_back({g, v, pitch});
    }
    kept.pitches = std::move(alive); // a scape without sources is dropped
    return out;
}

void playScape(Player& p, Listener const& l, Play const& play) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt = static_cast<PlaySoundPacket&>(*packet);
    // Create's continuous sound carries its own distance falloff (getVolume): play it one block
    // from the listener towards the mean position (inside the sound's min distance, so the game
    // adds no falloff of its own but keeps the direction)
    double dx = play.scape.x - l.ears.x, dy = play.scape.y - l.ears.y, dz = play.scape.z - l.ears.z;
    double d  = std::sqrt(dx * dx + dy * dy + dz * dz);
    double k  = d > 1 ? 1 / d : 0;
    pkt.mName   = play.scape.group == fb::Ambience::Cog ? kCogsSound : kRumbleSound;
    pkt.mPos    = BlockPos{static_cast<int>((l.ears.x + dx * k) * 8), static_cast<int>((l.ears.y + dy * k) * 8), static_cast<int>((l.ears.z + dz * k) * 8)};
    pkt.mVolume = play.volume;
    pkt.mPitch  = play.pitch;
    pkt.sendTo(p);
    ++st().ambientPlays;
}

std::vector<Listener> listenersNow() {
    std::vector<Listener> out;
    auto                  level = ll::service::getLevel();
    if (!level) return out;
    level->forEachPlayer([&](Player& p) {
        auto feet = p.getFeetPos();
        out.push_back({&p, GlobalPos{static_cast<int>(p.getDimensionId()), static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.y)), static_cast<int>(std::floor(feet.z))}, p.getPosition(), {}});
        return true;
    });
    return out;
}

void tickAmbience() {
    auto const& cfg       = config::get<FeedbackConfig>();
    auto        listeners = cfg.enableAmbientSounds ? listenersNow() : std::vector<Listener>{};
    if (listeners.empty()) {
        st().players.clear();
        return;
    }
    collectSources(listeners);
    std::unordered_map<std::int64_t, PlayerScapes> next;
    for (auto& l : listeners) {
        auto  id   = l.player->getOrCreateUniqueID().rawID;
        auto& kept = next[id] = std::move(st().players[id]);
        for (auto const& play : scapesFor(l, kept, cfg.ambientVolumeCap)) playScape(*l.player, l, play);
    }
    st().players = std::move(next); // players who left are forgotten
}

void tick() {
    auto& s = st();
    ++s.tick;
    for (auto it = s.queued.begin(); it != s.queued.end();) {
        if (it->second > s.tick) {
            ++it;
            continue;
        }
        auto pos = it->first;
        it       = s.queued.erase(it);
        spawnIndicators(pos);
    }
    if (s.tick % fb::kAmbientInterval == 0) tickAmbience();
}

} // namespace

class FeedbackModule {
public:
    void install() {
        mSpeed = onSpeedChanged({}, [](SpeedChange const& c) { onChange(c); });
        mTick  = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); });
    }
    void uninstall() {
        removeListener(mSpeed);
        ll::event::EventBus::getInstance().removeListener(mTick);
        st() = State{};
    }

private:
    ListenerId              mSpeed = 0;
    ll::event::ListenerPtr  mTick;
};

// ---- selftests (U37: dx 3780..3799) ---------------------------------------------------------------

namespace {

using selftest::Context;

constexpr char const* kSource = "levitate:kinetics_test_source";
constexpr char const* kCog    = "levitate:kinetics_test_cog";
constexpr char const* kShaft  = "levitate:kinetics_test_shaft";

std::optional<Burst> burstAt(GlobalPos const& pos, std::uint64_t since) {
    std::optional<Burst> out;
    for (auto const& b : st().bursts)
        if (b.pos == pos && b.tick > since) out = b;
    return out;
}

void wearGoggles(Player& p, bool on) {
    ItemStack goggles = on ? common::items::make("levitate:goggles").value_or(ItemStack{}) : ItemStack{};
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, goggles);
}

} // namespace

LV_REGISTER_SELFTEST("gaps.feedback_indicators", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3780);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& player = ctx.player();
    wearGoggles(player, true);
    ctx.require(goggles::isWearingGoggles(player), "the test player wears goggles");
    auto mark = st().tick;
    ctx.require(ctx.setBlock(p, kSource, {{"levitate:axis", 0}}), "place the generator");
    ctx.setBlock(GlobalPos{p.dim, p.x + 1, p.y, p.z}, kShaft, {{"levitate:axis", 0}});
    co_await ctx.ticks(5);
    auto first = burstAt(p, mark);
    ctx.check(first.has_value(), "a new generator (none -> slow) shows its indicators");
    if (first) {
        ctx.check(first->players >= 1, "sent to the player with goggles");
        ctx.check(first->speed == 10 && first->colour == 0x22FF22, "slow: green, 10 degrees per tick");
    }
    ctx.check(!burstAt(GlobalPos{p.dim, p.x + 1, p.y, p.z}, mark), "a driven shaft shows none");

    mark = st().tick;
    setGeneratedSpeed(p, 24); // still slow
    co_await ctx.ticks(4);
    ctx.check(!burstAt(p, mark), "no indicators without a speed level change");

    mark = st().tick;
    setGeneratedSpeed(p, -64); // medium
    co_await ctx.ticks(1);
    ctx.check(!burstAt(p, mark), "the indicators wait 2 ticks");
    co_await ctx.ticks(3);
    auto medium = burstAt(p, mark);
    ctx.check(medium && medium->speed == -20 && medium->colour == 0x0084FF, "medium: blue, -20 degrees per tick");

    wearGoggles(player, false);
    mark = st().tick;
    setGeneratedSpeed(p, 128); // fast
    co_await ctx.ticks(4);
    auto fast = burstAt(p, mark);
    ctx.check(fast && fast->speed == 30 && fast->colour == 0xFF55FF, "fast: pink, 30 degrees per tick");
    ctx.check(fast && fast->players == 0, "nobody without goggles gets them");

    mark = st().tick;
    setGeneratedSpeed(p, 0); // stops: queued, but nothing turns
    co_await ctx.ticks(4);
    ctx.check(!burstAt(p, mark), "a stopped generator shows none");
});

LV_REGISTER_SELFTEST("gaps.feedback_ambience", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3790);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kSource, {{"levitate:axis", 0}}), "place the generator");
    GlobalPos cog{p.dim, p.x + 1, p.y, p.z};
    ctx.setBlock(cog, kCog, {{"levitate:axis", 0}});
    co_await ctx.ticks(3);
    ctx.require(speedAt(cog) == 16, "the cog turns");

    auto& player = ctx.player();
    Listener near{&player, p, Vec3{static_cast<float>(p.x) + .5f, static_cast<float>(p.y) + 2.5f, static_cast<float>(p.z) + .5f}, {}};
    near.block.y += 2;
    GlobalPos farBlock = p;
    farBlock.x += 16;
    Listener far{&player, farBlock, Vec3{static_cast<float>(farBlock.x), static_cast<float>(farBlock.y), static_cast<float>(farBlock.z)}, {}};
    std::vector<Listener> ls{near, far};
    collectSources(ls);
    int kinetic = 0, cogs = 0;
    for (auto const& s : ls[0].sources) {
        if (s.pos != p && s.pos != cog) continue; // other tests may be running nearby
        (s.group == fb::Ambience::Cog ? cogs : kinetic)++;
        ctx.check(std::abs(s.pitch - .85f) < 1e-4f, "16 RPM: pitch .85");
    }
    ctx.check(kinetic == 2, "both rotating blocks hum, got " + std::to_string(kinetic));
    ctx.check(cogs == 1, "only the cogwheel ticks, got " + std::to_string(cogs));
    bool farHears = false;
    for (auto const& s : ls[1].sources) farHears = farHears || s.pos == p || s.pos == cog;
    ctx.check(!farHears, "16 blocks away: out of range");

    PlayerScapes kept;
    auto         plays = scapesFor(ls[0], kept, .1f);
    bool         cogScape = false;
    for (auto const& pl : plays)
        if (pl.scape.group == fb::Ambience::Cog) {
            cogScape = true;
            ctx.check(pl.volume > 0 && pl.volume <= .1f * 2 * 1.5f + 1e-4f, "cog scape volume within Create's cap");
            ctx.check(std::abs(pl.pitch - .85f) < 1e-4f, "cog scape pitch .85");
        }
    ctx.check(cogScape, "a cog soundscape plays");

    setGeneratedSpeed(p, 0);
    co_await ctx.ticks(2);
    std::vector<Listener> again{near};
    collectSources(again);
    bool still = false;
    for (auto const& s : again[0].sources) still = still || s.pos == p || s.pos == cog;
    ctx.check(!still, "stopped blocks are silent");
});

} // namespace lv::feature::kinetics

LV_REGISTER_MODULE(lv::feature::kinetics::FeedbackModule);
