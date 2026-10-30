// Package entities (U46, SPEC §3.6.25): Create's PackageEntity. Thrown, put down and dropped
// packages are `levitate:package` entities (BP entities/package.json, one RP model per style chosen
// by the variant its spawn event sets), so vanilla clients see them too.
//
// - The package item (contents, address, order data) is kept in the world DB under
//   `lv:packages:entity:<unique id>`; entities loaded with their chunk find it again there.
// - Every package item entity that joins the world becomes a package entity on the next tick with
//   1.5 times its motion (Create's PackageItem.createEntity / PackageEntity.fromDroppedItem).
// - Right-click with an empty hand: the package goes into the hand (Create's interact). A hit, an
//   explosion, a projectile, fire running out its 5 health or water break it open: its contents
//   drop (spawn eggs spawn their mob) with the pop sound (Create's destroy). The void and /kill
//   remove it with its contents (Create's BYPASSES_INVULNERABILITY).
// - Create's centerPackage: on a belt, a depot / ejector, a chute (landed on it) or inside a funnel
//   the package slides to the block's input point and is inserted after its insertion delay.

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/entity/MobDieEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/Mob.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltApi.h"
#include "feature/common/Capabilities.h"
#include "feature/common/FunnelsExt.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::packages {

namespace items = common::items;
namespace ia    = common::interactions;

namespace {

constexpr char const* kKeyPrefix = "lv:packages:entity:";
/// Create's PackageEntity.insertionDelay (starts full, +1 per tick, -3 per centering tick).
constexpr int kInsertionDelay = 30;
/// A funnel hand-off (see tickEntity) that nobody took turns back into an entity after this long,
/// and the entity does not try the funnel again for a while.
constexpr int kHandoffTicks    = 20;
constexpr int kHandoffCooldown = 100;

constexpr char const* kFunnels[] = {"levitate:andesite_funnel", "levitate:brass_funnel"};
constexpr char const* kChutes[]  = {"levitate:chute", "levitate:smart_chute"};

struct Tracked {
    ItemStack box;
    int       dim             = 0;
    int       insertionDelay  = kInsertionDelay;
    int       handoffCooldown = 0;
    int       airTicks        = 0; ///< ticks since it left the ground (landing sound)
};

struct EntityState {
    std::unordered_map<std::int64_t, Tracked> live;
    /// item entities spawned with a package: converted on the next tick
    std::vector<std::int64_t> pending;
    /// item entities we handed to a funnel -> tick they were spawned
    std::unordered_map<std::int64_t, std::uint64_t> handoffs;
    /// entities whose death was handled (removed on the next tick)
    std::unordered_set<std::int64_t> dying;
    bool          spawningOwn = false; ///< our own item spawns are not converted
    std::uint64_t tick        = 0;
    std::mt19937  rng{std::random_device{}()};
};

EntityState& E() {
    static EntityState s;
    return s;
}

/// Our own item spawns (contents, funnel hand-offs) are not converted.
struct OwnSpawn {
    OwnSpawn() { E().spawningOwn = true; }
    ~OwnSpawn() { E().spawningOwn = false; }
    OwnSpawn(OwnSpawn const&)            = delete;
    OwnSpawn& operator=(OwnSpawn const&) = delete;
};

std::string keyOf(std::int64_t id) { return kKeyPrefix + std::to_string(id); }

std::int64_t uidOf(Actor& a) { return a.getOrCreateUniqueID().rawID; }

Actor* fetch(std::int64_t id) {
    auto level = ll::service::getLevel();
    if (!level || !id) return nullptr;
    ActorUniqueID uid;
    uid.rawID = id;
    return level->fetchEntity(uid, false);
}

bool isPackageEntity(Actor const& a) { return a.getTypeName() == kPackageEntityId; }

void store(std::int64_t id, Tracked t) {
    if (game::db::available()) game::db::save(keyOf(id), items::toJson(t.box));
    E().live[id] = std::move(t);
}

void forget(std::int64_t id) {
    E().live.erase(id);
    if (game::db::available()) game::db::erase(keyOf(id));
}

/// The tracked state of a package entity (adopting one loaded with its chunk); nullptr when the
/// entity carries no package (Create discards those).
Tracked* trackedOf(Actor& a) {
    auto id = uidOf(a);
    auto it = E().live.find(id);
    if (it != E().live.end()) return &it->second;
    if (!game::db::available()) return nullptr;
    auto j = game::db::load(keyOf(id));
    if (!j) return nullptr;
    auto box = items::fromJson(*j);
    if (!isPackage(box)) return nullptr;
    Tracked t;
    t.box = box;
    t.dim = int(a.getDimensionId());
    return &(E().live[id] = std::move(t));
}

cp::V3 feetOf(Actor const& a) {
    auto p = a.getFeetPos();
    return {p.x, p.y, p.z};
}

void iconParticle(int dim, cp::V3 at, ItemStack const& stack) {
    // Create's PackageDestroyPacket: item particles of the box (ParticleType::IconCrack)
    LevelEventPacket pkt;
    pkt.mEventId = 0x4000 | 14; // LevelEvent::ParticleLegacyEvent | IconCrack
    pkt.mPos     = Vec3{float(at.x), float(at.y), float(at.z)};
    pkt.mData    = (int(stack.getId()) << 16) | (stack.getAuxValue() & 0xffff);
    // one particle per event: a few around the box
    static constexpr float kOffsets[][3] = {
        {0,     0,     0    },
        {.2f,   .1f,   -.15f},
        {-.2f,  -.1f,  .15f },
        {.1f,   -.15f, .2f  }
    };
    BlockPos     block{int(std::floor(at.x)), int(std::floor(at.y)), int(std::floor(at.z))};
    DimensionType type(dim);
    for (auto const& o : kOffsets) {
        pkt.mPos = Vec3{float(at.x) + o[0], float(at.y) + o[1], float(at.z) + o[2]};
        pkt.sendTo(block, type);
    }
}

void removeLater(std::int64_t id) {
    E().dying.insert(id);
    defer([id] {
        E().dying.erase(id);
        if (auto* a = fetch(id)) a->remove();
    });
}

/// Create's PackageEntity.centerPackage: slows down, moves 20% towards `target` (here through its
/// motion, so clients see it slide) and counts the insertion delay down. True when it may insert.
bool centerPackage(Actor& a, Tracked& t, cp::V3 target) {
    auto   v   = a.getPosDelta();
    cp::V3 at  = feetOf(a);
    cp::V3 to  = (target - at) * 0.2;
    a.lerpMotion(Vec3{float(v.x * .75 + to.x), float(v.y * .75 * .25 + to.y), float(v.z * .75 + to.z)});
    t.insertionDelay = std::max(t.insertionDelay - 3, 0);
    return t.insertionDelay == 0;
}

bool isOneOf(std::string const& id, auto const& ids) {
    return std::any_of(std::begin(ids), std::end(ids), [&](char const* x) { return id == x; });
}

/// Inserts the package into what it stands on / sits in (Create's callers of centerPackage).
/// True when the entity is gone.
bool tryInsert(Actor& a, Tracked& t, BlockSource& region) {
    cp::V3   feet = feetOf(a);
    auto     size = packageStyleSize(packageStyle(items::idOf(t.box)));
    BlockPos at{int(std::floor(feet.x)), int(std::floor(feet.y)), int(std::floor(feet.z))};
    auto     idAtPos = [&](BlockPos const& p) { return game::blockId(region.getBlock(p)); };
    auto     finish  = [&](char const* what, BlockPos const& p) {
        log().info("packages: a package entity went into the {} at {} {} {}", what, p.x, p.y, p.z);
        forget(uidOf(a));
        a.remove();
        return true;
    };
    bool falling = a.getPosDelta().y <= 0;

    // FunnelBlock.entityInside: inside the funnel block; the funnels take item entities, so the
    // package is handed to them as one at the funnel's centre
    BlockPos mid{at.x, int(std::floor(feet.y + size.second / 32.0)), at.z};
    if (t.handoffCooldown == 0 && isOneOf(idAtPos(mid), kFunnels)) {
        cp::V3 c{mid.x + .5, mid.y + .5, mid.z + .5};
        if (!centerPackage(a, t, c - cp::V3{0, size.second / 32.0, 0})) return false;
        auto       level = ll::service::getLevel();
        ItemActor* item  = nullptr;
        {
            OwnSpawn own;
            item = level ? level->getSpawner().spawnItem(region, t.box, nullptr, Vec3{float(c.x), float(c.y), float(c.z)}, 0) : nullptr;
        }
        if (!item) return false;
        item->lerpMotion(Vec3{0, 0, 0});
        item->mPickupDelay = kHandoffTicks + 5; // only the funnel may take it
        E().handoffs[uidOf(*item)] = E().tick;
        return finish("funnel", mid);
    }
    // BeltBlock.entityInside: on a belt segment that carries objects, not rising, no tunnel on it
    if (idAtPos(at) == belts::kBeltId && falling && feet.y - at.y <= 13.0 / 16.0 + .3 && !common::beltlink::hasAbove(region, at)
        && common::items::slotCount(region, at, 1) > 0) {
        if (!centerPackage(a, t, {at.x + .5, at.y + 13.0 / 16.0, at.z + .5})) return false;
        if (!items::isEmpty(common::items::insert(region, at, 1, t.box, false))) return false;
        return finish("belt", at);
    }
    // SharedDepotBlockMethods.onLanded: depots and ejectors (DirectBeltInputBehaviour, from above)
    auto id = idAtPos(at);
    if ((id == belts::kDepotId || id == belts::kEjectorId) && a.isOnGround()) {
        if (!centerPackage(a, t, {at.x + .5, at.y + 13.0 / 16.0, at.z + .5})) return false;
        belts::TransportedItem item;
        item.stack        = t.box;
        item.insertedFrom = 1;
        if (!items::isEmpty(belts::insertDirect(game::globalPos(region, at), item, 0, false))) return false;
        return finish(id == belts::kDepotId ? "depot" : "ejector", at);
    }
    // AbstractChuteBlock.updateEntityAfterFallOn: landed on a chute that takes items from above
    BlockPos below{at.x, int(std::floor(feet.y + .5)) - 1, at.z};
    if (isOneOf(idAtPos(below), kChutes) && a.isOnGround()) {
        auto gp = game::globalPos(region, below);
        // the package travels down into the chute (insertDirect's side = direction of travel)
        if (!belts::canInsertDirect(gp, 0)) return false;
        if (!centerPackage(a, t, {below.x + .5, below.y + 1.0, below.z + .5})) return false;
        belts::TransportedItem item;
        item.stack = t.box;
        if (!items::isEmpty(belts::insertDirect(gp, item, 0, false))) return false;
        return finish("chute", below);
    }
    return false;
}

void tickEntity(std::int64_t id, Tracked& t) {
    auto* a = fetch(id);
    if (!a) {
        E().live.erase(id); // unloaded (the DB keeps the box) or removed by something else
        return;
    }
    if (!a->isAlive() || E().dying.contains(id)) return;
    t.insertionDelay = std::min(t.insertionDelay + 1, kInsertionDelay);
    if (t.handoffCooldown > 0) --t.handoffCooldown;
    // Create's onInsideBlock: water (or a waterlogged block) breaks it open
    if (a->isInWater()) {
        destroyPackageEntity(*a);
        return;
    }
    // Create's fall sounds (getFallSounds): the chiseled bookshelf's
    if (a->isOnGround()) {
        if (t.airTicks > 3) playSound(int(a->getDimensionId()), feetOf(*a), "fall.chiseled_bookshelf", .5f, 1.f);
        t.airTicks = 0;
    } else {
        ++t.airTicks;
    }
    tryInsert(*a, t, a->getDimensionBlockSource());
}

/// Package item entities become package entities (Create's PackageEntity.fromDroppedItem).
/// Item entities other units show things with (belt / depot displays, potato cannon shots: pickup
/// delay 32767, Java's mark of an item entity that is not a real drop; tagged ones) stay items.
bool isDisplayItem(Actor& a) {
    return static_cast<ItemActor&>(a).mPickupDelay >= 0x7fff || a.hasTags();
}

void convertItem(std::int64_t itemId, int cooldown) {
    auto* a = fetch(itemId);
    if (!a || !a->isAlive() || a->getEntityTypeId() != ActorType::ItemEntity || isDisplayItem(*a)) return;
    auto& item = static_cast<ItemActor&>(*a);
    if (!isPackage(item.item())) return;
    ItemStack box  = item.item();
    auto      v    = a->getPosDelta();
    cp::V3    at   = feetOf(*a);
    int       dim  = int(a->getDimensionId());
    int       left = items::countOf(box);
    for (int i = items::countOf(box); i > 0; --i) {
        auto id = spawnPackageEntity(dim, at, items::withCount(box, 1), cp::V3{v.x, v.y, v.z} * 1.5);
        if (!id) break; // could not spawn: the rest stays an item entity
        --left;
        if (auto it = E().live.find(id); it != E().live.end()) it->second.handoffCooldown = cooldown;
    }
    if (left <= 0) a->remove();
    else if (left != items::countOf(box)) item.item() = items::withCount(box, left);
}


void tickAll() {
    auto& s = E();
    ++s.tick;
    if (!game::db::available()) return;
    // dropped package items of the last tick
    auto pending = std::move(s.pending);
    s.pending.clear();
    for (auto id : pending) convertItem(id, 0);
    // funnel hand-offs nobody took
    for (auto it = s.handoffs.begin(); it != s.handoffs.end();) {
        if (s.tick - it->second < kHandoffTicks) {
            ++it;
            continue;
        }
        convertItem(it->first, kHandoffCooldown);
        it = s.handoffs.erase(it);
    }
    // package entities loaded with their chunks; package item entities loaded with theirs (a world
    // saved during a funnel hand-off)
    if (s.tick % 20 == 0) {
        if (auto level = ll::service::getLevel()) {
            std::vector<std::int64_t> loose;
            for (auto* a : level->getRuntimeActorList()) {
                if (a && a->getEntityTypeId() == ActorType::ItemEntity && a->isAlive() && !isDisplayItem(*a)
                    && isPackage(static_cast<ItemActor*>(a)->item()) && !s.handoffs.contains(uidOf(*a)))
                    loose.push_back(uidOf(*a));
                if (!a || !isPackageEntity(*a) || !a->isAlive()) continue;
                auto id = uidOf(*a);
                if (s.live.contains(id) || s.dying.contains(id)) continue;
                if (!trackedOf(*a)) {
                    log().warn("packages: a package entity without a package was removed");
                    a->remove();
                }
            }
            for (auto id : loose) convertItem(id, 0);
        }
    }
    std::vector<std::int64_t> ids;
    ids.reserve(s.live.size());
    for (auto const& [id, t] : s.live) ids.push_back(id);
    for (auto id : ids) {
        auto it = s.live.find(id);
        if (it != s.live.end()) tickEntity(id, it->second);
    }
}

void onDie(ll::event::MobDieEvent& ev) {
    auto& mob = ev.self();
    if (!isPackageEntity(mob)) return;
    auto id = uidOf(mob);
    if (E().dying.contains(id)) return;
    auto cause = ev.source().mCause;
    using C    = SharedTypes::Legacy::ActorDamageCause;
    if (cause == C::Void || cause == C::SelfDestruct || cause == C::Override) {
        // Create: BYPASSES_INVULNERABILITY damage removes it without dropping anything
        log().info("packages: a package entity was removed with its contents");
        forget(id);
        removeLater(id);
        return;
    }
    destroyPackageEntity(mob);
}

} // namespace

int packageStyle(std::string_view itemId) {
    auto const& ids = packageIds();
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == itemId) return int(i);
    return -1;
}

std::pair<int, int> packageStyleSize(int style) {
    // Create's PackageStyles: cardboard 12x12, 10x12, 10x8, 12x10; the rare ones 12x10
    static constexpr std::pair<int, int> kCardboard[] = {
        {12, 12},
        {10, 12},
        {10, 8 },
        {12, 10}
    };
    if (style >= 0 && style < 4) return kCardboard[style];
    return {12, 10};
}

std::int64_t spawnPackageEntity(int dim, cp::V3 at, ItemStack const& box, cp::V3 motion) {
    if (!isPackage(box)) return 0;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(dim);
    if (!level || !region) return 0;
    BlockPos bp{int(std::floor(at.x)), int(std::floor(at.y)), int(std::floor(at.z))};
    if (!region->hasChunksAt(bp, 0, false)) return 0;
    int style = std::max(packageStyle(items::idOf(box)), 0);
    // the spawn event adds the style's component group (variant = model, collision box = size)
    ActorDefinitionIdentifier ident("levitate", "package", "levitate:style_" + std::to_string(style));
    float yaw   = std::uniform_real_distribution<float>(0.f, 360.f)(E().rng) - 180.f; // Create: a random yaw
    auto  owner = level->getActorFactory().createSpawnedActor(ident, nullptr, Vec3{float(at.x), float(at.y), float(at.z)}, Vec2{0, yaw});
    if (!owner) return 0;
    auto* added = level->addEntity(*region, std::move(owner));
    if (!added) return 0;
    added->mBuiltInComponents->mActorRotationComponent->mRot = Vec2{0, yaw};
    if (added->hasCategory(ActorCategory::Mob)) static_cast<Mob*>(added)->setYBodyRotation(yaw);
    if (motion.length() > 1e-9) added->lerpMotion(Vec3{float(motion.x), float(motion.y), float(motion.z)});
    auto    id = uidOf(*added);
    Tracked t;
    t.box = items::withCount(box, 1);
    t.dim = dim;
    store(id, std::move(t));
    return id;
}

std::optional<ItemStack> packageEntityBox(std::int64_t uniqueId) {
    if (auto it = E().live.find(uniqueId); it != E().live.end()) return it->second.box;
    if (auto* a = fetch(uniqueId); a && isPackageEntity(*a))
        if (auto* t = trackedOf(*a)) return t->box;
    return std::nullopt;
}

bool packageEntityNear(int dim, cp::V3 point, double r, double h) {
    auto* region = regionOf(dim);
    if (!region) return false;
    AABB box{Vec3{float(point.x - r), float(point.y), float(point.z - r)}, Vec3{float(point.x + r), float(point.y + h), float(point.z + r)}};
    for (auto* a : region->getEntities(box, 0.f))
        if (a && isPackageEntity(*a) && a->isAlive()) return true;
    return false;
}

void destroyPackageEntity(Actor& entity) {
    auto id = uidOf(entity);
    if (E().dying.contains(id)) return;
    auto*  t   = trackedOf(entity);
    int    dim = int(entity.getDimensionId());
    cp::V3 at  = feetOf(entity);
    int    stacks = 0;
    if (t) {
        // Create's dropAllDeathLoot: spawn eggs spawn at the entity's block, the rest drops
        ItemStack box  = t->box;
        auto&     aabb = entity.getAABB();
        cp::V3    mid{(aabb.min.x + aabb.max.x) / 2.0, (aabb.min.y + aabb.max.y) / 2.0, (aabb.min.z + aabb.max.z) / 2.0};
        cp::V3    block{std::floor(at.x) + .5, std::floor(at.y), std::floor(at.z) + .5};
        for (auto s : contentsOf(box)) {
            if (items::isEmpty(s)) continue;
            if (spawnFromEgg(entity.getDimensionBlockSource(), s, nullptr, block)) s = items::withCount(s, items::countOf(s) - 1);
            if (items::isEmpty(s)) continue;
            ++stacks;
            dropItem(dim, at, s); // never a package (packages don't go into packages)
        }
        iconParticle(dim, mid, box);
    }
    playSound(dim, at, "levitate.packages.package_pop");
    log().info("packages: a package entity broke open ({} stacks)", stacks);
    forget(id);
    removeLater(id);
}

bool pickUpPackageEntity(Player& player, Actor& entity) {
    auto* t = trackedOf(entity);
    if (!t || !items::isEmpty(player.getSelectedItem())) return false;
    auto id = uidOf(entity);
    if (E().dying.contains(id)) return false;
    player.setSelectedItem(t->box);
    player.sendInventory(false);
    float pitch = .75f + std::uniform_real_distribution<float>(0.f, 1.f)(E().rng);
    playSound(int(entity.getDimensionId()), feetOf(entity), "random.pop", .2f, pitch);
    log().info("packages: {} picked up a package entity", player.getRealName());
    forget(id);
    removeLater(id);
    return true;
}

} // namespace lv::feature::packages

namespace {
namespace pk = lv::feature::packages;
} // namespace

LL_TYPE_INSTANCE_HOOK(
    PackagesItemSpawnHook,
    ll::memory::HookPriority::Normal,
    Spawner,
    &Spawner::$spawnItem,
    ItemActor*,
    BlockSource&     region,
    ItemStack const& inst,
    Actor*           spawner,
    Vec3 const&      pos,
    int              throwTime
) {
    auto* item = origin(region, inst, spawner, pos, throwTime);
    if (item && !pk::E().spawningOwn && pk::isPackage(inst)) {
        auto level = ll::service::getLevel();
        if (level && &item->getLevel() == level.as_ptr()) pk::E().pending.push_back(item->getOrCreateUniqueID().rawID);
    }
    return item;
}

namespace lv::feature::packages {

namespace {
std::vector<ll::event::ListenerPtr>& listeners() {
    static std::vector<ll::event::ListenerPtr> l;
    return l;
}
std::vector<ia::HandlerId>& entityHandlers() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}
} // namespace

void installPackageEntities() {
    auto& bus = ll::event::EventBus::getInstance();
    listeners().push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
        try {
            tickAll();
        } catch (std::exception const& e) {
            log().error("packages: package entity tick failed: {}", e.what());
        }
    }));
    listeners().push_back(bus.emplaceListener<ll::event::MobDieEvent>([](ll::event::MobDieEvent& ev) {
        try {
            onDie(ev);
        } catch (std::exception const& e) {
            log().error("packages: package entity death failed: {}", e.what());
        }
    }));
    // Create's PackageEntity.interact: an empty hand takes the package
    entityHandlers().push_back(ia::onInteract({.block = kPackageEntityId}, [](ia::EntityContext const& c) {
        if (!c.itemId.empty()) return ia::Result::Pass;
        return pickUpPackageEntity(c.player, c.entity) ? ia::Result::Consume : ia::Result::Pass;
    }));
    // Create's PackageEntity.hurt from a player: one hit breaks it open (not for players who may
    // not build: adventure mode)
    entityHandlers().push_back(ia::onAttack({.block = kPackageEntityId}, [](ia::EntityContext const& c) {
        if (c.player.getPlayerGameType() == GameType::Adventure) return ia::Result::Deny;
        destroyPackageEntity(c.entity);
        return ia::Result::Consume;
    }));
    PackagesItemSpawnHook::hook();
    static bool lifecycle = false;
    if (!std::exchange(lifecycle, true)) {
        game::db::onWorldClosed([] {
            auto& s = E();
            s.live.clear();
            s.pending.clear();
            s.handoffs.clear();
            s.dying.clear();
        });
    }
}

void uninstallPackageEntities() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& l : listeners()) bus.removeListener(l);
    listeners().clear();
    for (auto id : entityHandlers()) ia::remove(id);
    entityHandlers().clear();
    PackagesItemSpawnHook::unhook();
}

} // namespace lv::feature::packages
