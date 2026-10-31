// Selftests of the bulk processing unit (U13): testPos dx 1300..1399 (x = column, dz 0..4); U39
// additions dx 3950..3989.
// Machines are driven by U2's test generator (levitate:kinetics_test_source).

#include <filesystem>
#include <functional>
#include <fstream>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/safety/RedactableString.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/animal/Horse.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/level/dimension/DimensionType.h"
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
#include "mod/Log.h"
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

/// testPos puts dx 39xx 150 blocks south, outside the run's ticking area (z ±64): the U39 tests
/// with entities move 205 blocks north, to rows no unit uses (z -55..-51).
constexpr int kU39Shift = -205;

struct Area {
    Context& ctx;
    int      col;
    int      zShift = 0; ///< extra dz (U39 tests: into the run's ticking area)
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(1300 + col + dx, dz + zShift);
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

// Every recipe file of every processing type loaded, and every vanilla item a recipe names exists on
// Bedrock (U54: recipes with conflict markers or Java-only ids used to be skipped silently).
LV_REGISTER_SELFTEST("bulk.recipe_items", [](Context& ctx) {
    namespace fs = std::filesystem;
    std::set<std::string> missing;
    std::size_t           checked = 0;
    auto want = [&](std::string const& id) {
        if (!id.starts_with("minecraft:")) return;
        ++checked;
        if (!items::make(id)) missing.insert(id);
    };
    auto wantTag = [&](std::string const& tag) {
        for (auto const& id : common::recipes::tagItems(tag)) want(id);
    };
    // walks the whole document: sequenced assembly steps, inline tags, alternatives
    std::function<void(nlohmann::json const&)> walk = [&](nlohmann::json const& j) {
        if (j.is_array()) {
            for (auto const& v : j) walk(v);
            return;
        }
        if (!j.is_object()) return;
        bool fluid = j.contains("amount"); // a fluid ingredient / result
        for (auto const& [k, v] : j.items()) {
            if (fluid) continue;
            if ((k == "item" || k == "id") && v.is_string()) want(v.get<std::string>());
            else if (k == "tag" && v.is_string()) wantTag(v.get<std::string>());
            else if (k == "tags" && v.is_object()) {
                for (auto const& [tag, list] : v.items())
                    if (list.is_array())
                        for (auto const& id : list)
                            if (id.is_string()) want(id.get<std::string>());
            } else walk(v);
        }
    };
    auto            dir = Mod::getInstance().getSelf().getModDir() / "recipes";
    std::string     unread;
    std::error_code ec;
    ctx.require(fs::is_directory(dir, ec), "recipes folder " + dir.string());
    for (auto const& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory()) continue;
        auto type = e.path().filename().string();
        if (type == "tags") continue;
        std::size_t files = 0;
        for (auto const& f : fs::recursive_directory_iterator(e.path(), ec))
            if (f.is_regular_file() && f.path().extension() == ".json") ++files;
        auto const& list = common::recipes::ofType(type);
        if (list.size() != files) unread += " " + type + " " + std::to_string(list.size()) + "/" + std::to_string(files);
        for (auto const& r : list) walk(r.raw);
    }
    ctx.check(unread.empty(), "recipe files that did not load:" + unread);
    for (auto tag : {"minecraft:sand", "minecraft:wool", "minecraft:dirt", "c:glass_blocks", "c:glass_panes", "c:cobblestones"}) wantTag(tag);
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

// ---- U39 (processing gaps): dx 3950..3984 (Area col 2650..2684) ------------------------------------------

namespace {

/// Wheels along z at `a.at(1, dy, 0)` / `a.at(1, dy, 2)` pulling down through the controller between.
ll::coro::CoroTask<GlobalPos> downCrusher(Context& ctx, Area const& a, int dy, float rpm = 256) {
    auto w1 = a.at(1, dy, 0), w2 = a.at(1, dy, 2), ctrl = a.at(1, dy, 1);
    ctx.setBlock(a.at(0, dy, 0), kSource, {{"levitate:axis", X}});
    ctx.setBlock(a.at(0, dy, 2), kSource, {{"levitate:axis", X}});
    ctx.setBlock(w1, std::string(kWheel), {{"levitate:axis", X}});
    ctx.setBlock(w2, std::string(kWheel), {{"levitate:axis", X}});
    ctx.cleanupBlock(ctrl);
    co_await ctx.ticks(2);
    float s = cb::controllerFacing(cb::Axis::X, cb::Face::South, 64) == cb::Face::Down ? rpm : -rpm;
    kinetics::setGeneratedSpeed(a.at(0, dy, 0), s);
    kinetics::setGeneratedSpeed(a.at(0, dy, 2), -s);
    // wait for the network and the controller (a busy server may take a few ticks)
    auto& region = overworld(ctx);
    for (int i = 0; i < 60; ++i) {
        co_await ctx.ticks(1);
        if (idAt(region, toBlockPos(ctrl)) == kController && stateOf(region.getBlock(toBlockPos(ctrl)), "levitate:valid")
            && crushingSpeedAt(region, ctrl) > 0)
            break;
    }
    co_return ctrl;
}

/// Temporarily different crushing damage.
struct CrushDamage {
    int saved;
    explicit CrushDamage(int d) : saved(config::mut<BulkConfig>().crushingDamage) { config::mut<BulkConfig>().crushingDamage = d; }
    ~CrushDamage() { config::mut<BulkConfig>().crushingDamage = saved; }
};

/// The U39 rows (dx 39xx) lie outside the run's ticking area: the test player watches from
/// nearby (spectator) so entities there tick.
ll::coro::CoroTask<> watchFrom(Context& ctx, GlobalPos const& p) {
    static_cast<SimulatedPlayer&>(ctx.player()).teleport(Vec3{p.x + .5f, float(p.y + 4), p.z - 4.5f}, DimensionType(0));
    co_await ctx.ticks(20); // the chunks around the player start ticking
}

Actor* spawnMob(BlockSource& region, char const* id, cb::Vec3d const& at) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    // the actor factory: a spawned mob, without the spawner's spawn rules
    auto owner = level->getActorFactory().createSpawnedActor(ActorDefinitionIdentifier(id), nullptr, Vec3{float(at.x), float(at.y), float(at.z)}, Vec2{0, 0});
    if (!owner) return nullptr;
    auto* mob = level->addEntity(region, std::move(owner));
    if (mob) mob->setPersistent(); // never despawned while the test runs
    return mob;
}

} // namespace

LV_REGISTER_SELFTEST("bulk.crusher_pass_through", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R64: the entity being crushed has no collision with the controller and goes through
    Area a{ctx, 2650, kU39Shift};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    CrushDamage none(0);
    co_await watchFrom(ctx, a.at(1, 3, 1));
    auto ctrl = co_await downCrusher(ctx, a, 3);
    ctx.require(stateOf(region.getBlock(toBlockPos(ctrl)), "levitate:facing") == kDown, "the controller pulls down");
    auto* pig = spawnMob(region, "minecraft:pig", {ctrl.x + .5, ctrl.y + 1.2, ctrl.z + .5});
    ctx.require(pig != nullptr, "spawn a pig above the crusher");
    auto id   = idOf(*pig);
    bool        went = false;
    std::string path;
    for (int i = 0; i < 40 && !went; ++i) {
        co_await ctx.ticks(2);
        auto* e = entityById(id);
        if (i < 8) path += e ? std::to_string(feetOf(*e).y - ctrl.y).substr(0, 5) + (e->isAlive() ? " " : "(dead) ") : "- ";
        if (e && feetOf(*e).y < ctrl.y - .2) went = true;
    }
    if (!went) ctx.info("pig y relative to the controller: " + path);
    if (!went) {
        auto* e = entityById(id);
        ctx.info(e ? "pig at y " + std::to_string(feetOf(*e).y) + ", controller at y " + std::to_string(ctrl.y) + ", taken "
                         + std::to_string(controllerAt(ctrl).entity == id)
                   : std::string("pig gone"));
    }
    ctx.check(went, "the pig was pulled through the controller");
    if (auto* e = entityById(id)) e->remove();
});

LV_REGISTER_SELFTEST("bulk.crusher_kills", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R65: Create's crush damage type: its death message, looting II for the drops
    {
        ActorDamageSource src(SharedTypes::Legacy::ActorDamageCause::Contact, "death.attack.create.crush");
        auto              msg = src.getDeathMessage("Pig", nullptr);
        ctx.info("death message: " + msg.first);
        ctx.check(msg.first == "death.attack.create.crush", "the death message is Create's: " + msg.first);
    }
    Area a{ctx, 2655, kU39Shift};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    CrushDamage lethal(100);
    co_await watchFrom(ctx, a.at(1, 3, 1));
    auto ctrl   = co_await downCrusher(ctx, a, 3);
    int  before = lootingBoosts();
    auto* pig   = spawnMob(region, "minecraft:pig", {ctrl.x + .5, ctrl.y + 1.2, ctrl.z + .5});
    ctx.require(pig != nullptr, "spawn a pig above the crusher");
    auto id = idOf(*pig);
    for (int i = 0; i < 40 && (entityById(id) || lootingBoosts() == before); ++i) co_await ctx.ticks(2);
    auto* left = entityById(id);
    ctx.check(!left || !left->isAlive(), "the crusher killed the pig");
    if (lootingBoosts() == before) ctx.info("crushing speed " + std::to_string(crushingSpeedAt(region, ctrl)) + ", pig " + (left ? "alive" : "gone"));
    ctx.check(lootingBoosts() > before, "its drops rolled with looting (" + std::to_string(lootingBoosts() - before) + " functions)");
    removeItems(region, around(ctrl, 5));
});

LV_REGISTER_SELFTEST("bulk.placement_veto", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R68: a nozzle without its fan is not placed at all (Create's canSurvive at placement)
    Area a{ctx, 2660, kU39Shift};
    ctx.require(co_await ctx.ensureLoaded(a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto& sp     = static_cast<SimulatedPlayer&>(ctx.player());
    auto floor = a.at(1, 0, 2);
    ctx.require(ctx.setBlock(floor, "minecraft:stone"), "a stone floor");
    ctx.cleanupBlock(a.at(1, 1, 2));
    sp.teleport(Vec3{floor.x - 2.5f, float(floor.y + 1), floor.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    sp.getInventory().setItem(0, stack(ctx, "levitate:nozzle", 2));
    sp.setSelectedSlot(0);
    sp.sendInventory(false);
    sp.simulateLookAt(Vec3{floor.x + .5f, floor.y + 1.f, floor.z + .5f}, sim::LookDuration::Instant);
    co_await ctx.ticks(2);
    sp.simulateUseItemInSlotOnBlock(0, toBlockPos(floor), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(idAt(region, toBlockPos(a.at(1, 1, 2))) != kNozzle, "no nozzle was placed on plain stone");
    ctx.check(items::countOf(sp.getInventory().getItem(0)) == 2, "the nozzle stays in the hand");
    // on top of an upward fan it is placed
    ctx.require(ctx.setBlock(floor, std::string(kFan), {{"levitate:facing", kUp}}), "an upward fan");
    co_await ctx.ticks(2);
    sp.simulateUseItemInSlotOnBlock(0, toBlockPos(floor), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{.5f, 1.f, .5f});
    co_await ctx.ticks(12);
    ctx.check(idAt(region, toBlockPos(a.at(1, 1, 2))) == kNozzle, "a nozzle in front of its fan is placed");
    sp.getInventory().setItem(0, ItemStack());
    sp.sendInventory(false);
});

LV_REGISTER_SELFTEST("bulk.fan_state_tag", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R66: the fan processing state of an item entity is kept on the entity (survives saving)
    struct Slow {
        int saved;
        Slow() : saved(config::mut<BulkConfig>().fanProcessingTime) { config::mut<BulkConfig>().fanProcessingTime = 400; }
        ~Slow() { config::mut<BulkConfig>().fanProcessingTime = saved; }
    } slow;
    Area a{ctx, 2665, kU39Shift};
    ctx.require(co_await loadArea(ctx, a.at(10)), "test area loaded");
    auto& region = overworld(ctx);
    auto  fan    = a.at(1, 0, 0);
    ctx.setBlock(a.at(0, 0, 0), kSource, {{"levitate:axis", X}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kEast}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), cb::fanPushing(cb::Face::East, 64) ? 64 : -64);
    ctx.setBlock(a.at(2, 0, 0), "minecraft:campfire");
    co_await watchFrom(ctx, a.at(4, 0, 0));
    co_await ctx.ticks(40);
    auto at = a.at(4, 0, 0);
    auto id = spawnItem(region, stack(ctx, "minecraft:beef"), {at.x + .5, at.y + .1, at.z + .5}, {});
    std::string tag;
    for (int i = 0; i < 40 && tag.empty(); ++i) {
        co_await ctx.ticks(2);
        if (auto* e = entityById(id); e && e->hasTags())
            for (auto const& t : e->getTags())
                if (t.starts_with("lv_fan:")) tag = t;
    }
    ctx.check(!tag.empty(), "the item carries its processing state");
    ctx.check(tag.starts_with("lv_fan:" + std::to_string(int(cb::FanType::Smoking)) + ":"), "smoking, time left: " + tag);
    cb::Box area{
        {fan.x + 1., fan.y - 1., fan.z - 1.},
        {fan.x + 30., fan.y + 2., fan.z + 2.}
    };
    removeItems(region, area);
});

LV_REGISTER_SELFTEST("bulk.haunted_horse", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R67: a haunted horse becomes a skeleton horse with its name, owner, taming and saddle
    Area a{ctx, 2670, kU39Shift};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto  at     = a.at(1, 0, 2);
    co_await watchFrom(ctx, at);
    co_await ctx.ticks(2);
    // a natural spawn is sometimes a foal, which can't be tamed: spawn again until the taming takes
    Actor*       horse = nullptr;
    std::int64_t hid   = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        horse = spawnMob(region, "minecraft:horse", {at.x + .5, at.y + .1, at.z + .5});
        ctx.require(horse != nullptr, "spawn a horse");
        horse->setNameTag("Haunted Test");
        hid = idOf(*horse);
        co_await ctx.ticks(2);
        horse = entityById(hid);
        ctx.require(horse != nullptr, "the horse is still there");
        static_cast<Horse*>(horse)->tameToPlayer(ctx.player(), false);
        co_await ctx.ticks(2);
        horse = entityById(hid);
        ctx.require(horse != nullptr, "the horse is still there after taming");
        if (horse->isTame()) break;
        horse->remove();
        co_await ctx.ticks(1);
    }
    ctx.require(horse->isTame(), "a horse was tamed");
    for (int i = 0; i <= 101; ++i) {
        auto* h = entityById(hid);
        if (!h) break;
        affectEntity(*h, cb::FanType::Haunting, 0);
    }
    co_await ctx.ticks(2);
    Actor* skeleton = nullptr;
    for (auto* e : entitiesIn(region, around(at, 3)))
        if (e->getTypeName() == "minecraft:skeleton_horse") skeleton = e;
    ctx.require(skeleton != nullptr, "a skeleton horse took the horse's place");
    ctx.check(skeleton->getNameTag() == "Haunted Test", "it keeps the name: " + skeleton->getNameTag());
    ctx.check(skeleton->isTame(), "it is tamed");
    ctx.check(skeleton->getOwnerId() == ctx.player().getOrCreateUniqueID(), "it has the horse's owner");
    skeleton->remove();
    removeItems(region, around(at, 4));
});

LV_REGISTER_SELFTEST("bulk.fan_multibox", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R71: the current reaches into blocks made of several boxes as far as Create's test points go
    Area a{ctx, 2675, kU39Shift};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto fan = a.at(1, 4, 2);
    co_await watchFrom(ctx, a.at(1, 1, 2));
    ctx.setBlock(a.at(1, 5, 2), kSource, {{"levitate:axis", Y}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kDown}});
    for (int dy = 1; dy < 4; ++dy) ctx.cleanupBlock(a.at(1, dy, 2));
    // bottom stairs: under the step the current stops at its top, elsewhere at the half
    ctx.require(ctx.setBlock(a.at(1, 1, 2), "minecraft:oak_stairs", {{"weirdo_direction", 0}, {"upside_down_bit", 0}}), "stairs below the fan");
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(1, 5, 2), cb::fanPushing(cb::Face::Down, 64) ? 64 : -64);
    co_await ctx.ticks(40);
    auto f = fanAt(fan);
    ctx.require(f.has_value(), "the fan's current");
    ctx.info("current length " + std::to_string(f->current.maxDistance));
    ctx.check(std::abs(f->current.maxDistance - (2.5f + 1 / 32.f)) < .01f, "the current goes half into the stairs: " + std::to_string(f->current.maxDistance));
});

LV_REGISTER_SELFTEST("bulk.soundscapes", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R69: a millstone with an input plays Create's milling soundscape to the players near it
    Area a{ctx, 2680, kU39Shift};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto  mill   = a.at(1, 1, 2);
    ctx.setBlock(a.at(1, 0, 2), kSource, {{"levitate:axis", Y}});
    ctx.setBlock(mill, std::string(kMillstone));
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(1, 0, 2), 64);
    static_cast<SimulatedPlayer&>(ctx.player()).teleport(Vec3{mill.x + 2.5f, float(mill.y), mill.z + .5f}, DimensionType(0));
    co_await ctx.ticks(3);
    int before = soundPlays();
    (void)common::items::insert(region, toBlockPos(mill), kUp, stack(ctx, "minecraft:wheat", 16), false);
    co_await ctx.ticks(10);
    ctx.check(soundPlays() > before, "milling sounds were sent, " + std::to_string(soundPlays() - before));
    co_await ctx.ticks(200);
    removeItems(region, around(mill, 3));
});

LV_REGISTER_SELFTEST("bulk.nozzle_overload", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R70: a pulling nozzle holding more than 256 entities explodes and the entities are gone
    Area a{ctx, 2685, kU39Shift};
    ctx.require(co_await loadArea(ctx, a.at(1)), "test area loaded");
    auto& region = overworld(ctx);
    auto  fan = a.at(2, 0, 2), nozzle = a.at(2, 1, 2);
    co_await watchFrom(ctx, a.at(-8, 0, 2));
    ctx.setBlock(a.at(2, -1, 2), kSource, {{"levitate:axis", Y}});
    ctx.setBlock(fan, std::string(kFan), {{"levitate:facing", kUp}});
    ctx.setBlock(nozzle, std::string(kNozzle), {{"levitate:facing", kUp}});
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(2, -1, 2), cb::fanPushing(cb::Face::Up, 256) ? -256 : 256);
    co_await ctx.ticks(5);
    // 260 sticks with different names (they don't merge) on a grid above the nozzle
    for (int i = 0; i < 260; ++i) {
        auto s = stack(ctx, "minecraft:stick");
        s.setCustomName(Bedrock::Safety::RedactableString("stick " + std::to_string(i)));
        spawnItem(region, s, {nozzle.x + .5 + (i % 13 - 6) * .3, nozzle.y + 3.0 + (i / 13) * .1, nozzle.z + .5 + (i % 7 - 3) * .3}, {});
    }
    bool gone = false;
    for (int i = 0; i < 30 && !gone; ++i) {
        co_await ctx.ticks(2);
        gone = countItems(region, around(nozzle, 8), "minecraft:stick") < 10;
    }
    ctx.check(gone, "the nozzle exploded and the sticks are gone, " + std::to_string(countItems(region, around(nozzle, 8), "minecraft:stick")) + " left");
    removeItems(region, around(nozzle, 10));
});

} // namespace lv::feature::bulk
