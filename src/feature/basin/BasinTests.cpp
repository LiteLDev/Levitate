// Selftests of the basin unit (U14, testPos dx 1400..1499).

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/MobSpawnerBlockActor.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::basin {

namespace {

using selftest::Context;
namespace items  = common::items;
namespace fluids = common::fluids;
namespace k      = lv::kinetics;

constexpr char const* kSource = "levitate:kinetics_test_source";
constexpr char const* kCog    = "levitate:kinetics_test_cog";

struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy;
        return p;
    }
};

BlockSource& region0() { return *regionOf(0); }

ItemStack stack(std::string const& id, int count = 1, int aux = 0) { return items::make(id, count, aux).value_or(items::empty()); }

BasinState& basinAt(Context& ctx, GlobalPos const& p) {
    auto* b = basinops::get(region0(), p);
    ctx.require(b != nullptr, "a basin at " + p.key());
    return *b;
}

int countIn(std::array<ItemStack, cb::kSlotCount> const& slots, std::string const& id) {
    int n = 0;
    for (auto const& s : slots)
        if (items::idOf(s) == id) n += items::countOf(s);
    return n;
}

int fluidIn(std::vector<cb::Fluid> const& tanks, std::string const& id) {
    int n = 0;
    for (auto const& t : tanks)
        if (t.id == id) n += t.amount;
    return n;
}

std::string spawnItem(GlobalPos const& p, float dy, ItemStack const& s) {
    auto  level = ll::service::getLevel();
    auto* e     = level->getSpawner().spawnItem(region0(), s, nullptr, Vec3{p.x + .5f, p.y + dy, p.z + .5f}, 10);
    if (!e) return "spawnItem returned null";
    auto at = e->getPosition();
    int  n  = 0;
    for (auto* a : level->getRuntimeActorList())
        if (a && a->getTypeName() == "minecraft:item") ++n;
    return "item spawned at " + std::to_string(at.x) + "," + std::to_string(at.y) + "," + std::to_string(at.z) + ", items in the level: " + std::to_string(n);
}

int itemsNear(GlobalPos const& p, std::string const& id) {
    AABB box{
        Vec3{p.x - 1.f, p.y - 1.f, p.z - 1.f},
        Vec3{p.x + 2.f, p.y + 2.f, p.z + 2.f}
    };
    int n = 0;
    for (auto* a : region0().getEntities(box, 0.f, ActorType::ItemEntity)) {
        auto& it = static_cast<ItemActor&>(*a);
        if (items::idOf(it.item()) == id) n += items::countOf(it.item());
    }
    return n;
}

void clearItems(GlobalPos const& p) {
    AABB box{
        Vec3{p.x - 2.f, p.y - 2.f, p.z - 2.f},
        Vec3{p.x + 3.f, p.y + 3.f, p.z + 3.f}
    };
    for (auto* a : region0().getEntities(box, 0.f, ActorType::ItemEntity)) a->remove();
}

bool useOn(SimulatedPlayer& sp, GlobalPos const& p, int slot, ScriptModuleMinecraft::ScriptFacing face, Vec3 hit) {
    return sp.simulateUseItemInSlotOnBlock(slot, game::blockPos(p), face, hit);
}

/// Puts `s` into hotbar slot 0 (replacing whatever is there) and selects it.
void hold(SimulatedPlayer& sp, ItemStack const& s) {
    sp.getInventory().setItem(0, s);
    sp.setSelectedSlot(0);
    sp.sendInventory(false);
}

void stand(SimulatedPlayer& sp, GlobalPos const& p) { sp.teleport(Vec3{p.x + .5f, float(p.y), p.z + .5f}, DimensionType(0)); }

/// Kinetic drive: a test source with a vertical shaft under a small cog meshing with `machine`'s
/// cog (mixer), or a source on the machine's shaft axis (press, axis x).
void driveMixer(Context& ctx, GlobalPos const& mixer, float rpm) {
    GlobalPos cog{mixer.dim, mixer.x + 1, mixer.y, mixer.z};
    GlobalPos src{mixer.dim, mixer.x + 1, mixer.y - 1, mixer.z};
    ctx.setBlock(src, kSource, {{"levitate:axis", 1}});
    ctx.setBlock(cog, kCog, {{"levitate:axis", 1}});
    k::setGeneratedSpeed(src, rpm);
}

void drivePress(Context& ctx, GlobalPos const& press, float rpm) {
    GlobalPos src{press.dim, press.x + 1, press.y, press.z};
    ctx.setBlock(src, kSource, {{"levitate:axis", 0}});
    k::setGeneratedSpeed(src, rpm);
}

} // namespace

LV_REGISTER_SELFTEST("basin.registry", [](Context& ctx) {
    for (auto const* id : {kBasin, kPress, kMixer, kBurner, kEmptyBurner, kLitBurner}) {
        ctx.check(game::blockWith(id) != nullptr, std::string(id) + " is in the behavior pack");
    }
    ctx.check(k::specOf(kPress) != nullptr && k::specOf(kMixer) != nullptr, "press and mixer are kinetic");
    ctx.check(k::stressImpactOf(kPress) == 8 && k::stressImpactOf(kMixer) == 4, "stress impacts 8 / 4");
    ctx.check(common::recipes::ofType("pressing").size() >= 8, "pressing recipes loaded");
    ctx.check(common::recipes::ofType("mixing").size() >= 11, "mixing recipes loaded");
    ctx.check(common::recipes::ofType("compacting").size() >= 7, "compacting recipes loaded");
    ctx.check(burners::fuelOf(stack("minecraft:coal")).burnTime == 1600, "coal burns 1600 ticks, got " + std::to_string(burners::fuelOf(stack("minecraft:coal")).burnTime));
    ctx.check(burners::fuelOf(stack("minecraft:stone")).fuel == cb::Fuel::None, "stone is no fuel");
    auto press = recipes::candidates(true);
    auto mixer = recipes::candidates(false);
    ctx.check(std::any_of(press.begin(), press.end(), [](auto const& r) { return r.crafting; }), "the press compacts crafting recipes (2x2 / 3x3)");
    ctx.check(std::any_of(mixer.begin(), mixer.end(), [](auto const& r) { return r.crafting; }), "the mixer runs shapeless crafting recipes");
    ctx.info("press: " + std::to_string(press.size()) + " recipes, mixer: " + std::to_string(mixer.size()));
});

LV_REGISTER_SELFTEST("basin.inventory", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1400};
    auto p = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kBasin), "place a basin");
    co_await ctx.ticks(5);
    auto& r  = region0();
    auto  bp = game::blockPos(p);
    auto rest = items::insert(r, bp, kUp, stack("minecraft:cobblestone", 60), false);
    ctx.check(items::isEmpty(rest), "60 cobblestone go in");
    rest = items::insert(r, bp, kUp, stack("minecraft:cobblestone", 10), false);
    ctx.check(items::countOf(rest) == 6, "one stack per item kind: 4 more fit, got rest " + std::to_string(items::countOf(rest)));
    ctx.check(items::isEmpty(items::insert(r, bp, kUp, stack("minecraft:dirt", 3), false)), "another item takes another slot");
    auto& b = basinAt(ctx, p);
    ctx.check(countIn(b.in, "minecraft:cobblestone") == 64 && countIn(b.in, "minecraft:dirt") == 3, "input slots hold them");
    auto got = items::extract(r, bp, kDown, items::any(), 2, false);
    ctx.check(items::idOf(got) == "minecraft:cobblestone" && items::countOf(got) == 2, "extraction takes from the first slot");
    ctx.check(fluids::fill(r, bp, kUp, {"minecraft:water", 1500}, false) == 1000, "a tank holds 1000 mB");
    ctx.check(fluids::fill(r, bp, kUp, {"minecraft:water", 100}, false) == 0, "the second tank takes another fluid only");
    ctx.check(fluids::fill(r, bp, kUp, {"minecraft:lava", 250}, false) == 250, "lava goes into the second tank");
    ctx.check(fluids::fill(r, bp, kUp, {"minecraft:milk", 250}, false) == 0, "no third fluid");
    auto drained = fluids::drain(r, bp, kUp, {"minecraft:lava", 100}, false);
    ctx.check(drained.fluid == "minecraft:lava" && drained.amount == 100, "drain by fluid");
    ctx.check(fluids::tanks(r, bp).size() == 4, "2 output + 2 input tanks");
    // persisted in BlockData: a reload of the cache reads the same contents
    basinops::forget(p);
    auto& again = basinAt(ctx, p);
    ctx.check(countIn(again.in, "minecraft:cobblestone") == 62 && fluidIn(again.tankIn, "minecraft:lava") == 150, "contents persist in BlockData");
    for (auto& s : again.in) s = items::empty();
    basinops::save(p, again);
});

LV_REGISTER_SELFTEST("basin.player_use", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1405};
    auto p = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kBasin), "place a basin");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    stand(sp, a.at(-2));
    co_await ctx.ticks(2);
    hold(sp, stack("minecraft:water_bucket"));
    sp.setSelectedSlot(0);
    useOn(sp, p, 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    auto& b = basinAt(ctx, p);
    ctx.check(fluidIn(b.tankIn, "minecraft:water") == 1000, "a water bucket fills 1000 mB");
    ctx.check(items::idOf(sp.getInventory().getItem(0)) == "minecraft:bucket" || sp.isCreative(), "the bucket is emptied");
    ctx.check(game::blockIdAt(region0(), game::blockPos(a.at(0, 1))) != "minecraft:water", "no water is placed next to the basin");
    hold(sp, stack("minecraft:bucket"));
    useOn(sp, p, 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(fluidIn(b.tankIn, "minecraft:water") == 0, "an empty bucket takes the water back");
    // empty hand: every item comes out
    basinops::insertInput(b, stack("minecraft:dirt", 5), false);
    hold(sp, items::empty());
    sp.simulateLookAt(Vec3{p.x + .5f, p.y + .9f, p.z + .5f}, sim::LookDuration::Instant);
    co_await ctx.ticks(2);
    if (!sp.simulateInteract()) {
        // the simulated player found nothing to interact with: use the block directly
        basinops::use(sp, region0(), p, items::empty(), kUp);
    }
    co_await ctx.ticks(5);
    ctx.check(countIn(b.in, "minecraft:dirt") == 0, "an empty hand takes the items");
    int dirt = 0;
    for (int i = 0; i < sp.getInventory().getContainerSize(); ++i)
        if (items::idOf(sp.getInventory().getItem(i)) == "minecraft:dirt") dirt += items::countOf(sp.getInventory().getItem(i));
    ctx.check(dirt == 5, "the player got the 5 dirt, got " + std::to_string(dirt));
});

LV_REGISTER_SELFTEST("basin.item_intake", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1410};
    auto p = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kBasin), "place a basin");
    co_await ctx.ticks(5);
    stand(static_cast<SimulatedPlayer&>(ctx.player()), a.at(-6, 0, 6)); // out of pickup range
    co_await ctx.ticks(2);
    ctx.info(spawnItem(p, 1.1f, stack("minecraft:sugar_cane", 7)));
    co_await ctx.ticks(2);
    {
        AABB        box{Vec3{p.x - 4.f, p.y - 4.f, p.z - 4.f}, Vec3{p.x + 5.f, p.y + 5.f, p.z + 5.f}};
        std::string seen;
        for (auto* e : region0().getEntities(box, 2.f)) {
            auto pos = e->getPosition();
            seen += e->getTypeName() + "@" + std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z) + " ";
        }
        ctx.info("entities near the basin after 2 ticks: " + seen + "; basin at " + p.key() + ", sugar cane item ok: " + std::to_string(!items::isEmpty(stack("minecraft:sugar_cane"))));
    }
    auto& b = basinAt(ctx, p);
    for (int i = 0; i < 12 && countIn(b.in, "minecraft:sugar_cane") < 7; ++i) co_await ctx.ticks(5);
    if (countIn(b.in, "minecraft:sugar_cane") < 7) {
        std::string seen;
        for (auto* e : region0().getEntities(AABB{Vec3{p.x - 3.f, p.y - 3.f, p.z - 3.f}, Vec3{p.x + 4.f, p.y + 4.f, p.z + 4.f}}, 2.f))
            seen += e->getTypeName() + "@" + std::to_string(e->getPosition().x) + "," + std::to_string(e->getPosition().y) + "," + std::to_string(e->getPosition().z) + " ";
        ctx.info("still around: " + seen);
    }
    ctx.check(countIn(b.in, "minecraft:sugar_cane") == 7, "items falling into the basin are collected, got " + std::to_string(countIn(b.in, "minecraft:sugar_cane")));
    clearItems(p);
});

LV_REGISTER_SELFTEST("basin.burner_fuel", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1415};
    auto p = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kBurner), "place a blaze burner");
    co_await ctx.ticks(2);
    auto& r  = region0();
    auto  bp = game::blockPos(p);
    ctx.check(common::heat::level(r, bp) == cb::HeatLevel::Smouldering, "a blaze without fuel smoulders");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    stand(sp, a.at(-2));
    co_await ctx.ticks(5);
    hold(sp, stack("minecraft:coal", 4));
    sp.setSelectedSlot(0);
    useOn(sp, p, 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    auto* d = burners::get(p);
    ctx.require(d != nullptr, "burner data");
    ctx.check(d->state.fuel == cb::Fuel::Normal && d->state.burnTime > 1500, "coal: normal fuel, ~1600 ticks");
    ctx.check(common::heat::level(r, bp) == cb::HeatLevel::Kindled, "kindled");
    ctx.check(sp.isCreative() || items::countOf(sp.getInventory().getItem(0)) == 3, "one coal used");
    // the blaze cake: seething, normal fuel refused afterwards
    auto [ok, rest] = burners::tryInsert(r, p, stack(kBlazeCake), false, true, false);
    if (items::make(kBlazeCake)) {
        ctx.check(ok && common::heat::level(r, bp) == cb::HeatLevel::Seething, "a blaze cake makes it seethe");
        ctx.check(!burners::tryInsert(r, p, stack("minecraft:coal"), false, true, true).first, "normal fuel can't replace special fuel");
    } else {
        ctx.info("levitate:blaze_cake is not registered yet (U9): cake checks skipped");
    }
    // running out: fading below 125 ticks, then smouldering
    d->state = {cb::Fuel::Normal, 130, false};
    co_await ctx.ticks(10);
    ctx.check(common::heat::level(r, bp) == cb::HeatLevel::Fading, "fading under 125 ticks");
    d->state.burnTime = 2;
    co_await ctx.ticks(4);
    ctx.check(common::heat::level(r, bp) == cb::HeatLevel::Smouldering, "smouldering when empty");
    // automation (arms, funnels) only refuels below the threshold
    ctx.check(items::countOf(items::insert(r, bp, kUp, stack("minecraft:coal", 2), false)) == 1, "the item capability takes one fuel item");
    ctx.check(items::countOf(items::insert(r, bp, kUp, stack("minecraft:coal", 1), false)) == 1, "no overflow for automation");
    // the creative cake cycles smouldering -> kindled -> seething -> smouldering
    d->state = {};
    setState(r, bp, kBlazeState, 1);
    std::vector<cb::HeatLevel> seen;
    for (int i = 0; i < 3; ++i) {
        burners::tryInsert(r, p, stack("minecraft:stone"), true, true, false); // refused, no effect
        auto lvlBefore = common::heat::level(r, bp);
        (void)lvlBefore;
        d->state.creative = false;
        if (!items::make(kCreativeCake)) break;
        burners::tryInsert(r, p, stack(kCreativeCake), true, true, false);
        seen.push_back(common::heat::level(r, bp));
    }
    if (seen.size() == 3) {
        ctx.check(seen[0] == cb::HeatLevel::Kindled && seen[1] == cb::HeatLevel::Seething && seen[2] == cb::HeatLevel::Smouldering,
                  "creative cake cycles kindled, seething, smouldering");
    } else {
        ctx.info("levitate:creative_blaze_cake is not registered yet (U9): creative checks skipped");
    }
});

LV_REGISTER_SELFTEST("basin.lit_burner", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1420};
    auto p = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kEmptyBurner), "place an empty burner");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    stand(sp, a.at(-2));
    co_await ctx.ticks(2);
    auto& r  = region0();
    auto  bp = game::blockPos(p);
    ctx.check(common::heat::level(r, bp) == cb::HeatLevel::None, "an empty burner gives no heat");
    hold(sp, stack("minecraft:flint_and_steel"));
    sp.setSelectedSlot(0);
    useOn(sp, p, 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(game::blockIdAt(r, bp) == kLitBurner, "flint and steel lights it");
    ctx.check(common::heat::level(r, bp) == cb::HeatLevel::Smouldering, "a lit burner is a passive heater");
    hold(sp, stack("minecraft:soul_soil"));
    useOn(sp, p, 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(stateAt(r, bp, kFlameState) == 1, "soul soil makes a soul flame");
    hold(sp, stack("minecraft:iron_shovel"));
    useOn(sp, p, 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(game::blockIdAt(r, bp) == kEmptyBurner, "a shovel puts it out");
});

LV_REGISTER_SELFTEST("basin.capture", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1425};
    auto p = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, "minecraft:mob_spawner"), "place a spawner");
    co_await ctx.ticks(5);
    auto& r  = region0();
    auto  bp = game::blockPos(p);
    auto* ba = r.getBlockEntity(bp);
    ctx.require(ba != nullptr, "the spawner has a block entity");
    static_cast<MobSpawnerBlockActor&>(*ba).setMob(r, ActorDefinitionIdentifier("minecraft:blaze"), nullptr);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    stand(sp, a.at(-2));
    co_await ctx.ticks(2);
    hold(sp, stack(kEmptyBurner));
    sp.setSelectedSlot(0);
    useOn(sp, p, 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    bool got = false;
    for (int i = 0; i < sp.getInventory().getContainerSize(); ++i)
        if (items::idOf(sp.getInventory().getItem(i)) == kBurner) got = true;
    ctx.check(got, "the empty burner captured a blaze from the spawner");
    ctx.check(game::blockIdAt(r, bp) == "minecraft:mob_spawner", "the spawner stays");
});

LV_REGISTER_SELFTEST("basin.mixing", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1430};
    auto basin = a.at(0);
    auto mixer = a.at(0, 2);
    ctx.require(co_await ctx.ensureLoaded(basin), "test area loaded");
    ctx.require(ctx.setBlock(basin, kBasin), "place a basin");
    ctx.require(ctx.setBlock(mixer, kMixer), "place a mixer");
    driveMixer(ctx, mixer, 64);
    co_await ctx.ticks(3);
    ctx.check(std::abs(k::speedAt(mixer)) == 64, "the mixer turns at 64 RPM via its cog, got " + std::to_string(k::speedAt(mixer)));
    auto& r = region0();
    auto& b = basinAt(ctx, basin);
    // mixing recipe: dirt + 250 mB water -> mud
    basinops::insertInput(b, stack("minecraft:dirt", 2), false);
    (void)fluids::fill(r, game::blockPos(basin), kUp, {"minecraft:water", 1000}, false);
    basinops::notifyChangeOfContents(b);
    for (int i = 0; i < 40 && countIn(b.out, "minecraft:mud") < 2; ++i) co_await ctx.ticks(5);
    ctx.check(countIn(b.out, "minecraft:mud") == 2, "2 mud mixed, got " + std::to_string(countIn(b.out, "minecraft:mud")));
    ctx.check(fluidIn(b.tankIn, "minecraft:water") == 500, "500 mB of water used");
    // automated shapeless crafting: pumpkin + sugar + egg -> pumpkin pie
    for (auto& s : b.out) s = items::empty();
    for (auto& t : b.tankIn) t = {};
    basinops::insertInput(b, stack("minecraft:pumpkin"), false);
    basinops::insertInput(b, stack("minecraft:sugar"), false);
    basinops::insertInput(b, stack("minecraft:egg"), false);
    basinops::notifyChangeOfContents(b);
    for (int i = 0; i < 40 && countIn(b.out, "minecraft:pumpkin_pie") < 1; ++i) co_await ctx.ticks(5);
    ctx.check(countIn(b.out, "minecraft:pumpkin_pie") == 1, "the mixer crafts shapeless recipes (pumpkin pie)");
    // too slow: the mixer needs medium speed
    for (auto& s : b.out) s = items::empty();
    k::setGeneratedSpeed(a.at(1, 1), 16);
    basinops::insertInput(b, stack("minecraft:dirt"), false);
    (void)fluids::fill(r, game::blockPos(basin), kUp, {"minecraft:water", 250}, false);
    basinops::notifyChangeOfContents(b);
    co_await ctx.ticks(120);
    ctx.check(countIn(b.out, "minecraft:mud") == 0, "nothing is mixed below 30 RPM");
    for (auto& s : b.in) s = items::empty();
    for (auto& t : b.tankIn) t = {};
    basinops::save(basin, b);
});

LV_REGISTER_SELFTEST("basin.heat", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1440};
    auto burner = a.at(0);
    auto basin  = a.at(0, 1);
    auto mixer  = a.at(0, 3);
    ctx.require(co_await ctx.ensureLoaded(basin), "test area loaded");
    ctx.require(ctx.setBlock(burner, "minecraft:stone"), "no heat yet");
    ctx.require(ctx.setBlock(basin, kBasin), "place a basin");
    ctx.require(ctx.setBlock(mixer, kMixer), "place a mixer");
    driveMixer(ctx, mixer, 128);
    co_await ctx.ticks(3);
    auto& r = region0();
    auto& b = basinAt(ctx, basin);
    // lava from cobblestone needs a seething burner
    basinops::insertInput(b, stack("minecraft:cobblestone", 1), false);
    basinops::notifyChangeOfContents(b);
    co_await ctx.ticks(80);
    ctx.check(fluidIn(b.tankOut, "minecraft:lava") == 0, "no lava without heat");
    ctx.setBlock(burner, kBurner, {{kBlazeState, 4}});
    co_await ctx.ticks(5);
    if (auto* d = burners::get(burner)) d->state = {cb::Fuel::None, 0, true}; // creative: stays seething
    setState(r, game::blockPos(burner), kBlazeState, 4);
    ctx.check(basinops::heatUnder(r, basin) == cb::HeatLevel::Seething, "the basin sees a seething burner");
    for (int i = 0; i < 40 && fluidIn(b.tankOut, "minecraft:lava") < 50; ++i) co_await ctx.ticks(5);
    ctx.check(fluidIn(b.tankOut, "minecraft:lava") == 50, "superheated mixing: cobblestone -> 50 mB lava, got " + std::to_string(fluidIn(b.tankOut, "minecraft:lava")));
    for (auto& t : b.tankOut) t = {};
    basinops::save(basin, b);
});

LV_REGISTER_SELFTEST("basin.compacting", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1450};
    auto basin = a.at(0);
    auto press = a.at(0, 2);
    ctx.require(co_await ctx.ensureLoaded(basin), "test area loaded");
    ctx.require(ctx.setBlock(basin, kBasin), "place a basin");
    ctx.require(ctx.setBlock(press, kPress, {{kAxisState, 0}}), "place a press");
    drivePress(ctx, press, 128);
    co_await ctx.ticks(3);
    ctx.check(k::speedAt(press) == 128, "the press turns at 128 RPM, got " + std::to_string(k::speedAt(press)));
    auto& b = basinAt(ctx, basin);
    // compacting recipe: 9 snow blocks -> ice
    basinops::insertInput(b, stack("minecraft:snow", 9), false);
    basinops::notifyChangeOfContents(b);
    for (int i = 0; i < 40 && countIn(b.out, "minecraft:ice") < 1; ++i) co_await ctx.ticks(5);
    ctx.check(countIn(b.out, "minecraft:ice") == 1, "9 snow blocks compacted into ice");
    // automated 3x3 crafting: 9 iron ingots -> iron block
    for (auto& s : b.out) s = items::empty();
    basinops::insertInput(b, stack("minecraft:iron_ingot", 18), false);
    basinops::notifyChangeOfContents(b);
    for (int i = 0; i < 60 && countIn(b.out, "minecraft:iron_block") < 2; ++i) co_await ctx.ticks(5);
    ctx.check(countIn(b.out, "minecraft:iron_block") == 2, "18 iron ingots compacted into 2 blocks, got " + std::to_string(countIn(b.out, "minecraft:iron_block")));
    for (auto& s : b.in) s = items::empty();
    for (auto& s : b.out) s = items::empty();
    basinops::save(basin, b);
});

LV_REGISTER_SELFTEST("basin.filter", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1460};
    auto basin = a.at(0);
    auto press = a.at(0, 2);
    ctx.require(co_await ctx.ensureLoaded(basin), "test area loaded");
    ctx.require(ctx.setBlock(basin, kBasin), "place a basin");
    ctx.require(ctx.setBlock(press, kPress, {{kAxisState, 0}}), "place a press");
    drivePress(ctx, press, 128);
    co_await ctx.ticks(3);
    auto& b = basinAt(ctx, basin);
    common::filterslot::set(basin, stack("minecraft:gold_block"), "filter");
    basinops::insertInput(b, stack("minecraft:iron_ingot", 9), false);
    basinops::notifyChangeOfContents(b);
    co_await ctx.ticks(80);
    ctx.check(countIn(b.out, "minecraft:iron_block") == 0, "a filter for gold blocks blocks the iron block recipe");
    common::filterslot::set(basin, stack("minecraft:iron_block"), "filter");
    basinops::notifyChangeOfContents(b);
    for (int i = 0; i < 40 && countIn(b.out, "minecraft:iron_block") < 1; ++i) co_await ctx.ticks(5);
    ctx.check(countIn(b.out, "minecraft:iron_block") == 1, "the matching filter lets it run");
    common::filterslot::set(basin, items::empty(), "filter");
    for (auto& s : b.out) s = items::empty();
    basinops::save(basin, b);
});

LV_REGISTER_SELFTEST("basin.press_world", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1470};
    auto floor = a.at(0);
    auto press = a.at(0, 2);
    ctx.require(co_await ctx.ensureLoaded(floor), "test area loaded");
    ctx.require(ctx.setBlock(floor, "minecraft:stone"), "floor");
    // walls so the dropped items stay under the press
    for (auto [dx, dz] : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{0, 1}}) ctx.setBlock(a.at(dx, 1, dz), "minecraft:glass");
    ctx.setBlock(a.at(1, 1), "minecraft:glass");
    ctx.require(ctx.setBlock(press, kPress, {{kAxisState, 0}}), "place a press");
    drivePress(ctx, press, 256);
    co_await ctx.ticks(3);
    stand(static_cast<SimulatedPlayer&>(ctx.player()), a.at(-6, 0, 6)); // out of pickup range
    co_await ctx.ticks(2);
    ctx.info(spawnItem(a.at(0, 1), .1f, stack("minecraft:sugar_cane", 2)));
    for (int i = 0; i < 40 && itemsNear(a.at(0, 1), "minecraft:paper") < 2; ++i) co_await ctx.ticks(5);
    ctx.check(itemsNear(a.at(0, 1), "minecraft:paper") == 2, "the press turned 2 sugar cane on the ground into paper (one per cycle)");
    clearItems(a.at(0, 1));
});

LV_REGISTER_SELFTEST("basin.spout", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1480};
    auto upper = a.at(0, 1);
    auto lower = a.at(1, 0);
    ctx.require(co_await ctx.ensureLoaded(upper), "test area loaded");
    ctx.require(ctx.setBlock(lower, kBasin), "place the lower basin");
    ctx.require(ctx.setBlock(a.at(0), "minecraft:stone"), "support");
    ctx.require(ctx.setBlock(upper, kBasin), "place the upper basin");
    co_await ctx.ticks(25);
    auto& r = region0();
    ctx.check(stateAt(r, game::blockPos(upper), kFacingState) == kEast, "the upper basin spouts east into the lower one");
    auto& up = basinAt(ctx, upper);
    ctx.check(basinops::acceptOutputs(r, upper, up, {stack("minecraft:gold_ingot", 3)}, {}, false), "outputs go to the spout");
    co_await ctx.ticks(3);
    auto& low = basinAt(ctx, lower);
    ctx.check(countIn(low.in, "minecraft:gold_ingot") == 3, "the spouted items arrive in the lower basin");
    for (auto& s : low.in) s = items::empty();
    basinops::save(lower, low);
});

LV_REGISTER_SELFTEST("basin.placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1490};
    auto ground = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(ground), "test area loaded");
    ctx.require(ctx.setBlock(ground, "minecraft:stone"), "ground");
    ctx.require(ctx.setBlock(a.at(0, 1), kBasin), "a basin");
    ctx.cleanupBlock(a.at(0, 2));
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    stand(sp, a.at(-3));
    co_await ctx.ticks(2);
    if (!sp.isCreative()) ctx.info("survival player");
    hold(sp, stack(kPress));
    sp.setSelectedSlot(0);
    useOn(sp, a.at(0, 1), 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(game::blockIdAt(region0(), game::blockPos(a.at(0, 2))) != kPress, "a press can't stand directly on a basin");
    // a burner faces the player
    ctx.require(ctx.setBlock(a.at(3), "minecraft:stone"), "ground 2");
    ctx.cleanupBlock(a.at(3, 1));
    hold(sp, stack(kEmptyBurner));
    auto target = a.at(3, 1);
    sp.simulateLookAt(Vec3{target.x + .5f, target.y + .5f, target.z + .5f}, sim::LookDuration::Instant);
    co_await ctx.ticks(2);
    useOn(sp, a.at(3), 0, ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(game::blockIdAt(region0(), game::blockPos(a.at(3, 1))) == kEmptyBurner, "an empty burner is placed");
    ctx.check(stateAt(region0(), game::blockPos(a.at(3, 1)), kFacingState) == kWest, "it faces the player (west)");
    clearItems(a.at(0, 2));
});

LV_REGISTER_SELFTEST("basin.sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1495};
    auto p = a.at(0);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kBasin), "place a basin");
    auto& player = ctx.player();
    net::setTestCompanion(player, true);
    co_await ctx.ticks(5);
    (void)net::takeTestMessages(player);
    auto& b = basinAt(ctx, p);
    basinops::insertInput(b, stack("minecraft:apple", 2), false);
    sync::dirty(p);
    co_await ctx.ticks(2);
    bool seen = false;
    for (auto const& m : net::takeTestMessages(player)) {
        if (m.channel != "basin.machines") continue;
        for (auto const& e : m.payload.value("list", Json::array()))
            if (e.value("k", -1) == 0 && e.contains("basin")) seen = true;
    }
    ctx.check(seen, "companions get the basin's contents");
    net::setTestCompanion(player, false);
    for (auto& s : b.in) s = items::empty();
    basinops::save(p, b);
});

} // namespace lv::feature::basin
