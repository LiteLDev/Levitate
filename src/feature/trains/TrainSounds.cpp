// Trains unit: what a running train sounds and looks like from the server, so vanilla clients hear
// and see it too (Create's client-side CarriageSounds / CarriageParticles, played here as world
// sounds and particles), and Create's ArrivalSoundQueue: after a trip of more than 100 blocks the
// train plays its bells, note blocks and whistles front to back as it nears the station.

#include <algorithm>
#include <cmath>
#include <map>
#include <random>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "core/steam/Whistle.h"
#include "feature/contraption/Contraption.h"
#include "feature/contraption/Internal.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "mod/Log.h"

namespace lv::feature::trains {

namespace cs = core::steam;

namespace {

void particle(int dim, ct::Vec3d const& at, char const* effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto d = level->getDimension(DimensionType(dim)).lock();
    if (!d) return;
    level->spawnParticleEffect(effect, Vec3{float(at.x), float(at.y), float(at.z)}, d.get());
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
float rand01() { return std::uniform_real_distribution<float>(0, 1)(rng()); }

/// Per train sound / particle state (Create keeps it on the carriage entities: not saved).
struct Effects {
    int    tick       = 0;
    bool   arrived    = true;
    double release    = 0;    ///< Train.accumulatedSteamRelease
    int    depressure = 0;    ///< CarriageParticles.depressurise
    double prevMotion = 0;
    double brakes     = 0;
    double lastNav    = 1e18; ///< distance to the station last tick (announce the arrival once)
    bool   announce   = false;
    // arrival sounds
    bool                                  playing  = false;
    bool                                  reversed = false;
    int                                   step     = 0;
    std::multimap<int, std::pair<std::uint64_t, cc::Pos>> queue; ///< blocks from the front, by block
};
std::map<std::uint64_t, Effects>& effects() {
    static std::map<std::uint64_t, Effects> m;
    return m;
}

bool isBell(std::string const& id) { return id == "minecraft:bell" || id == "levitate:peculiar_bell" || id == "levitate:haunted_bell"; }

/// Create's ArrivalSoundQueue.isPlayable / play.
void playBlock(Train const& t, std::uint64_t contraption, cc::Pos const& local) {
    auto* con = contraption::find(contraption);
    if (!con) return;
    contraption::CarriedBlock const* carried = nullptr;
    for (auto const& b : con->blocks())
        if (b.local == local) carried = &b;
    if (!carried) return;
    auto w  = con->toWorld(cc::Vec3d::center(local));
    int  dm = con->dimension();
    if (isBell(carried->id)) {
        playSound(dm, {w.x, w.y, w.z}, carried->id == "levitate:haunted_bell" ? "levitate.haunted_bell.use" : "block.bell.hit", 2.f, 1.f);
        return;
    }
    if (carried->id == "minecraft:noteblock") {
        // Create: 2^((note - 12) / 12) of the instrument (Bedrock keeps the note in the block entity)
        int note = 0;
        if (auto tag = CompoundTag::fromSnbt(carried->blockEntity); tag && tag->contains("note"))
            note = int(tag->at("note").get<ByteTag>().data);
        playSound(dm, {w.x, w.y, w.z}, "note.harp", 2.f, float(std::pow(2.0, (note - 12) / 12.0)));
        return;
    }
    if (carried->id == "levitate:steam_whistle") {
        // the whistle and its extensions above (only parsed for a whistle)
        std::map<cc::Pos, Block const*> blocks;
        for (auto const& b : con->blocks())
            if (b.local.x == local.x && b.local.z == local.z && b.local.y >= local.y)
                blocks[b.local] = contraption::detail::blockFromSnbt(b.block);
        auto const* block = blocks[local];
        int         size  = block ? int(contraption::detail::intStateOf(*block, "levitate:size").value_or(0)) : 0;
        int         pitch = cs::whistlePitch([&](int i) -> std::optional<cs::ExtensionShape> {
            auto it = blocks.find(local + cc::Pos{0, i, 0});
            if (it == blocks.end() || !it->second || it->second->getTypeName() != "levitate:steam_whistle_extension") return std::nullopt;
            return static_cast<cs::ExtensionShape>(std::clamp<std::int64_t>(contraption::detail::intStateOf(*it->second, "levitate:shape").value_or(0), 0, 2));
        });
        // Create: WHISTLE_TRAIN(_LOW) at 2^(((small ? 12 : 0) - pitch) / 12)
        float f = float(std::pow(2.0, ((size == 1 ? 12 : 0) - pitch) / 12.0));
        playSound(dm, {w.x, w.y, w.z}, size == 2 ? "levitate.trains.horn_low_end" : "levitate.trains.horn_end", 2.f, f);
    }
}

bool playable(std::string const& id) { return isBell(id) || id == "minecraft:noteblock" || id == "levitate:steam_whistle"; }

/// The playable blocks of the train ordered from its front (one step per block).
std::multimap<int, std::pair<std::uint64_t, cc::Pos>> soundQueue(Train const& t) {
    std::multimap<int, std::pair<std::uint64_t, cc::Pos>> q;
    auto front = ct::positionOf(tracks::railways(), t.body.front);
    if (!front) return q;
    for (auto const& c : t.carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con) continue;
        for (auto const& b : con->blocks()) {
            if (!playable(b.id)) continue;
            auto   w = con->toWorld(cc::Vec3d::center(b.local));
            double d = std::hypot(w.x - front->x, w.z - front->z);
            q.emplace(int(std::lround(d)), std::make_pair(c.contraption, b.local));
        }
    }
    return q;
}

void tickArrival(Train& t, Effects& e) {
    double dist = t.nav ? t.nav->distance : 1e18;
    // Create's Navigation.startNavigation: a trip of more than 100 blocks announces its arrival
    if (t.nav && dist > 100 && e.lastNav > 1e17) e.announce = true;
    if (!t.nav) e.announce = false;
    e.lastNav = t.nav ? dist : 1e18;
    if (e.announce && t.nav && std::abs(dist) < 60) {
        e.announce = false;
        e.queue    = soundQueue(t);
        if (!e.queue.empty()) {
            e.playing  = true;
            e.reversed = t.speed < 0;
            e.step     = e.reversed ? e.queue.rbegin()->first : e.queue.begin()->first;
            log().info("trains: train #{} '{}' announces its arrival ({} sound blocks)", t.id, t.name, e.queue.size());
        }
    }
    if (!e.playing || e.tick % 2 == 0) return;
    auto [from, to] = e.queue.equal_range(e.step);
    for (auto it = from; it != to; ++it) playBlock(t, it->second.first, it->second.second);
    bool more = e.reversed ? e.step > e.queue.begin()->first : e.step < e.queue.rbegin()->first;
    if (!more) {
        e.playing = false;
        e.queue.clear();
        return;
    }
    e.step += e.reversed ? -1 : 1;
}

/// Create's CarriageSounds (as world sounds) and CarriageParticles for one train.
void tickEffects(Train& t, Effects& e) {
    ++e.tick;
    if (t.carriages.empty()) return;
    auto* lead = contraption::find(t.carriages.front().contraption);
    if (!lead) return;
    double motion  = std::abs(t.speed);
    bool   stalled = t.speedBeforeStall.has_value();
    if (e.arrived && motion > 0.01) e.arrived = false;
    if (e.arrived) e.release /= 2;
    e.arrived = e.arrived || stalled;
    e.release = std::min(e.release + std::min(0.5, motion / 10), 10.0);
    auto origin = lead->toWorld(cc::Vec3d::center(t.carriages.front().bogeyLocal.empty() ? cc::Pos{} : t.carriages.front().bogeyLocal.front()));
    ct::Vec3d at{origin.x, origin.y, origin.z};
    int       dim = lead->dimension();
    if (!e.arrived && motion > 0.01) {
        float volume = float(std::min(1.0, motion * 2));
        // the steam rhythm of the leading carriage
        if ((3 + e.tick) % 4 == 0) playSound(dim, at, "levitate.trains.steam", volume * ((e.tick + 7) % 8 == 0 ? 0.75f : 0.45f), 1.17f);
        if (e.tick % 16 == 0) playSound(dim, at, "levitate.trains.steam", volume * 1.5f, 0.8f);
        // the wheels: faster and higher with the speed
        int every = std::clamp(int(8 / std::max(0.25, motion * 4)), 3, 16);
        if (e.tick % every == 0) playSound(dim, at, "levitate.trains.rolling", volume, float(std::clamp(motion * 2 + .25, .75, 1.95)));
    }
    bool stopped = motion < 0.002;
    if (stopped && !e.arrived && e.release > 1) {
        // the release when it comes to a stop: a hiss and a clunk
        e.arrived      = true;
        float rel      = float(e.release / 10);
        e.depressure   = int(20 * e.release / 10);
        playSound(dim, at, "random.fizz", .25f * rel, .78f);
        playSound(dim, at, "random.door_close", .2f * rel, 1.5f);
        playSound(dim, at, "levitate.trains.steam", .75f * rel, .5f);
    } else if (stopped && !e.arrived) {
        e.arrived = true;
    }
    if (!stopped) e.depressure = 0;
    if (e.depressure > 0) --e.depressure;
    e.brakes     = e.brakes + ((e.prevMotion > motion + motion / 512 ? 1.0 : 0.0) - e.brakes) * 0.625;
    e.prevMotion = motion;
    // particles at every bogey: sparks while braking fast, steam while depressurising
    for (auto const& c : t.carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con) continue;
        for (auto const& local : c.bogeyLocal) {
            bool  spark  = e.depressure == 0 || e.depressure > 10;
            float cutoff = motion < 1 / 8.0 ? 0.f : 1 / 8.f;
            if (motion > 1 / 6.0) cutoff = std::max(cutoff, float(e.brakes * 1.15));
            if (rand01() > cutoff && (spark || rand01() > 0.25f)) continue;
            auto w = con->toWorld(cc::Vec3d::center(local));
            particle(con->dimension(), {w.x, w.y + (spark ? -.6 : .32), w.z}, spark ? "minecraft:electric_spark_particle" : "minecraft:basic_smoke_particle");
        }
    }
}

} // namespace

void tickTrainEffects() {
    auto& all = effects();
    for (auto it = all.begin(); it != all.end();) {
        auto* t = findTrain(it->first);
        if (!t) it = all.erase(it);
        else ++it;
    }
    for (auto& [id, t] : state().trains) {
        if (t.invalid || t.disassembling) continue;
        auto& e = all[id];
        tickEffects(t, e);
        tickArrival(t, e);
    }
}

void resetTrainEffects() { effects().clear(); }

} // namespace lv::feature::trains
