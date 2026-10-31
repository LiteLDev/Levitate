// Trains unit: what the Levitate client mod sends while a player controls a train (Create's
// ControlsInputPacket, ControlsStopControllingPacket, TrainHUDUpdatePacket and HonkPacket), the
// train horn and the HUD data sent back. Vanilla clients drive with the PlayerAuthInputPacket
// (Hooks.cpp); companions send their keys on "trains.interact" and their throttle / horn on
// "trains.throttle".

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/block/Block.h"

#include "core/steam/Whistle.h"
#include "feature/advancements/Advancements.h"
#include "feature/contraption/Contraption.h"
#include "feature/contraption/Internal.h"
#include "feature/net/Net.h"
#include "feature/trains/Trains.h"
#include "mod/Log.h"

namespace lv::feature::trains {

namespace cs = core::steam;

namespace {

constexpr char kWhistle[]   = "levitate:steam_whistle";
constexpr char kExtension[] = "levitate:steam_whistle_extension";

/// Create's Train.honkTicks / lowHonk / honkPitch (the first whistle of the train).
struct Honk {
    int     ticks    = 0;
    int     nextTone = 0;
    bool    resolved = false; ///< determineHonk ran
    bool    found    = false; ///< the train has a whistle
    int     carriage = 0;
    cc::Pos local;
    int     size  = 0; ///< steam's `levitate:size`: 0 medium, 1 small, 2 large
    int     pitch = 0; ///< semitones of extension
};
std::map<std::uint64_t, Honk>& honks() {
    static std::map<std::uint64_t, Honk> m;
    return m;
}

/// Controllers (UUID) whose client mod sent key input in this controlling session.
std::set<std::string>& channelDrivers() {
    static std::set<std::string> s;
    return s;
}

Train* controlledBy(Player const& player) {
    auto uuid = uuidOf(player);
    for (auto& [id, t] : state().trains)
        if (!t.invalid && t.controller == uuid) return &t;
    return nullptr;
}

/// Create's Train.determineHonk: the first steam whistle carried by the train's carriages.
void determineHonk(Train const& t, Honk& h) {
    if (h.resolved) return;
    h.resolved = true;
    for (std::size_t c = 0; c < t.carriages.size(); ++c) {
        auto* con = contraption::find(t.carriages[c].contraption);
        if (!con) continue;
        std::map<cc::Pos, Block const*> blocks;
        for (auto const& b : con->blocks()) blocks[b.local] = contraption::detail::blockFromSnbt(b.block);
        for (auto const& [local, block] : blocks) {
            if (!block || block->getTypeName() != kWhistle) continue;
            h.found    = true;
            h.carriage = static_cast<int>(c);
            h.local    = local;
            h.size     = static_cast<int>(contraption::detail::intStateOf(*block, "levitate:size").value_or(0));
            h.pitch    = cs::whistlePitch([&](int i) -> std::optional<cs::ExtensionShape> {
                auto it = blocks.find(local + cc::Pos{0, i, 0});
                if (it == blocks.end() || !it->second || it->second->getTypeName() != kExtension) return std::nullopt;
                auto shape = contraption::detail::intStateOf(*it->second, "levitate:shape").value_or(0);
                return static_cast<cs::ExtensionShape>(std::clamp<std::int64_t>(shape, 0, 2));
            });
            return;
        }
    }
}

std::optional<ct::Vec3d> honkPosition(Train const& t, Honk const& h) {
    if (h.carriage >= static_cast<int>(t.carriages.size())) return std::nullopt;
    auto* con = contraption::find(t.carriages[static_cast<std::size_t>(h.carriage)].contraption);
    if (!con) return std::nullopt;
    auto w = con->toWorld(cc::Vec3d::center(h.local));
    return ct::Vec3d{w.x, w.y, w.z};
}

void sendHud(Player& player, Train const& t) {
    auto stats = t.stats();
    net::send(
        player,
        "trains.hud",
        {
            {"speed",    t.speed         },
            {"target",   t.targetSpeed   },
            {"top",      stats.maxSpeed  },
            {"throttle", t.throttle      },
            {"name",     t.name          },
            {"approach", t.nav && t.nav->manual},
        }
    );
}

} // namespace

void controlsInput(Player& player, Json const& pressed) {
    if (!pressed.is_array()) return;
    // Create's ControlsUtil order: forward, back, left, right, jump, sneak
    auto held = [&](std::size_t i) { return i < pressed.size() && pressed[i].is_boolean() && pressed[i].get<bool>(); };
    int  keys  = (held(0) ? 1 : 0) - (held(1) ? 1 : 0);
    int  steer = (held(2) ? 1 : 0) - (held(3) ? 1 : 0); // +1: left (A), as the auth input path
    if (controlledBy(player)) channelDrivers().insert(uuidOf(player));
    setInput(player, keys, steer, held(4), held(5));
}

bool drivesByChannel(Player const& player) { return channelDrivers().contains(uuidOf(player)); }

void controlsStop(Player& player) {
    if (auto* t = controlledBy(player)) stopControlling(*t);
}

void setThrottle(Player& player, double throttle) {
    auto* t = controlledBy(player);
    if (!t) return;
    double v = std::clamp(throttle, 1 / 18.0, 1.0);
    if (v == t->throttle) return;
    t->throttle = v;
    log().info("trains: train #{} '{}' throttle {}%", t->id, t->name, static_cast<int>(std::lround(v * 100)));
}

void setHonk(Player& player, bool honk) {
    auto* t = controlledBy(player);
    if (!t) return;
    auto& h = honks()[t->id];
    determineHonk(*t, h);
    if (!h.found) return; // Create: no whistle, no horn (lowHonk == null)
    advancements::award(player, "train_whistle"); // Create's HonkPacket.Serverbound
    bool was = h.ticks > 0;
    // Create's HonkPacket.Serverbound
    if (honk) h.ticks = h.ticks == 0 ? 20 : 13;
    else h.ticks = h.ticks > 5 ? 6 : 0;
    if (!was && h.ticks > 0) {
        h.nextTone = 0;
        log().info("trains: train #{} '{}' honks", t->id, t->name);
    }
}

int honkTicksOf(std::uint64_t trainId) {
    auto it = honks().find(trainId);
    return it == honks().end() ? 0 : it->second.ticks;
}

void controlsChanged(Train const& t, Player* player, bool active) {
    if (!active) honks().erase(t.id);
    if (player) channelDrivers().erase(uuidOf(*player));
    if (!player || !net::isCompanion(*player)) return;
    net::send(*player, "trains.controls", {{"active", active}, {"throttle", t.throttle}, {"name", t.name}});
    if (active) sendHud(*player, t);
}

void tickControls() {
    auto& s = state();
    for (auto it = honks().begin(); it != honks().end();) {
        auto* t = findTrain(it->first);
        if (!t || t->invalid) {
            it = honks().erase(it);
            continue;
        }
        auto& h = it->second;
        if (h.ticks > 0) {
            // Create's CarriageSounds: WHISTLE_TRAIN_MANUAL(_LOW) while honking (a large whistle
            // gives the low horn), its _END sound at 19 and 5 ticks left; pitch 2^(honkPitch / 12)
            // with honkPitch = (small ? 12 : 0) - extension semitones
            bool  low   = h.size == 2;
            float pitch = float(std::pow(2.0, ((h.size == 1 ? 12 : 0) - h.pitch) / 12.0));
            auto  at    = honkPosition(*t, h);
            if (at && h.nextTone <= 0) {
                // the horn sample lasts a second at pitch 1 (the low horn plays it at half speed):
                // restarted just before it ends it sounds continuous
                h.nextTone = std::max(4, int(18 / (pitch * (low ? 0.5f : 1.f))));
                playSound(t->dim, *at, low ? "levitate.trains.horn_low" : "levitate.trains.horn", 2.f, pitch);
            }
            if (at && (h.ticks == 19 || h.ticks == 5))
                playSound(t->dim, *at, low ? "levitate.trains.horn_low_end" : "levitate.trains.horn_end", h.ticks == 5 ? 2.f : 1.f, pitch);
            --h.nextTone;
            --h.ticks;
            if (h.ticks == 0) h.nextTone = 0;
        }
        ++it;
    }
    if (s.tick % 5 != 0) return;
    for (auto& [id, t] : s.trains) {
        if (t.invalid || t.controller.empty()) continue;
        auto* p = playerByUuid(t.controller);
        if (p && net::isCompanion(*p)) sendHud(*p, t);
    }
}

void resetControls() {
    honks().clear();
    channelDrivers().clear();
}

} // namespace lv::feature::trains
