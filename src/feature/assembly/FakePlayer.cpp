// The deployer's player (Create's DeployerFakePlayer).
//
// Bedrock has no fake players, but a simulated player is a real server player whose actions go
// through the same GameMode code as everyone's (item use, block placement, attacks, harvesting,
// the hooks of every unit). One simulated player per dimension, named "Deployer", acts for every
// deployer there: before an action it is moved to the deployer's hand and given the deployer's
// item, afterwards its inventory goes back to the deployer. Between actions it waits invisible,
// invulnerable and flying above the build limit, where it can't pick anything up (SPEC §8).
//
// Like Create's fake player it is no player for the others: it is taken out of every client's
// player list, command selectors (@a, @p, names) don't find it, it doesn't count for sleeping
// through the night, and what it kills drops no experience
// (DeployerFakePlayer.deployerKillsDoNotSpawnXP). Mobs it hits don't turn on it as configured
// by `ignoreDeployerAttacks` (DeployerFakePlayer.entitiesDontRetaliate).

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/entity/components_json_legacy/ExperienceRewardComponent.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/server/IPlayerSleepPercentageGetter.h"
#include "mc/server/ServerPlayerSleepManager.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/commands/CommandSelectorBase.h"
#include "mc/world/level/PlayerSleepStatus.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/monster/Skeleton.h"
#include "mc/world/actor/monster/Warden.h"
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

/// Takes the deployer players out of the player list of every client (or of `only`).
void hideFromPlayerList(Player const* only = nullptr) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerList);
    if (!packet) return;
    auto& pkt = static_cast<PlayerListPacket&>(*packet);
    for (auto const& [dim, e] : players()) {
        if (e.id == 0) continue;
        auto* p = detail::playerById(e.id);
        if (!p) continue;
        PlayerListPacketPayload::RemoveEntry entry;
        entry.mAction = PlayerListPacketType::Remove;
        entry.mUUID   = p->getUuid();
        pkt.mEntries->emplace_back(entry);
    }
    if (pkt.mEntries->empty()) return;
    if (only) pkt.sendTo(*only);
    else pkt.sendToClients();
}

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
            if (detail::currentTick() - e.created < 2) return nullptr;
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
    // the game announces the new player to everyone: take it out again once that went out
    detail::defer([] { detail::defer([] { hideFromPlayerList(); }); });
    log().info("assembly: deployer player created in dimension {}", dim);
    return nullptr; // usable from the next ticks on
}

bool isFake(Player const& player) {
    if (!player.isSimulatedPlayer()) return false;
    for (auto const& [dim, e] : players())
        if (e.id != 0 && e.id == const_cast<Player&>(player).getOrCreateUniqueID().rawID) return true;
    return false;
}

bool isFakeId(std::int64_t uniqueId) {
    if (uniqueId == 0 || uniqueId == -1) return false;
    for (auto const& [dim, e] : players())
        if (e.id == uniqueId) return true;
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

namespace {

// command selectors don't see the deployer players (Create's fake player is no server player)
LL_TYPE_INSTANCE_HOOK(
    DeployerSelectorHook,
    ll::memory::HookPriority::Normal,
    CommandSelectorBase,
    &CommandSelectorBase::filter,
    bool,
    ::CommandOrigin const& commandOrigin,
    ::Actor&               actor
) {
    if (actor.isSimulatedPlayer() && isFakeId(actor.getOrCreateUniqueID().rawID)) return false;
    return origin(commandOrigin, actor);
}

// sleeping through the night: the deployer players are not among the players who must sleep
LL_TYPE_INSTANCE_HOOK(
    DeployerSleepHook,
    ll::memory::HookPriority::Normal,
    ServerPlayerSleepManager,
    &ServerPlayerSleepManager::$updateSleepingPlayerList,
    void
) {
    origin();
    auto level = ll::service::getLevel();
    if (!level || std::none_of(players().begin(), players().end(), [](auto const& kv) { return kv.second.id != 0; })) return;
    int real = 0;
    level->forEachPlayer([&](Player& p) {
        if (!(p.isSimulatedPlayer() && isFakeId(p.getOrCreateUniqueID().rawID)) && !p.isSpectator()) ++real;
        return true;
    });
    float goal = (*mPlayerSleepPercentageGetter)->getPlayerSleepPercentageGoal();
    if (goal > 1.f) goal /= 100.f;
    auto& status   = *mPlayerSleepStatus;
    int   required = std::max(1, static_cast<int>(std::ceil(goal * static_cast<float>(real))));
    if (required >= status.mRequiredSleepingPlayerCount) return; // never makes sleeping harder
    status.mRequiredSleepingPlayerCount = required;
    if (status.mSleepingPlayerCount >= required && status.mSleepingPlayerCount > 0) mEnoughPlayersAreSleeping = true;
}

// DeployerFakePlayer.deployerKillsDoNotSpawnXP
LL_TYPE_INSTANCE_HOOK(
    DeployerExperienceHook,
    ll::memory::HookPriority::Normal,
    ExperienceRewardComponent,
    &ExperienceRewardComponent::getOnDeathExperience,
    int,
    ::Actor& owner
) {
    if (isFakeId(owner.mLastHurtByPlayerId->rawID)) return 0;
    return origin(owner);
}

// CKinetics.ignoreDeployerAttacks (DeployerFakePlayer.entitiesDontRetaliate): mobs of the
// configured set never take a deployer player as their target (creepers by default)
bool ignoresDeployer(Actor const& mob, ::Actor* target) {
    if (!target || !target->isSimulatedPlayer() || !isFakeId(target->getOrCreateUniqueID().rawID)) return false;
    auto const& setting = assembly::config().ignoreDeployerAttacks;
    return setting == "ALL" || (setting == "CREEPERS" && mob.getEntityTypeId() == ActorType::Creeper);
}

LL_TYPE_INSTANCE_HOOK(DeployerAggroHook, ll::memory::HookPriority::Normal, Mob, &Mob::$setTarget, void, ::Actor* target) {
    if (ignoresDeployer(*this, target)) return;
    origin(target);
}
// skeletons and wardens override setTarget
LL_TYPE_INSTANCE_HOOK(DeployerAggroSkeletonHook, ll::memory::HookPriority::Normal, Skeleton, &Skeleton::$setTarget, void, ::Actor* target) {
    if (ignoresDeployer(*this, target)) return;
    origin(target);
}
LL_TYPE_INSTANCE_HOOK(DeployerAggroWardenHook, ll::memory::HookPriority::Normal, Warden, &Warden::$setTarget, void, ::Actor* target) {
    if (ignoresDeployer(*this, target)) return;
    origin(target);
}

ll::event::ListenerPtr gJoin;

} // namespace

void install() {
    DeployerSelectorHook::hook();
    DeployerSleepHook::hook();
    DeployerExperienceHook::hook();
    DeployerAggroHook::hook();
    DeployerAggroSkeletonHook::hook();
    DeployerAggroWardenHook::hook();
    gJoin = ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerJoinEvent>([](ll::event::PlayerJoinEvent& ev) {
        // the joining player got the whole list: take the deployer players out of it
        auto id = ev.self().getOrCreateUniqueID().rawID;
        detail::defer([id] {
            if (auto* p = detail::playerById(id)) hideFromPlayerList(p);
        });
    });
}

void uninstall() {
    DeployerSelectorHook::unhook();
    DeployerSleepHook::unhook();
    DeployerExperienceHook::unhook();
    DeployerAggroHook::unhook();
    DeployerAggroSkeletonHook::unhook();
    DeployerAggroWardenHook::unhook();
    if (gJoin) ll::event::EventBus::getInstance().removeListener(gJoin);
    gJoin = nullptr;
}

} // namespace lv::feature::assembly::fakeplayer
