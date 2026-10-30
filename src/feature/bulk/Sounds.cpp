// Sounds of the bulk processing machines that Create plays on the client, sent by the server to
// every player (vanilla and client mod):
//
// - Create's MILLING / CRUSHING soundscapes (SoundScapes.milling / crushing): millstones with an
//   input and crushing wheel controllers with something inside group per listener and pitch group
//   (SoundScapes.addSound) and repeat vanilla sounds at Create's rates (RepeatingSound: CRUSHING_1
//   = netherrack hit every tick, CRUSHING_2 = gravel place every 2 ticks, crushing only:
//   CRUSHING_3 = netherite break every 2 ticks) at the soundscape's volume (SoundScape.getVolume).
// - Create's air current wind (AirCurrent.enableClientPlayerSound): a player caught in a fan's
//   current hears wind, louder with the fan's speed, higher with the player's speed. Create loops
//   the elytra sound; Levitate plays its own 0.5 s wind sample (tools/sounds/bulk.ps1) every 10
//   ticks.

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"

#include "core/kinetics/Feedback.h"
#include "feature/bulk/Bulk.h"

namespace lv::feature::bulk {

namespace fb = core::kinetics::feedback;

namespace {

/// Create's ambientVolumeCap default (CClient).
constexpr float kVolumeCap = .1f;

struct Repeating {
    char const* sound;
    float       relativeVolume;
    float       relativePitch;
    int         delay;
};
constexpr Repeating kMilling[] = {
    {"hit.netherrack", 1.545f, .75f, 1},
    {"dig.gravel",     .425f,  .75f, 2}
};
constexpr Repeating kCrushing[] = {
    {"hit.netherrack", 1.545f, .75f,  1},
    {"dig.gravel",     .425f,  .75f,  2},
    {"dig.netherite",  2.f,    1.75f, 2}
};

struct ScapeSource {
    Scape     group;
    GlobalPos pos;
    float     pitch;
};

struct Wind {
    float         volume = 0;
    float         pitch  = 1;
    std::uint64_t tick   = 0;
};

struct SoundState {
    std::vector<ScapeSource>               sources;
    std::unordered_map<std::int64_t, Wind> wind; ///< players in a current this tick
    int                                    plays = 0;
};
SoundState& st() {
    static SoundState s;
    return s;
}

void playTo(Player& p, std::string const& name, Vec3 const& at, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = name;
    pkt.mPos    = BlockPos{static_cast<int>(at.x * 8), static_cast<int>(at.y * 8), static_cast<int>(at.z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(p);
    ++st().plays;
}

} // namespace

void addScapeSource(Scape group, GlobalPos const& pos, float pitch) { st().sources.push_back({group, pos, pitch}); }

void addWindListener(Actor const& player, float fanSpeed, float playerSpeed) {
    auto& w  = st().wind[idOf(player)];
    w.volume = std::clamp(std::abs(fanSpeed) / 128.f * .4f, .01f, .4f);
    w.pitch  = std::clamp(playerSpeed * .5f, .5f, 2.f);
    w.tick   = currentTick();
}

int soundPlays() { return st().plays; }

void tickSounds() {
    auto& s     = st();
    auto  level = ll::service::getLevel();
    if (!level) {
        s.sources.clear();
        s.wind.clear();
        return;
    }
    auto tick = currentTick();
    if (!s.sources.empty()) {
        level->forEachPlayer([&](Player& p) {
            auto      feet = p.getFeetPos();
            GlobalPos at{
                static_cast<int>(p.getDimensionId()),
                int(std::floor(feet.x)),
                int(std::floor(feet.y)),
                int(std::floor(feet.z))
            };
            Vec3 ears = p.getPosition();
            // SoundScapes.addSound: by (group, pitch group); SoundScape.getMeanPos / getVolume
            struct Group {
                double x = 0, y = 0, z = 0;
                int    count = 0;
            };
            std::map<std::pair<Scape, fb::PitchGroup>, Group> groups;
            for (auto const& src : s.sources) {
                if (!fb::inAmbientRange(src.pos, at)) continue;
                auto& g  = groups[{src.group, fb::pitchGroupOf(src.pitch)}];
                g.x     += src.pos.x + .5;
                g.y     += src.pos.y + .5;
                g.z     += src.pos.z + .5;
                ++g.count;
            }
            for (auto const& [key, g] : groups) {
                double mx = g.x / g.count, my = g.y / g.count, mz = g.z / g.count;
                double dx = mx - ears.x, dy = my - ears.y, dz = mz - ears.z;
                double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                float  v = fb::scapeVolume(g.count, d, kVolumeCap);
                if (v <= 0) continue;
                // the scape's own falloff is in its volume: played one block from the ears
                double k = d > 1 ? 1 / d : 0;
                Vec3   to{float(ears.x + dx * k), float(ears.y + dy * k), float(ears.z + dz * k)};
                auto   play = [&](Repeating const& r) {
                    if (tick % r.delay == 0) playTo(p, r.sound, to, v * r.relativeVolume, r.relativePitch);
                };
                if (key.first == Scape::Milling)
                    for (auto const& r : kMilling) play(r);
                else
                    for (auto const& r : kCrushing) play(r);
            }
            return true;
        });
    }
    s.sources.clear();
    // the wind: every 10 ticks while the player is in a current (the sample is 0.5 s long)
    std::erase_if(s.wind, [&](auto const& kv) { return kv.second.tick != tick; });
    if (tick % 10 == 0) {
        for (auto const& [id, w] : s.wind) {
            auto* a = entityById(id);
            if (!a || !a->isPlayer()) continue;
            auto& p = static_cast<Player&>(*a);
            playTo(p, "levitate.fan_wind", p.getPosition(), w.volume, w.pitch);
        }
    }
}

void clearSounds() { st() = SoundState{}; }

} // namespace lv::feature::bulk
