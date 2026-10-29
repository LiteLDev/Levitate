// Selftests of the bulk processing unit (U13): testPos dx 1300..1399 (x = column, dz 0..4).
// Machines are driven by U2's test generator (levitate:kinetics_test_source).

#include <filesystem>
#include <fstream>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"

#include "nlohmann/json.hpp"

#include "feature/bulk/Bulk.h"
#include "feature/common/BulkExt.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Main.h"
#include "selftest/SelfTest.h"

namespace lv::feature::bulk {

void recordPlacement(Player& player, BlockPos const& clicked, int face);

namespace {

using selftest::Context;
namespace items = common::items;

constexpr char const* kSource = "levitate:kinetics_test_source";
constexpr char const* kShaft  = "levitate:kinetics_test_shaft";
constexpr int         X = 0, Y = 1, Z = 2;
constexpr int         kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;

struct Area {
    Context& ctx;
    int      col;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(1300 + col + dx, dz);
        p.y += dy;
        return p;
    }
};

BlockSource& overworld(Context& ctx) {
    auto* r = regionOf(0);
    if (!r) ctx.fail("no overworld");
    return *r;
}

ItemStack stack(Context& ctx, char const* id, int count = 1) {
    auto s = items::make(id, count);
    if (!s) ctx.fail(std::string("unknown item ") + id);
    return *s;
}

int countItems(BlockSource& region, cb::Box const& box, std::string const& id) {
    int n = 0;
    for (auto* a : entitiesIn(region, box)) {
        auto* s = itemOf(*a);
        if (s && items::idOf(*s) == id) n += items::countOf(*s);
    }
    return n;
}

void removeItems(BlockSource& region, cb::Box const& box) {
    for (auto* a : entitiesIn(region, box))
        if (isItem(*a)) a->remove();
}

cb::Box around(GlobalPos const& p, double r) {
    return {
        {p.x + .5 - r, p.y + .5 - r, p.z + .5 - r},
        {p.x + .5 + r, p.y + .5 + r, p.z + .5 + r}
    };
}

/// Loads the area; the test player then watches as a spectator (never picks up the test items).
ll::coro::CoroTask<bool> loadArea(Context& ctx, GlobalPos pos) {
    bool ok = co_await ctx.ensureLoaded(pos);
    ctx.player().setPlayerGameType(GameType::Spectator);
    co_return ok;
}

/// Temporarily shorter fan processing (restored by the destructor).
struct FastFans {
    int saved;
    FastFans() : saved(config::mut<BulkConfig>().fanProcessingTime) { config::mut<BulkConfig>().fanProcessingTime = 10; }
    ~FastFans() { config::mut<BulkConfig>().fanProcessingTime = saved; }
};

} // namespace

LV_REGISTER_SELFTEST("bulk.registry", [](Context& ctx) {
    for (auto id : {kMillstone, kWheel, kController, kFan, kNozzle}) {
        ctx.check(game::blockWith(id) != nullptr, std::string(id) + " is in the behavior pack");
    }
    ctx.check(kinetics::specOf(kMillstone) && kinetics::specOf(kWheel) && kinetics::specOf(kFan), "kinetic specs are registered");
    ctx.check(!kinetics::specOf(kNozzle) && !kinetics::specOf(kController), "nozzle and controller are not kinetic");
    ctx.check(kinetics::stressImpactOf(std::string(kMillstone)) == 4, "millstone impact 4 SU/RPM");
    ctx.check(kinetics::stressImpactOf(std::string(kWheel)) == 8, "crushing wheel impact 8 SU/RPM");
    ctx.check(kinetics::stressImpactOf(std::string(kFan)) == 2, "encased fan impact 2 SU/RPM");
    auto n = [](char const* t) { return common::recipes::ofType(t).size(); };
    ctx.check(n("milling") >= 45, "milling recipes loaded: " + std::to_string(n("milling")));
    ctx.check(n("crushing") >= 50, "crushing recipes loaded: " + std::to_string(n("crushing")));
    ctx.check(n("splashing") >= 15, "splashing recipes loaded: " + std::to_string(n("splashing")));
    ctx.check(n("haunting") >= 18, "haunting recipes loaded: " + std::to_string(n("haunting")));
    ctx.info("recipes: milling " + std::to_string(n("milling")) + ", crushing " + std::to_string(n("crushing")) + ", splashing "
             + std::to_string(n("splashing")) + ", haunting " + std::to_string(n("haunting")));
});

// Every vanilla item named by the unit's recipes exists on Bedrock (Create items belong to U9).
LV_REGISTER_SELFTEST("bulk.recipe_items", [](Context& ctx) {
    std::set<std::string> missing;
    std::size_t           checked = 0;
    auto want = [&](std::string const& id) {
        if (!id.starts_with("minecraft:")) return;
        ++checked;
        if (!items::make(id)) missing.insert(id);
    };
    for (auto type : {"milling", "crushing", "splashing", "haunting"}) {
        for (auto const& r : common::recipes::ofType(type)) {
            for (auto const& in : r.ingredients)
                for (auto const& id : in.items) want(id);
            for (auto const& out : r.results) want(out.id);
        }
    }
    // tag members used by the recipes
    for (auto tag : {"minecraft:sand", "minecraft:wool", "minecraft:dirt", "c:glass_blocks", "c:glass_panes", "c:cobblestones"})
        for (auto const& id : common::recipes::tagItems(tag)) want(id);
    std::string list;
    for (auto const& m : missing) list += (list.empty() ? "" : ", ") + m;
    ctx.check(missing.empty(), "unknown Bedrock items: " + list);
    ctx.info("checked " + std::to_string(checked) + " item references");
});

LV_REGISTER_SELFTEST("bulk.millstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 0};
    ctx.require(co_await loadArea(ctx, a.at(0)), "test area loaded");
    auto& region = overworld(ctx);
    auto  mill   = a.at(0, 1);
    ctx.require(ctx.setBlock(a.at(0), kSource, {{"levitate:axis", Y}}), "place the source");
    ctx.require(ctx.setBlock(mill, std::string(kMillstone)), "place the millstone");
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0), 256);
    co_await ctx.ticks(2);
    ctx.require(kinetics::speedAt(mill) == 256, "the millstone turns at 256 RPM, got " + std::to_string(kinetics::speedAt(mill)));
    auto bp = toBlockPos(mill);
    // only millable items go in
    auto rest = common::items::insert(region, bp, kUp, stack(ctx, "minecraft:diamond"), false);
    ctx.check(items::countOf(rest) == 1, "a diamond is refused");
    rest = common::items::insert(region, bp, kUp, stack(ctx, "minecraft:bone", 2), false);
    ctx.check(items::isEmpty(rest), "two bones go in");
    ctx.check(items::countOf(millstoneAt(mill).input) == 2, "the input slot holds them");
    // 100 ticks per bone at 256 RPM: 16 per tick -> 7 ticks each, plus the recipe lookups
    co_await ctx.ticks(40);
    auto v = millstoneAt(mill);
    ctx.check(items::isEmpty(v.input), "both bones were milled");
    int meal = 0;
    for (auto const& s : v.outputs)
        if (items::idOf(s) == "minecraft:bone_meal") meal += items::countOf(s);
    ctx.check(meal >= 6, "at least 6 bone meal, got " + std::to_string(meal));
    // extraction from the outputs only
    auto got = common::items::extract(region, bp, kDown, common::items::any(), 2, false);
    ctx.check(items::idOf(got) == "minecraft:bone_meal" && items::countOf(got) == 2, "two bone meal extracted");
    // an empty hand takes the rest
    auto& player = ctx.player();
    ctx.check(millstoneTakeOut(player, mill), "right-click with an empty hand");
    auto after = millstoneAt(mill);
    bool empty = true;
    for (auto const& s : after.outputs) empty = empty && items::isEmpty(s);
    ctx.check(empty, "the outputs went to the player");
    // stopped: nothing happens
    kinetics::setGeneratedSpeed(a.at(0), 0);
    co_await ctx.ticks(2);
    (void)common::items::insert(region, bp, kUp, stack(ctx, "minecraft:bone"), false);
    co_await ctx.ticks(20);
    ctx.check(items::countOf(millstoneAt(mill).input) == 1, "a stopped millstone keeps its input");
    removeItems(region, around(mill, 3));
});

LV_REGISTER_SELFTEST("bulk.millstone_throw", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 6};
    ctx.require(co_await loadArea(ctx, a.at(0)), "test area loaded");
    auto& region = overworld(ctx);
    auto  mill   = a.at(0, 1);
    ctx.require(ctx.setBlock(a.at(0), kSource, {{"levitate:axis", Y}}), "place the source");
    ctx.require(ctx.setBlock(mill, std::string(kMillstone)), "place the millstone");
    co_await ctx.ticks(2);
    spawnItem(region, stack(ctx, "minecraft:cobblestone", 3), {mill.x + .5, mill.y + 1.6, mill.z + .5}, {});
    co_await ctx.ticks(30);
    auto v = millstoneAt(mill);
    ctx.check(items::idOf(v.input) == "minecraft:cobblestone", "the thrown cobblestone landed in the millstone");
    ctx.check(countItems(region, around(mill, 2), "minecraft:cobblestone") == 0, "no cobblestone entity left on top");
    removeItems(region, around(mill, 3));
});

LV_REGISTER_SELFTEST("bulk.crusher_controller", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 10};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto  w1 = a.at(1, 1, 0), w2 = a.at(1, 1, 2), ctrl = a.at(1, 1, 1);
    ctx.require(ctx.setBlock(a.at(0, 1, 0), kSource, {{"levitate:axis", X}}), "source 1");
    ctx.require(ctx.setBlock(a.at(0, 1, 2), kSource, {{"levitate:axis", X}}), "source 2");
    ctx.setBlock(w1, std::string(kWheel), {{"levitate:axis", X}});
    ctx.setBlock(w2, std::string(kWheel), {{"levitate:axis", X}});
    ctx.cleanupBlock(ctrl);
    co_await ctx.ticks(3);
    // both turn the same way: an invalid controller
    ctx.check(idAt(region, toBlockPos(ctrl)) == kController, "a controller appears between the wheels");
    ctx.check(stateOf(region.getBlock(toBlockPos(ctrl)), "levitate:valid") == 0, "wheels turning the same way: not valid");
    kinetics::setGeneratedSpeed(a.at(0, 1, 2), -16);
    co_await ctx.ticks(3);
    auto expected = cb::controllerFacing(cb::Axis::X, cb::Face::South, kinetics::speedAt(w1));
    ctx.check(stateOf(region.getBlock(toBlockPos(ctrl)), "levitate:valid") == 1, "wheels turning inwards: valid");
    ctx.check(
        stateOf(region.getBlock(toBlockPos(ctrl)), "levitate:facing") == int(expected),
        "controller faces " + std::string(core::kinetics::faceName(expected))
    );
    ctx.check(crushingSpeedAt(region, ctrl) == cb::crushingSpeedOf(16), "crushing speed |16| / 50");
    // removing a wheel removes the controller
    game::clearBlock(region, toBlockPos(w2));
    co_await ctx.ticks(3);
    ctx.check(idAt(region, toBlockPos(ctrl)) != kController, "the controller goes with the wheel");
});

LV_REGISTER_SELFTEST("bulk.crusher_crushes", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 15};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    // wheels two blocks up so the output has room below
    auto w1 = a.at(1, 2, 0), w2 = a.at(1, 2, 2), ctrl = a.at(1, 2, 1);
    ctx.setBlock(a.at(0, 2, 0), kSource, {{"levitate:axis", X}});
    ctx.setBlock(a.at(0, 2, 2), kSource, {{"levitate:axis", X}});
    ctx.setBlock(w1, std::string(kWheel), {{"levitate:axis", X}});
    ctx.setBlock(w2, std::string(kWheel), {{"levitate:axis", X}});
    ctx.cleanupBlock(ctrl);
    co_await ctx.ticks(2);
    // pick the directions so that the pair pulls down
    float s = cb::controllerFacing(cb::Axis::X, cb::Face::South, 64) == cb::Face::Down ? 256.f : -256.f;
    kinetics::setGeneratedSpeed(a.at(0, 2, 0), s);
    kinetics::setGeneratedSpeed(a.at(0, 2, 2), -s);
    co_await ctx.ticks(3);
    ctx.require(stateOf(region.getBlock(toBlockPos(ctrl)), "levitate:facing") == kDown, "the controller faces down");
    // through the capability (a funnel / hopper above)
    auto rest = common::items::insert(region, toBlockPos(ctrl), kUp, stack(ctx, "minecraft:cobblestone", 2), false);
    ctx.check(items::isEmpty(rest), "the controller takes the cobblestone");
    auto rest2 = common::items::insert(region, toBlockPos(ctrl), kUp, stack(ctx, "minecraft:cobblestone"), false);
    ctx.check(items::countOf(rest2) == 1, "an occupied controller refuses more");
    co_await ctx.ticks(80);
    cb::Box below{
        {ctrl.x - .5, ctrl.y - 2.5, ctrl.z - .5},
        {ctrl.x + 1.5, ctrl.y + .2, ctrl.z + 1.5}
    };
    int gravel = countItems(region, below, "minecraft:gravel");
    ctx.check(gravel == 2, "two gravel came out below, got " + std::to_string(gravel));
    removeItems(region, around(ctrl, 4));
    // thrown in from above
    spawnItem(region, stack(ctx, "minecraft:gravel"), {ctrl.x + .5, ctrl.y + 1.5, ctrl.z + .5}, {});
    co_await ctx.ticks(80);
    ctx.check(countItems(region, below, "minecraft:sand") == 1, "thrown gravel was crushed to sand");
    ctx.check(countItems(region, around(ctrl, 4), "minecraft:gravel") == 0, "no gravel left");
    removeItems(region, around(ctrl, 4));
});

LV_REGISTER_SELFTEST("bulk.crusher_damages", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 20};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto  w1 = a.at(1, 1, 0), w2 = a.at(1, 1, 2), ctrl = a.at(1, 1, 1);
    ctx.setBlock(a.at(0, 1, 0), kSource, {{"levitate:axis", X}});
    ctx.setBlock(a.at(0, 1, 2), kSource, {{"levitate:axis", X}});
    ctx.setBlock(w1, std::string(kWheel), {{"levitate:axis", X}});
    ctx.setBlock(w2, std::string(kWheel), {{"levitate:axis", X}});
    ctx.cleanupBlock(ctrl);
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 1, 2), -16);
    co_await ctx.ticks(3);
    auto level = ll::service::getLevel();
    ctx.require(level != nullptr, "level");
    auto* mob = level->getSpawner().spawnMob(
        region,
        ActorDefinitionIdentifier("minecraft:pig"),
        nullptr,
        Vec3{ctrl.x + .5f, ctrl.y + .9f, ctrl.z + .5f},
        false,
        false,
        false
    );
    ctx.require(mob != nullptr, "spawn a pig in the crusher");
    auto id     = idOf(*mob);
    int  health = mob->getHealth();
    co_await ctx.ticks(30);
    auto* pig = entityById(id);
    ctx.check(!pig || !pig->isAlive() || pig->getHealth() < health, "the pig took crushing damage");
    if (pig && pig->isAlive()) pig->remove();
    removeItems(region, around(ctrl, 4));
});

LV_REGISTER_SELFTEST("bulk.fan_current", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 36};
    ctx.require(co_await loadArea(ctx, a.at(10)), "test area loaded");
    auto& region = overworld(ctx);
    auto  fan    = a.at(1, 0, 0);
    ctx.setBlock(a.at(0, 0, 0), kSource, {{"levitate:axis", X}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kEast}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), 256);
    co_await ctx.ticks(3);
    auto f = fanAt(fan);
    ctx.require(f.has_value(), "the fan is tracked");
    ctx.check(std::abs(f->current.maxDistance - 20) < 1e-3, "256 RPM: 20 blocks, got " + std::to_string(f->current.maxDistance));
    bool expectPush = cb::fanPushing(cb::Face::East, 256);
    ctx.check(f->current.pushing == expectPush, "push direction follows the speed sign");
    // a wall 6 blocks in front: the current stops before it
    ctx.setBlock(a.at(7, 0, 0), "minecraft:stone");
    co_await ctx.ticks(35); // fanBlockCheckRate
    f = fanAt(fan);
    ctx.check(f && std::abs(f->current.maxDistance - 5) < 1e-3, "blocked after 5 blocks, got " + std::to_string(f ? f->current.maxDistance : -1));
    // a lit campfire 2 blocks in front: smoking air downstream (the campfire lets the air through)
    ctx.setBlock(a.at(3, 0, 0), "minecraft:campfire");
    co_await ctx.ticks(35);
    f = fanAt(fan);
    if (f && f->current.pushing) {
        ctx.check(f->current.typeAt(.5f) == cb::FanType::None, "before the campfire: plain air");
        ctx.check(f->current.typeAt(3.5f) == cb::FanType::Smoking, "after the campfire: smoking");
    } else if (f) {
        ctx.check(f->current.typeAt(.5f) == cb::FanType::Smoking, "pulled through the campfire: smoking near the fan");
    }
    // slower: shorter reach (lerp 3..20 by speed / 256)
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), 32);
    game::clearBlock(region, toBlockPos(a.at(7, 0, 0)));
    co_await ctx.ticks(35);
    f = fanAt(fan);
    ctx.check(f && std::abs(f->current.maxDistance - (3 + 17 / 8.f)) < 1e-3, "32 RPM: 5.125 blocks");
    auto src = common::airflow::sourceAt(fan);
    ctx.check(src && src->speed == 32 && src->facing == kEast, "the airflow extension reports the fan");
    ctx.check(common::airflow::typeAt(region, toBlockPos(a.at(3, 0, 0))) == "smoking", "a campfire is a smoking catalyst");
});

LV_REGISTER_SELFTEST("bulk.fan_pushes", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 36};
    ctx.require(co_await loadArea(ctx, a.at(10, 0, 2)), "test area loaded");
    auto& region = overworld(ctx);
    auto  fan    = a.at(1, 0, 2);
    ctx.setBlock(a.at(0, 0, 2), kSource, {{"levitate:axis", X}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kEast}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 2), 256);
    co_await ctx.ticks(3);
    auto f = fanAt(fan);
    ctx.require(f && f->current.maxDistance > 10, "the fan blows");
    bool pushing = f->current.pushing;
    auto start   = a.at(pushing ? 3 : 12, 0, 2);
    auto id      = spawnItem(region, stack(ctx, "minecraft:stick"), {start.x + .5, start.y + .1, start.z + .5}, {});
    co_await ctx.ticks(30);
    auto* e = entityById(id);
    ctx.require(e != nullptr, "the stick still exists");
    double moved = feetOf(*e).x - (start.x + .5);
    ctx.check(pushing ? moved > 1 : moved < -1, "the item was carried along the current (" + std::to_string(moved) + " blocks)");
    e->remove();
});

LV_REGISTER_SELFTEST("bulk.fan_washing", [](Context& ctx) -> ll::coro::CoroTask<> {
    FastFans fast;
    Area     a{ctx, 36};
    ctx.require(co_await loadArea(ctx, a.at(1, 0, 4)), "test area loaded");
    auto& region = overworld(ctx);
    // a fan 3 blocks up blowing down into a one-block water pit in the ground (water can't spread)
    auto fan = a.at(1, 3, 4), pit = a.at(1, -1, 4);
    ctx.setBlock(a.at(1, 4, 4), kSource, {{"levitate:axis", Y}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kDown}});
    auto const* ground = &region.getBlock(toBlockPos(pit));
    auto const* water  = game::blockWith("minecraft:water");
    ctx.require(water && game::setBlock(region, toBlockPos(pit), *water), "dig the water pit");
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(1, 4, 4), cb::fanPushing(cb::Face::Down, 64) ? 64 : -64);
    co_await ctx.ticks(40);
    auto f = fanAt(fan);
    ctx.check(f && f->current.pushing && f->current.typeAt(3.5f) == cb::FanType::Splashing, "a washing current into the pit");
    auto at = a.at(1, 1, 4);
    spawnItem(region, stack(ctx, "minecraft:red_wool", 2), {at.x + .5, at.y + .1, at.z + .5}, {});
    co_await ctx.ticks(60);
    auto area = around(a.at(1, 0, 4), 2.5);
    ctx.check(countItems(region, area, "minecraft:white_wool") == 2, "red wool was washed white");
    ctx.check(countItems(region, area, "minecraft:red_wool") == 0, "no red wool left");
    removeItems(region, area);
    game::setBlock(region, toBlockPos(pit), *ground);
});
LV_REGISTER_SELFTEST("bulk.fan_smoking", [](Context& ctx) -> ll::coro::CoroTask<> {
    FastFans fast;
    Area     a{ctx, 60};
    ctx.require(co_await loadArea(ctx, a.at(10)), "test area loaded");
    auto& region = overworld(ctx);
    auto  fan    = a.at(1, 0, 0);
    ctx.setBlock(a.at(0, 0, 0), kSource, {{"levitate:axis", X}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kEast}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), cb::fanPushing(cb::Face::East, 64) ? 64 : -64);
    ctx.setBlock(a.at(2, 0, 0), "minecraft:campfire");
    co_await ctx.ticks(40);
    auto f = fanAt(fan);
    ctx.require(f && f->current.typeAt(3.f) == cb::FanType::Smoking, "a lit campfire makes smoking air");
    auto at = a.at(4, 0, 0);
    spawnItem(region, stack(ctx, "minecraft:beef"), {at.x + .5, at.y + .1, at.z + .5}, {});
    co_await ctx.ticks(40);
    cb::Box area{
        {fan.x + 1., fan.y - 1., fan.z - 1.},
        {fan.x + 30., fan.y + 2., fan.z + 2.}
    };
    ctx.check(countItems(region, area, "minecraft:cooked_beef") == 1, "the beef was smoked");
    removeItems(region, area);
});

// Blasting and haunting results (no entities: lava flows, soul fire needs soul soil).
LV_REGISTER_SELFTEST("bulk.fan_types", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 85};
    ctx.require(co_await loadArea(ctx, a.at(0)), "test area loaded");
    auto& region = overworld(ctx);
    auto  p      = a.at(0, 0, 0);
    ctx.setBlock(a.at(0, -1, 0), "minecraft:soul_soil");
    ctx.setBlock(p, "minecraft:soul_campfire");
    ctx.check(catalystAt(region, toBlockPos(p)) == cb::FanType::Haunting, "a lit soul campfire haunts");
    ctx.setBlock(p, "minecraft:campfire", {{"extinguished", 1}});
    ctx.check(catalystAt(region, toBlockPos(p)) == cb::FanType::None, "an unlit campfire does nothing");
    ctx.setBlock(p, "minecraft:lava");
    ctx.check(catalystAt(region, toBlockPos(p)) == cb::FanType::Blasting, "lava blasts");
    ctx.setBlock(p, "minecraft:air");
    co_await ctx.ticks(1);
    auto res = [&](char const* id, cb::FanType t) { return fanProcess(stack(ctx, id, 3), t); };
    auto iron = res("minecraft:raw_iron", cb::FanType::Blasting);
    ctx.check(iron && !iron->empty() && items::idOf(iron->front()) == "minecraft:iron_ingot" && items::countOf(iron->front()) == 3, "raw iron blasts into 3 ingots");
    auto beef = res("minecraft:beef", cb::FanType::Blasting);
    ctx.check(beef && beef->empty(), "food in blasting air burns up");
    ctx.check(canFanProcess(stack(ctx, "minecraft:stick"), cb::FanType::Blasting), "blasting destroys anything flammable");
    ctx.check(!canFanProcess(stack(ctx, "minecraft:netherite_ingot"), cb::FanType::Blasting), "fire resistant items survive blasting");
    auto soul = res("minecraft:sand", cb::FanType::Haunting);
    ctx.check(soul && !soul->empty() && items::idOf(soul->front()) == "minecraft:soul_sand", "sand haunts into soul sand");
    ctx.check(!res("minecraft:stick", cb::FanType::Haunting), "sticks don't haunt");
    auto glass = res("minecraft:red_stained_glass", cb::FanType::Splashing);
    ctx.check(glass && !glass->empty() && items::idOf(glass->front()) == "minecraft:glass", "stained glass washes clear");
    ctx.check(common::airflow::processingTime(17) == config().fanProcessingTime * 2 + 1, "17 items take two rounds");
});

LV_REGISTER_SELFTEST("bulk.nozzle", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 26};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto  fan = a.at(1, 1, 2), nozzle = a.at(1, 2, 2);
    ctx.setBlock(a.at(1, 0, 2), kSource, {{"levitate:axis", Y}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kUp}});
    ctx.setBlock(nozzle, std::string(kNozzle), {{"levitate:facing", kUp}});
    co_await ctx.ticks(3);
    ctx.check(idAt(region, toBlockPos(nozzle)) == kNozzle, "a nozzle in front of a fan stays");
    auto f = fanAt(fan);
    ctx.check(f && f->current.maxDistance < .25f, "the nozzle blocks the fan's own current");
    kinetics::setGeneratedSpeed(a.at(1, 0, 2), cb::fanPushing(cb::Face::Up, 128) ? 128 : -128);
    co_await ctx.ticks(10);
    cb::Vec3d centre{nozzle.x + .5, nozzle.y + .5, nozzle.z + .5};
    auto      id = spawnItem(region, stack(ctx, "minecraft:stick"), centre + cb::Vec3d{3, 0, 0}, {});
    co_await ctx.ticks(30);
    auto* e = entityById(id);
    ctx.check(e && (feetOf(*e) - centre).length() > 3.6, "the nozzle blows items away");
    if (e) e->remove();
    // without the fan the nozzle pops off
    game::clearBlock(region, toBlockPos(fan));
    co_await ctx.ticks(3);
    ctx.check(idAt(region, toBlockPos(nozzle)) != kNozzle, "the nozzle drops without its fan");
    removeItems(region, around(nozzle, 6));
});

LV_REGISTER_SELFTEST("bulk.placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 31};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto& player = ctx.player();
    // a shaft (axis x) west of the spot: the fan's back connects to it and it blows away (east)
    ctx.setBlock(a.at(0, 1, 2), kShaft, {{"levitate:axis", X}});
    co_await ctx.ticks(2);
    auto spot = a.at(1, 1, 2);
    recordPlacement(player, toBlockPos(spot), kUp);
    ctx.setBlock(spot, std::string(kFan));
    co_await ctx.ticks(3);
    ctx.check(stateOf(region.getBlock(toBlockPos(spot)), "levitate:facing") == kEast, "the fan faces away from the shaft");
    // a crushing wheel next to the shaft takes its axis
    auto wheel = a.at(0, 1, 3);
    ctx.setBlock(a.at(0, 1, 4), kShaft, {{"levitate:axis", Z}});
    co_await ctx.ticks(2);
    recordPlacement(player, toBlockPos(wheel), kUp);
    ctx.setBlock(wheel, std::string(kWheel));
    co_await ctx.ticks(3);
    ctx.check(stateOf(region.getBlock(toBlockPos(wheel)), "levitate:axis") == Z, "the wheel takes the shaft's axis");
    // a nozzle without a fan behind it pops off
    auto nozzle = a.at(2, 1, 0);
    recordPlacement(player, toBlockPos(nozzle), kUp);
    ctx.setBlock(nozzle, std::string(kNozzle));
    co_await ctx.ticks(3);
    ctx.check(idAt(region, toBlockPos(nozzle)) != kNozzle, "a nozzle needs a fan");
    removeItems(region, around(a.at(1, 1, 2), 4));
});

} // namespace lv::feature::bulk
