// Selftests of the package entities (U46, testPos dx 4600..4699, moved to the ticking strip at
// z spawn - 55 like U44's).

#include <string>
#include <vector>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltApi.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/packages/Packages.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
namespace pk    = lv::feature::packages;
namespace items = lv::feature::common::items;
namespace ia    = lv::feature::common::interactions;
using lv::core::GlobalPos;

constexpr int kRowDz = -230;

GlobalPos at(Context& ctx, int dx, int dy = 0) {
    auto p = ctx.testPos(dx, kRowDz);
    p.y += dy;
    return p;
}

ItemStack stack(char const* id, int count) { return items::make(id, count).value_or(ItemStack::EMPTY_ITEM()); }

AABB around(GlobalPos const& p, float r = 3.f) {
    return AABB{Vec3{p.x - r, p.y - 2.f, p.z - r}, Vec3{p.x + 1 + r, p.y + 4.f, p.z + 1 + r}};
}

std::vector<Actor*> packageEntities(GlobalPos const& p, float r = 3.f) {
    std::vector<Actor*> out;
    auto*               region = pk::regionOf(p.dim);
    if (!region) return out;
    for (auto* a : region->getEntities(around(p, r), 0.f))
        if (a && a->isAlive() && a->getTypeName() == pk::kPackageEntityId) out.push_back(a);
    return out;
}

int droppedItems(GlobalPos const& p, char const* id, float r = 3.f) {
    auto* region = pk::regionOf(p.dim);
    if (!region) return 0;
    int n = 0;
    for (auto* a : region->getEntities(around(p, r), 0.f, ActorType::ItemEntity)) {
        auto& item = static_cast<ItemActor&>(*a);
        if (items::idOf(item.item()) == id) n += items::countOf(item.item());
    }
    return n;
}

void clearAround(GlobalPos const& p, float r = 4.f) {
    auto* region = pk::regionOf(p.dim);
    if (!region) return;
    for (auto* a : region->getEntities(around(p, r), 0.f))
        if (a && (a->getTypeName() == pk::kPackageEntityId || a->getEntityTypeId() == ActorType::ItemEntity)) a->remove();
}

int countPlayer(Player& player, std::string const& id) {
    int   n   = 0;
    auto& inv = player.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i)
        if (items::idOf(inv.getItem(i)) == id) n += items::countOf(inv.getItem(i));
    return n;
}

void clearInventory(Player& player) {
    auto& inv = player.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) inv.setItem(i, items::empty());
}

} // namespace

LV_REGISTER_SELFTEST("packages.entity_put_down", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4600);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{pos.x + 0.5f, pos.y + 0.f, pos.z + 2.5f}, DimensionType(pos.dim));
    co_await ctx.ticks(2);
    ItemStack box = pk::containing({stack("minecraft:emerald", 5)});
    sp.simulateSetItem(box, true, 0);
    co_await ctx.ticks(1);
    // a right-click on the ground under the test position
    sp.simulateUseItemInSlotOnBlock(0, BlockPos{pos.x, pos.y - 1, pos.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.f, 0.5f});
    co_await ctx.ticks(3);
    auto found = packageEntities(pos);
    ctx.require(found.size() == 1, "one package entity was put down, found " + std::to_string(found.size()));
    auto* e = found.front();
    ctx.check(std::abs(e->getFeetPos().x - (pos.x + 0.5f)) < 0.2f && std::abs(e->getFeetPos().z - (pos.z + 0.5f)) < 0.2f, "it stands where the click was");
    ctx.check(e->getVariant() == pk::packageStyle(items::idOf(box)), "its variant is the package's style");
    auto kept = pk::packageEntityBox(e->getOrCreateUniqueID().rawID);
    ctx.check(kept && pk::contentsOf(*kept).size() == 9 && items::idOf(pk::contentsOf(*kept)[0]) == "minecraft:emerald", "the entity keeps the package's contents");
    ctx.check(countPlayer(sp, items::idOf(box)) == 0, "the package left the hand");
    // a second click at the same spot does nothing (Create: a package is already there)
    sp.simulateSetItem(pk::containing({stack("minecraft:coal", 1)}), true, 0);
    co_await ctx.ticks(6);
    sp.simulateUseItemInSlotOnBlock(0, BlockPos{pos.x, pos.y - 1, pos.z}, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.f, 0.5f});
    co_await ctx.ticks(3);
    ctx.check(packageEntities(pos).size() == 1, "no second package where one stands");
    clearAround(pos);
    clearInventory(sp);
});

LV_REGISTER_SELFTEST("packages.entity_throw", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4610);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos, 12);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{pos.x + 0.5f, pos.y + 0.f, pos.z + 0.5f}, DimensionType(pos.dim));
    co_await ctx.ticks(2);
    sp.simulateLookAt(BlockPos{pos.x + 10, pos.y + 2, pos.z}, sim::LookDuration::Instant);
    co_await ctx.ticks(2);
    auto thrown = pk::containing({stack("minecraft:clay_ball", 3)});
    sp.simulateSetItem(thrown, true, 0);
    co_await ctx.ticks(1);
    pk::throwPackage(sp, 20); // fully charged: speed 2
    co_await ctx.ticks(1);
    if (!sp.isCreative()) ctx.check(countPlayer(sp, items::idOf(thrown)) == 0, "the thrown package left the inventory");
    auto found = packageEntities(pos, 6);
    ctx.require(found.size() == 1, "the thrown package is an entity");
    auto id = found.front()->getOrCreateUniqueID().rawID;
    auto v  = found.front()->getPosDelta();
    ctx.check(v.x > 0.8f, "it keeps the throw's motion (vx " + std::to_string(v.x) + ")");
    co_await ctx.ticks(10);
    auto kept = pk::packageEntityBox(id);
    ctx.check(kept.has_value(), "the flying package still carries its box");
    auto found2 = packageEntities(pos, 12);
    ctx.check(!found2.empty() && found2.front()->getFeetPos().x > pos.x + 3.f, "it flew east");
    clearAround(pos, 16);
    clearInventory(sp);
});

LV_REGISTER_SELFTEST("packages.entity_pick_up", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4620);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{pos.x + 0.5f, pos.y + 0.f, pos.z + 1.8f}, DimensionType(pos.dim));
    clearInventory(sp);
    co_await ctx.ticks(2);
    auto box = pk::containing({stack("minecraft:bread", 4)});
    pk::setAddress(box, "home");
    auto id = pk::spawnPackageEntity(pos.dim, {pos.x + .5, double(pos.y), pos.z + .5}, box);
    ctx.require(id != 0, "package entity spawned");
    co_await ctx.ticks(2);
    auto found = packageEntities(pos);
    ctx.require(found.size() == 1, "the entity is there");
    if (!sp.simulateInteract(*found.front())) {
        // the simulated player's interact did not reach GameMode::interact: dispatch it as that hook does
        ctx.info("simulateInteract found no interaction; dispatching the right-click");
        std::string itemId, entityId = found.front()->getTypeName();
        auto const& item = sp.getSelectedItem();
        auto        p    = found.front()->getFeetPos();
        ia::EntityContext ec{sp, item, itemId, *found.front(), entityId, {p.x, p.y, p.z}, false};
        ctx.check(ia::dispatchInteract(ec) == ia::Result::Consume, "the right-click is taken");
    }
    co_await ctx.ticks(2);
    auto const& held = sp.getSelectedItem();
    ctx.check(pk::isPackage(held) && pk::addressOf(held) == "home" && items::idOf(pk::contentsOf(held)[0]) == "minecraft:bread",
              "an empty hand takes the package with its contents and address");
    ctx.check(packageEntities(pos).empty(), "the entity is gone");
    ctx.check(!pk::packageEntityBox(id), "its stored box is gone");
    clearInventory(sp);
});

LV_REGISTER_SELFTEST("packages.entity_break", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4630);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{pos.x + 0.5f, pos.y + 0.f, pos.z + 1.8f}, DimensionType(pos.dim));
    clearInventory(sp);
    co_await ctx.ticks(2);
    auto box = pk::containing({stack("minecraft:emerald", 6), stack("minecraft:coal", 2)});
    auto id  = pk::spawnPackageEntity(pos.dim, {pos.x + .5, double(pos.y), pos.z + .5}, box);
    ctx.require(id != 0, "package entity spawned");
    co_await ctx.ticks(2);
    auto found = packageEntities(pos);
    ctx.require(found.size() == 1, "the entity is there");
    sp.simulateAttack(found.front());
    co_await ctx.ticks(3);
    ctx.check(packageEntities(pos).empty(), "a hit breaks the package");
    ctx.check(droppedItems(pos, "minecraft:emerald") == 6 && droppedItems(pos, "minecraft:coal") == 2, "its contents drop");
    ctx.check(droppedItems(pos, items::idOf(box).c_str()) == 0, "the package item itself does not drop");
    clearAround(pos);
});

LV_REGISTER_SELFTEST("packages.entity_from_dropped_item", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4640);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos);
    auto box = pk::containing({stack("minecraft:apple", 2)});
    pk::dropItem(pos.dim, {pos.x + .5, pos.y + .5, pos.z + .5}, box, lv::core::packages::V3{0.1, 0.2, 0});
    co_await ctx.ticks(3);
    ctx.check(droppedItems(pos, items::idOf(box).c_str()) == 0, "no package item entity stays");
    auto found = packageEntities(pos);
    ctx.require(found.size() == 1, "a dropped package becomes a package entity");
    auto kept = pk::packageEntityBox(found.front()->getOrCreateUniqueID().rawID);
    ctx.check(kept && items::idOf(pk::contentsOf(*kept)[0]) == "minecraft:apple", "with its contents");
    clearAround(pos);
});

LV_REGISTER_SELFTEST("packages.entity_water", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4650);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos);
    ctx.require(ctx.setBlock(pos, "minecraft:water"), "water placed");
    co_await ctx.ticks(1);
    auto id = pk::spawnPackageEntity(pos.dim, {pos.x + .5, pos.y + .1, pos.z + .5}, pk::containing({stack("minecraft:stick", 3)}));
    ctx.require(id != 0, "package entity spawned");
    co_await ctx.ticks(4);
    ctx.check(packageEntities(pos).empty(), "water breaks the package open");
    ctx.check(droppedItems(pos, "minecraft:stick") == 3, "its contents drop");
    clearAround(pos);
});

LV_REGISTER_SELFTEST("packages.entity_depot", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4660);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos);
    ctx.require(ctx.setBlock(pos, lv::feature::belts::kDepotId), "depot placed");
    co_await ctx.ticks(2);
    auto box = pk::containing({stack("minecraft:iron_ingot", 1)});
    auto id  = pk::spawnPackageEntity(pos.dim, {pos.x + .4, pos.y + 1.2, pos.z + .6}, box);
    ctx.require(id != 0, "package entity spawned");
    bool in = false;
    for (int i = 0; i < 60 && !in; ++i) {
        co_await ctx.ticks(1);
        auto* region = pk::regionOf(pos.dim);
        in           = region && pk::isPackage(lv::feature::common::items::extract(*region, lv::game::blockPos(pos), 1, lv::feature::common::items::any(), 1, true));
    }
    ctx.check(in, "a package landing on a depot is put on it");
    ctx.check(packageEntities(pos).empty(), "the entity is gone");
    clearAround(pos);
});

LV_REGISTER_SELFTEST("packages.entity_kill", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 4670);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    clearAround(pos);
    auto id = pk::spawnPackageEntity(pos.dim, {pos.x + .5, double(pos.y), pos.z + .5}, pk::containing({stack("minecraft:flint", 4)}));
    ctx.require(id != 0, "package entity spawned");
    co_await ctx.ticks(2);
    auto found = packageEntities(pos);
    ctx.require(found.size() == 1, "the entity is there");
    ctx.require(found.front()->hasCategory(ActorCategory::Mob), "the package entity is a mob (dies through MobDieEvent)");
    found.front()->kill(); // /kill (Create: BYPASSES_INVULNERABILITY removes it, contents and all)
    co_await ctx.ticks(3);
    ctx.check(packageEntities(pos).empty(), "the entity is gone");
    ctx.check(droppedItems(pos, "minecraft:flint") == 0, "nothing drops");
    ctx.check(!pk::packageEntityBox(id), "its stored box is gone");
    clearAround(pos);
});
