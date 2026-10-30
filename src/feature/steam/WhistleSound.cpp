// Steam unit: the tone of a sounding whistle as one looping sound instance per whistle (Create's
// WhistleSoundInstance). The server gives the instance a sound handle and plays it looping on the
// clients of the players within 64 blocks (PlaySoundPacket with a loop count and a server sound
// handle); pitch changes go to those clients as SetPitch (ClientboundUpdateSoundDataPacket), a
// player entering the range gets the instance, a player leaving it gets Stop, and a whistle that
// stops fades out over 4 ticks (Create: volume - .25 per tick) before Stop. Every client, with or
// without the client mod, plays it (the game's own sound engine).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <variant>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/audio/ServerSoundHandle.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/ClientboundUpdateSoundDataPacket.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"

#include "feature/steam/SteamInternal.h"
#include "mod/Log.h"

namespace lv::feature::steam {

namespace {

constexpr double        kRange     = 64;          ///< Create: maxVolume reaches 0 at 64 blocks
constexpr double        kLeave     = kRange + 4;  ///< hysteresis before a listener gets Stop
constexpr int           kFadeTicks = 4;           ///< Create's fade: 1 -> 0 at .25 per tick
constexpr int           kLoopCount = -1;          ///< loops until stopped
constexpr std::uint64_t kHandleBase = 0x4C56'5354'0000'0000ull; ///< "LVST": clear of the game's own handles

struct Tone {
    std::uint64_t                 handle = 0;
    std::string                   sound;
    float                         pitch = 1;
    std::set<std::int64_t>        listeners; ///< players' unique ids
    std::optional<std::uint64_t>  stopAt;    ///< fading out: Stop at this tick
};

std::map<GlobalPos, Tone>& tones() {
    static std::map<GlobalPos, Tone> m;
    return m;
}
std::uint64_t gNextHandle = 1;

void writeHandle(void* storage, std::uint64_t handle) { std::memcpy(storage, &handle, sizeof(handle)); }

/// The PlaySound of the tone for one player (looping, with the instance's handle).
void sendPlay(Player& player, GlobalPos const& pos, Tone const& t) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt      = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName      = t.sound;
    pkt.mPos       = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume    = 1.f;
    pkt.mPitch     = t.pitch;
    pkt.mLoopCount = kLoopCount;
    // std::optional<ServerSoundHandle> (MSVC layout: the value, then the engaged flag); the
    // handle has no usable constructor, it is an opaque 64-bit id
    auto* raw = reinterpret_cast<unsigned char*>(&pkt.mServerSoundHandle);
    writeHandle(raw, t.handle);
    raw[sizeof(ServerSoundHandle)] = 1;
    pkt.sendTo(player);
}

template <class Event>
void sendEvent(Player& player, std::uint64_t handle, Event const& event) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::ClientboundUpdateSoundData);
    if (!packet) return;
    auto& pkt = static_cast<ClientboundUpdateSoundDataPacket&>(*packet);
    writeHandle(&pkt.mServerSoundHandle, handle);
    *pkt.mEvent = event;
    pkt.sendTo(player);
}

SoundDataEvent::SetPitch setPitch(float pitch) {
    SoundDataEvent::SetPitch e;
    e.mPitch = pitch;
    return e;
}

SoundDataEvent::Fade fadeOut() {
    SoundDataEvent::Fade e;
    e.mDuration     = kFadeTicks / 20.f;
    e.mTargetVolume = 0.f;
    return e;
}

/// Runs `fn` for every listener of a tone that is still online.
template <class Fn>
void forListeners(Tone const& t, Fn&& fn) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (t.listeners.contains(p.getOrCreateUniqueID().rawID)) fn(p);
        return true;
    });
}

void stopNow(Tone const& t) {
    forListeners(t, [&](Player& p) { sendEvent(p, t.handle, SoundDataEvent::Stop{}); });
}

double distanceSq(Player& p, GlobalPos const& pos) {
    auto   e  = p.getPosition();
    double dx = e.x - (pos.x + .5), dy = e.y - (pos.y + .5), dz = e.z - (pos.z + .5);
    return dx * dx + dy * dy + dz * dz;
}

/// Players entering the range get the tone, players leaving it get Stop.
void updateListeners(GlobalPos const& pos, Tone& t) {
    auto level = ll::service::getLevel();
    if (!level) return;
    std::set<std::int64_t> present;
    level->forEachPlayer([&](Player& p) {
        auto id = p.getOrCreateUniqueID().rawID;
        if (static_cast<int>(p.getDimensionId()) != pos.dim) return true;
        double d = distanceSq(p, pos);
        if (t.listeners.contains(id)) {
            if (d > kLeave * kLeave) {
                sendEvent(p, t.handle, SoundDataEvent::Stop{});
                return true;
            }
            present.insert(id);
        } else if (d <= kRange * kRange) {
            sendPlay(p, pos, t);
            present.insert(id);
        }
        return true;
    });
    t.listeners = std::move(present);
}

} // namespace

void whistleTone(GlobalPos const& whistle, std::string const& sound, float pitch) {
    auto& t = tones()[whistle];
    if (t.handle != 0 && (t.sound != sound || t.stopAt)) {
        // a new size (octave) is a new instance (Create: getOctave() != size)
        stopNow(t);
        t = Tone{};
    }
    if (t.handle == 0) {
        t.handle = kHandleBase + gNextHandle++;
        t.sound  = sound;
        t.pitch  = pitch;
        updateListeners(whistle, t);
        log().info("steam: whistle at {} {} {} plays {} as a looping sound for {} player(s)", whistle.x, whistle.y, whistle.z, sound,
                   t.listeners.size());
        return;
    }
    if (std::abs(t.pitch - pitch) > 1e-4f) {
        t.pitch = pitch;
        forListeners(t, [&](Player& p) { sendEvent(p, t.handle, setPitch(pitch)); });
    }
}

void whistleChiff(GlobalPos const& whistle, float pitch) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) != whistle.dim) return true;
        double maxVolume = std::clamp((kRange - std::sqrt(distanceSq(p, whistle))) / kRange, 0.0, 1.0);
        if (maxVolume <= 0) return true;
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
        if (!packet) return true;
        auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
        pkt.mName   = "levitate.steam.whistle_chiff";
        pkt.mPos    = BlockPos{whistle.x * 8 + 4, whistle.y * 8 + 4, whistle.z * 8 + 4};
        pkt.mVolume = static_cast<float>(maxVolume * .175);
        pkt.mPitch  = pitch;
        pkt.sendTo(p);
        return true;
    });
}

void whistleQuiet(GlobalPos const& whistle) {
    auto it = tones().find(whistle);
    if (it == tones().end() || it->second.stopAt) return;
    auto& t  = it->second;
    t.stopAt = currentTick() + kFadeTicks;
    forListeners(t, [&](Player& p) { sendEvent(p, t.handle, fadeOut()); });
}

void tickWhistleTones() {
    auto now = currentTick();
    for (auto it = tones().begin(); it != tones().end();) {
        auto& [pos, t] = *it;
        if (t.stopAt) {
            if (now >= *t.stopAt) {
                stopNow(t);
                it = tones().erase(it);
                continue;
            }
        } else if (now % 2 == 0) {
            updateListeners(pos, t);
        }
        ++it;
    }
}

void resetWhistleTones() {
    for (auto const& [pos, t] : tones()) stopNow(t);
    tones().clear();
}

std::uint64_t whistleToneHandle(GlobalPos const& whistle) {
    auto it = tones().find(whistle);
    return it == tones().end() || it->second.stopAt ? 0 : it->second.handle;
}

int whistleToneListeners(GlobalPos const& whistle) {
    auto it = tones().find(whistle);
    return it == tones().end() ? 0 : static_cast<int>(it->second.listeners.size());
}

} // namespace lv::feature::steam
