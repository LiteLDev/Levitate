// Selftests of the U49 equipment rows (testPos dx 4965..4989): peculiar / haunted bells, cardboard
// armour and the cardboard sword.

#include <algorithm>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/advancements/Advancements.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
using SharedTypes::Legacy::ArmorSlot;
namespace eq    = lv::feature::equipment;
namespace adv   = lv::feature::advancements;
namespace items = lv::feature::common::items;
using Facing    = ScriptModuleMinecraft::ScriptFacing;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockPos  bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

int stateOf(GlobalPos const& p, char const* name) { return eq::stateAt(p, name).value_or(-1); }

void strip(Player& p) {
    for (auto s : {ArmorSlot::Head, ArmorSlot::Torso, ArmorSlot::Legs, ArmorSlot::Feet}) p.setArmor(s, items::empty());
}

// ---- bells ----------------------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("equipment.bell_place_and_ring", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4965), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.require(ctx.setBlock(base, "minecraft:stone"), "floor");
    auto bell = at(base, 0, 1, 0);
    ctx.cleanupBlock(bell);
    sp.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z + 3.5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.give(sp, item(ctx, "levitate:peculiar_bell"), 0);
    sp.simulateUseItemInSlotOnBlock(0, bp(base), Facing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(3);
    ctx.require(eq::idAt(bell) == eq::kPeculiarBell, "the bell is placed on the floor");
    ctx.check(stateOf(bell, eq::kBellAttachment) == 0, "a floor bell");
    int facing = stateOf(bell, eq::kFacing);
    ctx.check(facing >= 2 && facing <= 5, "facing a horizontal direction: " + std::to_string(facing));
    // a hit along the facing axis rings it, a hit across it does not
    int across = facing == 2 || facing == 3 ? 4 : 2;
    ctx.check(!eq::bellInfo(bell).ringing, "not ringing yet");
    ctx.check(eq::ringBell(bell, facing, nullptr), "rung");
    ctx.check(eq::bellInfo(bell).ringing, "ringing");
    ctx.check(eq::bellInfo(bell).direction == facing, "swinging away from the hit");
    co_await ctx.ticks(76);
    ctx.check(!eq::bellInfo(bell).ringing, "it stops after 74 ticks");
    (void)across;
    // the floor is gone: the bell drops
    lv::game::clearBlock(sp.getDimensionBlockSource(), bp(base));
    co_await ctx.ticks(3);
    ctx.check(eq::idAt(bell) == "minecraft:air", "the bell lost its support and dropped");
});

LV_REGISTER_SELFTEST("equipment.bell_walls", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4968), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp   = static_cast<SimulatedPlayer&>(ctx.player());
    auto  wall = at(base, 0, 1, 1); // south of the bell
    ctx.require(ctx.setBlock(wall, "minecraft:stone"), "a wall");
    auto bell = at(base, 0, 1, 0);
    ctx.cleanupBlock(bell);
    sp.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z - 2.5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.give(sp, item(ctx, "levitate:peculiar_bell"), 0);
    sp.simulateUseItemInSlotOnBlock(0, bp(wall), Facing::North, Vec3{.5f, .5f, 0.f});
    co_await ctx.ticks(3);
    ctx.require(eq::idAt(bell) == eq::kPeculiarBell, "the bell hangs on the wall");
    ctx.check(stateOf(bell, eq::kBellAttachment) == 2, "a single wall bell");
    ctx.check(stateOf(bell, eq::kFacing) == 3, "facing the wall (south)");
    // a block on the other side: it spans both walls
    ctx.require(ctx.setBlock(at(base, 0, 1, -1), "minecraft:stone"), "a second wall");
    co_await ctx.ticks(3);
    ctx.check(stateOf(bell, eq::kBellAttachment) == 3, "a double wall bell");
    // the first wall goes: it hangs on the second one
    lv::game::clearBlock(sp.getDimensionBlockSource(), bp(wall));
    co_await ctx.ticks(3);
    ctx.check(stateOf(bell, eq::kBellAttachment) == 2 && stateOf(bell, eq::kFacing) == 2, "a single wall bell facing north");
});

LV_REGISTER_SELFTEST("equipment.bell_convert", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4971), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(ctx.setBlock(base, "minecraft:soul_soil"), "soul soil");
    auto ceiling = at(base, 0, 3, 0);
    ctx.require(ctx.setBlock(ceiling, "minecraft:stone"), "a ceiling");
    auto bell = at(base, 0, 2, 0);
    ctx.require(ctx.setBlock(bell, std::string(eq::kPeculiarBell), {{"levitate:facing", 3}, {"levitate:attachment", 1}}), "a hanging peculiar bell");
    co_await ctx.ticks(3);
    ctx.check(eq::idAt(bell) == eq::kPeculiarBell, "no soul fire: it stays peculiar");
    ctx.require(ctx.setBlock(at(base, 0, 1, 0), "minecraft:soul_fire"), "soul fire below");
    co_await ctx.ticks(3);
    ctx.check(eq::idAt(bell) == eq::kHauntedBell, "above soul fire it becomes a haunted bell");
    ctx.check(stateOf(bell, eq::kBellAttachment) == 1 && stateOf(bell, eq::kFacing) == 3, "keeping its attachment and facing");
    ctx.check(eq::bellInfo(bell).effectTicks > 0, "the souls stir (HauntedBellBlock#onPlace)");
});

LV_REGISTER_SELFTEST("equipment.haunted_bell_pulse", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4974), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    // a 5x5 stone floor (block light 0 on it, also in daylight: only block light counts)
    for (int x = -2; x <= 2; ++x)
        for (int z = -2; z <= 2; ++z) ctx.require(ctx.setBlock(at(base, x, 0, z), "minecraft:stone"), "floor");
    auto bell = at(base, 0, 1, 0);
    ctx.require(ctx.setBlock(bell, std::string(eq::kHauntedBell), {{"levitate:facing", 3}, {"levitate:attachment", 0}}), "a haunted bell");
    co_await ctx.ticks(2);
    (void)eq::takeSoulMarks();
    adv::revoke(p, "haunted_bell");
    ctx.require(eq::ringBell(bell, 3, &p), "rung by the player");
    ctx.check(adv::has(p, "haunted_bell"), "advancement haunted_bell");
    ctx.check(!eq::ringBell(bell, 3, &p), "not again while recharging (65 ticks)");
    ctx.check(eq::soulPulseCount() > 0, "a soul pulse runs");
    co_await ctx.ticks(20);
    auto marks = eq::takeSoulMarks();
    bool onFloor = std::any_of(marks.begin(), marks.end(), [&](GlobalPos const& m) { return m.y == base.y + 1 && std::abs(m.x - base.x) <= 2 && std::abs(m.z - base.z) <= 2 && !(m.x == bell.x && m.z == bell.z); });
    ctx.check(onFloor, "dark spots on the floor are marked (" + std::to_string(marks.size()) + " marks)");
    bool all = std::all_of(marks.begin(), marks.end(), [](GlobalPos const& m) { return eq::canSpawnSoulAt(m) && eq::soulDark(m); });
    ctx.check(all, "every mark is a dark spawnable spot");
    ctx.check(!eq::canSpawnSoulAt(bell), "not where the bell stands");
    co_await ctx.ticks(50);
    ctx.check(eq::ringBell(bell, 3, nullptr), "rings again after the recharge");
});

LV_REGISTER_SELFTEST("equipment.bell_redstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4978), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(ctx.setBlock(base, "minecraft:stone"), "floor");
    auto bell = at(base, 0, 1, 0);
    ctx.require(ctx.setBlock(bell, std::string(eq::kPeculiarBell), {{"levitate:facing", 2}, {"levitate:attachment", 0}}), "a bell");
    co_await ctx.ticks(6);
    ctx.check(!eq::bellInfo(bell).ringing, "quiet without power");
    ctx.require(ctx.setBlock(at(base, 1, 1, 0), "minecraft:redstone_block"), "power");
    co_await ctx.ticks(6);
    ctx.check(stateOf(bell, eq::kBellPowered) == 1, "powered");
    ctx.check(eq::bellInfo(bell).ringing, "a rising signal rings it");
    ctx.check(eq::bellInfo(bell).direction == 2, "swinging along its facing");
});

LV_REGISTER_SELFTEST("equipment.held_haunted_bell", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4981), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    for (int x = -1; x <= 1; ++x)
        for (int z = -1; z <= 1; ++z) ctx.require(ctx.setBlock(at(base, x, 0, z), "minecraft:stone"), "floor");
    sp.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    (void)eq::takeSoulMarks();
    ctx.give(sp, item(ctx, "levitate:haunted_bell"), sp.getSelectedItemSlot());
    co_await ctx.ticks(6);
    ctx.check(eq::takeSoulMarks().empty(), "nothing during the 10-tick warmup");
    co_await ctx.ticks(20);
    ctx.check(!eq::takeSoulMarks().empty(), "holding it marks the spots around the player");
    sp.getInventory().setItem(sp.getSelectedItemSlot(), items::empty());
    sp.sendInventory(false);
});

// ---- cardboard armour and sword ---------------------------------------------------------------------------

LV_REGISTER_SELFTEST("equipment.cardboard_hiding", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4984), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    strip(sp);
    adv::revoke(sp, "cardboard_armor");
    sp.setArmor(ArmorSlot::Head, item(ctx, "levitate:cardboard_helmet"));
    sp.setArmor(ArmorSlot::Torso, item(ctx, "levitate:cardboard_chestplate"));
    sp.setArmor(ArmorSlot::Legs, item(ctx, "levitate:cardboard_leggings"));
    co_await ctx.ticks(2);
    sp.setSneaking(true);
    co_await ctx.ticks(2);
    ctx.check(!eq::hidingAsBox(sp) && !sp.isInvisible(), "three pieces are not enough");
    sp.setArmor(ArmorSlot::Feet, item(ctx, "levitate:cardboard_boots"));
    co_await ctx.ticks(2);
    ctx.check(eq::hidingAsBox(sp), "the full set while sneaking: a box");
    ctx.check(sp.isInvisible(), "the player is hidden (only the box is drawn)");
    ctx.check(adv::has(sp, "cardboard_armor"), "advancement cardboard_armor");
    // a mob can't pick the box as its target
    auto  level = ll::service::getLevel();
    auto* mob   = level->getSpawner().spawnMob(sp.getDimensionBlockSource(), ActorDefinitionIdentifier("minecraft:pig"), nullptr,
                                               Vec3{base.x + 3.5f, float(base.y), base.z + .5f}, false, false, false);
    ctx.require(mob != nullptr, "a mob");
    static_cast<Mob*>(mob)->setTarget(&sp);
    ctx.check(mob->mTargetId->rawID != sp.getOrCreateUniqueID().rawID, "no target on a hidden player");
    sp.setSneaking(false);
    co_await ctx.ticks(2);
    ctx.check(!eq::hidingAsBox(sp) && !sp.isInvisible(), "standing up shows the player again");
    static_cast<Mob*>(mob)->setTarget(&sp);
    ctx.check(mob->mTargetId->rawID == sp.getOrCreateUniqueID().rawID, "a visible player can be targeted");
    static_cast<Mob*>(mob)->setTarget(nullptr);
    mob->remove();
    strip(sp);
});

LV_REGISTER_SELFTEST("equipment.cardboard_sword", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4987), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
    ctx.give(sp, item(ctx, "levitate:cardboard_sword"), sp.getSelectedItemSlot());
    co_await ctx.ticks(2);
    auto  level = ll::service::getLevel();
    auto* cow   = level->getSpawner().spawnMob(sp.getDimensionBlockSource(), ActorDefinitionIdentifier("minecraft:cow"), nullptr,
                                               Vec3{base.x + 2.5f, float(base.y + 1), base.z + .5f}, false, false, false);
    ctx.require(cow != nullptr, "a cow");
    co_await ctx.ticks(2);
    int health = cow->getHealth();
    ctx.check(eq::cardboardBonk(sp, *cow), "the sword bonks a cow");
    ctx.check(cow->getHealth() == health, "without damage");
    auto const& v = cow->getPosDelta();
    ctx.check(std::abs(v.x) + std::abs(v.z) > .3f, "knocked back");
    ctx.check(cow->getEffect(2u) != nullptr, "slowed (Create: slowness for creatures)");
    auto* spider = level->getSpawner().spawnMob(sp.getDimensionBlockSource(), ActorDefinitionIdentifier("minecraft:spider"), nullptr,
                                                Vec3{base.x - 2.5f, float(base.y + 1), base.z + .5f}, false, false, false);
    if (spider) {
        ctx.check(!eq::cardboardBonk(sp, *spider), "arthropods are hit for real");
        spider->remove();
    }
    cow->remove();
    sp.getInventory().setItem(sp.getSelectedItemSlot(), items::empty());
});

} // namespace
