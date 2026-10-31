// Selftests of the equipment unit (U31, testPos dx 3100..3199).

#include <algorithm>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/GameType.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "core/equipment/Gear.h"
#include "core/equipment/Symmetry.h"
#include "core/equipment/Toolbox.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "feature/redstone/RedstoneLink.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
using SharedTypes::Legacy::ArmorSlot;
namespace eq     = lv::feature::equipment;
namespace ce     = lv::core::equipment;
namespace items  = lv::feature::common::items;
namespace common = lv::feature::common;
namespace net    = lv::feature::net;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockPos  bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

int countEntities(Player& p, GlobalPos const& c, std::string const& id, double r = 3) {
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

void clearItems(Player& p, GlobalPos const& c, double r = 4) {
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

} // namespace

LV_REGISTER_SELFTEST("equipment.toolbox_compartments", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3100), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(ctx.setBlock(base, "levitate:brown_toolbox", {{"levitate:facing", 0}}), "toolbox placed");
    eq::setToolboxInventory(base, nlohmann::json::object());
    auto& region = ctx.player().getDimensionBlockSource();
    ctx.check(items::slotCount(region, bp(base), -1) == ce::kToolboxSlots, "32 slots through the item capability");
    auto rest = items::insert(region, bp(base), 1, item(ctx, "minecraft:cobblestone", 100), false);
    ctx.check(items::isEmpty(rest), "100 cobblestone fit");
    rest = items::insert(region, bp(base), 1, item(ctx, "minecraft:dirt", 10), false);
    ctx.check(items::isEmpty(rest), "dirt starts the second compartment");
    auto inv = ce::ToolboxInv::fromJson(eq::toolboxInventory(base));
    ctx.check(inv.count(0) == 100 && inv.filters[0].match.find("cobblestone") != std::string::npos, "compartment 0 holds the cobblestone");
    ctx.check(inv.count(1) == 10, "compartment 1 holds the dirt");
    // a shulker box or a toolbox can't go in (Create: canFitInsideContainerItems)
    rest = items::insert(region, bp(base), 1, item(ctx, "levitate:red_toolbox", 1), false);
    ctx.check(!items::isEmpty(rest), "a toolbox does not fit in a toolbox");
    auto got = items::extract(region, bp(base), 0, items::any(), 64, false);
    ctx.check(items::idOf(got) == "minecraft:cobblestone" && items::countOf(got) == 64, "extract takes a stack of the first compartment");
});

LV_REGISTER_SELFTEST("equipment.toolbox_break_keeps_contents", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3105), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearItems(p, base);
    ctx.require(ctx.setBlock(base, "levitate:blue_toolbox", {{"levitate:facing", 2}}), "toolbox placed");
    eq::setToolboxInventory(base, nlohmann::json::object());
    auto& region = p.getDimensionBlockSource();
    (void)items::insert(region, bp(base), 1, item(ctx, "minecraft:iron_ingot", 20), false);
    ctx.require(lv::game::clearBlock(region, bp(base)), "toolbox removed");
    co_await ctx.ticks(3);
    // the dropped toolbox carries its contents
    bool found = false;
    AABB box{Vec3{float(base.x - 2), float(base.y - 2), float(base.z - 2)}, Vec3{float(base.x + 3), float(base.y + 3), float(base.z + 3)}};
    for (auto* e : region.getEntities(box, 0.0f)) {
        if (!e || !e->isType(ActorType::ItemEntity)) continue;
        auto const& s = static_cast<ItemActor*>(e)->item();
        if (items::idOf(s) != "levitate:blue_toolbox") continue;
        auto data = items::getData(s, eq::kToolboxData);
        auto inv  = ce::ToolboxInv::fromJson(data.value("inv", nlohmann::json::object()));
        found     = inv.count(0) == 20;
    }
    ctx.check(found, "the broken toolbox drops as an item with its 20 iron ingots");
    clearItems(p, base);
});

LV_REGISTER_SELFTEST("equipment.toolbox_place_with_contents", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3110), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    ctx.require(ctx.setBlock(base, "minecraft:stone"), "floor");
    auto top = at(base, 0, 1, 0);
    ctx.cleanupBlock(top);
    sp.teleport(Vec3{base.x + 2.5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ce::ToolboxInv inv;
    (void)inv.insert({"x", items::toJson(item(ctx, "minecraft:apple")).dump(), 5, 64}, false, false);
    auto box = item(ctx, "levitate:green_toolbox");
    items::setData(box, eq::kToolboxData, {{"inv", inv.toJson()}});
    ctx.give(sp, box, 0);
    sp.simulateUseItemInSlotOnBlock(0, bp(base), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(3);
    ctx.require(eq::idAt(top) == "levitate:green_toolbox", "the toolbox is placed");
    auto placed = ce::ToolboxInv::fromJson(eq::toolboxInventory(top));
    ctx.check(placed.count(0) == 5, "the placed toolbox has the item's 5 apples");
    ctx.check(eq::stateAt(top, eq::kFacing).value_or(-1) == lv::game::opposite(lv::game::playerFacing(sp)), "it faces the player");
});

LV_REGISTER_SELFTEST("equipment.toolbox_equip", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3115), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    ctx.require(ctx.setBlock(base, "levitate:brown_toolbox", {{"levitate:facing", 0}}), "toolbox placed");
    eq::setToolboxInventory(base, nlohmann::json::object());
    auto& region = p.getDimensionBlockSource();
    (void)items::insert(region, bp(base), 1, item(ctx, "minecraft:torch", 64), false);
    p.teleport(Vec3{base.x + 2.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.require(eq::nearbyToolboxes(p).size() == 1, "the toolbox is in range");
    ctx.require(eq::equipFromToolbox(p, base, 0, 0), "torches equipped to slot 0");
    ctx.check(items::countOf(p.getInventory().getItem(0)) == 32, "the hotbar slot holds (64 + 1) / 2 = 32 torches");
    // using some refills the slot from the compartment
    p.getInventory().setItem(0, item(ctx, "minecraft:torch", 10));
    co_await ctx.ticks(2);
    ctx.check(items::countOf(p.getInventory().getItem(0)) == 32, "the linked slot is refilled to 32");
    // unequip returns the items
    ctx.check(eq::unequipSlot(p, 0, false), "unequip");
    ctx.check(items::isEmpty(p.getInventory().getItem(0)), "the slot is empty after unequipping");
    auto inv = ce::ToolboxInv::fromJson(eq::toolboxInventory(base));
    ctx.check(inv.count(0) == 64 - 22, "the toolbox holds what was not used (42)");
    // return items
    p.getInventory().setItem(3, item(ctx, "minecraft:torch", 5));
    p.getInventory().setItem(4, item(ctx, "minecraft:stone", 5));
    eq::depositAll(p, {base});
    ctx.check(items::isEmpty(p.getInventory().getItem(3)), "torches go back to their compartment");
    ctx.check(items::countOf(p.getInventory().getItem(4)) == 5, "stone has no compartment and stays");
    clearInventory(p);
});

LV_REGISTER_SELFTEST("equipment.toolbox_radial_protocol", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3120), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    ctx.require(ctx.setBlock(base, "levitate:red_toolbox", {{"levitate:facing", 0}}), "toolbox placed");
    eq::setToolboxInventory(base, nlohmann::json::object());
    (void)items::insert(p.getDimensionBlockSource(), bp(base), 1, item(ctx, "minecraft:arrow", 30), false);
    p.teleport(Vec3{base.x + 1.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    net::setTestCompanion(p, true);
    (void)net::takeTestMessages(p);
    net::injectFromClient(p, "equipment.toolbox.radial", nlohmann::json::object());
    bool ok = false;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "equipment.toolbox.radial") ok = m.payload.value("boxes", nlohmann::json::array()).size() == 1;
    ctx.check(ok, "the radial menu lists the toolbox");
    net::injectFromClient(p, "equipment.toolbox.select", {{"pos", {base.dim, base.x, base.y, base.z}}, {"compartment", 0}});
    ctx.check(items::idOf(p.getSelectedItem()) == "minecraft:arrow", "selecting the compartment equips the arrows");
    net::injectFromClient(p, "equipment.toolbox.unequip", nlohmann::json::object());
    ctx.check(items::isEmpty(p.getSelectedItem()), "unequip from the menu");
    // the native toolbox screen opens for companions
    eq::openToolbox(p, base);
    bool opened = false;
    for (auto const& m : net::takeTestMessages(p))
        if (m.channel == "screen.open" && m.payload.value("type", std::string()) == "equipment.toolbox") opened = true;
    ctx.check(opened, "right-click opens the native toolbox screen");
    net::setTestCompanion(p, false);
    clearInventory(p);
});

LV_REGISTER_SELFTEST("equipment.toolbox_dye", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3125), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(base, "levitate:brown_toolbox", {{"levitate:facing", 1}}), "toolbox placed");
    eq::setToolboxInventory(base, nlohmann::json::object());
    (void)items::insert(p.getDimensionBlockSource(), bp(base), 1, item(ctx, "minecraft:stick", 7), false);
    auto dye = item(ctx, "minecraft:lime_dye");
    eq::useToolbox(p, dye, base);
    ctx.check(eq::idAt(base) == "levitate:lime_toolbox", "a dye recolours the toolbox");
    ctx.check(eq::stateAt(base, eq::kFacing).value_or(-1) == 1, "it keeps its facing");
    ctx.check(ce::ToolboxInv::fromJson(eq::toolboxInventory(base)).count(0) == 7, "and its contents");
});

LV_REGISTER_SELFTEST("equipment.backtank_fill", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3130), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto tank = at(base, 0, 1, 0);
    ctx.require(ctx.setBlock(base, "levitate:kinetics_test_source", {{"levitate:axis", 1}}), "source below");
    ctx.require(ctx.setBlock(tank, "levitate:copper_backtank", {{"levitate:facing", 0}}), "backtank placed");
    eq::setBlockAir(tank, 0);
    co_await ctx.ticks(2);
    lv::kinetics::setGeneratedSpeed(base, 256);
    co_await ctx.ticks(40);
    int air = eq::blockAir(tank);
    // 256 RPM: +5 air every tick
    ctx.check(air >= 100, "a backtank on a 256 RPM shaft fills quickly (air " + std::to_string(air) + ")");
    lv::kinetics::setGeneratedSpeed(base, 0);
    co_await ctx.ticks(2);
    int stopped = eq::blockAir(tank);
    co_await ctx.ticks(10);
    ctx.check(eq::blockAir(tank) == stopped, "it stops filling without rotation");
    ctx.check(lv::kinetics::stressImpactOf("levitate:copper_backtank") == 4, "stress impact 4 (Create)");
});

LV_REGISTER_SELFTEST("equipment.backtank_wear_and_tools", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3135), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    // right-click a placed backtank with an empty chest slot puts it on
    ctx.require(ctx.setBlock(base, "levitate:copper_backtank", {{"levitate:facing", 0}}), "backtank placed");
    eq::setBlockAir(base, 900);
    ItemStack empty = items::empty();
    ctx.require(eq::useBacktankBlock(p, empty, base), "right-click wears it");
    ctx.check(eq::idAt(base) == "minecraft:air", "the block is gone");
    auto const& chest = p.getArmor(ArmorSlot::Torso);
    ctx.require(items::idOf(chest) == "levitate:copper_backtank", "the backtank is worn");
    ctx.check(eq::airOf(chest) == 900, "with its 900 air");
    // air tools use air instead of durability
    p.getInventory().setItem(p.getSelectedItemSlot(), item(ctx, "levitate:extendo_grip"));
    ctx.check(eq::extendoReach(p) == 3.f, "one grip: +3 reach");
    p.setOffhandSlot(item(ctx, "levitate:extendo_grip"));
    ctx.check(eq::extendoReach(p) == 5.f, "two grips: +5 reach");
    eq::extendoAction(p);
    ctx.check(eq::airOf(p.getArmor(ArmorSlot::Torso)) == 899, "a grip action costs 1 air (900 / 1000)");
    ctx.check(p.getSelectedItem().getDamageValue() == 0, "the grip keeps its durability");
    ctx.check(eq::useAirOrDurability(p, true, 200), "a cannon shot is paid with air");
    ctx.check(eq::airOf(p.getArmor(ArmorSlot::Torso)) == 895, "a cannon shot costs 4 air (900 / 200)");
    // without air the tool takes durability
    auto tank = p.getArmor(ArmorSlot::Torso);
    eq::setAir(tank, 0);
    p.setArmor(ArmorSlot::Torso, tank);
    eq::extendoAction(p);
    ctx.check(p.getSelectedItem().getDamageValue() == 1, "without air the grip loses 1 durability");
    // the backtank can't break (its durability is the air bar)
    auto copy = p.getArmor(ArmorSlot::Torso);
    copy.hurtAndBreak(5000, &p);
    ctx.check(!items::isEmpty(copy) && items::idOf(copy) == "levitate:copper_backtank", "the backtank is unbreakable");
    clearInventory(p);
});

LV_REGISTER_SELFTEST("equipment.diving_helmet", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3140), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    for (int x = -1; x <= 1; ++x)
        for (int y = 0; y <= 3; ++y)
            for (int z = -1; z <= 1; ++z) {
                auto q = at(base, x, y, z);
                bool wall = x != 0 || z != 0 || y == 0;
                ctx.setBlock(q, wall ? "minecraft:glass" : "minecraft:water");
            }
    auto tank = item(ctx, "levitate:copper_backtank");
    eq::setAir(tank, 100);
    p.setArmor(ArmorSlot::Torso, tank);
    p.setArmor(ArmorSlot::Head, item(ctx, "levitate:copper_diving_helmet"));
    p.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(45);
    int air = eq::airOf(p.getArmor(ArmorSlot::Torso));
    ctx.check(air < 100 && air >= 97, "under water the helmet uses 1 air per second (air " + std::to_string(air) + ")");
    ctx.check(p.getEffect(13u) != nullptr, "the wearer can breathe (water breathing)");
    // the netherite set grants fire immunity
    clearInventory(p);
    auto ntank = item(ctx, "levitate:netherite_backtank");
    eq::setAir(ntank, 100);
    p.setArmor(ArmorSlot::Torso, ntank);
    p.setArmor(ArmorSlot::Head, item(ctx, "levitate:netherite_diving_helmet"));
    p.setArmor(ArmorSlot::Legs, item(ctx, "minecraft:netherite_leggings"));
    p.setArmor(ArmorSlot::Feet, item(ctx, "levitate:netherite_diving_boots"));
    co_await ctx.ticks(3);
    ctx.check(p.getEffect(12u) != nullptr, "the full netherite diving set gives fire resistance");
    clearInventory(p);
    p.teleport(Vec3{base.x + 3.5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
});

LV_REGISTER_SELFTEST("equipment.potato_cannon", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3145), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    // farmland under open sky: a potato shot at its top plants potatoes
    auto soil = base;
    ctx.require(ctx.setBlock(soil, "minecraft:farmland"), "farmland");
    ctx.cleanupBlock(at(soil, 0, 1, 0));
    sp.teleport(Vec3{soil.x + 3.5f, float(soil.y + 5), soil.z + .5f}, DimensionType(0));
    sp.simulateLookAt(Vec3{soil.x + .5f, float(soil.y + 1), soil.z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(2);
    ctx.give(sp, item(ctx, "levitate:potato_cannon"), 0);
    ctx.give(sp, item(ctx, "minecraft:potato", 3), 5);
    ctx.require(eq::firePotatoCannon(sp), "the cannon fires");
    ctx.check(items::countOf(sp.getInventory().getItem(5)) == 2, "one potato was used");
    ctx.check(eq::projectileCount() >= 1, "a projectile is in flight");
    co_await ctx.ticks(10);
    ctx.check(eq::idAt(at(soil, 0, 1, 0)) == "minecraft:potatoes", "the potato planted itself on the farmland");
    clearItems(sp, soil, 6);
    clearInventory(sp);
});

LV_REGISTER_SELFTEST("equipment.potato_cannon_damage", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3150), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    auto level = ll::service::getLevel();
    ctx.require(level != nullptr, "level");
    auto  target = at(base, 6, 0, 0);
    auto* mob    = level->getSpawner().spawnMob(sp.getDimensionBlockSource(), ActorDefinitionIdentifier("minecraft:pig"), nullptr,
                                                Vec3{target.x + .5f, float(target.y), target.z + .5f}, false, false, false);
    ctx.require(mob != nullptr, "a pig to shoot at");
    auto pigId = eq::uidOf(*mob);
    sp.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(3);
    sp.simulateLookAt(Vec3{target.x + .5f, float(target.y) + .5f, target.z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(3);
    int before = mob->getHealth();
    ctx.give(sp, item(ctx, "levitate:potato_cannon"), 0);
    ctx.give(sp, item(ctx, "minecraft:baked_potato", 4), 5);
    ActorUniqueID id;
    id.rawID  = pigId;
    Actor* pig = nullptr;
    int    now = before;
    // the pig wanders: aim at where it is now, and shoot again (after the cooldown) if it moved away
    for (int attempt = 0; attempt < 3 && now >= before; ++attempt) {
        pig = level->fetchEntity(id, false);
        if (!pig || !pig->isAlive()) break;
        auto at = pig->getPosition();
        sp.simulateLookAt(Vec3{at.x, at.y + .4f, at.z}, sim::LookDuration::Continuous);
        co_await ctx.ticks(2);
        ctx.require(eq::firePotatoCannon(sp), "the cannon fires");
        co_await ctx.ticks(16);
        pig = level->fetchEntity(id, false);
        now = pig && pig->isAlive() ? pig->getHealth() : 0;
    }
    ctx.check(now < before, "the baked potato hurt the pig (" + std::to_string(before) + " -> " + std::to_string(now) + ")");
    ctx.check(!pig || !pig->isAlive() || pig->isOnFire(), "and set it on fire");
    if (pig && pig->isAlive()) pig->remove();
    clearItems(sp, base, 8);
    clearInventory(sp);
});

LV_REGISTER_SELFTEST("equipment.symmetry_wand", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3155), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    clearInventory(sp);
    // mirror at base (plane, flips X); place stone 2 east of it, against a floor block
    ce::Mirror m;
    m.type = ce::MirrorType::Plane, m.orientation = 0, m.x = base.x, m.y = base.y, m.z = base.z, m.placed = true, m.enabled = true;
    auto wand = item(ctx, "levitate:wand_of_symmetry");
    items::setData(wand, eq::kSymmetryData, m.toJson());
    ctx.give(sp, wand, 1);
    auto floorE = at(base, 2, -1, 0), floorW = at(base, -2, -1, 0);
    ctx.setBlock(floorE, "minecraft:stone");
    ctx.setBlock(floorW, "minecraft:stone");
    auto east = at(base, 2, 0, 0), west = at(base, -2, 0, 0);
    ctx.cleanupBlock(east);
    ctx.cleanupBlock(west);
    sp.teleport(Vec3{base.x + 4.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.give(sp, item(ctx, "minecraft:cobblestone", 2), 0);
    sp.simulateUseItemInSlotOnBlock(0, bp(floorE), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(3);
    ctx.require(eq::idAt(east) == "minecraft:cobblestone", "the block is placed");
    ctx.check(eq::idAt(west) == "minecraft:cobblestone", "and mirrored across the plane");
    ctx.check(items::isEmpty(sp.getInventory().getItem(0)), "the copy used an item from the inventory");
    // breaking is mirrored too
    lv::game::clearBlock(sp.getDimensionBlockSource(), bp(east));
    eq::mirrorBreak(sp, east, "minecraft:cobblestone");
    co_await ctx.ticks(2);
    ctx.check(eq::idAt(west) != "minecraft:cobblestone", "breaking one breaks the mirrored block");
    // block states are mirrored (stairs turn around)
    auto stairs = at(base, 1, 0, 2);
    auto mirror = at(base, -1, 0, 2);
    ctx.cleanupBlock(mirror);
    ctx.setBlock(stairs, "minecraft:oak_stairs", {{"weirdo_direction", 0}});
    ctx.give(sp, item(ctx, "minecraft:oak_stairs", 1), 2); // survival: the copy uses an item
    eq::mirrorPlacement(sp, stairs);
    auto const* b = eq::blockAt(mirror);
    ctx.check(b && std::string(b->getTypeName()) == "minecraft:oak_stairs", "stairs are mirrored");
    auto dir = b ? lv::game::intState(*b, "weirdo_direction") : std::nullopt;
    ctx.check(dir && *dir == 1, "mirrored east-facing stairs face west (weirdo_direction " + std::to_string(dir.value_or(-1)) + ")");
    // diagonal copies turn blocks too: x <-> z makes west-facing stairs face north
    if (b) {
        auto const& turned = eq::mirroredBlock(*b, ce::Mat2{0, 1, 1, 0});
        auto        d2     = lv::game::intState(turned, "weirdo_direction");
        ctx.check(d2 && *d2 == 3, "a diagonal mirror turns west-facing stairs north (weirdo_direction " + std::to_string(d2.value_or(-1)) + ")");
    }
    // Create's findAndRemoveInInventory takes the block's item: redstone for a wire
    auto wire = at(base, 1, 0, 4), wireCopy = at(base, -1, 0, 4);
    ctx.setBlock(at(wire, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(at(wireCopy, 0, -1, 0), "minecraft:stone");
    ctx.cleanupBlock(wireCopy);
    ctx.setBlock(wire, "minecraft:redstone_wire");
    clearInventory(sp);
    ctx.give(sp, wand, 1);
    ctx.give(sp, item(ctx, "minecraft:redstone", 1), 3);
    eq::mirrorPlacement(sp, wire);
    ctx.check(eq::idAt(wireCopy) == "minecraft:redstone_wire", "redstone wire is mirrored in survival");
    ctx.check(items::isEmpty(sp.getInventory().getItem(3)), "the copy used the redstone dust");
    // survival copies need a replaceable spot; creative ones overwrite it
    auto solid = at(base, 1, 0, 6), solidCopy = at(base, -1, 0, 6);
    ctx.setBlock(solidCopy, "minecraft:dirt");
    ctx.setBlock(solid, "minecraft:cobblestone");
    ctx.give(sp, item(ctx, "minecraft:cobblestone", 1), 3);
    eq::mirrorPlacement(sp, solid);
    ctx.check(eq::idAt(solidCopy) == "minecraft:dirt", "survival: a solid block is not replaced");
    sp.setPlayerGameType(GameType::Creative);
    eq::mirrorPlacement(sp, solid);
    ctx.check(eq::idAt(solidCopy) == "minecraft:cobblestone", "creative: the copy overwrites the block");
    sp.setPlayerGameType(GameType::Survival);
    ctx.cleanupBlock(solid);
    ctx.cleanupBlock(solidCopy);
    ctx.cleanupBlock(wire);
    clearItems(sp, base, 5);
    clearInventory(sp);
});

LV_REGISTER_SELFTEST("equipment.symmetry_math", [](Context& ctx) {
    ce::Mirror m;
    m.type = ce::MirrorType::Triple;
    ctx.check(ce::mirrorPositions(m, 3, 0, 1).size() == 7, "octagonal mirror makes 7 copies");
    m.type        = ce::MirrorType::Cross;
    m.orientation = 1;
    auto ps       = ce::mirrorPositions(m, 3, 0, 1);
    ctx.check(ps.size() == 3, "rectangular diagonal mirror makes 3 copies");
});

LV_REGISTER_SELFTEST("equipment.worldshaper", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3160), 0, 3, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(base, "minecraft:stone"), "target block");
    for (int x = -1; x <= 1; ++x)
        for (int y = 1; y <= 3; ++y)
            for (int z = -1; z <= 1; ++z) ctx.cleanupBlock(at(base, x, y, z));
    ce::ShaperSettings st;
    st.brush      = ce::Brush::Cuboid;
    st.params     = {3, 3, 3};
    st.tool       = ce::ShaperTool::Fill;
    st.placement  = ce::Placement::Attached;
    st.configured = true;
    auto const* glass = lv::game::blockWith("minecraft:glass");
    ctx.require(glass != nullptr, "glass block");
    st.material = eq::shaperMaterialOf(*glass);
    ctx.require(eq::shaperMaterial(st) != nullptr, "the material round-trips");
    int changed = eq::shapeAt(p, st, base, 1);
    ctx.check(changed == 27, "a 3x3x3 cuboid attached on top fills 27 blocks (" + std::to_string(changed) + ")");
    ctx.check(eq::idAt(at(base, 0, 1, 0)) == "minecraft:glass" && eq::idAt(at(base, 1, 3, 1)) == "minecraft:glass", "the cube sits on the clicked face");
    ctx.check(eq::idAt(base) == "minecraft:stone", "the clicked block stays (Fill only replaces air)");
    st.tool = ce::ShaperTool::Clear;
    changed = eq::shapeAt(p, st, base, 1);
    ctx.check(eq::idAt(at(base, 0, 2, 0)) == "minecraft:air", "clear removes them again");
});

LV_REGISTER_SELFTEST("equipment.sand_paper_and_fertilizer", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3170), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    // sand paper scrapes oxidation
    ctx.require(ctx.setBlock(base, "minecraft:weathered_copper"), "weathered copper");
    auto paper = item(ctx, "levitate:sand_paper");
    ctx.check(eq::sandBlock(p, paper, base), "sand paper scrapes the block");
    ctx.check(eq::idAt(base) == "minecraft:exposed_copper", "weathered -> exposed");
    ctx.check(paper.getDamageValue() == 1, "it uses 1 of 8 durability");
    // polishing needs a sandpaper_polishing recipe for the other hand's item
    p.getInventory().setItem(p.getSelectedItemSlot(), item(ctx, "levitate:sand_paper"));
    p.setOffhandSlot(item(ctx, "minecraft:stone"));
    ctx.check(!eq::startSanding(p, true), "stone can't be polished");
    if (auto quartz = items::make("levitate:rose_quartz", 2)) {
        p.setOffhandSlot(*quartz);
        ctx.check(eq::startSanding(p, true), "rose quartz can be polished");
        co_await ctx.ticks(40);
        bool polished = false;
        auto& inv = p.getInventory();
        for (int i = 0; i < inv.getContainerSize(); ++i) polished = polished || items::idOf(inv.getItem(i)) == "levitate:polished_rose_quartz";
        ctx.check(polished, "after 32 ticks the rose quartz is polished");
    } else {
        ctx.info("levitate:rose_quartz not registered yet (U9): polishing test skipped");
    }
    // tree fertilizer grows a sapling at once
    auto soil = at(base, 3, 0, 0), sap = at(base, 3, 1, 0);
    ctx.require(ctx.setBlock(soil, "minecraft:dirt"), "dirt");
    for (int y = 2; y <= 9; ++y)
        for (int x = -2; x <= 2; ++x)
            for (int z = -2; z <= 2; ++z) ctx.cleanupBlock(at(sap, x, y - 1, z));
    ctx.require(ctx.setBlock(sap, "minecraft:oak_sapling"), "oak sapling");
    auto fert = item(ctx, "levitate:tree_fertilizer", 2);
    ctx.check(eq::useTreeFertilizer(p, fert, sap), "fertilizer used on the sapling");
    ctx.check(eq::idAt(sap) == "minecraft:oak_log", "the sapling became a tree");
    ctx.check(items::countOf(fert) == 1, "one fertilizer was used");
    clearInventory(p);
});

LV_REGISTER_SELFTEST("equipment.linked_controller", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace rl = lv::feature::redstone::link;
    auto base = at(ctx.testPos(3180), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    clearInventory(p);
    auto controller = item(ctx, "levitate:linked_controller");
    auto j          = [&](char const* id) { return items::toJson(item(ctx, id)); };
    auto pair       = [](nlohmann::json a, nlohmann::json b) { return nlohmann::json::array({a, b}); };
    nlohmann::json none = nullptr;
    nlohmann::json freq = nlohmann::json::array({pair(j("minecraft:redstone"), j("minecraft:diamond")), pair(none, none), pair(none, none),
                                                 pair(none, none), pair(j("minecraft:gold_ingot"), none), pair(none, none)});
    items::setData(controller, eq::kControllerData, nlohmann::json{{"freq", freq}});
    p.getInventory().setItem(p.getSelectedItemSlot(), controller);
    p.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    auto key  = rl::keyOf(item(ctx, "minecraft:redstone"), item(ctx, "minecraft:diamond"));
    auto gold = rl::keyOf(item(ctx, "minecraft:gold_ingot"), items::empty());
    eq::controllerButtons(p, {true, false, false, false, false, false}, std::nullopt);
    ctx.check(rl::powerAt(base, key) == 15, "button forward transmits on its frequency (U23 link network)");
    ctx.check(rl::powerAt(at(base, rl::range() + 50, 0, 0), key) == 0, "out of link range: no signal");
    eq::controllerButtons(p, {false, false, false, false, true, false}, std::nullopt);
    ctx.check(rl::powerAt(base, key) == 0, "releasing the button stops the signal");
    ctx.check(rl::powerAt(base, gold) == 15, "jump transmits on its own frequency");
    eq::controllerButtons(p, {false, false, false, false, false, false}, std::nullopt);
    ctx.check(rl::powerAt(base, gold) == 0, "and stops");
    // bind mode: right-click a redstone link, then press a button
    auto link = at(base, 0, 0, 2);
    ctx.require(ctx.setBlock(link, "levitate:redstone_link", {{"levitate:facing", 1}}), "redstone link");
    auto& doc                 = lv::feature::common::blockdata::edit(link);
    doc["redstone"]["first"]  = j("minecraft:emerald");
    doc["redstone"]["last"]   = j("minecraft:lapis_lazuli");
    auto held = p.getSelectedItem();
    ctx.check(eq::controllerUseOn(p, held, link) && eq::controllerActive(p), "right-clicking a link enters bind mode");
    eq::controllerButtons(p, {false, true, false, false, false, false}, std::nullopt);
    eq::controllerButtons(p, {false, false, false, false, false, false}, std::nullopt);
    auto bound = items::getData(p.getSelectedItem(), eq::kControllerData)["freq"][1];
    ctx.check(bound.is_array() && items::idOf(items::fromJson(bound[0])) == "minecraft:emerald", "the pressed button (back) took the link's frequency");
    eq::controllerButtons(p, {false, true, false, false, false, false}, std::nullopt);
    ctx.check(rl::powerAt(link, rl::keyOf(item(ctx, "minecraft:emerald"), item(ctx, "minecraft:lapis_lazuli"))) == 15, "the bound button powers that frequency");
    eq::controllerButtons(p, {false, false, false, false, false, false}, std::nullopt);
    held = p.getSelectedItem();
    ctx.check(eq::controllerUse(p, held) && !eq::controllerActive(p), "right-click stops using it");
    // lectern: right-click a lectern with the controller
    auto lectern = at(base, 1, 0, 0); // Create's playerInRange: eye within 1.8 blocks of its centre
    ctx.require(ctx.setBlock(lectern, "minecraft:lectern"), "lectern");
    held = p.getSelectedItem();
    ctx.check(eq::lecternUse(p, held, lectern), "the controller goes on the lectern");
    ctx.check(eq::idAt(lectern) == "levitate:lectern_controller", "a lectern controller");
    ctx.check(eq::lecternUse(p, held, lectern) && eq::controllerActive(p), "right-clicking it starts using it");
    eq::controllerButtons(p, {true, false, false, false, false, false}, lectern);
    ctx.check(rl::powerAt(lectern, key) == 15, "the lectern transmits from its own position");
    eq::controllerButtons(p, {false, false, false, false, false, false}, lectern);
    ctx.check(eq::lecternUse(p, held, lectern) && !eq::controllerActive(p), "right-click again stops");
    // Create's playerInRange: too far to start, and walking away stops it
    p.teleport(Vec3{base.x - 2.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.check(!eq::lecternInRange(p, lectern), "3 blocks away is out of the lectern's range");
    ctx.check(eq::lecternUse(p, held, lectern) && !eq::controllerActive(p), "too far away: right-click does not start it");
    p.teleport(Vec3{base.x + .5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    ctx.check(eq::lecternUse(p, held, lectern) && eq::controllerActive(p), "back in range: it starts");
    p.teleport(Vec3{base.x - 2.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(3);
    ctx.check(!eq::controllerActive(p), "walking out of range stops using it");
    (void)lv::feature::common::blockdata::take(lectern); // no controller drop when the test area is cleared
    clearInventory(p);
});

// U59: backtank comparators, stew ammo, bells, worldshaper, knockback, Infinity (dx 5900..5999)
#include "feature/equipment/SelfTestsU59.inc"
