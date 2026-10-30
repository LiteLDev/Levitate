// Selftests of the potato projectile entity and Create's enchantments (U45, testPos dx 4550..4569).

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/Difficulty.h"
#include "mc/deps/shared_types/legacy/ContainerType.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/managers/models/ContainerManagerModel.h"
#include "mc/world/containers/managers/models/EnchantingContainerManagerModel.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/item/enchanting/EnchantmentInstance.h"
#include "mc/world/item/enchanting/ItemEnchantOption.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "core/equipment/Gear.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
using SharedTypes::Legacy::ArmorSlot;
namespace eq    = lv::feature::equipment;
namespace ce    = lv::core::equipment;
namespace items = lv::feature::common::items;
namespace adv   = lv::feature::advancements;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

Actor* actorById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level || !uid) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->fetchEntity(id, false);
}

int countItems(Player& p, GlobalPos const& c, std::string const& id, double r = 4) {
    auto& region = p.getDimensionBlockSource();
    AABB  box{Vec3{float(c.x - r), float(c.y - r), float(c.z - r)}, Vec3{float(c.x + r + 1), float(c.y + r + 1), float(c.z + r + 1)}};
    int   n = 0;
    for (auto* e : region.getEntities(box, 0.0f)) {
        if (!e || !e->isType(ActorType::ItemEntity)) continue;
        auto const& s = static_cast<ItemActor*>(e)->item();
        if (items::idOf(s) == id) n += items::countOf(s);
    }
    return n;
}

void clearItems(Player& p, GlobalPos const& c, double r = 6) {
    auto& region = p.getDimensionBlockSource();
    AABB  box{Vec3{float(c.x - r), float(c.y - r), float(c.z - r)}, Vec3{float(c.x + r + 1), float(c.y + r + 1), float(c.z + r + 1)}};
    for (auto* e : region.getEntities(box, 0.0f))
        if (e && e->isType(ActorType::ItemEntity)) e->remove();
}

void clearInventory(Player& p) {
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, items::empty());
    p.setOffhandSlot(items::empty());
    for (auto s : {ArmorSlot::Head, ArmorSlot::Torso, ArmorSlot::Legs, ArmorSlot::Feet}) p.setArmor(s, items::empty());
}

/// A 3 x 3 stone wall facing west at `x` around (y, z).
void wall(Context& ctx, GlobalPos c) {
    for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) ctx.setBlock(at(c, 0, dy, dz), "minecraft:stone");
}

/// The projectile (simulation) of a projectile entity.
std::optional<eq::ProjectileInfo> projectile(std::int64_t entity) {
    for (auto const& p : eq::projectiles())
        if (p.entity == entity) return p;
    return std::nullopt;
}

ll::coro::CoroTask<bool> waitGone(std::int64_t entity, int maxTicks) {
    for (int i = 0; i < maxTicks; ++i) {
        if (!projectile(entity)) co_return true;
        co_await Context::ticks(1);
    }
    co_return !projectile(entity);
}

void addEffect(Actor& a, unsigned id, int ticks) {
    EffectDuration d;
    d.mValue = ticks;
    MobEffectInstance inst(id, d);
    a.addEffect(inst);
}

} // namespace

// The cannon fires a real projectile entity: tagged, of the ammo's variant, never saved, moved every
// tick to the simulated position, removed when it pops on a wall (no recovery without the enchantment).
LV_REGISTER_SELFTEST("equipment.potato_entity", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4550), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    auto target = at(base, 10, 1, 0);
    for (int dz = -2; dz <= 2; ++dz) wall(ctx, at(target, 0, 0, dz));
    clearItems(sp, target, 4);
    sp.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    sp.simulateLookAt(Vec3{target.x + .5f, sp.getEyePos().y, target.z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(3);
    ctx.give(sp, item(ctx, "levitate:potato_cannon"), 0);
    ctx.give(sp, item(ctx, "minecraft:potato", 2), 5);
    ctx.require(eq::firePotatoCannon(sp), "the cannon fires");
    auto list = eq::projectiles();
    ctx.require(list.size() == 1, "one projectile in flight, got " + std::to_string(list.size()));
    auto  id = list[0].entity;
    auto* e  = actorById(id);
    ctx.require(e != nullptr, "the projectile is an entity");
    ctx.check(e->getTypeName() == eq::kPotatoProjectile, "of type levitate:potato_projectile, got " + e->getTypeName());
    ctx.check(e->hasTag(eq::kPotatoProjectileTag), "tagged");
    ctx.check(e->getVariant() == ce::potatoAmmoIndex("minecraft:potato"), "variant 0 (the potato texture), got " + std::to_string(e->getVariant()));
    CompoundTag tag;
    ctx.check(!e->save(tag), "never saved with the world");
    ctx.check(std::fabs(list[0].recovery) < 1e-9, "Create: no recovery without Potato Recovery");
    double x0 = e->getPosition().x;
    co_await ctx.ticks(2);
    e = actorById(id);
    ctx.require(e != nullptr, "still flying");
    auto p = projectile(id);
    ctx.require(p.has_value(), "still simulated");
    ctx.check(e->getPosition().x > x0 + 2, "the entity moves with the projectile (" + std::to_string(x0) + " -> " + std::to_string(e->getPosition().x) + ")");
    ctx.check(std::fabs(e->getPosition().x - p->x) < 0.01 && std::fabs(e->getPosition().y - p->y) < 0.01, "to the simulated position");
    ctx.require(co_await waitGone(id, 30), "it pops on the wall");
    co_await ctx.ticks(2);
    auto* gone = actorById(id);
    ctx.check(!gone || !gone->isAlive(), "and the entity is removed");
    ctx.check(countItems(sp, target, "minecraft:potato") == 0, "no potato is recovered without the enchantment");
    clearItems(sp, target, 4);
    clearInventory(sp);
});

// Every ammo item picks its texture (the entity variant) and Create's render mode in the resource pack.
LV_REGISTER_SELFTEST("equipment.potato_variants", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4553), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = ctx.player();
    int   dim = static_cast<int>(sp.getDimensionId());
    for (auto id : {"minecraft:carrot", "minecraft:cake", "minecraft:tropical_fish", "minecraft:melon_block", "levitate:blaze_cake"}) {
        // straight down into the ground next to the test position
        auto e = eq::launchProjectile(dim, base.x + .5, base.y + 1.5, base.z + .5, 0, -1, 0, id, 0, 0);
        ctx.require(e != 0, std::string("a projectile of ") + id);
        auto* a = actorById(e);
        ctx.require(a != nullptr, "its entity");
        ctx.check(a->getVariant() == ce::potatoAmmoIndex(id), std::string(id) + ": variant " + std::to_string(ce::potatoAmmoIndex(id)) + ", got " + std::to_string(a->getVariant()));
        ctx.check(a->getMarkVariant() == 0, "not stuck");
        ctx.require(co_await waitGone(e, 20), "it pops on the ground");
    }
    clearItems(sp, base, 3);
});

// Create's sticky types stick to the entity they hit, slide down it and pop below its feet.
LV_REGISTER_SELFTEST("equipment.potato_sticky", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4556), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp    = ctx.player();
    auto  level = ll::service::getLevel();
    ctx.require(level != nullptr, "level");
    auto  target = at(base, 4, 0, 0);
    Actor* mob    = level->getSpawner().spawnMob(sp.getDimensionBlockSource(), ActorDefinitionIdentifier("minecraft:cow"), nullptr,
                                                Vec3{target.x + .5f, float(target.y), target.z + .5f}, false, false, false);
    ctx.require(mob != nullptr, "a cow to hit");
    auto cowId = eq::uidOf(*mob);
    co_await ctx.ticks(2);
    mob = actorById(cowId);
    ctx.require(mob != nullptr, "the cow");
    float before = mob->getHealth();
    auto  cp     = mob->getPosition();
    int   dim    = static_cast<int>(sp.getDimensionId());
    auto  e = eq::launchProjectile(dim, cp.x - 2.0, cp.y + 0.6, cp.z, 1.5, 0, 0, "minecraft:cake", eq::uidOf(sp), 0);
    ctx.require(e != 0, "a cake projectile");
    co_await ctx.ticks(3);
    auto p = projectile(e);
    ctx.require(p.has_value(), "the cake did not pop at once");
    ctx.check(p->stuckTo == cowId, "the cake sticks to the cow");
    mob = actorById(cowId);
    ctx.require(mob != nullptr, "the cow is still there");
    ctx.check(mob->getHealth() < before, "and hurt it");
    auto* a = actorById(e);
    ctx.check(a && a->getMarkVariant() == 1, "drawn as stuck (mark variant 1)");
    double off0 = p->y - mob->getPosition().y;
    co_await ctx.ticks(8);
    p   = projectile(e);
    mob = actorById(cowId);
    ctx.check(p && mob && p->y - mob->getPosition().y < off0 - 0.2, "it slides down the cow");
    ctx.check(co_await waitGone(e, 40), "and pops below the cow's feet");
    if (auto* m = actorById(cowId)) m->remove();
    clearItems(sp, target, 4);
});

// Projectiles of different types pop each other; both owners get "potato_cannon_collide".
LV_REGISTER_SELFTEST("equipment.potato_collide", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4559), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp  = ctx.player();
    int   dim = static_cast<int>(sp.getDimensionId());
    adv::revoke(sp, "potato_cannon_collide");
    // A falls from rest; after 10 ticks (Create: only projectiles older than 10 ticks collide) B flies
    // into it from 2 blocks west with the same vertical speed
    auto a = eq::launchProjectile(dim, base.x + .5, base.y + 14, base.z + .5, 0, 0, 0, "minecraft:potato", eq::uidOf(sp), 0);
    ctx.require(a != 0, "projectile A");
    co_await ctx.ticks(10);
    auto pa = projectile(a);
    ctx.require(pa.has_value(), "A is still falling");
    auto const* potato = ce::potatoTypeFor("minecraft:potato");
    ce::Vec3f   v{};
    for (int i = 0; i < 10; ++i) v = ce::potatoStep(v, *potato);
    auto b = eq::launchProjectile(dim, pa->x - 2, pa->y, pa->z, 1, v.y, 0, "minecraft:carrot", eq::uidOf(sp), 0);
    ctx.require(b != 0, "projectile B");
    bool ga = co_await waitGone(a, 6);
    bool gb = co_await waitGone(b, 1);
    ctx.check(ga && gb, "both projectiles popped");
    ctx.check(adv::has(sp, "potato_cannon_collide"), "the owner got potato_cannon_collide");
    clearItems(sp, base, 4);
});

// Create's recovery rule: the recovery roll (else the type's drop: a bowl for suspicious stew), and the
// cannon's Potato Recovery level sets the chance (0 without it).
LV_REGISTER_SELFTEST("equipment.potato_recovery", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4562), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp  = static_cast<SimulatedPlayer&>(ctx.player());
    int   dim = static_cast<int>(sp.getDimensionId());
    auto  w   = at(base, 5, 2, 0);
    wall(ctx, w);
    clearItems(sp, w, 5);
    auto shoot = [&](char const* id, double chance) { return eq::launchProjectile(dim, w.x - 3.0, w.y + .4, w.z + .5, 1.2, 0, 0, id, 0, chance); };
    auto e     = shoot("minecraft:apple", 1.0);
    ctx.require(co_await waitGone(e, 20), "an apple hits the wall");
    co_await ctx.ticks(2);
    ctx.check(countItems(sp, w, "minecraft:apple") == 1, "recovered with chance 1");
    e = shoot("minecraft:apple", 0.0);
    ctx.require(co_await waitGone(e, 20), "a second apple hits the wall");
    co_await ctx.ticks(2);
    ctx.check(countItems(sp, w, "minecraft:apple") == 1, "not recovered with chance 0");
    e = shoot("minecraft:suspicious_stew", 0.0);
    ctx.require(co_await waitGone(e, 20), "a suspicious stew hits the wall");
    co_await ctx.ticks(2);
    ctx.check(countItems(sp, w, "minecraft:bowl") == 1, "the stew leaves its bowl");
    clearItems(sp, w, 5);
    // the enchanted cannon
    clearInventory(sp);
    auto cannon = item(ctx, "levitate:potato_cannon");
    eq::setEnchantLevel(cannon, eq::kPotatoRecovery, 2, &sp);
    ctx.check(eq::enchantLevel(cannon, eq::kPotatoRecovery) == 2, "the cannon has Potato Recovery II");
    auto lore = cannon.getCustomLore();
    ctx.check(!lore.empty() && lore[0].find("Potato Recovery II") != std::string::npos, "shown as a lore line, got " + (lore.empty() ? std::string("none") : lore[0]));
    sp.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    sp.simulateLookAt(Vec3{base.x + .5f, float(base.y) + 30.f, base.z + 3.5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(3);
    ctx.give(sp, cannon, 0);
    ctx.give(sp, item(ctx, "minecraft:apple", 3), 5);
    ctx.require(eq::firePotatoCannon(sp), "the enchanted cannon fires");
    auto list = eq::projectiles();
    ctx.require(!list.empty(), "a projectile");
    ctx.check(std::fabs(list.back().recovery - 0.375) < 1e-9, "Potato Recovery II: 37.5 % (got " + std::to_string(list.back().recovery) + ")");
    co_await waitGone(list.back().entity, 120);
    clearItems(sp, base, 8);
    clearInventory(sp);
});

// Create's CureZombieVillager: a golden apple cures a weakened zombie villager.
LV_REGISTER_SELFTEST("equipment.potato_golden_apple", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4565), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp    = ctx.player();
    auto  level = ll::service::getLevel();
    ctx.require(level != nullptr, "level");
    auto difficulty = level->getDifficulty();
    level->setDifficulty(SharedTypes::Legacy::Difficulty::Easy); // zombies need a difficulty
    auto  target = at(base, 4, 0, 0);
    Actor* mob    = level->getSpawner().spawnMob(sp.getDimensionBlockSource(), ActorDefinitionIdentifier("minecraft:zombie_villager_v2"), nullptr,
                                                Vec3{target.x + .5f, float(target.y), target.z + .5f}, false, false, false);
    if (!mob) level->setDifficulty(difficulty);
    ctx.require(mob != nullptr, "a zombie villager");
    auto zid = eq::uidOf(*mob);
    addEffect(*mob, 18, 1200); // weakness
    addEffect(*mob, 12, 1200); // fire resistance: no sunburn (a hurt mob is briefly immune, the hit would not count)
    co_await ctx.ticks(1);
    mob = actorById(zid);
    ctx.require(mob != nullptr, "the zombie villager");
    auto cp  = mob->getPosition();
    int  dim = static_cast<int>(sp.getDimensionId());
    auto e   = eq::launchProjectile(dim, cp.x - 2.0, cp.y + 0.8, cp.z, 1.5, 0, 0, "minecraft:golden_apple", eq::uidOf(sp), 1.0);
    ctx.require(e != 0, "a golden apple projectile");
    float before = mob->getHealth();
    ctx.check(co_await waitGone(e, 10), "the golden apple hits");
    co_await ctx.ticks(3);
    mob = actorById(zid);
    ctx.require(mob != nullptr, "the zombie villager is still there");
    ctx.info("health " + std::to_string(before) + " -> " + std::to_string(mob->getHealth()) + ", weakness "
             + (mob->getEffect(18u) ? "yes" : "no") + ", strength " + (mob->getEffect(5u) ? "yes" : "no"));
    ctx.check(mob->getEffect(18u) == nullptr && mob->getEffect(5u) != nullptr, "the zombie villager starts to convert (weakness removed, strength given)");
    ctx.check(countItems(sp, target, "minecraft:golden_apple") == 0, "the golden apple is used up");
    if (auto* m = actorById(zid)) m->remove();
    level->setDifficulty(difficulty);
    clearItems(sp, target, 4);
});

// Capacity: + enchantedBacktankCapacity (300) air per level, in the item and in the placed block.
LV_REGISTER_SELFTEST("equipment.potato_capacity", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4566), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    auto tank = item(ctx, "levitate:copper_backtank");
    ctx.check(eq::maxAir(tank) == 900, "900 air without Capacity");
    eq::setEnchantLevel(tank, eq::kCapacity, 2, &sp);
    ctx.check(eq::maxAir(tank) == 1500, "Capacity II: 1500 air, got " + std::to_string(eq::maxAir(tank)));
    eq::setAir(tank, 1400);
    ctx.check(eq::airOf(tank) == 1400, "it holds 1400 air");
    auto lore = tank.getCustomLore();
    ctx.check(!lore.empty() && lore[0].find("Capacity II") != std::string::npos, "shown as a lore line");
    // place it (the player's placement): the block keeps the level and the air
    auto floor = base;
    ctx.require(ctx.setBlock(floor, "minecraft:stone"), "floor");
    auto pos = at(floor, 0, 1, 0);
    ctx.cleanupBlock(pos);
    sp.teleport(Vec3{floor.x + 2.5f, float(floor.y + 1), floor.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.give(sp, tank, 0);
    sp.simulateUseItemInSlotOnBlock(0, BlockPos{floor.x, floor.y, floor.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(2);
    ctx.require(eq::isBacktank(eq::idAt(pos)), "the backtank was placed");
    ctx.check(eq::blockMaxAir(pos) == 1500, "the placed backtank holds 1500 air, got " + std::to_string(eq::blockMaxAir(pos)));
    ctx.check(eq::blockAir(pos) == 1400, "with its 1400 air, got " + std::to_string(eq::blockAir(pos)));
    // wear it again: the item keeps the level
    clearInventory(sp);
    ItemStack empty;
    ctx.require(eq::useBacktankBlock(sp, empty, pos), "put on from the block");
    auto const& worn = sp.getArmor(ArmorSlot::Torso);
    ctx.check(eq::enchantLevel(worn, eq::kCapacity) == 2, "the worn backtank keeps Capacity II");
    ctx.check(eq::airOf(worn) == 1400, "and its air");
    clearInventory(sp);
});

// The enchanting table offers Create's enchantment as Java does (deterministic per seed and option)
// and the option applied to the item adds it.
LV_REGISTER_SELFTEST("equipment.potato_enchant_table", [](Context& ctx) -> ll::coro::CoroTask<> {
    // the roll alone: deterministic, only for the items it applies to, Create's levels
    auto r1 = eq::rollTableEnchant("levitate:potato_cannon", 1, 30, 1234, 2);
    auto r2 = eq::rollTableEnchant("levitate:potato_cannon", 1, 30, 1234, 2);
    ctx.check(r1 == r2, "the roll is deterministic per seed and option");
    ctx.check(r1.first == eq::kPotatoRecovery && r1.second >= 1 && r1.second <= 2, "alone in the pool it is always picked at cost 30");
    ctx.check(eq::rollTableEnchant("minecraft:stone", 1, 30, 1, 0).second == 0, "nothing for other items");
    ctx.check(eq::rollTableEnchant("levitate:copper_backtank", 9, 30, 1, 0).first == eq::kCapacity, "Capacity for backtanks");
    ctx.check(eq::rollTableEnchant("levitate:potato_cannon", 1, 3, 1, 0).second == 0, "nothing at a low cost");

    // a real table with 16 bookshelves
    auto base = at(ctx.testPos(4568), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    ctx.require(ctx.setBlock(base, "minecraft:enchanting_table"), "enchanting table");
    for (int dx = -2; dx <= 2; ++dx)
        for (int dz = -2; dz <= 2; ++dz)
            if (std::max(std::abs(dx), std::abs(dz)) == 2) ctx.setBlock(at(base, dx, 0, dz), "minecraft:bookshelf");
    sp.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(3);
    bool opened = sp.simulateInteract(BlockPos{base.x, base.y, base.z}, ScriptModuleMinecraft::ScriptFacing::Up);
    co_await ctx.ticks(2);
    std::shared_ptr<ContainerManagerModel> model = sp.mContainerManager;
    ctx.info(std::string("interact ") + (opened ? "used" : "not used") + ", container type "
             + std::to_string(model ? static_cast<int>(model->getContainerType()) : -1));
    if (!model || model->getContainerType() != SharedTypes::Legacy::ContainerType::Enchantment) {
        // a simulated player has no screen: the model the game makes for a table, opened by hand
        auto own = std::make_shared<EnchantingContainerManagerModel>(static_cast<ContainerID>(50), sp, BlockPos{base.x, base.y, base.z});
        own->postInit();
        model = own;
    }
    ctx.require(model && model->getContainerType() == SharedTypes::Legacy::ContainerType::Enchantment, "an enchanting container");
    auto& table = static_cast<EnchantingContainerManagerModel&>(*model);
    auto  cannon = item(ctx, "levitate:potato_cannon");
    int   found  = -1;
    for (int seed = 1; seed <= 60 && found < 0; ++seed) {
        sp.mEnchantmentSeed = seed * 7919;
        table.setSlot(0, cannon, false);
        table.setSlot(1, item(ctx, "minecraft:lapis_lazuli", 3), false);
        table.recalculateOptions();
        auto rolls = eq::tableRolls(sp);
        auto const& options = table.mEnchantOptions.get();
        ctx.require(rolls.size() == options.size(), "a roll per option");
        for (std::size_t i = 0; i < rolls.size(); ++i)
            if (rolls[i].second > 0) found = static_cast<int>(i);
    }
    ctx.require(found >= 0, "some option offers Potato Recovery");
    auto rolls = eq::tableRolls(sp);
    table.recalculateOptions();
    ctx.check(eq::tableRolls(sp) == rolls, "the same options roll the same again");
    auto const& option = table.mEnchantOptions.get()[found];
    ctx.info("option " + std::to_string(found) + " (cost " + std::to_string(option.mCost) + "): " + rolls[found].first + " " + std::to_string(rolls[found].second));
    // the option applied to the item (Bedrock's EnchantUtils::applyEnchant inside the table's craft request)
    auto copy = cannon;
    eq::asTableEnchant(sp, [&] { EnchantUtils::applyEnchant(copy, option.mEnchants.get(), false); });
    ctx.check(eq::enchantLevel(copy, eq::kPotatoRecovery) == rolls[found].second, "the enchanted cannon got Potato Recovery " + std::to_string(rolls[found].second));
    ctx.check(!copy.getCustomLore().empty(), "with its lore line");
    table.setSlot(0, items::empty(), false);
    table.setSlot(1, items::empty(), false);
    clearInventory(sp);
});

// Anvils merge Create's enchantments as Java's anvil does; grindstones remove them.
LV_REGISTER_SELFTEST("equipment.potato_anvil_grindstone", [](Context& ctx) {
    auto a = item(ctx, "levitate:potato_cannon");
    auto b = item(ctx, "levitate:potato_cannon");
    EnchantmentInstance unbreaking;
    unbreaking.mEnchantType = Enchant::Type::Unbreaking;
    unbreaking.mLevel       = 1;
    EnchantUtils::applyEnchant(a, unbreaking, false);
    EnchantUtils::applyEnchant(b, unbreaking, false);
    eq::setEnchantLevel(a, eq::kPotatoRecovery, 1);
    eq::setEnchantLevel(b, eq::kPotatoRecovery, 1);
    ItemStack out = a;
    EnchantUtils::combineEnchantedItems(a, b, out, false);
    ctx.check(eq::enchantLevel(out, eq::kPotatoRecovery) == 2, "I + I = II, got " + std::to_string(eq::enchantLevel(out, eq::kPotatoRecovery)));
    EnchantUtils::removeEnchants(out);
    ctx.check(eq::enchantLevel(out, eq::kPotatoRecovery) == 0, "the grindstone removes it");
    ctx.check(out.getCustomLore().empty(), "and its lore line");
});
