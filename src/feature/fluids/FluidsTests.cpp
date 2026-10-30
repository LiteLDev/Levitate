// Selftests of the fluids unit (U19, testPos dx 1900..1999; U40's additions 4040..4049).
//
// `levitate:fluids_test_depot` stands in for Create's depot (U11) in these tests: one slot from
// the top through the item capability, and a mounted 8000 mB tank on contraptions.

#include <cmath>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/Actor.h"
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
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/fluids/FluidsInternal.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pipes/FluidFX.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

namespace lv::feature::fluids {

namespace {

using selftest::Context;
namespace items = common::items;
namespace cfl   = common::fluids;
namespace bd    = common::blockdata;
namespace ctr   = feature::contraption;

constexpr char kTestKey[] = "fluids_test";

/// testPos puts dx 40xx 155 blocks south, outside the run's ticking area (z +-64): the U40 tests
/// move 201 blocks north, to rows no unit uses (z -46..-42), so their entities keep ticking.
constexpr int kU40Shift = -201;

// ---- the test depot ------------------------------------------------------------------------------
struct TestDepotRegistrar {
    TestDepotRegistrar() {
        items::Handler h;
        h.slots   = [](common::Access const&) { return 1; };
        h.getSlot = [](common::Access const& at, int) -> ItemStack {
            auto const* d = bd::get(game::globalPos(at.region, at.pos));
            return d && d->contains(kTestKey) ? items::fromJson((*d)[kTestKey]) : items::empty();
        };
        h.insert = [](common::Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
            auto        p = game::globalPos(at.region, at.pos);
            auto const* d = bd::get(p);
            if (d && d->contains(kTestKey) && !(*d)[kTestKey].is_null()) return stack;
            if (!simulate) bd::edit(p)[kTestKey] = items::toJson(stack);
            return items::empty();
        };
        h.extract = [](common::Access const& at, items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
            auto        p = game::globalPos(at.region, at.pos);
            auto const* d = bd::get(p);
            if (!d || !d->contains(kTestKey)) return items::empty();
            auto stack = items::fromJson((*d)[kTestKey]);
            if (items::isEmpty(stack) || (filter && !filter(stack))) return items::empty();
            int n = std::min(amount, items::countOf(stack));
            if (!simulate) {
                if (n >= items::countOf(stack)) bd::edit(p).erase(kTestKey);
                else bd::edit(p)[kTestKey] = items::toJson(items::withCount(stack, items::countOf(stack) - n));
            }
            return items::withCount(stack, n);
        };
        items::registerBlock(kTestDepot, h);
        // mounted on a contraption: an 8000 mB tank carried in our data carrier ("fluids")
        ctr::registerFluidStorage(
            kTestDepot,
            {[](Json const& carried) -> std::optional<ctr::FluidTank> {
                 auto t = carried.value("fluids", Json::object()).value(kDataKey, Json::object()).value("tank", Json::object());
                 return ctr::FluidTank{t.value("fluid", ""), t.value("amount", 0), 8000};
             },
             [](Json& carried, ctr::FluidTank const& tank) {
                 auto& t = carried["fluids"][kDataKey]["tank"];
                 t       = {{"fluid", tank.fluid}, {"amount", tank.amount}};
             }}
        );
    }
};
TestDepotRegistrar const gTestDepot;

// ---- helpers -------------------------------------------------------------------------------------
GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

ItemStack item(Context& ctx, std::string const& id, int count = 1, int aux = 0) {
    auto s = items::make(id, count, aux);
    ctx.require(s.has_value(), id + " exists");
    return *s;
}

ItemStack slot0(Context& ctx, GlobalPos const& p) {
    common::Access acc{regionOf(ctx), toBlockPos(p), 1};
    auto           h = items::find(acc);
    return h && h->getSlot ? h->getSlot(acc, 0) : items::empty();
}

FluidStack tank(Context& ctx, GlobalPos const& p, int face = -1) {
    auto t = cfl::tanks(regionOf(ctx), toBlockPos(p), face);
    return t.empty() ? FluidStack{} : t.front().contents;
}

std::string idAt(Context& ctx, GlobalPos const& p) { return game::blockIdAt(regionOf(ctx), toBlockPos(p)); }

int countItemsNear(Context& ctx, GlobalPos const& p, std::string const& id, bool remove) {
    AABB box{
        Vec3{float(p.x) - 1.5f, float(p.y) - 1.5f, float(p.z) - 1.5f},
        Vec3{float(p.x) + 2.5f, float(p.y) + 2.5f, float(p.z) + 2.5f}
    };
    int n = 0;
    for (auto* a : regionOf(ctx).getEntities(box, 0.0f)) {
        if (!a || a->getEntityTypeId() != ActorType::ItemEntity) continue;
        auto& ia = static_cast<ItemActor&>(*a);
        if (items::idOf(ia.item()) != id) continue;
        n += items::countOf(ia.item());
        if (remove) a->remove();
    }
    return n;
}

bool useOn(SimulatedPlayer& sp, GlobalPos const& p) {
    return sp.simulateUseItemInSlotOnBlock(0, toBlockPos(p), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.0f, 0.5f});
}

/// An empty bucket used while looking at `p` (Bedrock uses it "in the air").
ll::coro::CoroTask<> useEmptyBucket(Context& ctx, SimulatedPlayer& sp, GlobalPos p) {
    sp.teleport(Vec3{p.x + .5f, float(p.y + 1), p.z - 1.5f}, DimensionType(p.dim));
    co_await ctx.ticks(2);
    sp.simulateLookAt(Vec3{p.x + .5f, p.y + .4f, p.z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(4);
    useOn(sp, p);
    sp.simulateUseItemInSlot(0);
    co_await ctx.ticks(2);
}

int countBlocks(Context& ctx, GlobalPos const& from, GlobalPos const& to, std::string const& id) {
    int n = 0;
    for (int x = from.x; x <= to.x; ++x)
        for (int y = from.y; y <= to.y; ++y)
            for (int z = from.z; z <= to.z; ++z)
                if (idAt(ctx, {from.dim, x, y, z}) == id) ++n;
    return n;
}

} // namespace

// ---- registry ----------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("fluids.registry", [](Context& ctx) {
    for (auto const* id : {kSpout, kItemDrain, kHosePulley, kPfi, kHoneyBlock, kChocolateBlock, kTestDepot})
        ctx.check(game::blockWith(id) != nullptr, std::string(id) + " is registered");
    ctx.check(items::make(kHoneyBucket).has_value() && items::make(kChocolateBucket).has_value(), "buckets are registered");

    auto water = emptyItem(item(ctx, "minecraft:water_bucket"));
    ctx.check(water && water->fluid == FluidStack{"minecraft:water", 1000} && items::idOf(water->container) == "minecraft:bucket",
              "water bucket -> 1000 mB water + bucket");
    auto harm = emptyItem(item(ctx, "minecraft:splash_potion", 1, 23));
    ctx.check(harm && harm->fluid == FluidStack{"levitate:potion/splash/harming", 250}
                  && items::idOf(harm->container) == "minecraft:glass_bottle",
              "splash potion of harming -> 250 mB potion fluid");
    auto honey = emptyItem(item(ctx, kHoneyBucket));
    ctx.check(honey && honey->fluid == FluidStack{"levitate:honey", 1000}, "honey bucket -> 1000 mB honey");
    auto bottle = emptyItem(item(ctx, "minecraft:honey_bottle"));
    ctx.check(bottle && bottle->fluid == FluidStack{"levitate:honey", 250}, "honey bottle -> 250 mB honey (emptying recipe)");
    ctx.check(!emptyItem(item(ctx, "minecraft:powder_snow_bucket")), "powder snow is no fluid for the drain (as Create)");

    auto glass = item(ctx, "minecraft:glass_bottle");
    ctx.check(requiredAmount(glass, {"minecraft:water", 1000}, false) == 250, "glass bottle takes 250 mB water");
    ctx.check(requiredAmount(glass, {"levitate:honey", 1000}, false) == -1, "a glass bottle takes honey only by recipe");
    ctx.check(requiredAmount(glass, {"levitate:honey", 1000}, true) == 250, "the spout fills honey bottles (filling recipe)");
    auto filled = fillItem(glass, {"levitate:potion/lingering/swiftness", 250}, false);
    ctx.check(items::idOf(filled) == "minecraft:lingering_potion" && filled.getAuxValue() == 14, "potion fluid keeps its type and bottle");
    auto bucket = item(ctx, "minecraft:bucket");
    ctx.check(items::idOf(fillItem(bucket, {"levitate:chocolate", 1000}, false)) == kChocolateBucket, "bucket + chocolate -> chocolate bucket");
    ctx.check(requiredAmount(bucket, {"levitate:tea", 1000}, false) == -1, "builder's tea has no bucket");
    ctx.check(requiredAmount(item(ctx, "minecraft:dirt"), {"minecraft:water", 1000}, true) == 500, "dirt needs 500 mB water (filling recipe)");
    ctx.check(requiredAmount(item(ctx, "minecraft:bread"), {"minecraft:milk", 1000}, true) == 250, "bread + milk (sweet roll recipe)");
    ctx.check(common::fluidtypes::displayName("levitate:honey", "en_US") == "Honey", "fluid name");
});

// ---- spout -------------------------------------------------------------------------------------

namespace {
/// depot at p, air above, spout two blocks above the depot
ll::coro::CoroTask<bool> spoutRig(Context& ctx, GlobalPos p) {
    if (!co_await ctx.ensureLoaded(p)) co_return false;
    ctx.setBlock(p, kTestDepot);
    ctx.setBlock(at(p, 0, 1, 0), "minecraft:air");
    ctx.setBlock(at(p, 0, 2, 0), kSpout);
    co_await ctx.ticks(2);
    co_return true;
}
} // namespace

LV_REGISTER_SELFTEST("fluids.spout_fills_bottles", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1905), 0, 1, 0);
    ctx.require(co_await spoutRig(ctx, p), "test area loaded");
    auto& region = regionOf(ctx);
    auto  spout  = at(p, 0, 2, 0);
    ctx.require(cfl::fill(region, toBlockPos(spout), 1, {"minecraft:water", 1000}, false) == 1000, "spout takes 1000 mB from the top");
    ctx.check(cfl::fill(region, toBlockPos(spout), 1, {"minecraft:lava", 100}, false) == 0, "no second fluid");
    auto rest = items::insert(region, toBlockPos(p), 1, item(ctx, "minecraft:glass_bottle"), false);
    ctx.require(items::isEmpty(rest), "bottle on the depot");
    co_await ctx.ticks(10);
    ctx.check(items::idOf(slot0(ctx, p)) == "minecraft:glass_bottle", "still filling after 10 ticks");
    co_await ctx.ticks(12);
    auto out = slot0(ctx, p);
    ctx.check(items::idOf(out) == "minecraft:potion" && out.getAuxValue() == 0, "a water bottle after 20 ticks, got " + items::idOf(out));
    ctx.check(tank(ctx, spout).amount == 750, "250 mB used");
});

LV_REGISTER_SELFTEST("fluids.spout_recipes_and_waiting", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1910), 0, 1, 0);
    ctx.require(co_await spoutRig(ctx, p), "test area loaded");
    auto& region = regionOf(ctx);
    auto  spout  = at(p, 0, 2, 0);
    // a bucket needs 1000 mB: the spout waits with 500
    (void)cfl::fill(region, toBlockPos(spout), 1, {"minecraft:water", 500}, false);
    (void)items::insert(region, toBlockPos(p), 1, item(ctx, "minecraft:bucket"), false);
    co_await ctx.ticks(25);
    ctx.check(items::idOf(slot0(ctx, p)) == "minecraft:bucket", "waits for enough fluid");
    (void)cfl::fill(region, toBlockPos(spout), 1, {"minecraft:water", 500}, false);
    co_await ctx.ticks(25);
    ctx.check(items::idOf(slot0(ctx, p)) == "minecraft:water_bucket", "fills the bucket once it has 1000 mB");
    ctx.check(tank(ctx, spout).empty(), "tank empty");
    (void)items::extract(region, toBlockPos(p), 1, items::any(), 64, false);
    // filling recipe: dirt + 500 mB water -> grass block
    (void)cfl::fill(region, toBlockPos(spout), 1, {"minecraft:water", 1000}, false);
    (void)items::insert(region, toBlockPos(p), 1, item(ctx, "minecraft:dirt"), false);
    co_await ctx.ticks(25);
    ctx.check(items::idOf(slot0(ctx, p)) == "minecraft:grass_block", "dirt became a grass block");
    ctx.check(tank(ctx, spout).amount == 500, "500 mB used");
    // blocked processing position: nothing happens
    (void)items::extract(region, toBlockPos(p), 1, items::any(), 64, false);
    (void)items::insert(region, toBlockPos(p), 1, item(ctx, "minecraft:glass_bottle"), false);
    ctx.setBlock(at(p, 0, 1, 0), "minecraft:stone");
    co_await ctx.ticks(25);
    ctx.check(items::idOf(slot0(ctx, p)) == "minecraft:glass_bottle", "a solid block between spout and depot blocks it");
});

// ---- spout over depots and belts (Create's BeltProcessingBehaviour, U11's processors) ---------------

namespace {
/// Takes every stack off a depot / belt segment through its item capability; counts `id`.
int takeAll(Context& ctx, GlobalPos const& p, std::string const& id) {
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        auto got = items::extract(regionOf(ctx), toBlockPos(p), 1, items::any(), 64, false);
        if (items::isEmpty(got)) break;
        if (items::idOf(got) == id) n += items::countOf(got);
    }
    return n;
}
int countOnTransporter(GlobalPos const& p, std::string const& id) {
    int n = 0;
    for (auto const& t : belts::itemsAt(p))
        if (items::idOf(t.stack) == id) n += items::countOf(t.stack);
    return n;
}
} // namespace

LV_REGISTER_SELFTEST("fluids.spout_depot", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(4040, kU40Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, belts::kDepotId);
    ctx.setBlock(at(p, 0, 1, 0), "minecraft:air");
    ctx.setBlock(at(p, 0, 2, 0), kSpout);
    co_await ctx.ticks(2);
    auto& region = regionOf(ctx);
    auto  spout  = at(p, 0, 2, 0);
    ctx.require(cfl::fill(region, toBlockPos(spout), 1, {"minecraft:water", 1000}, false) == 1000, "spout filled");
    auto fluid0 = pipes::fx::stats().fluid;
    ctx.require(items::isEmpty(items::insert(region, toBlockPos(p), 1, item(ctx, "minecraft:glass_bottle", 2), false)), "two bottles on the depot");
    co_await ctx.ticks(12);
    ctx.check(countOnTransporter(p, "minecraft:glass_bottle") == 2 && tank(ctx, spout).amount == 1000, "the depot holds them while the spout fills");
    bool done = false;
    for (int i = 0; i < 120 && !done; ++i) {
        co_await ctx.ticks(1);
        done = tank(ctx, spout).amount == 500;
    }
    ctx.check(done, "two bottles filled one after the other (" + std::to_string(tank(ctx, spout).amount) + " mB left)");
    ctx.check(pipes::fx::stats().fluid >= fluid0 + 40, "stream drops and splashes for both");
    co_await ctx.ticks(25);
    ctx.check(tank(ctx, spout).amount == 500, "nothing more to fill");
    ctx.check(takeAll(ctx, p, "minecraft:potion") == 2, "the depot holds two water bottles");
});

LV_REGISTER_SELFTEST("fluids.drain_direct_input", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(4042, kU40Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kItemDrain);
    co_await ctx.ticks(2);
    ctx.require(belts::hasDirectInput(p), "the item drain is a direct belt input (belt ends, saws, chutes)");
    belts::TransportedItem t;
    t.stack = item(ctx, "minecraft:water_bucket", 2);
    auto rest = belts::insertDirect(p, t, 5, false); // travelling east: enters from the west
    ctx.check(items::countOf(rest) == 1, "one bucket at a time (it holds fluid)");
    ctx.check(items::idOf(slot0(ctx, p)) == "minecraft:water_bucket", "the drain holds the bucket");
    bool emptied = false;
    for (int i = 0; i < 60 && !emptied; ++i) {
        co_await ctx.ticks(1);
        emptied = tank(ctx, p).amount == 1000;
    }
    ctx.check(emptied, "the bucket was emptied into the drain");
    (void)items::extract(regionOf(ctx), toBlockPos(p), 1, items::any(), 64, false);
    (void)countItemsNear(ctx, p, "minecraft:bucket", true); // an ejected bucket
});

LV_REGISTER_SELFTEST("fluids.spout_belt",[](Context& ctx) -> ll::coro::CoroTask<> {
    GlobalPos base = at(ctx.testPos(4044, kU40Shift), 0, 1, 0);
    auto      bx   = [&](int dx, int dy = 0, int dz = 1) { return at(base, dx, dy, dz); };
    ctx.require(co_await ctx.ensureLoaded(bx(5)), "test area loaded");
    // a 4 block belt along +x (shafts on axis z, driven from the north), a depot at its end
    ctx.setBlock(bx(0), "levitate:kinetics_test_shaft", {{"levitate:axis", 2}});
    ctx.setBlock(bx(3), "levitate:kinetics_test_shaft", {{"levitate:axis", 2}});
    for (int i = 1; i < 3; ++i) ctx.cleanupBlock(bx(i));
    ctx.setBlock(bx(0, 0, 0), "levitate:kinetics_test_source", {{"levitate:axis", 2}});
    co_await ctx.ticks(2);
    auto& sp = ctx.spawnPlayer("fluids_belt");
    sp.teleport(Vec3{base.x + 1.5f, float(base.y + 1), base.z - 1.5f}, DimensionType(base.dim));
    co_await ctx.ticks(2);
    auto connector = item(ctx, belts::kBeltConnectorId, 4);
    sp.setSelectedItem(connector);
    for (auto const& shaft : {bx(0), bx(3)}) {
        sp.teleport(Vec3{shaft.x + .5f, float(shaft.y + 1), shaft.z - 1.5f}, DimensionType(base.dim));
        co_await ctx.ticks(5);
        auto use = sp.getSelectedItem();
        sp.simulateUseItemOnBlock(use, toBlockPos(shaft), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 0.9f, 0.5f});
        co_await ctx.ticks(5);
    }
    sp.setSelectedItem(ItemStack());
    ctx.require(belts::beltAt(bx(2)).has_value(), "belt connected");
    lv::kinetics::setGeneratedSpeed(bx(0, 0, 0), -96);
    ctx.setBlock(bx(4), belts::kDepotId);
    ctx.setBlock(bx(2, 1), "minecraft:air");
    auto spout = bx(2, 2);
    ctx.setBlock(spout, kSpout);
    co_await ctx.ticks(3);
    auto& region = regionOf(ctx);
    ctx.require(cfl::fill(region, toBlockPos(spout), 1, {"minecraft:water", 1000}, false) == 1000, "spout filled");
    ctx.require(items::isEmpty(items::insert(region, toBlockPos(bx(0)), 1, item(ctx, "minecraft:glass_bottle", 3), false)), "three bottles on the belt");
    bool held = false;
    for (int i = 0; i < 60 && !held; ++i) {
        co_await ctx.ticks(1);
        for (auto const& t : belts::itemsAt(bx(2))) held |= t.locked;
    }
    ctx.check(held, "the moving belt stops the bottles under the spout");
    bool done = false;
    for (int i = 0; i < 200 && !done; ++i) {
        co_await ctx.ticks(1);
        done = tank(ctx, spout).amount == 250;
    }
    ctx.check(done, "three bottles filled (" + std::to_string(tank(ctx, spout).amount) + " mB left)");
    co_await ctx.ticks(40);
    int potions = 0, bottles = 0;
    for (int i = 0; i <= 4; ++i) {
        potions += countOnTransporter(bx(i), "minecraft:potion");
        bottles += countOnTransporter(bx(i), "minecraft:glass_bottle");
    }
    ctx.check(potions == 3 && bottles == 0, "three water bottles moved on (" + std::to_string(potions) + " water, " + std::to_string(bottles) + " empty)");
    lv::kinetics::setGeneratedSpeed(bx(0, 0, 0), 0);
    for (int i = 0; i <= 4; ++i) (void)takeAll(ctx, bx(i), "");
});

LV_REGISTER_SELFTEST("fluids.spout_fills_cauldron", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1915), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "minecraft:cauldron");
    ctx.setBlock(at(p, 0, 2, 0), kSpout);
    co_await ctx.ticks(2);
    auto& region = regionOf(ctx);
    (void)cfl::fill(region, toBlockPos(at(p, 0, 2, 0)), 1, {"minecraft:lava", 1000}, false);
    co_await ctx.ticks(25);
    ctx.check(tank(ctx, p) == FluidStack{"minecraft:lava", 1000}, "the cauldron is full of lava, got " + idAt(ctx, p));
    ctx.check(tank(ctx, at(p, 0, 2, 0)).empty(), "spout emptied");
});

// ---- item drain --------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("fluids.drain_empties_and_ejects", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1920), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kItemDrain);
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(2);
    auto& region = regionOf(ctx);
    auto  rest   = items::insert(region, toBlockPos(p), 4, item(ctx, "minecraft:water_bucket"), false);
    ctx.require(items::isEmpty(rest), "the drain takes the bucket from the west");
    ctx.check(!items::isEmpty(items::insert(region, toBlockPos(p), 4, item(ctx, "minecraft:stone"), true)), "one item at a time");
    co_await ctx.ticks(45);
    ctx.check(tank(ctx, p) == FluidStack{"minecraft:water", 1000}, "1000 mB water in the drain");
    ctx.check(items::isEmpty(slot0(ctx, p)), "the bucket left the drain");
    // ejected east as an item entity (the test player standing next to it may have picked it up)
    int  ejected = countItemsNear(ctx, at(p, 2, 0, 0), "minecraft:bucket", true);
    auto& inv    = ctx.player().getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i)
        if (items::idOf(inv.getItem(i)) == "minecraft:bucket") ++ejected;
    ctx.check(ejected >= 1, "the empty bucket was ejected east");
    ctx.check(cfl::fill(region, toBlockPos(p), 1, {"minecraft:water", 100}, true) == 0, "pipes can't fill the drain");
    auto drained = cfl::drain(region, toBlockPos(p), 0, {"", 400}, false);
    ctx.check(drained == FluidStack{"minecraft:water", 400}, "pipes drain it");
});

LV_REGISTER_SELFTEST("fluids.drain_passes_items_on", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1925), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kItemDrain);
    ctx.setBlock(at(p, 1, 0, 0), kTestDepot);
    co_await ctx.ticks(2);
    auto& region = regionOf(ctx);
    (void)items::insert(region, toBlockPos(p), 4, item(ctx, "minecraft:potion", 1, 5), false);
    co_await ctx.ticks(45);
    ctx.check(tank(ctx, p) == FluidStack{"levitate:potion/regular/night_vision", 250}, "potion fluid in the drain");
    ctx.check(items::idOf(slot0(ctx, at(p, 1, 0, 0))) == "minecraft:glass_bottle", "the bottle moved on into the depot");
    // items that hold no fluid just pass through
    (void)items::insert(region, toBlockPos(p), 4, item(ctx, "minecraft:stone", 3), false);
    co_await ctx.ticks(12);
    ctx.check(items::idOf(slot0(ctx, p)) == "minecraft:stone", "stone waits at the far edge (the depot is full)");
});

LV_REGISTER_SELFTEST("fluids.drain_player_exchange", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1930), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kItemDrain);
    co_await ctx.ticks(2);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.give(sp, item(ctx, "minecraft:lava_bucket"), 0);
    bool used = useOn(sp, p);
    co_await ctx.ticks(2);
    ctx.check(used, "the player can use the bucket on the drain");
    ctx.check(tank(ctx, p) == FluidStack{"minecraft:lava", 1000}, "the bucket was emptied into the drain");
    ctx.check(items::idOf(sp.getSelectedItem()) == "minecraft:bucket", "the player keeps an empty bucket");
    co_await ctx.ticks(6);
    co_await useEmptyBucket(ctx, sp, p);
    ctx.check(tank(ctx, p).empty(), "the bucket took the lava back");
    bool hasLava = false;
    auto& inv    = sp.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) hasLava |= items::idOf(inv.getItem(i)) == "minecraft:lava_bucket";
    ctx.check(hasLava, "the player got a lava bucket");
});

// ---- hose pulley -------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("fluids.hose_pulley", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(1937);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    // a 3x3 pool of water on a stone floor, walled with glass
    for (int dx = -2; dx <= 2; ++dx)
        for (int dz = -2; dz <= 2; ++dz) {
            ctx.setBlock(at(base, dx, 0, dz), "minecraft:stone");
            bool inner = std::abs(dx) <= 1 && std::abs(dz) <= 1;
            ctx.setBlock(at(base, dx, 1, dz), inner ? "minecraft:water" : "minecraft:glass");
        }
    auto hose = at(base, 0, 6, 0);
    auto src  = at(base, -1, 6, 0);
    ctx.setBlock(hose, kHosePulley, {{kFacing, 2}});
    ctx.setBlock(src, "levitate:kinetics_test_source", {{"levitate:axis", 0}});
    co_await ctx.ticks(3);
    float lower = 256;
    kinetics::setGeneratedSpeed(src, lower);
    co_await ctx.ticks(4);
    if (hoseOffset(hose) <= 0) {
        lower = -256; // the other direction lowers it
        kinetics::setGeneratedSpeed(src, lower);
        co_await ctx.ticks(4);
    }
    ctx.require(hoseOffset(hose) > 0, "the hose goes down");
    co_await ctx.ticks(20);
    ctx.check(std::abs(hoseOffset(hose) - 5.f) < 1e-3, "the hose stops on the floor (offset 5), got " + std::to_string(hoseOffset(hose)));
    auto& region = regionOf(ctx);
    int   back   = 3; // facing north: pipes on the south side
    ctx.check(cfl::drain(region, toBlockPos(hose), 2, {"", 1000}, true).empty(), "no pipe on the front");
    auto got = cfl::drain(region, toBlockPos(hose), back, {"", 1000}, false);
    ctx.check(got == FluidStack{"minecraft:water", 1000}, "drained 1000 mB water");
    ctx.check(countBlocks(ctx, at(base, -1, 1, -1), at(base, 1, 1, 1), "minecraft:water") == 8, "one source removed");
    int filled = cfl::fill(region, toBlockPos(hose), back, {"minecraft:water", 1000}, false);
    ctx.check(filled == 1000, "filled 1000 mB back");
    ctx.check(countBlocks(ctx, at(base, -1, 1, -1), at(base, 1, 1, 1), "minecraft:water") == 9, "one source placed");
    // a small threshold makes the pool infinite: water is drained without removing blocks
    auto& cfg     = lv::config::mut<FluidsConfig>();
    int   saved   = cfg.hosePulleyBlockThreshold;
    cfg.hosePulleyBlockThreshold = 4;
    (void)cfl::fill(region, toBlockPos(hose), back, {"minecraft:lava", 1}, true); // resets nothing, different fluid
    auto inf = cfl::drain(region, toBlockPos(hose), back, {"", 1000}, false);
    cfg.hosePulleyBlockThreshold = saved;
    ctx.check(inf == FluidStack{"minecraft:water", 1000}, "drained from the infinite pool");
    ctx.check(countBlocks(ctx, at(base, -1, 1, -1), at(base, 1, 1, 1), "minecraft:water") >= 8, "an infinite pool keeps its blocks");
    // reverse: the hose comes back up
    kinetics::setGeneratedSpeed(src, -lower);
    co_await ctx.ticks(20);
    ctx.check(hoseOffset(hose) == 0, "the hose is wound up again");
});

LV_REGISTER_SELFTEST("fluids.hose_pulley_placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1944), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "minecraft:stone");
    ctx.cleanupBlock(at(p, 0, 1, 0));
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{p.x - 2.5f, float(p.y + 1), p.z + .5f}, DimensionType(p.dim));
    sp.simulateLookAt(Vec3{p.x + 5.f, float(p.y + 1), p.z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(5);
    ctx.give(sp, item(ctx, kHosePulley), 0);
    bool used = useOn(sp, p);
    co_await ctx.ticks(3);
    ctx.check(used && idAt(ctx, at(p, 0, 1, 0)) == kHosePulley, "placed a hose pulley");
    ctx.check(intStateAt(at(p, 0, 1, 0), kFacing) == 4, "it faces the player (west), got " + std::to_string(intStateAt(at(p, 0, 1, 0), kFacing)));
});

// ---- honey and chocolate -----------------------------------------------------------------------

LV_REGISTER_SELFTEST("fluids.honey_bucket", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1946), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "minecraft:stone");
    ctx.cleanupBlock(at(p, 0, 1, 0));
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.give(sp, item(ctx, kHoneyBucket), 0);
    useOn(sp, p);
    co_await ctx.ticks(2);
    ctx.check(idAt(ctx, at(p, 0, 1, 0)) == kHoneyBlock, "a honey source was placed");
    ctx.check(items::idOf(sp.getSelectedItem()) == "minecraft:bucket", "the bucket is empty");
    co_await ctx.ticks(6);
    co_await useEmptyBucket(ctx, sp, at(p, 0, 1, 0));
    ctx.check(idAt(ctx, at(p, 0, 1, 0)) == "minecraft:air", "picked up again");
    ctx.check(items::idOf(sp.getSelectedItem()) == kHoneyBucket, "a honey bucket again");
});

LV_REGISTER_SELFTEST("fluids.chocolate_with_lava", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1950), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(at(p, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(at(p, 1, -1, 0), "minecraft:stone");
    ctx.setBlock(p, kChocolateBlock);
    co_await ctx.ticks(2);
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:lava");
    co_await ctx.ticks(15);
    ctx.check(idAt(ctx, at(p, 1, 0, 0)) == "minecraft:obsidian", "a lava source next to chocolate turns into obsidian");
    ctx.check(idAt(ctx, p) == kChocolateBlock, "the chocolate stays");
    // a player in it is slowed down
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{p.x + .5f, float(p.y), p.z + .5f}, DimensionType(p.dim));
    co_await ctx.ticks(8);
    ctx.check(sp.getEffect(2u) != nullptr, "slowed down in chocolate");
    sp.removeAllEffects();
});

// ---- portable fluid interface ------------------------------------------------------------------

LV_REGISTER_SELFTEST("fluids.portable_fluid_interface", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx.testPos(1960), 0, 3, 0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kTestDepot);
    ctx.setBlock(at(p, 1, 0, 0), kPfi, {{kFacing, 5}});
    ctx.setBlock(at(p, 4, 0, 0), kPfi, {{kFacing, 4}});
    for (int dx = 2; dx <= 3; ++dx) ctx.cleanupBlock(at(p, dx, 0, 0));
    co_await ctx.ticks(2);
    setTank(editData(p), {"minecraft:water", 4000});
    ctr::addGlue(p.dim, core::contraption::GlueBox::of({p.x, p.y, p.z}, {p.x + 1, p.y, p.z}));
    struct GlueGuard {
        GlobalPos p;
        ~GlueGuard() { ctr::clearGlue(p.dim, {p.x, p.y, p.z}, 6); }
    } glueGuard{p};
    ctr::AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    auto r          = ctr::assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    auto id = *r.id;
    struct Guard {
        ctr::Id id;
        ~Guard() {
            if (ctr::find(id)) ctr::disassemble(id);
        }
    } guard{id};
    auto* c = ctr::find(id);
    ctx.require(c != nullptr, "contraption exists");
    ctr::Motion m;
    m.velocity = {1.0 / 20, 0, 0};
    m.ticks    = 40;
    c->setMotion(m);
    co_await ctx.ticks(30);
    c = ctr::find(id);
    ctx.require(c != nullptr, "contraption still exists");
    double x = c->transform().offset.x;
    ctx.check(std::abs(x - 1.0) < 0.1, "stopped in front of the stationary interface (offset " + std::to_string(x) + ")");
    auto& region = regionOf(ctx);
    auto  got    = cfl::drain(region, toBlockPos(at(p, 4, 0, 0)), 5, {"", 500}, false);
    ctx.check(got == FluidStack{"minecraft:water", 500}, "drained 500 mB from the contraption's tank");
    ctx.check(cfl::fill(region, toBlockPos(at(p, 4, 0, 0)), 5, {"minecraft:water", 200}, false) == 200, "filled 200 mB back");
    co_await ctx.ticks(90);
    c = ctr::find(id);
    ctx.require(c != nullptr, "contraption still exists");
    x = c->transform().offset.x;
    ctx.check(x > 1.5, "moved on after the timeout (offset " + std::to_string(x) + ")");
    ctx.cleanupBlock(at(p, 2, 0, 0));
    ctx.cleanupBlock(at(p, 3, 0, 0));
    ctr::disassemble(id);
    co_await ctx.ticks(2);
    auto const* d = dataAt(at(p, 2, 0, 0));
    ctx.check(d && tankOf(*d).amount == 3700, "the depot kept its tank (3700 mB) through the move");
});

} // namespace lv::feature::fluids
