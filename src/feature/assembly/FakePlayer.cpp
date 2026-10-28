// The deployer's player (Create's DeployerFakePlayer).
//
// Bedrock has no fake players, but a simulated player is a real server player whose actions go
// through the same GameMode code as everyone's (item use, block placement, attacks, harvesting,
// the hooks of every unit). One simulated player per dimension, named "Deployer", acts for every
// deployer there: before an action it is moved to the deployer's hand and given the deployer's
// item, afterwards its inventory goes back to the deployer. Between actions it waits invisible,
// invulnerable and flying above the build limit, where it can't pick anything up (SPEC §8).

#include <map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/assembly/AssemblyInternal.h"
#include "mod/Log.h"

namespace lv::feature::assembly::fakeplayer {

namespace {
struct Entry {
    std::int64_t  id       = 0;
    std::uint64_t created  = 0;
};
std::map<int, Entry>& players() {
    static std::map<int, Entry> m;
    return m;
}

std::string nameFor(int dim) { return dim == 0 ? "Deployer" : "Deployer " + std::to_string(dim); }

void configure(SimulatedPlayer& p) {
    p.setPlayerGameType(GameType::Survival);
    p.setAbility(AbilitiesIndex::Invulnerable, true);
    p.setAbility(AbilitiesIndex::MayFly, true);
    p.setAbility(AbilitiesIndex::Flying, true);
    p.setInvisible(true);
    p.mCanPickupItems = false;
}
} // namespace

SimulatedPlayer* get(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto& e = players()[dim];
    if (e.id != 0) {
        auto* p = detail::playerById(e.id);
        if (p && p->isSimulatedPlayer() && static_cast<int>(p->getDimensionId()) == dim) {
            // a freshly created player needs a few ticks before it can act
            if (detail::currentTick() - e.created < 3) return nullptr;
            return static_cast<SimulatedPlayer*>(p);
        }
        if (p && p->isSimulatedPlayer()) static_cast<SimulatedPlayer*>(p)->simulateDisconnect();
        e = {};
    }
    auto* region = detail::regionOf(dim);
    if (!region) return nullptr;
    // spawn high above the spawn column: nothing to pick up there
    auto spawn = level->getSharedSpawnPos();
    Vec3 pos{static_cast<float>(spawn.x) + 0.5f, 400.f, static_cast<float>(spawn.z) + 0.5f};
    auto sp = SimulatedPlayer::create(nameFor(dim), pos, DimensionType(dim));
    if (!sp) {
        log().warn("assembly: could not create the deployer player of dimension {}", dim);
        return nullptr;
    }
    configure(*sp);
    e.id      = sp->getOrCreateUniqueID().rawID;
    e.created = detail::currentTick();
    log().info("assembly: deployer player created in dimension {}", dim);
    return nullptr; // usable from the next ticks on
}

bool isFake(Player const& player) {
    if (!player.isSimulatedPlayer()) return false;
    for (auto const& [dim, e] : players())
        if (e.id != 0 && e.id == const_cast<Player&>(player).getOrCreateUniqueID().rawID) return true;
    return false;
}

void forget() { players().clear(); }

void removeAll() {
    for (auto const& [dim, e] : players()) {
        if (e.id == 0) continue;
        if (auto* p = detail::playerById(e.id); p && p->isSimulatedPlayer()) static_cast<SimulatedPlayer*>(p)->simulateDisconnect();
    }
    players().clear();
}

} // namespace lv::feature::assembly::fakeplayer
