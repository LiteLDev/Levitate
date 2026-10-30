// Selftests of the crafter unit (U16): `/create selftest "crafter.*"`. testPos dx 1600..1699.

#include <cmath>

#include "nlohmann/json.hpp"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/phys/AABB.h"

#include "core/crafter/Breaking.h"
#include "feature/belts/BeltApi.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/crafter/Internal.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::crafter {

namespace {

using selftest::Context;
namespace ci = common::items;
namespace k  = core::kinetics;

constexpr char kSource[] = "levitate:kinetics_test_source";

GlobalPos at(GlobalPos p, int dx, int dy, int dz) {
    p.x += dx;
    p.y += dy;
    p.z += dz;
    return p;
}

ll::coro::CoroTask<GlobalPos> area(Context& ctx, int dx) {
    auto p = ctx.testPos(dx);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    co_return p;
}

/// A crafter facing south (front +z) with an arrow.
void crafter(Context& ctx, GlobalPos const& p, cr::Pointing pointing) {
    ctx.require(ctx.setBlock(p, std::string(kCrafter), {{kFacing, 3}, {kPointing, static_cast<int>(pointing)}, {kCovered, 0}}), "crafter placed");
}

/// A test generator behind `p` (z - 1) turning along z at `rpm`.
void power(Context& ctx, GlobalPos const& p, float rpm) {
    auto s = at(p, 0, 0, -1);
    ctx.require(ctx.setBlock(s, kSource, {{"levitate:axis", 2}}), "source placed");
    kinetics::refresh(s);
    kinetics::setGeneratedSpeed(s, rpm);
}

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = ci::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

BlockSource& region(Context& ctx, GlobalPos const& p) {
    auto* r = world::regionOf(p.dim);
    ctx.require(r != nullptr, "dimension loaded");
    return *r;
}

/// Total count of item entities of `id` around `p` (radius r); removes them when `take`.
int itemsAround(Context& ctx, GlobalPos const& p, std::string const& id, double r = 3, bool take = true) {
    int n = 0;
    for (auto* a : region(ctx, p).getEntities(AABB{Vec3{float(p.x - r), float(p.y - r), float(p.z - r)}, Vec3{float(p.x + 1 + r), float(p.y + 1 + r), float(p.z + 1 + r)}}, 0.f, ActorType::ItemEntity)) {
        auto st = world::itemOf(*a);
        if (!id.empty() && ci::idOf(st) != id) continue;
        n += ci::countOf(st);
        if (take) a->remove();
    }
    return n;
}

int countIn(Context& ctx, GlobalPos const& p, std::string const& id) {
    auto& reg = region(ctx, p);
    int   n   = 0, slots = ci::slotCount(reg, game::blockPos(p), -1);
    auto  h   = common::items::find({reg, game::blockPos(p), -1});
    if (!h) return 0;
    for (int i = 0; i < slots; ++i) {
        auto s = h->getSlot({reg, game::blockPos(p), -1}, i);
        if (ci::idOf(s) == id) n += ci::countOf(s);
    }
    return n;
}

template <class F>
ll::coro::CoroTask<bool> waitFor(int ticks, F cond) {
    for (int i = 0; i < ticks; ++i) {
        if (cond()) co_return true;
        co_await Context::ticks(1);
    }
    co_return cond();
}

} // namespace

LV_REGISTER_SELFTEST("crafter.geometry", [](Context& ctx) {
    ctx.check(cr::targetDirection(Face::South, cr::Pointing::Left) == Face::West, "left of a south-facing crafter is west");
    ctx.check(cr::pointingFromFacing(Face::Up, Face::North) == cr::Pointing::Down, "placed on top points down");
    ctx.check(cr::countDownSpeed(32) == 32 && cr::countDownSpeed(1) == 4, "countdown speed");
    ctx.check(cr::ticksToBreak(1.5f, cr::stationaryBreakSpeed(64)) == 28, "stone at 64 rpm takes 28 ticks");
});

LV_REGISTER_SELFTEST("crafter.recipes", [](Context& ctx) {
    std::map<cr::Cell, ItemStack> g;
    g[{0, 0}] = item(ctx, "minecraft:oak_planks");
    g[{0, 1}] = item(ctx, "minecraft:oak_planks");
    auto r    = crafterlogic::tryToApplyRecipe(g);
    ctx.require(r.has_value(), "two planks stacked make sticks");
    ctx.check(ci::idOf(*r) == "minecraft:stick" && ci::countOf(*r) == 4, "4 sticks, got " + ci::idOf(*r));
    std::map<cr::Cell, ItemStack> one;
    one[{0, 0}] = item(ctx, "minecraft:oak_log");
    one[{1, 0}] = ci::empty(); // a covered / empty crafter next to it
    auto p      = crafterlogic::tryToApplyRecipe(one);
    ctx.check(p && ci::idOf(*p) == "minecraft:oak_planks" && ci::countOf(*p) == 4, "a log makes 4 planks (shapeless)");
    std::map<cr::Cell, ItemStack> bad;
    bad[{0, 0}] = item(ctx, "minecraft:dirt");
    bad[{3, 3}] = item(ctx, "minecraft:dirt");
    ctx.check(!crafterlogic::tryToApplyRecipe(bad), "no recipe for scattered dirt");
});

LV_REGISTER_SELFTEST("crafter.grid_craft", [](Context& ctx) -> ll::coro::CoroTask<> {
    // 2x2 crafters facing south; arrows lead to the bottom-left one, which points down into a chest
    auto base = co_await area(ctx, 1600);
    auto bl = base, br = at(base, 1, 0, 0), tl = at(base, 0, 1, 0), tr = at(base, 1, 1, 0);
    crafter(ctx, bl, cr::Pointing::Down);
    crafter(ctx, br, cr::Pointing::Left);
    crafter(ctx, tl, cr::Pointing::Down);
    crafter(ctx, tr, cr::Pointing::Down);
    auto chest = at(bl, 0, -1, 0);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest placed");
    power(ctx, bl, 32);
    co_await ctx.ticks(3);
    ctx.require(std::abs(kinetics::speedAt(br)) >= 32, "the grid turns (cogs), speed " + std::to_string(kinetics::speedAt(br)));
    auto& reg = region(ctx, bl);
    for (auto const& p : {bl, br, tl, tr}) {
        auto rest = ci::insert(reg, game::blockPos(p), 3, item(ctx, "minecraft:oak_planks", 2), false);
        ctx.check(ci::countOf(rest) == 1, "a crafter takes one item");
    }
    auto* s = crafterlogic::at(bl);
    ctx.require(s && s->phase != cr::Phase::Idle, "crafting started once the grid was full");
    bool done = co_await waitFor(400, [&] { return countIn(ctx, chest, "minecraft:crafting_table") > 0; });
    ctx.check(done, "the crafting table arrived in the chest");
    s = crafterlogic::at(bl);
    ctx.check(s && s->phase == cr::Phase::Idle, "back to idle");
    ctx.cleanupBlock(chest);
});

LV_REGISTER_SELFTEST("crafter.redstone_start", [](Context& ctx) -> ll::coro::CoroTask<> {
    // two crafters (right -> left), only the left one filled: redstone starts the incomplete craft
    auto a = co_await area(ctx, 1610);
    auto b = at(a, 1, 0, 0);
    crafter(ctx, a, cr::Pointing::Down);
    crafter(ctx, b, cr::Pointing::Left);
    auto chest = at(a, 0, -1, 0);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest placed");
    power(ctx, a, 64);
    co_await ctx.ticks(3);
    auto rest = ci::insert(region(ctx, a), game::blockPos(a), 3, item(ctx, "minecraft:oak_log"), false);
    ctx.check(ci::isEmpty(rest), "the log went in");
    co_await ctx.ticks(25);
    ctx.check(crafterlogic::at(a)->phase == cr::Phase::Idle, "an incomplete grid waits");
    auto rs = at(a, 0, 1, 0);
    ctx.require(ctx.setBlock(rs, "minecraft:redstone_block"), "redstone placed");
    bool done = co_await waitFor(300, [&] { return countIn(ctx, chest, "minecraft:oak_planks") >= 4; });
    ctx.check(done, "the incomplete craft made planks");
    ctx.cleanupBlock(chest);
});

LV_REGISTER_SELFTEST("crafter.connected_inputs", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto a = co_await area(ctx, 1620);
    auto b = at(a, 1, 0, 0), c = at(a, 0, 1, 0);
    crafter(ctx, a, cr::Pointing::Down);
    crafter(ctx, b, cr::Pointing::Left);
    crafter(ctx, c, cr::Pointing::Down);
    co_await ctx.ticks(1);
    ctx.check(crafterlogic::shouldConnect(a, Face::North, Face::East), "neighbours with the same facing connect");
    ctx.check(!crafterlogic::shouldConnect(a, Face::South, Face::East), "not from the front");
    crafterlogic::toggleConnection(a, b);
    crafterlogic::toggleConnection(a, c);
    auto& reg = region(ctx, a);
    ctx.check(ci::slotCount(reg, game::blockPos(b), -1) == 3, "the group has 3 slots");
    // unpowered: items are only stored; top row first, then left to right
    auto rest = ci::insert(reg, game::blockPos(b), -1, item(ctx, "minecraft:stone", 2), false);
    ctx.check(ci::isEmpty(rest), "both items went in");
    ctx.check(ci::idOf(crafterlogic::at(c)->slot) == "minecraft:stone", "the top crafter filled first");
    ctx.check(ci::idOf(crafterlogic::at(a)->slot) == "minecraft:stone", "then the bottom-left one");
    ctx.check(ci::isEmpty(crafterlogic::at(b)->slot), "the last one stays empty");
    crafterlogic::toggleConnection(a, b); // disconnect b again
    ctx.check(ci::slotCount(reg, game::blockPos(b), -1) == 1, "b is alone again");
    ctx.check(ci::slotCount(reg, game::blockPos(a), -1) == 2, "a and c stay connected");
    ctx.check(ci::isEmpty(ci::extract(reg, game::blockPos(a), -1, ci::any(), 64, true)), "no extraction");
    ctx.setBlock(a, "minecraft:air");
    co_await ctx.ticks(2);
    ctx.check(itemsAround(ctx, a, "minecraft:stone") >= 2, "breaking a crafter ejects the group's items");
    ctx.check(ci::slotCount(reg, game::blockPos(c), -1) == 1, "the removed crafter left its group");
});

LV_REGISTER_SELFTEST("crafter.cover", [](Context& ctx) -> ll::coro::CoroTask<> {
    // a covered crafter counts as a present (empty) cell: the log alone completes the grid
    auto a = co_await area(ctx, 1630);
    auto b = at(a, 1, 0, 0);
    crafter(ctx, a, cr::Pointing::Down);
    crafter(ctx, b, cr::Pointing::Left);
    auto chest = at(a, 0, -1, 0);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest placed");
    power(ctx, a, 64);
    co_await ctx.ticks(3);
    crafterlogic::at(b)->covered = true;
    crafterlogic::save(b);
    ctx.check(ci::countOf(ci::insert(region(ctx, a), game::blockPos(b), 3, item(ctx, "minecraft:stone"), true)) == 1,
              "a covered crafter takes nothing");
    auto rest = ci::insert(region(ctx, a), game::blockPos(a), 3, item(ctx, "minecraft:oak_log"), false);
    ctx.check(ci::isEmpty(rest), "the log went in");
    bool done = co_await waitFor(300, [&] { return countIn(ctx, chest, "minecraft:oak_planks") >= 4; });
    ctx.check(done, "the covered grid crafted planks without redstone");
    ctx.cleanupBlock(chest);
});

LV_REGISTER_SELFTEST("crafter.hand_use", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto a = co_await area(ctx, 1640);
    crafter(ctx, a, cr::Pointing::Up);
    co_await ctx.ticks(1);
    auto& player = ctx.player();
    player.setSelectedItem(item(ctx, "minecraft:oak_planks", 5));
    ctx.check(crafterlogic::use(player, a, static_cast<int>(Face::South), .5, .5, 1), "front use handled");
    ctx.check(ci::countOf(player.getSelectedItem()) == 4, "one plank inserted");
    ctx.check(ci::idOf(crafterlogic::at(a)->slot) == "minecraft:oak_planks", "the slot holds the plank");
    ctx.check(!crafterlogic::use(player, a, static_cast<int>(Face::North), .5, .5, 0), "the back does nothing");
    player.setSelectedItem(ci::empty());
    ctx.check(crafterlogic::use(player, a, static_cast<int>(Face::South), .5, .5, 1), "empty hand takes the item");
    ctx.check(ci::isEmpty(crafterlogic::at(a)->slot), "the slot is empty again");
});

LV_REGISTER_SELFTEST("crafter.saw_cutting", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 1650);
    ctx.require(ctx.setBlock(p, std::string(kSaw), {{kFacing, 1}, {kAlongFirst, 0}}), "saw placed");
    // drive it with a source beside it along x (the blade axis for along_first = 0 is z: put it behind)
    auto src = at(p, 0, 0, -1);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 2}}), "source placed");
    kinetics::refresh(src);
    kinetics::setGeneratedSpeed(src, 48);
    co_await ctx.ticks(3);
    ctx.require(std::abs(kinetics::speedAt(p)) >= 48, "the saw turns: " + std::to_string(kinetics::speedAt(p)));
    auto rest = ci::insert(region(ctx, p), game::blockPos(p), 0, item(ctx, "minecraft:oak_log", 3), false);
    ctx.check(ci::countOf(rest) == 2, "the saw takes one item at a time");
    bool done = co_await waitFor(200, [&] { return itemsAround(ctx, p, "minecraft:stripped_oak_log", 3, false) > 0; });
    ctx.check(done, "the log came out stripped");
    itemsAround(ctx, p, "", 4);
});

LV_REGISTER_SELFTEST("crafter.saw_filter", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 1660);
    ctx.require(ctx.setBlock(p, std::string(kSaw), {{kFacing, 1}, {kAlongFirst, 1}}), "saw placed");
    auto src = at(p, -1, 0, 0);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 0}}), "source placed");
    kinetics::refresh(src);
    kinetics::setGeneratedSpeed(src, 96);
    co_await ctx.ticks(3);
    auto options = sawrecipes::forInput(p, item(ctx, "minecraft:stone"));
    ctx.require(options.size() >= 2, "stone has several stonecutter cuts: " + std::to_string(options.size()));
    common::filterslot::set(p, item(ctx, "minecraft:stone_brick_stairs"));
    auto filtered = sawrecipes::forInput(p, item(ctx, "minecraft:stone"));
    ctx.require(filtered.size() == 1, "the filter picks one cut");
    ctx.check(ci::idOf(filtered[0].display) == "minecraft:stone_brick_stairs", "the filtered cut");
    auto rest = ci::insert(region(ctx, p), game::blockPos(p), 0, item(ctx, "minecraft:stone"), false);
    ctx.check(ci::isEmpty(rest), "stone went in");
    bool done = co_await waitFor(200, [&] { return itemsAround(ctx, p, "minecraft:stone_brick_stairs", 3, false) > 0; });
    ctx.check(done, "the saw made stairs");
    itemsAround(ctx, p, "", 4);
});

LV_REGISTER_SELFTEST("crafter.saw_fell_tree", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 1670);
    p.y -= 1;
    // a saw facing east cuts the trunk in front of it, the whole tree falls
    ctx.require(ctx.setBlock(p, std::string(kSaw), {{kFacing, 5}, {kAlongFirst, 1}}), "saw placed"); // axis y
    auto src = at(p, 0, -1, 0);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 1}}), "source placed");
    kinetics::refresh(src);
    kinetics::setGeneratedSpeed(src, 256);
    auto trunk = at(p, 1, 0, 0);
    for (int y = 0; y < 5; ++y) ctx.setBlock(at(trunk, 0, y, 0), "minecraft:oak_log");
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
            if (dx || dz) ctx.setBlock(at(trunk, dx, 4, dz), "minecraft:oak_leaves");
    bool done = co_await waitFor(200, [&] { return world::idAt(at(trunk, 0, 4, 0)) != "minecraft:oak_log"; });
    ctx.check(done, "the top log fell");
    ctx.check(world::idAt(trunk) != "minecraft:oak_log", "the cut log is gone");
    ctx.check(world::idAt(at(trunk, 1, 4, 0)) != "minecraft:oak_leaves", "the leaves fell too");
    co_await ctx.ticks(2);
    ctx.check(itemsAround(ctx, trunk, "minecraft:oak_log", 6) >= 5, "five logs dropped");
    itemsAround(ctx, trunk, "", 6);
});

LV_REGISTER_SELFTEST("crafter.drill_break", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 1680);
    ctx.require(ctx.setBlock(p, std::string(kDrill), {{kFacing, 5}}), "drill placed");
    auto src = at(p, -1, 0, 0);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 0}}), "source placed");
    kinetics::refresh(src);
    kinetics::setGeneratedSpeed(src, 64);
    auto target = at(p, 1, 0, 0);
    ctx.setBlock(target, "minecraft:stone");
    co_await ctx.ticks(3);
    ctx.require(std::abs(kinetics::speedAt(p)) >= 64, "the drill turns");
    bool done = co_await waitFor(80, [&] { return world::idAt(target) == "minecraft:air"; });
    ctx.check(done, "the stone broke");
    co_await ctx.ticks(1);
    ctx.check(itemsAround(ctx, target, "minecraft:cobblestone", 2) == 1, "it dropped cobblestone");
    ctx.setBlock(target, "minecraft:bedrock");
    co_await ctx.ticks(60);
    ctx.check(world::idAt(target) == "minecraft:bedrock", "bedrock stays");
    ctx.check(cr::drillDamage(64) == 2 + 2 + 1, "damage at 64 rpm");
});

LV_REGISTER_SELFTEST("crafter.drill_contraption", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace ct = feature::contraption;
    namespace cc = core::contraption;
    auto p = co_await area(ctx, 1690);
    // anchor stone, a drill facing east on it and a chest: glued, moved east into dirt
    ctx.setBlock(p, "minecraft:stone");
    auto drill = at(p, 0, 1, 0), chest = at(p, -1, 1, 0);
    ctx.require(ctx.setBlock(drill, std::string(kDrill), {{kFacing, 5}}), "drill placed");
    ctx.setBlock(chest, "minecraft:chest");
    auto dirt = at(drill, 2, 0, 0);
    ctx.setBlock(dirt, "minecraft:dirt");
    ct::addGlue(p.dim, cc::GlueBox::of({chest.x, p.y, chest.z}, {p.x, drill.y, p.z}));
    ct::AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    auto r          = ct::assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    auto id = *r.id;
    ct::Motion m;
    m.velocity = {0.25, 0, 0};
    m.ticks    = -1;
    ct::find(id)->setMotion(m);
    bool broke = co_await waitFor(120, [&] { return world::idAt(dirt) != "minecraft:dirt"; });
    ctx.check(broke, "the moving drill broke the dirt");
    co_await ctx.ticks(2);
    int stored = 0;
    if (auto* c = ct::find(id)) {
        c->stop();
        for (int i = 0; i < c->storage().slotCount(); ++i)
            if (ci::idOf(c->storage().slot(i)) == "minecraft:dirt") stored += ci::countOf(c->storage().slot(i));
    }
    ctx.check(stored == 1, "the dirt went into the contraption's chest: " + std::to_string(stored));
    co_await ctx.ticks(1);
    auto* c = ct::find(id);
    ctx.require(c != nullptr, "contraption alive");
    auto off = c->transform().offset;
    ct::disassemble(id);
    ct::clearGlue(p.dim, {p.x, p.y, p.z}, 8);
    int dx = static_cast<int>(std::lround(off.x));
    for (auto const& q : {p, drill, chest}) ctx.cleanupBlock(at(q, dx, 0, 0));
    co_await ctx.ticks(1);
    if (auto* reg = world::regionOf(p.dim))
        if (auto container = reg->tryGetContainer(game::blockPos(at(chest, dx, 0, 0)))) container->removeAllItems();
});

// ---- U39 (processing gaps): dx 3990..3999 -------------------------------------------------------------

LV_REGISTER_SELFTEST("crafter.saw_direct_input", [](Context& ctx) -> ll::coro::CoroTask<> {
    // R85: the saw hands its outputs to the block with Create's DirectBeltInputBehaviour next to it
    // (a depot, through the belts unit's direct input)
    auto p = co_await area(ctx, 3990);
    ctx.require(ctx.setBlock(p, std::string(kSaw), {{kFacing, 1}, {kAlongFirst, 0}}), "saw placed");
    auto src = at(p, 0, 0, -1);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", 2}}), "source placed");
    kinetics::refresh(src);
    kinetics::setGeneratedSpeed(src, 64);
    std::vector<GlobalPos> depots{at(p, 1, 0, 0), at(p, -1, 0, 0), at(p, 0, 0, 1)};
    for (auto const& d : depots) ctx.require(ctx.setBlock(d, "levitate:depot"), "a depot beside the saw");
    co_await ctx.ticks(3);
    auto rest = ci::insert(region(ctx, p), game::blockPos(p), 0, item(ctx, "minecraft:oak_log"), false);
    ctx.require(ci::isEmpty(rest), "the saw takes the log");
    auto onDepots = [&] {
        int n = 0;
        for (auto const& d : depots)
            for (auto const& t : belts::itemsAt(d))
                if (ci::idOf(t.stack) == "minecraft:stripped_oak_log") n += ci::countOf(t.stack);
        return n;
    };
    bool done = co_await waitFor(200, [&] { return onDepots() > 0; });
    ctx.check(done, "the stripped log went onto the depot the items move to");
    ctx.check(itemsAround(ctx, p, "minecraft:stripped_oak_log", 3, false) == 0, "nothing was dropped as an item entity");
    for (auto const& d : depots) (void)ci::extract(region(ctx, d), game::blockPos(d), 1, ci::any(), 64, false);
    itemsAround(ctx, p, "", 4);
});

LV_REGISTER_SELFTEST("crafter.saw_damage_source", [](Context& ctx) {
    // R88: Create's damage types of the saw and the drill name the death message
    ActorDamageSource saw(SharedTypes::Legacy::ActorDamageCause::Contact, kSawDeath);
    auto              msg = saw.getDeathMessage("Pig", nullptr);
    ctx.check(msg.first.find("mechanical_saw") != std::string::npos, "the saw's death message: " + msg.first);
});

} // namespace lv::feature::crafter
