// Potato cannon (Create's PotatoCannonItem / PotatoProjectileEntity / the built-in projectile types).
//
// Each projectile is a `levitate:potato_projectile` entity (behaviour pack: no physics, no gravity,
// no collision, immune to damage; never saved with the world). Its variant (an init event) picks the
// ammo item's texture and Create's render mode in the resource pack (tumbling billboard, or along the
// motion with a spin); a stuck projectile (event `levitate:stuck`) is drawn as Create's StuckToEntity.
// The server simulates Create's flight (core/equipment: v = (v - 0.05·gravity) · drag), its block /
// entity / projectile hits, and moves the entity every tick (a normal move: clients interpolate).

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/FallingBlockActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SuspiciousStewItem.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/ShapeType.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"
#include "mc/util/VariantParameterList.h"

#include "core/equipment/Gear.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/DeathMessages.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;

namespace {

constexpr double kHalfWidth = 0.125; ///< Create: the projectile is 0.25 x 0.25
constexpr double kHeight    = 0.25;
constexpr double kHitMargin = 0.3;   ///< targets' boxes grow by this much (Java's projectile pick margin)
constexpr int    kMaxAge    = 400;   ///< a projectile flying longer than 20 s pops

struct Projectile {
    int                   dim = 0;
    ce::Vec3f             pos, vel;
    ce::PotatoType const* type = nullptr;
    ItemStack             ammo;
    std::int64_t          owner    = 0;
    std::int64_t          entity   = 0; ///< the projectile entity
    int                   age      = 0;
    double                recovery = 0; ///< Create's recoveryChance (0 for the extra shots of a split)
    std::int64_t          stuck    = 0; ///< Create's stuckEntity
    ce::Vec3f             stuckOffset;
    double                stuckFall = 0;
    std::set<std::int64_t> passed; ///< entities it flew through (a powered wither)
    bool                  dead = false;
};
std::vector<Projectile>& list() {
    static std::vector<Projectile> v;
    return v;
}
std::map<std::int64_t, std::uint64_t>& cannonCooldowns() {
    static std::map<std::int64_t, std::uint64_t> m;
    return m;
}

Actor* actorById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level || !uid) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->fetchEntity(id, false);
}

Vec3 mcVec(ce::Vec3f const& v) { return Vec3{float(v.x), float(v.y), float(v.z)}; }

void addEffect(Actor& a, unsigned id, int ticks, int amplifier) {
    EffectDuration d;
    d.mValue = ticks;
    MobEffectInstance inst(id, d);
    inst.mAmplifier = amplifier;
    a.addEffect(inst);
}

/// One item particle of the stack (Create's ItemParticleOption; Bedrock's legacy icon crack).
void itemParticle(int dim, double x, double y, double z, ItemStack const& stack) {
    LevelEventPacket pkt;
    pkt.mEventId = 0x4000 | 14; // LevelEvent::ParticleLegacyEvent | ParticleType::IconCrack
    pkt.mPos     = Vec3{float(x), float(y), float(z)};
    pkt.mData    = (int(stack.getId()) << 16) | (stack.getAuxValue() & 0xffff);
    pkt.sendTo(BlockPos{int(std::floor(x)), int(std::floor(y)), int(std::floor(z))}, DimensionType(dim));
}

/// Create's PotatoProjectileEntity#playHitSound (AllSoundEvents.POTATO_HIT: an item frame breaking at
/// 0.75 / 0.75 and weeping vines breaking at 0.75 / 1.25).
void hitSound(int dim, double x, double y, double z) {
    sound(dim, x, y, z, "levitate.potato_cannon.hit", 1.f, 1.f);
    sound(dim, x, y, z, "levitate.potato_cannon.hit_vines", 1.f, 1.f);
}

/// Create's pop: 7 item particles at the hit and the hit sound at the projectile.
void pop(Projectile const& p, double x, double y, double z) {
    for (int i = 0; i < 7; ++i) itemParticle(p.dim, x, y, z, p.ammo);
    hitSound(p.dim, p.pos.x, p.pos.y, p.pos.z);
}

/// Create's recoverItem / spawnAtLocation(dropStack): the recovery roll, else the type's drop.
void recoverOrDrop(Projectile const& p, double x, double y, double z) {
    if (random01() <= p.recovery && p.recovery > 0) dropAt(p.dim, x, y, z, items::withCount(p.ammo, 1));
    else if (!p.type->dropItem.empty())
        if (auto drop = items::make(p.type->dropItem, 1)) dropAt(p.dim, x, y, z, *drop);
}

/// Ray / box slab test: the segment a -> b hits the box (min, max) at parameter t in [0, 1].
std::optional<double> segmentBox(ce::Vec3f a, ce::Vec3f b, ce::Vec3f mn, ce::Vec3f mx) {
    double t0 = 0, t1 = 1;
    double av[3] = {a.x, a.y, a.z}, bv[3] = {b.x, b.y, b.z}, lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
    for (int i = 0; i < 3; ++i) {
        double d = bv[i] - av[i];
        if (std::fabs(d) < 1e-9) {
            if (av[i] < lo[i] || av[i] > hi[i]) return std::nullopt;
            continue;
        }
        double u = (lo[i] - av[i]) / d, v = (hi[i] - av[i]) / d;
        if (u > v) std::swap(u, v);
        t0 = std::max(t0, u);
        t1 = std::min(t1, v);
        if (t0 > t1) return std::nullopt;
    }
    return t0;
}

/// Sends the entity's place and rotation (Create's render modes read the rotation for TowardMotion).
void moveEntity(Projectile const& p) {
    auto* e = actorById(p.entity);
    if (!e) return;
    Vec2 rot{0, 0};
    if (p.stuck) rot = Vec2{0, float(ce::potatoYaw(p.stuckOffset))};
    else if (p.type->render == ce::PotatoRender::TowardMotion) rot = Vec2{float(ce::potatoPitch(p.vel)), float(ce::potatoYaw(p.vel))};
    e->moveTo(mcVec(p.pos), rot);
}

void removeEntity(Projectile& p) {
    if (auto* e = actorById(p.entity)) e->remove();
    p.entity = 0;
}

Projectile* projectileByEntity(std::int64_t uid) {
    for (auto& p : list())
        if (p.entity == uid && !p.dead) return &p;
    return nullptr;
}

/// Create's ChorusTeleport (diameter 20): up to 16 tries; true when the target moved.
bool chorusTeleport(Projectile const& p, Actor& target) {
    if (!target.hasCategory(ActorCategory::Mob)) return false;
    auto pos = target.getPosition();
    for (int i = 0; i < 16; ++i) {
        double nx = pos.x + (random01() - .5) * 20, nz = pos.z + (random01() - .5) * 20;
        double ny = std::clamp(pos.y + (std::floor(random01() * 20) - 10), -63.0, 318.0);
        GlobalPos at{p.dim, int(std::floor(nx)), int(std::floor(ny)), int(std::floor(nz))};
        GlobalPos below{at.dim, at.x, at.y - 1, at.z};
        if (!replaceableAt(at) || replaceableAt(below)) continue;
        sound(p.dim, pos.x, pos.y, pos.z, "mob.endermen.portal", 1.f, 1.f);
        if (target.getVehicle()) target.stopRiding(true, false, false, false);
        target.teleportTo(Vec3{float(nx), float(at.y), float(nz)}, true, 0, 0, false);
        setMotion(target, 0, 0, 0);
        sound(p.dim, nx, at.y, nz, "mob.endermen.portal", 1.f, 1.f);
        return true;
    }
    return false;
}

void goldenAppleEffects(Actor& target) {
    addEffect(target, 10, 100, 1);  // regeneration II 5 s
    addEffect(target, 22, 2400, 0); // absorption 2 min
}

/// Create's onEntityHit actions; true when the item is used up (no recovery, no drop).
bool entityHitAction(Projectile& p, Actor& target, double x, double y, double z) {
    using E       = ce::PotatoEffect;
    auto const& t = *p.type;
    bool living   = target.hasCategory(ActorCategory::Mob);
    switch (t.effect) {
    case E::Glowing: return t.consumed; // Bedrock has no glowing effect (SPEC §8)
    case E::Poison:
    case E::Slowness:
        if (living) addEffect(target, static_cast<unsigned>(t.effectId), t.effectTicks, t.effectAmplifier);
        return t.consumed;
    case E::ChorusTeleport: return chorusTeleport(p, target);
    case E::GoldenApple: {
        // Create's CureZombieVillager: a weakened zombie villager is cured as if fed a golden apple
        std::string id = target.getTypeName();
        if ((id == "minecraft:zombie_villager_v2" || id == "minecraft:zombie_villager") && target.getEffect(18u) != nullptr) {
            VariantParameterList params{};
            params.mSelf = &target;
            target.executeEvent("villager_converted", params);
            log().info("equipment: a potato cannon cured the zombie villager at {} {} {}", int(std::floor(x)), int(std::floor(y)), int(std::floor(z)));
            return true;
        }
        if (living) goldenAppleEffects(target);
        return true;
    }
    case E::EnchantedGoldenApple:
        if (living) {
            addEffect(target, 10, 400, 1);  // regeneration II 20 s
            addEffect(target, 11, 6000, 0); // resistance 5 min
            addEffect(target, 12, 6000, 0); // fire resistance 5 min
            addEffect(target, 22, 2400, 3); // absorption IV 2 min
        }
        return true;
    case E::Pufferfish:
        if (living) {
            addEffect(target, 19, 1200, 1); // poison II 60 s
            addEffect(target, 17, 300, 2);  // hunger III 15 s
            addEffect(target, 9, 300, 0);   // nausea 15 s
        }
        return true;
    case E::SuspiciousStew:
        // Create's SuspiciousStew: the stew's own effects (Bedrock keeps them per aux value)
        if (living) {
            auto const& effects = SuspiciousStewItem::_getStewEffects();
            auto        aux     = static_cast<std::size_t>(p.ammo.getAuxValue());
            if (aux < effects.size()) target.addEffect(effects[aux]);
        }
        return false;
    default: return false;
    }
}

/// Create's knockback: LivingEntity#knockback(knockback · 0.6) along the horizontal motion.
void knockback(Actor& target, ce::Vec3f const& vel, double strength) {
    double h = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    if (h < 1e-6 || strength <= 0) return;
    auto const& v  = target.getPosDelta();
    double      dx = vel.x / h * strength, dz = vel.z / h * strength;
    double      vy = target.isOnGround() ? std::min(0.4, v.y / 2 + strength) : v.y;
    setMotion(target, v.x / 2 + dx, vy, v.z / 2 + dz);
}

void stick(Projectile& p, Actor& target) {
    auto tp       = target.getPosition();
    p.stuck       = uidOf(target);
    p.stuckOffset = {p.pos.x - tp.x, p.pos.y - tp.y, p.pos.z - tp.z};
    p.stuckFall   = 0;
    p.vel         = {};
    if (auto* e = actorById(p.entity)) {
        VariantParameterList params{};
        params.mSelf = e;
        e->executeEvent("levitate:stuck", params);
    }
}

/// Create's PotatoProjectileEntity#onHitEntity on another projectile entity: true when it hit.
bool hitProjectile(Projectile& p, Projectile& q, double x, double y, double z) {
    if (p.age < 10 && q.age < 10) return false; // Create: both just launched
    if (p.type != q.type) {
        for (auto uid : {p.owner, q.owner})
            if (auto* pl = playerById(uid)) advancements::award(*pl, "potato_cannon_collide");
        log().info("equipment: potato projectiles collided ({} and {})", p.type->id, q.type->id);
    }
    pop(p, x, y, z);
    // the other projectile is hurt: it pops (Create's PotatoProjectileEntity#hurt)
    pop(q, q.pos.x, q.pos.y, q.pos.z);
    q.dead = true;
    // the hit action on a non-living target (potions / food effects: only whether it is used up; a
    // chorus fruit teleports nothing), then the recovery roll
    if (!p.type->consumed) recoverOrDrop(p, x, y, z);
    hitSound(p.dim, p.pos.x, p.pos.y, p.pos.z);
    p.dead = true;
    return true;
}

/// Create's PotatoProjectileEntity#onHitEntity.
void hitEntity(Projectile& p, Actor& target, double x, double y, double z) {
    auto const& t = *p.type;
    if (!target.isAlive()) return;
    Actor*      owner = actorById(p.owner);
    pop(p, x, y, z);
    std::string ttype = target.getTypeName();
    if (ttype == "minecraft:wither" && target.getHealth() <= target.getMaxHealth() / 2) { // powered: immune, flies on
        p.passed.insert(uidOf(target));
        return;
    }
    if (t.effect == ce::PotatoEffect::SetOnFire) target.setOnFire(t.effectSeconds, true);
    bool enderman = ttype == "minecraft:enderman";
    // Create's AllDamageTypes.POTATO_CANNON: "%1$s was shot by %2$s's Potato Cannon", or ".item"
    // naming the custom-named item the shooter holds
    if (owner) {
        std::string ownerName = owner->isPlayer() ? static_cast<Player*>(owner)->getRealName() : owner->getNameTag();
        if (ownerName.empty()) {
            // a vanilla mob's name key has no namespace (entity.zombie.name)
            std::string type = owner->getTypeName();
            if (type.starts_with("minecraft:")) type = type.substr(10);
            ownerName = "%entity." + type + ".name";
        }
        std::string custom = owner->isPlayer() ? static_cast<Player*>(owner)->getSelectedItem().getCustomName() : std::string();
        if (!custom.empty()) common::deathmsg::mark(target, "death.attack.create.potato_cannon.item", {ownerName, custom});
        else common::deathmsg::mark(target, "death.attack.create.potato_cannon", {ownerName});
    } else {
        common::deathmsg::mark(target, "death.attack.create.potato_cannon", {"%entity.levitate:potato_projectile.name"});
    }
    bool hurt     = owner ? target.hurtByCause(t.damage, SharedTypes::Legacy::ActorDamageCause::Projectile, *owner)
                          : target.hurtByCause(t.damage, SharedTypes::Legacy::ActorDamageCause::Projectile);
    if (!hurt || enderman) {
        p.dead = true;
        return;
    }
    if (!entityHitAction(p, target, x, y, z)) recoverOrDrop(p, x, y, z);
    bool living = target.hasCategory(ActorCategory::Mob) || target.isPlayer();
    if (!living) {
        hitSound(p.dim, p.pos.x, p.pos.y, p.pos.z);
        p.dead = true;
        return;
    }
    knockback(target, p.vel, t.knockback * 0.6);
    // Create: the owner gets "potato_cannon" for a monster it killed or another player it hit
    if (auto* pl = playerById(p.owner)) {
        bool killed = !target.isAlive() || target.getHealth() <= 0;
        if ((killed && target.hasCategory(ActorCategory::Monster)) || (target.isPlayer() && uidOf(target) != p.owner))
            advancements::award(*pl, "potato_cannon");
    }
    bool alive = target.isAlive() && target.getHealth() > 0;
    if (t.sticky && alive) {
        stick(p, target);
        log().info("equipment: a {} projectile stuck to {}", t.id, ttype);
    } else p.dead = true;
}

/// Create's PotatoProjectileEntity#onHitBlock with the type's block hit action.
void hitBlock(Projectile& p, BlockPos const& bp, int face, double x, double y, double z) {
    using E       = ce::PotatoEffect;
    auto const& t = *p.type;
    pop(p, x, y, z);
    GlobalPos at{p.dim, bp.x, bp.y, bp.z};
    static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    GlobalPos            next = face >= 0 && face < 6 ? GlobalPos{p.dim, bp.x + dx[face], bp.y + dy[face], bp.z + dz[face]} : at;
    bool                 consumed = false;
    // Projectile#onHitBlock: the block's own projectile hit first (BlockState#onProjectileHit: bells
    // ring, targets fire, ...); Create's bells are AbstractBellBlocks (BellBlock#onProjectileHit)
    if (isBell(idAt(at))) {
        bellHit(at, face, y - bp.y, playerById(p.owner));
    } else if (auto const* b = blockAt(at); b && std::string_view(b->getTypeName()).starts_with("minecraft:")) {
        if (auto* region = regionOf(p.dim); region && actorById(p.entity))
            b->getBlockType().onProjectileHit(*region, bp, *actorById(p.entity));
    }
    if (t.effect == E::PlantPotato || t.effect == E::PlantCarrot) {
        // Create's PlantCrop: only the top face with room above; the crop grows on farmland
        if (face == 1 && replaceableAt(next)) {
            consumed = true;
            if (idAt(at) == "minecraft:farmland") {
                place(next, t.effect == E::PlantPotato ? "minecraft:potatoes" : "minecraft:carrots");
                sound(p.dim, x, y, z, "dig.grass", 1.f, 1.f);
                log().info("equipment: a potato cannon planted {} at {} {} {}", t.id, next.x, next.y, next.z);
            }
        }
    } else if (t.effect == E::PlaceMelon || t.effect == E::PlacePumpkin) {
        // Create's PlaceBlockOnGround: placed on a top face, else dropped as a falling block
        std::string block = t.effect == E::PlaceMelon ? "minecraft:melon_block" : "minecraft:pumpkin";
        if (replaceableAt(next)) {
            consumed = true;
            if (face == 1) place(next, block);
            else if (auto const* b = game::blockWith(block); b && regionOf(p.dim)) {
                double fy = y - 0.5;
                if (!replaceableAt({p.dim, next.x, next.y + 1, next.z})) fy = std::min(fy, double(next.y));
                if (!replaceableAt({p.dim, next.x, next.y - 1, next.z})) fy = std::max(fy, double(next.y));
                auto level = ll::service::getLevel();
                auto owner = level->getActorFactory().createSpawnedActor(
                    ActorDefinitionIdentifier("minecraft:falling_block"), nullptr, Vec3{next.x + .5f, float(fy), next.z + .5f}, Vec2{0, 0}
                );
                if (auto* a = owner ? Actor::tryGetFromEntity(*owner, false) : nullptr) {
                    static_cast<FallingBlockActor*>(a)->setFallingBlock(*b, false);
                    level->addEntity(*regionOf(p.dim), std::move(owner));
                }
            }
        }
    }
    if (!consumed) recoverOrDrop(p, x, y, z);
    p.dead = true;
}

/// Create's barrel: ShootableGadgetItemMethods#getGunBarrelVec(player, mainHand, (0.75, -0.15, 1.5)).
ce::Vec3f barrel(Player& player, bool mainHand, double lx, double ly, double lz) {
    auto   eye   = player.getEyePos();
    auto   rot   = player.getRotation();
    double pitch = -rot.x * std::numbers::pi / 180.0, yaw = -rot.y * std::numbers::pi / 180.0;
    double x = (mainHand ? -1 : 1) * lx, y = ly, z = lz; // right-handed: the main hand is on the right
    // Vec3#xRot(pitch), then #yRot(yaw)
    double y1 = y * std::cos(pitch) + z * std::sin(pitch), z1 = z * std::cos(pitch) - y * std::sin(pitch);
    double x2 = x * std::cos(yaw) + z1 * std::sin(yaw), z2 = z1 * std::cos(yaw) - x * std::sin(yaw);
    return {eye.x + x2, eye.y + y1, eye.z + z2};
}

std::int64_t spawnProjectile(Projectile& p) {
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(p.dim);
    if (!level || !region) return 0;
    int  variant = std::max(0, ce::potatoAmmoIndex(items::idOf(p.ammo)));
    auto owner   = level->getActorFactory().createSpawnedActor(
        ActorDefinitionIdentifier("levitate", "potato_projectile", "levitate:ammo_" + std::to_string(variant)),
        nullptr,
        mcVec(p.pos),
        Vec2{0, 0}
    );
    if (!owner) return 0;
    auto* added = level->addEntity(*region, std::move(owner));
    if (!added) return 0;
    added->addTag(kPotatoProjectileTag);
    p.entity = uidOf(*added);
    moveEntity(p);
    return p.entity;
}

void tickOne(Projectile& p) {
    auto* region = regionOf(p.dim);
    ++p.age;
    if (!region || p.age > kMaxAge || !actorById(p.entity)) {
        p.dead = true;
        return;
    }
    // stuck to an entity (Create: falls off slowly, pops below the entity's feet)
    if (p.stuck) {
        auto* e = actorById(p.stuck);
        if (e && e->isAlive()) {
            auto ep = e->getPosition();
            if (ce::potatoStuckPops(p.pos.y, ep.y)) {
                pop(p, p.pos.x, p.pos.y, p.pos.z);
                p.dead = true;
                return;
            }
            p.stuckOffset.y = ce::potatoStuckStep(p.stuckOffset.y, p.stuckFall, *p.type);
            p.pos           = {ep.x + p.stuckOffset.x, ep.y + p.stuckOffset.y, ep.z + p.stuckOffset.z};
            moveEntity(p);
            return;
        }
        // Create: the stuck entity is gone, the projectile flies on from rest
        p.stuck = 0;
        p.vel   = {};
        if (auto* a = actorById(p.entity)) {
            VariantParameterList params{};
            params.mSelf = a;
            a->executeEvent("levitate:unstuck", params);
        }
    }
    ce::Vec3f a = p.pos, b{p.pos.x + p.vel.x, p.pos.y + p.vel.y, p.pos.z + p.vel.z};
    // blocks
    HitResult hr = region->clip(
        mcVec(a), mcVec(b), false, ShapeType::Collision, 64, false, false, nullptr,
        [](BlockSource const&, Block const&, bool) { return true; }, false
    );
    double len    = std::sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y + p.vel.z * p.vel.z);
    double tBlock = 2;
    if (hr.mType == HitResultType::Tile) {
        double dx = hr.mPos.x - a.x, dy = hr.mPos.y - a.y, dz = hr.mPos.z - a.z;
        tBlock    = len > 1e-9 ? std::sqrt(dx * dx + dy * dy + dz * dz) / len : 0;
    }
    // entities (living ones and players; other potato projectiles below)
    Actor* best  = nullptr;
    double tBest = 2;
    AABB   box{
        Vec3{float(std::min(a.x, b.x) - 1), float(std::min(a.y, b.y) - 1), float(std::min(a.z, b.z) - 1)},
        Vec3{float(std::max(a.x, b.x) + 1), float(std::max(a.y, b.y) + 1), float(std::max(a.z, b.z) + 1)}
    };
    for (auto* e : region->getEntities(box, 0.0f)) {
        if (!e || !e->isAlive() || e->isType(ActorType::ItemEntity)) continue;
        auto uid = uidOf(*e);
        if (uid == p.entity || p.passed.contains(uid)) continue;
        if (uid == p.owner && p.age < 5) continue; // Create: not before it left its owner
        if (!e->hasCategory(ActorCategory::Mob) && !e->isPlayer()) continue;
        auto const& bb = e->getAABB();
        auto        t  = segmentBox(a, b, {bb.min.x - kHitMargin, bb.min.y - kHitMargin, bb.min.z - kHitMargin},
                                    {bb.max.x + kHitMargin, bb.max.y + kHitMargin, bb.max.z + kHitMargin});
        if (t && *t < tBest) tBest = *t, best = e;
    }
    Projectile* other  = nullptr;
    double      tOther = 2;
    for (auto& q : list()) {
        if (&q == &p || q.dead || q.dim != p.dim) continue;
        ce::Vec3f lo{q.pos.x - kHalfWidth - kHitMargin, q.pos.y - kHitMargin, q.pos.z - kHalfWidth - kHitMargin};
        ce::Vec3f hi{q.pos.x + kHalfWidth + kHitMargin, q.pos.y + kHeight + kHitMargin, q.pos.z + kHalfWidth + kHitMargin};
        auto      t = segmentBox(a, b, lo, hi);
        if (t && *t < tOther && (p.age >= 10 || q.age >= 10)) tOther = *t, other = &q;
    }
    auto at = [&](double t) { return ce::Vec3f{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
    if (other && tOther <= tBest && tOther <= tBlock) {
        auto h = at(tOther);
        p.pos  = h;
        hitProjectile(p, *other, h.x, h.y, h.z);
        return;
    }
    if (best && tBest <= tBlock) {
        auto h = at(tBest);
        p.pos  = h;
        hitEntity(p, *best, h.x, h.y, h.z);
        if (!p.dead) moveEntity(p);
        return;
    }
    if (tBlock <= 1) {
        p.pos = {hr.mPos.x, hr.mPos.y, hr.mPos.z};
        hitBlock(p, hr.mBlock, hr.mFacing, hr.mPos.x, hr.mPos.y, hr.mPos.z);
        return;
    }
    p.pos = b;
    p.vel = ce::potatoStep(p.vel, *p.type);
    // Create's trail (AbstractHurtingProjectile#getTrailParticle: an air particle half a block up)
    particle(p.dim, p.pos.x, p.pos.y + 0.5, p.pos.z, "levitate:potato_trail");
    moveEntity(p);
}

/// Projectile entities nobody simulates (a reload, a crash before the save hook could run).
void sweepOrphans() {
    auto level = ll::service::getLevel();
    if (!level) return;
    std::vector<Actor*> orphans;
    for (auto* a : level->getRuntimeActorList()) {
        if (!a || a->getTypeName() != kPotatoProjectile) continue;
        if (!projectileByEntity(uidOf(*a))) orphans.push_back(a);
    }
    for (auto* a : orphans) a->remove();
}

} // namespace

Vec3d gunBarrel(Player& player, bool mainHand, double lx, double ly, double lz) {
    auto v = barrel(player, mainHand, lx, ly, lz);
    return {v.x, v.y, v.z};
}

// ---- public -----------------------------------------------------------------------------------------------

std::int64_t launchProjectile(int dim, double x, double y, double z, double vx, double vy, double vz, std::string const& ammoId, std::int64_t owner,
                              double recoveryChance, int aux) {
    auto const* type = ce::potatoTypeFor(ammoId);
    auto        ammo = items::make(ammoId, 1, aux);
    if (!type || !ammo) return 0;
    Projectile p;
    p.dim      = dim;
    p.pos      = {x, y, z};
    p.vel      = {vx, vy, vz};
    p.type     = type;
    p.ammo     = *ammo;
    p.owner    = owner;
    p.recovery = recoveryChance;
    if (!spawnProjectile(p)) return 0;
    list().push_back(std::move(p));
    return list().back().entity;
}

bool firePotatoCannon(Player& player) {
    auto  uid = uidOf(player);
    auto  now = currentTick();
    auto& cd  = cannonCooldowns()[uid];
    if (now < cd) return true;
    ItemStack ammo;
    int       slot = -1;
    {
        // the ammo (vanilla getProjectile order: off hand, main hand, inventory)
        auto check = [&](ItemStack const& s) { return !items::isEmpty(s) && ce::potatoTypeFor(items::idOf(s)); };
        if (check(player.getOffhandSlot())) ammo = player.getOffhandSlot(), slot = -2;
        else if (check(player.getSelectedItem())) ammo = player.getSelectedItem(), slot = player.getSelectedItemSlot();
        else {
            auto& inv = player.getInventory();
            for (int i = 0; i < inv.getContainerSize(); ++i)
                if (check(inv.getItem(i))) {
                    ammo = inv.getItem(i), slot = i;
                    break;
                }
        }
    }
    if (slot == -1) return false;
    auto const* type = ce::potatoTypeFor(items::idOf(ammo));
    if (!type) return false;
    bool mainHand = items::idOf(player.getSelectedItem()) == kPotatoCannon;
    bool dual     = items::idOf(player.getOffhandSlot()) == kPotatoCannon && mainHand;
    ItemStack cannon = mainHand ? player.getSelectedItem() : player.getOffhandSlot();
    int  dim  = static_cast<int>(player.getDimensionId());
    if (!regionOf(dim)) return false;
    cd = now + static_cast<std::uint64_t>(ce::potatoCooldown(*type, dual));
    // Create: from the barrel, along the look corrected towards the barrel, at 2 · velocity
    auto      look  = player.getViewVector(1.0f);
    auto      eye   = player.getEyePos();
    ce::Vec3f start = barrel(player, mainHand, 0.75, -0.15, 1.5);
    ce::Vec3f corr  = barrel(player, mainHand, -0.05, 0, 0);
    ce::Vec3f dir{look.x + (corr.x - eye.x), look.y + (corr.y - eye.y), look.z + (corr.z - eye.z)};
    double    dl    = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    double    speed = ce::potatoSpeed(*type);
    if (dl < 1e-9) dir = {look.x, look.y, look.z}, dl = 1;
    double recovery = ce::potatoRecoveryChance(enchantLevel(cannon, kPotatoRecovery));
    // Create: split shots add a fixed 0.1 offset perpendicular to the motion, spread evenly
    // (i * 360 / split, +-20 degrees) around one random base angle (VecHelper.lookAt frame)
    double fx = dir.x / dl, fy = dir.y / dl, fz = dir.z / dl;
    double upx = 0, upy = 1, upz = 0;
    if (std::abs(fy) > 1 - 1e-3) upy = 0, upz = fy > 0 ? 1 : -1;
    double rx = fy * upz - fz * upy, ry = fz * upx - fx * upz, rz = fx * upy - fy * upx; // fwd x up
    double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rl, ry /= rl, rz /= rl;
    upx = ry * fz - rz * fy, upy = rz * fx - rx * fz, upz = rx * fy - ry * fx; // right x fwd
    double baseDeg = 360.0 * random01();
    for (int i = 0; i < type->split; ++i) {
        ce::Vec3f v{fx * speed, fy * speed, fz * speed};
        if (type->split > 1) {
            double ang = (baseDeg + i * 360.0 / type->split + (random01() - .5) * 40.0) * std::numbers::pi / 180.0;
            double ox = -0.1 * std::sin(ang), oy = 0.1 * std::cos(ang); // (0, 0.1, 0) turned about Z
            v.x += ox * rx + oy * upx;
            v.y += ox * ry + oy * upy;
            v.z += ox * rz + oy * upz;
        }
        launchProjectile(dim, start.x, start.y, start.z, v.x, v.y, v.z, items::idOf(ammo), uid, i == 0 ? recovery : 0.0,
                         ammo.getAuxValue());
    }
    // Create's PotatoCannonPacket: the fwoomp at the barrel and two item particles
    float pitch = type->pitch + float(random01() - .5) / 4.f;
    sound(dim, start.x, start.y, start.z, "levitate.potato_cannon.fwoomp", 1.f, pitch);
    for (int i = 0; i < 2; ++i) itemParticle(dim, start.x, start.y, start.z, ammo);
    if (!player.isCreative()) {
        if (slot == -2) player.setOffhandSlot(items::withCount(ammo, items::countOf(ammo) - 1));
        else player.getInventory().setItem(slot, items::withCount(ammo, items::countOf(ammo) - 1));
        player.sendInventory(false);
    }
    useAirOrDurability(player, mainHand, config().maxPotatoCannonShots);
    log().info("equipment: {} fired a potato cannon ({})", player.getRealName(), type->id);
    return true;
}

void tickProjectiles() {
    auto& l = list();
    for (std::size_t i = 0; i < l.size(); ++i)
        if (!l[i].dead) tickOne(l[i]); // (a hit may mark a later projectile dead)
    for (auto& p : l)
        if (p.dead) removeEntity(p);
    std::erase_if(l, [](Projectile const& p) { return p.dead; });
    if (currentTick() % 40 == 0) sweepOrphans();
}

void resetProjectiles() {
    list().clear();
    cannonCooldowns().clear();
}

int projectileCount() { return static_cast<int>(list().size()); }

std::vector<ProjectileInfo> projectiles() {
    std::vector<ProjectileInfo> out;
    for (auto const& p : list())
        out.push_back({p.entity, p.type->id, p.pos.x, p.pos.y, p.pos.z, p.stuck, p.recovery});
    return out;
}

} // namespace lv::feature::equipment

// The projectile entities are never written to the world (Create's are, but ours are simulated by
// the server only while it runs: a saved one would hang in the air without its simulation).
LL_TYPE_INSTANCE_HOOK(EquipmentProjectileSaveHook, ll::memory::HookPriority::Normal, ::Actor, &::Actor::save, bool, CompoundTag& entityTag) {
    if (!isClientSide() && getTypeName() == lv::feature::equipment::kPotatoProjectile) return false;
    return origin(entityTag);
}

namespace lv::feature::equipment {
namespace {
class ProjectileModule {
public:
    void install() { EquipmentProjectileSaveHook::hook(); }
    void uninstall() { EquipmentProjectileSaveHook::unhook(); }
};
} // namespace
} // namespace lv::feature::equipment

LV_REGISTER_MODULE(lv::feature::equipment::ProjectileModule);
