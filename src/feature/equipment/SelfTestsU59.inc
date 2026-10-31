// Included at the end of SelfTests.cpp (its helpers `at` and `clearInventory`). Selftests of the U59 alignment of the equipment with Create (testPos dx 5900..5999): comparator
// output of backtanks, suspicious stew ammo, the potato cannon's death message, bells rung by
// projectiles, the worldshaper's material pick and surface brush, the extendo grip's knockback,
// Infinity on the potato cannon and the diving helmet's breathing rules.

#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SuspiciousStewItem.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantResult.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/item/enchanting/EnchantmentInstance.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "core/equipment/Shaper.h"
#include "feature/common/DeathMessages.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

/// testPos puts dx 59xx 250 blocks south, outside the area the selftest run keeps ticking: the
/// tests with entities move to z -63 (a row no unit uses) and columns 90..99.
constexpr int kU59Shift = -313;

using lv::core::GlobalPos;
using lv::selftest::Context;
using SharedTypes::Legacy::ArmorSlot;
namespace eq    = lv::feature::equipment;
namespace ce    = lv::core::equipment;
namespace items = lv::feature::common::items;
namespace rs    = lv::feature::common::redstone;

ItemStack u59Item(Context& ctx, std::string const& id, int count = 1, int aux = 0) {
    auto s = items::make(id, count, aux);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

Actor* u59SpawnMob(Context& ctx, char const* id, GlobalPos const& p) {
    auto level = ll::service::getLevel();
    ctx.require(level != nullptr, "level");
    Actor* mob = level->getSpawner().spawnMob(ctx.player().getDimensionBlockSource(), ActorDefinitionIdentifier(id), nullptr,
                                              Vec3{p.x + .5f, float(p.y), p.z + .5f}, false, true, false);
    ctx.require(mob != nullptr, std::string("spawned ") + id);
    return mob;
}

bool u59InFlight(std::int64_t entity) {
    for (auto const& p : eq::projectiles())
        if (p.entity == entity) return true;
    return false;
}

ll::coro::CoroTask<bool> u59WaitGone(std::int64_t entity, int maxTicks) {
    for (int i = 0; i < maxTicks; ++i) {
        if (!u59InFlight(entity)) co_return true;
        co_await Context::ticks(1);
    }
    co_return !u59InFlight(entity);
}

} // namespace

// Create's BacktankBlock.getAnalogOutputSignal: fractionToRedstoneLevel(air / max air).
LV_REGISTER_SELFTEST("equipment.u59_backtank_comparator", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(5900), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, "levitate:copper_backtank"), "a copper backtank");
    auto* region = eq::regionOf(p.dim);
    ctx.require(region != nullptr, "region");
    eq::setBlockAir(p, 0);
    auto empty = rs::analogOutput(*region, lv::game::blockPos(p));
    ctx.check(empty && *empty == 0, "an empty backtank gives 0, got " + std::to_string(empty.value_or(-1)));
    int max = eq::blockMaxAir(p);
    eq::setBlockAir(p, max / 2);
    auto half = rs::analogOutput(*region, lv::game::blockPos(p));
    ctx.check(half && *half == rs::fractionToRedstoneLevel(double(max / 2) / max), "half full: " + std::to_string(half.value_or(-1)));
    eq::setBlockAir(p, max);
    auto full = rs::analogOutput(*region, lv::game::blockPos(p));
    ctx.check(full && *full == 15, "full: 15, got " + std::to_string(full.value_or(-1)));
    ctx.cleanupBlock(p);
});

// Create's SuspiciousStew hit action: the stew's own effects; the kill message names the cannon.
LV_REGISTER_SELFTEST("equipment.u59_potato_stew", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(5990, kU59Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp  = ctx.player();
    int   dim = static_cast<int>(sp.getDimensionId());
    for (int dx = -1; dx <= 5; ++dx)
        for (int dz = -1; dz <= 1; ++dz) ctx.setBlock(at(base, dx, -1, dz), "minecraft:stone");
    Actor* pig = u59SpawnMob(ctx, "minecraft:pig", at(base, 4, 0, 0));
    auto   pigId = pig->getOrCreateUniqueID().rawID;
    co_await ctx.ticks(2);
    auto const& stews = SuspiciousStewItem::_getStewEffects();
    int         aux   = 7; // Bedrock: the oxeye daisy stew
    unsigned    want  = stews[aux].mId;
    auto e = eq::launchProjectile(dim, base.x + .5, base.y + .6, base.z + .5, 1.0, 0, 0, "minecraft:suspicious_stew", 0, 0, aux);
    ctx.require(e != 0, "a stew projectile");
    ctx.require(co_await u59WaitGone(e, 30), "it hits");
    auto* p2 = ll::service::getLevel()->fetchEntity(ActorUniqueID{pigId}, false);
    ctx.require(p2 != nullptr, "the pig is still there");
    ctx.check(p2->getEffect(want) != nullptr, "the pig got the stew's effect " + std::to_string(want));
    auto msg = lv::feature::common::deathmsg::peek(*p2, "Pig");
    ctx.check(msg.first == "death.attack.create.potato_cannon", "a kill by it reads Create's potato cannon message, got " + msg.first);
    p2->remove();
});

// AbstractBellBlock (BellBlock#onProjectileHit): a potato hitting the bell's side rings it.
LV_REGISTER_SELFTEST("equipment.u59_bell_projectile", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(5993, kU59Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp   = ctx.player();
    int   dim  = static_cast<int>(sp.getDimensionId());
    auto  bell = at(base, 3, 0, 0);
    ctx.setBlock(at(bell, 0, -1, 0), "minecraft:stone");
    // a floor bell facing east (attachment 0): rung on its facing axis, here from the west
    ctx.require(ctx.setBlock(bell, std::string(eq::kPeculiarBell), {{eq::kBellAttachment, 0}, {"levitate:facing", 5}}), "a peculiar bell");
    co_await ctx.ticks(1);
    ctx.check(!eq::bellInfo(bell).ringing, "the bell is still");
    auto e = eq::launchProjectile(dim, base.x + .5, bell.y + .45, bell.z + .5, 1.0, 0, 0, "minecraft:potato", 0, 0);
    ctx.require(e != 0, "a potato");
    ctx.require(co_await u59WaitGone(e, 20), "it hits the bell");
    co_await ctx.ticks(1);
    ctx.check(eq::bellInfo(bell).ringing, "the potato rang the bell");
    ctx.cleanupBlock(bell);
});

// Create's ZapperInteractionHandler.trySelect: leaves are stored persistent; door halves are refused.
LV_REGISTER_SELFTEST("equipment.u59_worldshaper_pick", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(5905), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    auto leaves = at(base, 3, 1, 0);
    ctx.require(ctx.setBlock(leaves, "minecraft:oak_leaves", {{"persistent_bit", 0}}), "natural oak leaves");
    sp.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    sp.simulateLookAt(Vec3{leaves.x + .5f, leaves.y + .5f, leaves.z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(3);
    ce::ShaperSettings st;
    st.configured = true;
    auto shaper   = u59Item(ctx, "levitate:handheld_worldshaper");
    items::setData(shaper, eq::kShaperData, st.toJson());
    sp.getInventory().setItem(sp.getSelectedItemSlot(), shaper);
    ItemStack held = sp.getSelectedItem();
    ctx.require(eq::shaperPick(sp, held), "the shaper picks the leaves");
    auto picked = ce::ShaperSettings::fromJson(items::getData(sp.getSelectedItem(), eq::kShaperData));
    auto const* m = eq::shaperMaterial(picked);
    ctx.require(m != nullptr, "a material");
    ctx.check(lv::game::intState(*m, "persistent_bit").value_or(0) == 1, "stored as persistent leaves (they never decay)");
    held = sp.getSelectedItem();
    ctx.check(!eq::shaperPick(sp, held), "the same block again changes nothing");
    // the upper half of a door is refused
    ctx.require(ctx.setBlock(leaves, "minecraft:wooden_door", {{"upper_block_bit", 1}}), "a door's upper half");
    held = sp.getSelectedItem();
    ctx.check(!eq::shaperPick(sp, held), "door halves are not picked");
    ctx.cleanupBlock(leaves);
    clearInventory(sp);
});

// Create's DynamicBrush (surface): a block covered by a sturdy face is skipped, a torch is not.
LV_REGISTER_SELFTEST("equipment.u59_surface_brush", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(5910), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    auto  a = base, b = at(base, 1, 0, 0);
    ctx.require(ctx.setBlock(a, "minecraft:stone") && ctx.setBlock(b, "minecraft:stone"), "two stone blocks");
    for (int dx = -1; dx <= 2; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
            if (!(dz == 0 && (dx == 0 || dx == 1))) ctx.setBlock(at(base, dx, 0, dz), "minecraft:glass");
    ctx.require(ctx.setBlock(at(a, 0, 1, 0), "minecraft:torch", {{"torch_facing_direction", 5}}), "a torch on the first");
    ctx.require(ctx.setBlock(at(b, 0, 1, 0), "minecraft:smooth_stone_slab", {{"top_slot_bit", 0}}), "a bottom slab on the second");
    ce::ShaperSettings st;
    st.brush      = ce::Brush::Surface;
    st.params     = {3, 1, 1};
    st.tool       = ce::ShaperTool::Replace;
    st.configured = true;
    st.material   = eq::shaperMaterialOf(*lv::game::blockWith("minecraft:dirt"));
    eq::shapeAt(p, st, a, 1);
    ctx.check(eq::idAt(a) == "minecraft:dirt", "under a torch: replaced");
    ctx.check(eq::idAt(b) == "minecraft:stone", "under a bottom slab (a sturdy bottom face): kept");
    for (int dx = -1; dx <= 2; ++dx)
        for (int dy = 0; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz) ctx.cleanupBlock(at(base, dx, dy, dz));
});

// Create's attacksByExtendoGripHaveMoreKnockback: +2 knockback strength.
LV_REGISTER_SELFTEST("equipment.u59_grip_knockback", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(5996, kU59Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    for (int dx = -1; dx <= 8; ++dx)
        for (int dz = -2; dz <= 2; ++dz) ctx.setBlock(at(base, dx, -1, dz), "minecraft:stone");
    sp.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    Actor* pig = u59SpawnMob(ctx, "minecraft:pig", at(base, 2, 0, 0));
    co_await ctx.ticks(5);
    sp.getInventory().setItem(sp.getSelectedItemSlot(), u59Item(ctx, "levitate:extendo_grip"));
    sp.simulateLookAt(*pig, sim::LookDuration::Continuous);
    co_await ctx.ticks(2);
    float x0 = pig->getPosition().x;
    ctx.require(sp.simulateAttack(pig), "the player hits the pig");
    co_await ctx.ticks(4);
    ctx.check(pig->getPosition().x - x0 > 2.f, "the pig flies away (" + std::to_string(pig->getPosition().x - x0) + " blocks)");
    pig->remove();
    clearInventory(sp);
});

// PotatoCannonItem#supportsEnchantment: no Infinity.
LV_REGISTER_SELFTEST("equipment.u59_cannon_infinity", [](Context& ctx) {
    auto                cannon = u59Item(ctx, "levitate:potato_cannon");
    EnchantmentInstance infinity;
    infinity.mEnchantType = Enchant::Type::Infinity;
    infinity.mLevel       = 1;
    ctx.check(EnchantUtils::canEnchant(cannon, infinity, false).result == EnchantResultType::Fail, "Infinity can't go on the potato cannon");
    EnchantmentInstance power;
    power.mEnchantType = Enchant::Type::Power;
    power.mLevel       = 1;
    ctx.check(EnchantUtils::canEnchant(cannon, power, false).result != EnchantResultType::Fail, "Power can");
});

// Create's DivingHelmetItem: Aqua Affinity built in.
LV_REGISTER_SELFTEST("equipment.u59_helmet_aqua_affinity", [](Context& ctx) {
    auto& p = ctx.player();
    clearInventory(p);
    p.setArmor(ArmorSlot::Head, u59Item(ctx, "levitate:copper_diving_helmet"));
    ctx.check(eq::hasAquaAffinityHelmet(p), "a copper diving helmet gives Aqua Affinity");
    p.setArmor(ArmorSlot::Head, u59Item(ctx, "minecraft:iron_helmet"));
    ctx.check(!eq::hasAquaAffinityHelmet(p), "an iron helmet does not");
    clearInventory(p);
});

// Create's lava swimming multiplier (DivingBootsItem#getMovementMultiplier) is checked by gtest;
// here: netherite boots in lava speed a falling, moving player up.
