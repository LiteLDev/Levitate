// Held tools: extendo grip (reach, air), potato cannon (Create's PotatoProjectileEntity simulated
// on the server and shown as an item), sand paper (polishing, scraping) and tree fertilizer.

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/FertilizerType.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/ShapeType.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"

#include "core/equipment/Gear.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;

// ---- extendo grip -------------------------------------------------------------------------------------

float extendoReach(Player const& player) {
    bool main = items::idOf(player.getSelectedItem()) == kExtendoGrip;
    bool off  = items::idOf(player.getOffhandSlot()) == kExtendoGrip;
    if (main && off) return 5.f;
    if (main || off) return 3.f;
    return 0.f;
}

void extendoAction(Player& player) {
    bool main = items::idOf(player.getSelectedItem()) == kExtendoGrip;
    bool off  = items::idOf(player.getOffhandSlot()) == kExtendoGrip;
    if (!main && !off) return;
    // Create: the main hand grip is checked first
    useAirOrDurability(player, main, config().maxExtendoGripActions);
}

// ---- potato cannon --------------------------------------------------------------------------------------

namespace {

struct Projectile {
    int                   dim = 0;
    ce::Vec3f             pos, vel;
    ce::PotatoType const* type = nullptr;
    ItemStack             ammo;
    std::int64_t          owner   = 0;
    std::int64_t          display = 0; ///< the item actor that shows the projectile
    int                   age     = 0;
    bool                  recoverable = true; ///< only the first projectile of a split shot
};
std::vector<Projectile>& projectiles() {
    static std::vector<Projectile> v;
    return v;
}
std::uint64_t gProjectileSerial = 0;
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

void addEffect(Actor& a, unsigned id, int ticks, int amplifier) {
    EffectDuration d;
    d.mValue = ticks;
    MobEffectInstance inst(id, d);
    inst.mAmplifier = amplifier;
    a.addEffect(inst);
}

/// The ammo the cannon fires (vanilla getProjectile order: off hand, main hand, inventory).
int findAmmo(Player& p, ItemStack& out) {
    auto check = [&](ItemStack const& s) { return !items::isEmpty(s) && ce::potatoTypeFor(items::idOf(s)); };
    if (check(p.getOffhandSlot())) {
        out = p.getOffhandSlot();
        return -2;
    }
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& s = inv.getItem(i);
        if (check(s)) {
            out = s;
            return i;
        }
    }
    return -1;
}

void takeAmmo(Player& p, int slot) {
    if (p.isCreative()) return;
    if (slot == -2) {
        auto s = p.getOffhandSlot();
        p.setOffhandSlot(items::withCount(s, items::countOf(s) - 1));
    } else if (slot >= 0) {
        auto& inv = p.getInventory();
        auto  s   = inv.getItem(slot);
        inv.setItem(slot, items::withCount(s, items::countOf(s) - 1));
    }
    p.sendInventory(false);
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

void pop(Projectile const& p, double x, double y, double z) {
    sound(p.dim, x, y, z, "mob.slime.small", 1.f, .9f + float(random01()) * .2f);
    for (int i = 0; i < 4; ++i) particle(p.dim, x, y, z, "minecraft:basic_crit_particle");
}

void recover(Projectile const& p, double x, double y, double z) {
    if (p.type->consumed || !p.recoverable) return;
    // Create: the projectile item is recovered with 12.5 % (+ Potato Recovery, not on Bedrock)
    if (random01() < ce::potatoRecoveryChance(0)) dropAt(p.dim, x, y, z, items::withCount(p.ammo, 1));
}

void hitEntity(Projectile& p, Actor& target, double x, double y, double z) {
    using E      = ce::PotatoEffect;
    Actor* owner = actorById(p.owner);
    auto const& t = *p.type;
    if (t.effect == E::SetOnFire) target.setOnFire(t.effectSeconds, true);
    std::string ttype = target.getTypeName();
    bool        enderman = ttype == "minecraft:enderman";
    if (ttype == "minecraft:wither" && target.getHealth() <= target.getMaxHealth() / 2) return; // powered wither
    if (owner) target.hurtByCause(t.damage, SharedTypes::Legacy::ActorDamageCause::Projectile, *owner);
    else target.hurtByCause(t.damage, SharedTypes::Legacy::ActorDamageCause::Projectile);
    if (enderman) return;
    // knockback along the horizontal motion
    double h = std::sqrt(p.vel.x * p.vel.x + p.vel.z * p.vel.z);
    if (h > 1e-6 && !target.isType(ActorType::ItemEntity)) {
        double k = t.knockback * 0.6;
        auto const& v = target.getPosDelta();
        setMotion(target, v.x + p.vel.x / h * k, v.y + 0.1, v.z + p.vel.z / h * k);
    }
    switch (t.effect) {
    case E::Poison:
    case E::Slowness: addEffect(target, static_cast<unsigned>(t.effectId), t.effectTicks, t.effectAmplifier); break;
    case E::ChorusTeleport: {
        auto pos = target.getPosition();
        for (int i = 0; i < 16; ++i) {
            double nx = pos.x + (random01() - .5) * 20, nz = pos.z + (random01() - .5) * 20;
            double ny = std::clamp(pos.y + (random01() - .5) * 20, -63.0, 318.0);
            GlobalPos at{p.dim, int(std::floor(nx)), int(std::floor(ny)), int(std::floor(nz))};
            GlobalPos below{at.dim, at.x, at.y - 1, at.z};
            if (!replaceableAt(at) || replaceableAt(below)) continue;
            target.teleportTo(Vec3{float(nx), float(at.y), float(nz)}, true, 0, 0, false);
            sound(p.dim, nx, ny, nz, "mob.endermen.portal", 1.f, 1.f);
            break;
        }
        break;
    }
    case E::GoldenApple:
        addEffect(target, 10, 100, 1); // regeneration II 5 s
        addEffect(target, 22, 2400, 0); // absorption 2 min
        break;
    case E::EnchantedGoldenApple:
        addEffect(target, 10, 400, 1);
        addEffect(target, 11, 6000, 0);
        addEffect(target, 12, 6000, 0);
        addEffect(target, 22, 2400, 3);
        break;
    case E::Pufferfish:
        addEffect(target, 19, 1200, 1); // poison II 60 s
        addEffect(target, 17, 300, 2);  // hunger III 15 s
        addEffect(target, 9, 300, 0);   // nausea 15 s
        break;
    case E::SuspiciousStew: dropAt(p.dim, x, y, z, *items::make("minecraft:bowl", 1)); break;
    default: break;
    }
    recover(p, x, y, z);
}

void hitBlock(Projectile& p, BlockSource& region, BlockPos const& bp, int face, double x, double y, double z) {
    using E       = ce::PotatoEffect;
    auto const& t = *p.type;
    GlobalPos   at{p.dim, bp.x, bp.y, bp.z};
    GlobalPos   above{p.dim, bp.x, bp.y + 1, bp.z};
    std::string id = idAt(at);
    if ((t.effect == E::PlantPotato || t.effect == E::PlantCarrot) && face == 1 && id == "minecraft:farmland" && idAt(above) == "minecraft:air") {
        place(above, t.effect == E::PlantPotato ? "minecraft:potatoes" : "minecraft:carrots");
        sound(p.dim, x, y, z, "dig.grass", 1.f, 1.f);
        log().info("equipment: a potato cannon planted {} at {} {} {}", t.id, above.x, above.y, above.z);
        return; // the item is used up
    }
    if (t.effect == E::PlaceMelon || t.effect == E::PlacePumpkin) {
        GlobalPos target = at;
        static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
        if (face >= 0 && face < 6) target = {p.dim, bp.x + dx[face], bp.y + dy[face], bp.z + dz[face]};
        if (replaceableAt(target)) {
            place(target, t.effect == E::PlaceMelon ? "minecraft:melon_block" : "minecraft:pumpkin");
            return;
        }
    }
    (void)region;
    recover(p, x, y, z);
}

} // namespace

bool firePotatoCannon(Player& player) {
    auto uid  = uidOf(player);
    auto now  = currentTick();
    auto& cd  = cannonCooldowns()[uid];
    if (now < cd) return true;
    ItemStack ammo;
    int       slot = findAmmo(player, ammo);
    if (slot == -1) return false;
    auto const* type = ce::potatoTypeFor(items::idOf(ammo));
    if (!type) return false;
    bool dual = items::idOf(player.getOffhandSlot()) == kPotatoCannon && items::idOf(player.getSelectedItem()) == kPotatoCannon;
    cd        = now + static_cast<std::uint64_t>(ce::potatoCooldown(*type, dual));
    auto  eye   = player.getEyePos();
    auto  look  = player.getViewVector(1.0f);
    double speed = ce::potatoSpeed(*type);
    int   dim   = static_cast<int>(player.getDimensionId());
    auto* region = regionOf(dim);
    auto  level  = ll::service::getLevel();
    if (!region || !level) return false;
    ce::Vec3f start{eye.x + look.x * 1.5, eye.y + look.y * 1.5 - 0.15, eye.z + look.z * 1.5};
    for (int i = 0; i < type->split; ++i) {
        ce::Vec3f v{look.x * speed, look.y * speed, look.z * speed};
        if (type->split > 1) {
            // Create: split shots spread around the aim (i * 360 / split ± 20 degrees, offset 0.1)
            double ang = (i * 360.0 / type->split + (random01() - .5) * 40.0) * std::numbers::pi / 180.0;
            double ux = -look.z, uz = look.x; // horizontal perpendicular
            double ul = std::sqrt(ux * ux + uz * uz);
            if (ul < 1e-6) ux = 1, uz = 0, ul = 1;
            ux /= ul, uz /= ul;
            v.x += (ux * std::cos(ang)) * 0.1 * speed;
            v.y += std::sin(ang) * 0.1 * speed;
            v.z += (uz * std::cos(ang)) * 0.1 * speed;
        }
        Projectile p;
        p.dim         = dim;
        p.pos         = start;
        p.vel         = v;
        p.type        = type;
        p.ammo        = items::withCount(ammo, 1);
        p.owner       = uid;
        p.recoverable = i == 0;
        // the shown item carries a unique tag so it never merges with items on the ground
        auto shown = p.ammo;
        items::setData(shown, "lv:projectile", static_cast<std::int64_t>(++gProjectileSerial));
        auto* actor = level->getSpawner().spawnItem(*region, shown, nullptr, Vec3{float(start.x), float(start.y), float(start.z)}, 0);
        if (actor) {
            actor->pickupDelay() = 32767;
            p.display            = uidOf(*actor);
        }
        projectiles().push_back(std::move(p));
    }
    takeAmmo(player, slot);
    useAirOrDurability(player, items::idOf(player.getSelectedItem()) == kPotatoCannon, config().maxPotatoCannonShots);
    sound(dim, eye.x, eye.y, eye.z, "random.bow", 1.f, type->pitch + float(random01() - .5) * .25f);
    log().info("equipment: {} fired a potato cannon ({})", player.getRealName(), type->id);
    return true;
}

void tickProjectiles() {
    auto& list = projectiles();
    for (std::size_t i = 0; i < list.size();) {
        auto& p      = list[i];
        bool  done   = false;
        auto* region = regionOf(p.dim);
        ++p.age;
        if (!region || p.age > 400) done = true;
        if (!done) {
            ce::Vec3f a = p.pos, b{p.pos.x + p.vel.x, p.pos.y + p.vel.y, p.pos.z + p.vel.z};
            // blocks
            HitResult hr = region->clip(
                Vec3{float(a.x), float(a.y), float(a.z)},
                Vec3{float(b.x), float(b.y), float(b.z)},
                false,
                ShapeType::Collision,
                64,
                false,
                false,
                nullptr,
                [](BlockSource const&, Block const&, bool) { return true; },
                false
            );
            double tBlock = 2;
            if (hr.mType == HitResultType::Tile) {
                double len = std::sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y + p.vel.z * p.vel.z);
                double dx = hr.mPos.x - a.x, dy = hr.mPos.y - a.y, dz = hr.mPos.z - a.z;
                tBlock    = len > 1e-9 ? std::sqrt(dx * dx + dy * dy + dz * dz) / len : 0;
            }
            // entities
            Actor* best  = nullptr;
            double tBest = 2;
            AABB   box{
                Vec3{float(std::min(a.x, b.x) - 1), float(std::min(a.y, b.y) - 1), float(std::min(a.z, b.z) - 1)},
                Vec3{float(std::max(a.x, b.x) + 1), float(std::max(a.y, b.y) + 1), float(std::max(a.z, b.z) + 1)}
            };
            for (auto* e : region->getEntities(box, 0.0f)) {
                if (!e || !e->isAlive() || e->isType(ActorType::ItemEntity) || uidOf(*e) == p.display) continue;
                if (uidOf(*e) == p.owner && p.age < 5) continue;
                if (!e->hasCategory(ActorCategory::Mob) && !e->isPlayer()) continue;
                auto const& bb = e->getAABB();
                auto        t  = segmentBox(a, b, {bb.min.x - .3, bb.min.y - .3, bb.min.z - .3}, {bb.max.x + .3, bb.max.y + .3, bb.max.z + .3});
                if (t && *t < tBest) tBest = *t, best = e;
            }
            if (best && tBest <= tBlock) {
                double x = a.x + (b.x - a.x) * tBest, y = a.y + (b.y - a.y) * tBest, z = a.z + (b.z - a.z) * tBest;
                hitEntity(p, *best, x, y, z);
                pop(p, x, y, z);
                done = true;
            } else if (tBlock <= 1) {
                hitBlock(p, *region, hr.mBlock, hr.mFacing, hr.mPos.x, hr.mPos.y, hr.mPos.z);
                pop(p, hr.mPos.x, hr.mPos.y, hr.mPos.z);
                done = true;
            } else {
                p.pos = b;
                p.vel = ce::potatoStep(p.vel, *p.type);
                if (auto* d = actorById(p.display)) {
                    d->teleportTo(Vec3{float(p.pos.x), float(p.pos.y), float(p.pos.z)}, false, 0, 0, true);
                    setMotion(*d, p.vel.x, p.vel.y, p.vel.z);
                }
            }
        }
        if (done) {
            if (auto* d = actorById(p.display)) d->remove();
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        } else ++i;
    }
}

void resetProjectiles() {
    projectiles().clear();
    cannonCooldowns().clear();
}

int projectileCount() { return static_cast<int>(projectiles().size()); }

// ---- sand paper ---------------------------------------------------------------------------------------------

namespace {
struct Sanding {
    std::uint64_t start    = 0;
    bool          mainHand = true;
    bool          fromHand = true; ///< polishing the item in the other hand (else an item entity)
    std::int64_t  entity   = 0;
};
std::map<std::int64_t, Sanding>& sandings() {
    static std::map<std::int64_t, Sanding> m;
    return m;
}
constexpr int kSandingTicks = 32;

bool polishable(ItemStack const& s) {
    return !items::isEmpty(s) && common::recipes::findFirst("sandpaper_polishing", s) != nullptr;
}
} // namespace

bool startSanding(Player& player, bool mainHand) {
    ItemStack other = mainHand ? player.getOffhandSlot() : player.getSelectedItem();
    Sanding   s;
    s.start    = currentTick();
    s.mainHand = mainHand;
    if (polishable(other)) {
        s.fromHand = true;
    } else {
        // an item entity near the looked-at point, within 3 blocks
        auto hit = player.traceRay(3.0f, false, true);
        if (hit.mType != HitResultType::Tile) return false;
        auto* region = regionOf(static_cast<int>(player.getDimensionId()));
        if (!region) return false;
        AABB box{Vec3{hit.mPos.x - 1, hit.mPos.y - 1, hit.mPos.z - 1}, Vec3{hit.mPos.x + 1, hit.mPos.y + 1, hit.mPos.z + 1}};
        for (auto* e : region->getEntities(box, 0.0f)) {
            if (!e || !e->isType(ActorType::ItemEntity)) continue;
            if (!polishable(static_cast<ItemActor*>(e)->item())) continue;
            s.fromHand = false;
            s.entity   = uidOf(*e);
            break;
        }
        if (!s.entity) return false;
    }
    sandings()[uidOf(player)] = s;
    return true;
}

void cancelSanding(Player& player) { sandings().erase(uidOf(player)); }

void tickSanding() {
    for (auto it = sandings().begin(); it != sandings().end();) {
        auto*  player = playerById(it->first);
        auto&  s      = it->second;
        bool   drop   = !player;
        if (player) {
            ItemStack paper = s.mainHand ? player->getSelectedItem() : player->getOffhandSlot();
            if (!isPaper(items::idOf(paper))) drop = true;
            else {
                auto age = currentTick() - s.start;
                auto pos = player->getPosition();
                int  dim = static_cast<int>(player->getDimensionId());
                if (age >= 6 && (age - 6) % 7 == 0) sound(dim, pos.x, pos.y, pos.z, "step.sand", .9f, 1.f + float(random01()) * .2f);
                if (age >= kSandingTicks) {
                    ItemStack source;
                    ItemActor* entity = nullptr;
                    if (s.fromHand) source = s.mainHand ? player->getOffhandSlot() : player->getSelectedItem();
                    else if (auto* a = actorById(s.entity); a && a->isType(ActorType::ItemEntity)) {
                        entity = static_cast<ItemActor*>(a);
                        source = entity->item();
                    }
                    auto const* r = polishable(source) ? common::recipes::findFirst("sandpaper_polishing", source) : nullptr;
                    if (r) {
                        auto rest = items::withCount(source, items::countOf(source) - 1);
                        if (entity) {
                            if (items::isEmpty(rest)) entity->remove();
                            else entity->item() = rest;
                        } else if (s.mainHand) player->setOffhandSlot(rest);
                        else player->setSelectedItem(rest);
                        for (auto const& out : common::recipes::rollResults(*r)) giveOrDrop(*player, out);
                        if (!player->isCreative()) {
                            paper.hurtAndBreak(1, player);
                            if (s.mainHand) player->setSelectedItem(paper);
                            else player->setOffhandSlot(paper);
                        }
                        player->sendInventory(false);
                        sound(dim, pos.x, pos.y, pos.z, "step.sand", 1.f, 1.2f);
                        for (int i = 0; i < 5; ++i) particle(dim, pos.x, pos.y + 1.2, pos.z, "minecraft:basic_crit_particle");
                        log().info("equipment: {} polished {}", player->getRealName(), items::idOf(source));
                    }
                    drop = true;
                }
            }
        }
        if (drop) it = sandings().erase(it);
        else ++it;
    }
}

bool sandBlock(Player& player, ItemStack& held, GlobalPos const& pos) {
    // Create: sand paper scrapes oxidation and removes wax like an axe
    std::string id = idAt(pos);
    if (!id.starts_with("minecraft:") || id.find("copper") == std::string::npos) return false;
    for (auto bad : {"stairs", "slab", "door", "bulb", "lantern", "chain", "torch", "ore", "raw", "grate"})
        if (id.find(bad) != std::string::npos) return false;
    std::string next;
    if (id.starts_with("minecraft:waxed_")) next = "minecraft:" + id.substr(16);
    else if (auto p = id.find("oxidized_"); p != std::string::npos) next = id.substr(0, p) + "weathered_" + id.substr(p + 9);
    else if (auto p2 = id.find("weathered_"); p2 != std::string::npos) next = id.substr(0, p2) + "exposed_" + id.substr(p2 + 10);
    else if (auto p3 = id.find("exposed_"); p3 != std::string::npos) {
        next = id.substr(0, p3) + id.substr(p3 + 8);
        if (next == "minecraft:copper") next = "minecraft:copper_block";
    }
    if (next.empty() || !game::blockWith(next)) return false;
    place(pos, next);
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "step.sand", 1.f, 1.f);
    if (!player.isCreative()) {
        held.hurtAndBreak(1, &player);
        player.sendInventory(false);
    }
    log().info("equipment: {} sanded {} into {}", player.getRealName(), id, next);
    return true;
}

} // namespace lv::feature::equipment
