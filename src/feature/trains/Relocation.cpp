// Train relocation with the wrench (Create's TrainRelocator, U38 R146).
//
// Right-clicking a standing train with the wrench selects it ("Click a Track to Relocate ... to.
// Sneak-Click to abort"). While the player holds the wrench, the track they look at is checked:
// the train must fit on the track there (every point on track, facing the look direction) within
// 24 blocks of where it stands; the action bar says whether it can be relocated, and the client
// mod draws the carriages at the target while they fit there. Clicking the track moves the
// train there (also a derailed one), sneak-clicking aborts. A moving train can't be relocated.

#include <cmath>
#include <map>
#include <optional>
#include <string>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/phys/HitResult.h"

#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/net/Net.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::trains {

void relocate(Train& t, ct::Body body);

namespace {

namespace ia = common::interactions;

constexpr double kReach         = 5.0;
constexpr double kMaxRelocation = 24.0; ///< Create: the train may move at most this far

struct Relocating {
    std::uint64_t           train = 0;
    std::optional<ct::Body> target; ///< the valid target looked at
    std::string             status; ///< last action bar key sent
    std::string             sent;   ///< last ghost sent to the client mod
};

std::map<std::string, Relocating>& relocating() {
    static std::map<std::string, Relocating> r;
    return r;
}

/// Distance along the ray to a box (nullopt: missed).
std::optional<double> rayBox(cc::Vec3d const& o, cc::Vec3d const& d, cc::Box const& b) {
    double lo[3]{b.min.x, b.min.y, b.min.z}, hi[3]{b.max.x, b.max.y, b.max.z};
    double org[3]{o.x, o.y, o.z}, dir[3]{d.x, d.y, d.z};
    double tmin = -1e18, tmax = 1e18;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-9) {
            if (org[i] < lo[i] || org[i] > hi[i]) return std::nullopt;
            continue;
        }
        double t1 = (lo[i] - org[i]) / dir[i], t2 = (hi[i] - org[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return std::nullopt;
    }
    if (tmax < 0) return std::nullopt;
    return std::max(0.0, tmin);
}

cc::Vec3d ccv(Vec3 const& v) { return {v.x, v.y, v.z}; }

/// The train whose carriage the player looks at within reach.
Train* trainLookedAt(Player& player) {
    cc::Vec3d eye = ccv(player.getEyePos()), dir = ccv(player.getViewVector());
    Train*    best = nullptr;
    double    bestD = kReach;
    for (auto& [id, t] : state().trains) {
        if (t.invalid || t.dim != int(player.getDimensionId())) continue;
        for (auto const& car : t.carriages) {
            auto* con = contraption::find(car.contraption);
            if (!con) continue;
            auto const& tr = con->transform();
            auto lo = tr.toLocal(eye), ld = tr.unrotateVector(dir);
            for (auto const& b : con->blocks()) {
                if (auto d = rayBox(lo, ld, cc::Box::unit(b.local)); d && *d < bestD) {
                    bestD = *d;
                    best  = &t;
                }
            }
        }
    }
    return best;
}

void say(Player& player, Relocating& r, std::string const& key, std::string const& fallback, char colour, std::vector<std::string> const& args = {}) {
    if (r.status == key) return;
    r.status = key;
    actionBar(player, std::string("§") + colour + trFor(player, key, fallback, args.empty() ? std::string() : args[0]));
}

/// The carriages at the target as 8 world corners each, for the client mod's preview.
nlohmann::json ghostOf(Train const& t, ct::Body const& body) {
    auto boxes = nlohmann::json::array();
    for (auto const& [id, tr] : carriageTransforms(t, body)) {
        auto bounds = contraption::bounds(id);
        if (!bounds) continue;
        auto corners = nlohmann::json::array();
        for (int i = 0; i < 8; ++i) {
            cc::Vec3d c{i & 1 ? bounds->max.x : bounds->min.x, i & 2 ? bounds->max.y : bounds->min.y, i & 4 ? bounds->max.z : bounds->min.z};
            auto      w = tr.toWorld(c);
            corners.push_back(nlohmann::json::array({w.x, w.y, w.z}));
        }
        boxes.push_back(std::move(corners));
    }
    return boxes;
}

void sendGhost(Player& player, Relocating& r, nlohmann::json msg) {
    auto text = msg.dump();
    if (text == r.sent) return;
    r.sent = text;
    net::send(player, "trains.relocate", msg);
}

void clearGhost(Player& player) {
    if (net::isCompanion(player)) net::send(player, "trains.relocate", {{"boxes", nlohmann::json::array()}});
}

void stop(Player& player, std::string const& uuid) {
    relocating().erase(uuid);
    clearGhost(player);
}

/// Create's TrainRelocator.visualise / relocateClient: what the player looks at now.
void evaluate(Player& player, Relocating& r) {
    auto* t = findTrain(r.train);
    if (!t) return;
    r.target.reset();
    auto hit = player.traceRay(static_cast<float>(kReach), false, true);
    if (hit.mType != HitResultType::Tile) {
        say(player, r, "create.train.relocate", "Click a Track to Relocate {0} to. Sneak-Click to abort", 'f', {t->name});
        sendGhost(player, r, {{"boxes", nlohmann::json::array()}});
        return;
    }
    GlobalPos block{int(player.getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    if (idAt(block) != tracks::kTrackId) {
        say(player, r, "create.train.relocate", "Click a Track to Relocate {0} to. Sneak-Click to abort", 'f', {t->name});
        sendGhost(player, r, {{"boxes", nlohmann::json::array()}});
        return;
    }
    ct::Vec3d point{block.x + 0.5, double(block.y), block.z + 0.5};
    double    dx = point.x - t->lastFront.x, dy = point.y - t->lastFront.y, dz = point.z - t->lastFront.z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kMaxRelocation) {
        say(player, r, "create.train.relocate.too_far", "Cannot relocate Train this far away", 'c');
        sendGhost(player, r, {{"boxes", nlohmann::json::array()}});
        return;
    }
    auto      look = player.getViewVector();
    ct::Vec3d dir{look.x, 0, look.z};
    auto      body = bodyAt(*t, point, dir, 1.0);
    if (body && overlapsOtherTrain(*t, *body)) body.reset(); // Create: another train is in the way
    if (body && crossesPortal(*body)) body.reset();       // Create: the probe may not pass a portal
    if (!body) {
        say(player, r, "create.train.relocate.invalid", "Cannot relocate Train to here", 'c');
        // Create shows where it would stand: the train's points on the nearest track anyway
        sendGhost(player, r, {{"boxes", nlohmann::json::array()}, {"valid", false}});
        return;
    }
    r.target = body;
    say(player, r, "create.train.relocate.valid", "Can relocate to here, Click to Confirm", 'a');
    sendGhost(player, r, {{"boxes", ghostOf(*t, *body)}, {"valid", true}});
}

bool holdsWrench(Player& player) { return common::items::idOf(player.getSelectedItem()) == kWrench; }

/// A click with the wrench (`onBlock`: on a block). True when it was ours.
bool click(Player& player, bool sneaking) {
    auto uuid = uuidOf(player);
    auto it   = relocating().find(uuid);
    if (it != relocating().end()) {
        auto& r = it->second;
        if (sneaking) {
            actionBar(player, "§f" + trFor(player, "create.train.relocate.abort", "Relocation aborted"));
            log().info("trains: {} aborted the relocation of train #{}", player.getRealName(), r.train);
            stop(player, uuid);
            return true;
        }
        evaluate(player, r);
        auto* t = findTrain(r.train);
        if (!t || !r.target) {
            r.status.clear();
            evaluate(player, r); // say again why not
            return true;
        }
        relocate(*t, std::move(*r.target));
        actionBar(player, "§a" + trFor(player, "create.train.relocate.success", "Relocation successful"));
        log().info("trains: train #{} '{}' relocated by {}", t->id, t->name, player.getRealName());
        stop(player, uuid);
        return true;
    }
    if (sneaking) return false;
    auto* t = trainLookedAt(player);
    if (!t) return false;
    if (std::abs(t->speed) > 1e-6) {
        actionBar(player, "§c" + trFor(player, "create.train.cannot_relocate_moving", "Cannot relocate a moving Train"));
        return true;
    }
    auto& r = relocating()[uuid];
    r       = Relocating{t->id};
    log().info("trains: {} is relocating train #{} '{}'", player.getRealName(), t->id, t->name);
    evaluate(player, r);
    return true;
}

std::map<std::string, std::uint64_t>& lastClick() {
    static std::map<std::string, std::uint64_t> m;
    return m;
}

/// Bedrock repeats a held click: one per 5 ticks.
bool repeated(Player& player) {
    auto& last = lastClick()[uuidOf(player)];
    auto  now  = state().tick;
    bool  rep  = last && now - last < 5;
    if (!rep) last = now;
    return rep;
}

} // namespace

void relocate(Train& t, ct::Body body) {
    // Create's TrainRelocator.relocate: the train stands on the new track, stopped
    t.body             = std::move(body);
    t.derailed         = false;
    t.trackMissingSent = false;
    t.speed = t.targetSpeed = 0;
    t.reattachCooldown = 0;
    cancelNavigation(t);
    t.currentStation.clear(); // it no longer stands at the station it was at
    placeCarriages(t);
    if (auto p = ct::positionOf(tracks::railways(), t.body.front)) t.lastFront = *p;
    auto d = t.body.front.to.location() - t.body.front.from.location();
    if (d.length() > 1e-6) t.lastDir = d.normalize();

}

bool relocationClick(Player& player, bool sneaking) { return click(player, sneaking); }

bool isRelocating(Player const& player) { return relocating().contains(uuidOf(player)); }

class RelocationModule {
public:
    void install() {
        mHandlers.push_back(ia::onUseOn({"", std::string(kWrench)}, [](ia::UseOnContext const& c) {
            if (!isRelocating(c.player) && (c.sneaking || !trainLookedAt(c.player))) return ia::Result::Pass;
            if (!repeated(c.player)) click(c.player, c.sneaking);
            c.player.sendInventory(false);
            return ia::Result::Consume;
        }, ia::priority::High));
        mHandlers.push_back(ia::onUse(std::string(kWrench), [](ia::UseContext const& c) {
            if (c.onBlock) return ia::Result::Pass;
            if (!isRelocating(c.player) && (c.sneaking || !trainLookedAt(c.player))) return ia::Result::Pass;
            if (!repeated(c.player)) click(c.player, c.sneaking);
            return ia::Result::Consume;
        }, ia::priority::High));
        // the carriage blocks are display entities: a click on one is an entity interaction
        mHandlers.push_back(ia::onInteract({"minecraft:falling_block", std::string(kWrench)}, [](ia::EntityContext const& c) {
            if (!isRelocating(c.player) && (c.sneaking || !trainLookedAt(c.player))) return ia::Result::Pass;
            if (!repeated(c.player)) click(c.player, c.sneaking);
            return ia::Result::Consume;
        }, ia::priority::High));
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            if (relocating().empty() || state().tick % 2) return;
            std::vector<std::string> gone;
            for (auto& [uuid, r] : relocating()) {
                auto* p = playerByUuid(uuid);
                if (!p || !findTrain(r.train) || !holdsWrench(*p)) {
                    // Create: putting the wrench away ends the relocation
                    gone.push_back(uuid); // (erased after the loop)
                    if (p) clearGhost(*p);
                    continue;
                }
                evaluate(*p, r);
            }
            for (auto const& u : gone) relocating().erase(u);
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mTick);
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
        relocating().clear();
    }

private:
    std::vector<ia::HandlerId> mHandlers;
    ll::event::ListenerPtr     mTick;
};

} // namespace lv::feature::trains

LV_REGISTER_MODULE(lv::feature::trains::RelocationModule);
