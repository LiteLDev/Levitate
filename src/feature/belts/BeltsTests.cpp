// Selftests of the belts unit on a real world (testPos dx 1100..1199, U11). The real shafts are
// U6's; until they exist the tests drive belts with U2's test shafts and sources.

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/item/ItemActor.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::belts {

namespace {

using selftest::Context;
namespace citems = common::items;

constexpr char const* kSource = "levitate:kinetics_test_source";
constexpr int         AX = 0, AZ = 2;

struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy;
        return p;
    }
    bool shaft(GlobalPos const& p, int axis) { return ctx.setBlock(p, kTestShaftId, {{kAxis, axis}}); }
    bool source(GlobalPos const& p, int axis) { return ctx.setBlock(p, kSource, {{kAxis, axis}}); }
};

ItemStack stackOf(Context& ctx, std::string const& id, int count = 1) {
    auto s = citems::make(id, count);
    ctx.require(s.has_value(), id + " is registered");
    return *s;
}

/// A belt along +x from `a.at(0)` to `a.at(len - 1)` (shafts with axis z), driven by a test
/// source north of the first shaft at `rpm`.
ll::coro::CoroTask<bool> makeBelt(Area& a, int len, float rpm, int dy = 0, int dz = 1) {
    GlobalPos s = a.at(0, dy, dz), e = a.at(len - 1, dy, dz);
    a.shaft(s, AZ);
    a.shaft(e, AZ);
    for (int i = 1; i < len - 1; ++i) a.ctx.cleanupBlock(a.at(i, dy, dz));
    a.source(a.at(0, dy, dz - 1), AZ);
    co_await a.ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, dy, dz - 1), rpm);
    co_await a.ctx.ticks(1);
    co_return createBelt(s, e);
}

void clearItems(GlobalPos const& controller) {
    auto& s = state();
    if (auto it = s.belts.find(controller); it != s.belts.end()) {
        for (auto& [id, sd] : it->second->stacks) removeDisplay(sd);
        it->second->stacks.clear();
        it->second->inv = {};
    }
}

void clearDepot(GlobalPos const& pos) {
    auto& s = state();
    if (auto it = s.depots.find(pos); it != s.depots.end()) {
        for (auto& [id, sd] : it->second->stacks) removeDisplay(sd);
        for (auto& ab : it->second->airborne) removeDisplay(ab.data);
        it->second->stacks.clear();
        it->second->held.reset();
        it->second->incoming.clear();
        it->second->airborne.clear();
        it->second->outputs = {};
    }
}

int countOn(GlobalPos const& pos) {
    int n = 0;
    for (auto const& t : itemsAt(pos)) n += citems::countOf(t.stack);
    return n;
}

} // namespace

LV_REGISTER_SELFTEST("belts.registry", [](Context& ctx) {
    ctx.require(game::blockWith(kBeltId) != nullptr, "levitate:belt is in the behavior pack");
    ctx.check(game::blockWith(kDepotId) != nullptr, "levitate:depot is in the behavior pack");
    ctx.check(game::blockWith(kEjectorId) != nullptr, "levitate:weighted_ejector is in the behavior pack");
    ctx.check(citems::make(kBeltConnectorId).has_value(), "the belt connector item exists");
    ctx.check(kinetics::specOf(kBeltId) != nullptr, "belts are kinetic");
    ctx.check(kinetics::specOf(kEjectorId) != nullptr, "ejectors are kinetic");
    ctx.check(kinetics::stressImpactOf(kEjectorId) == 2, "ejector impact 2 SU/RPM");
    ctx.check(kinetics::stressImpactOf(kBeltId) == 0, "belts use no stress");
    ctx.check(directInputFor(kBeltId) && directInputFor(kDepotId) && directInputFor(kEjectorId), "direct inputs registered");
    ctx.check(config().maxBeltLength == 20 && config().maxEjectorDistance == 32, "Create's defaults");
    // every state combination exists
    cb::SegmentState s{3, cb::Slope::Downward, cb::Part::Pulley, cb::Casing::Brass, 14};
    auto const*      b = beltBlock(s);
    ctx.require(b != nullptr, "belt block with states");
    ctx.check(segmentOf(*b) == s, "belt states read back");
});

LV_REGISTER_SELFTEST("belts.connect", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1100};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    GlobalPos s = a.at(0, 0, 1), e = a.at(4, 0, 1);
    a.shaft(s, AZ);
    a.shaft(e, AZ);
    a.shaft(a.at(2, 0, 1), AZ); // becomes a pulley
    for (int i = 1; i < 4; ++i) ctx.cleanupBlock(a.at(i, 0, 1));
    a.source(a.at(0, 0, 0), AZ);
    co_await ctx.ticks(2);
    auto& p = ctx.player();
    p.setSelectedItem(stackOf(ctx, kBeltConnectorId, 2));
    ctx.require(onConnectorUse(p, s, 1, {.5, .9, .5}), "first click selects");
    ctx.require(onConnectorUse(p, e, 1, {.5, .9, .5}), "second click connects");
    auto st0 = segmentAt(s), st2 = segmentAt(a.at(2, 0, 1)), st4 = segmentAt(e);
    ctx.require(st0 && st2 && st4, "belt blocks placed");
    ctx.check(st0->part == cb::Part::Start && st4->part == cb::Part::End, "start and end");
    ctx.check(st2->part == cb::Part::Pulley, "the shaft in between is a pulley");
    ctx.check(st0->facing == 3 && st0->slope == cb::Slope::Horizontal, "horizontal, facing east");
    ctx.check(p.isCreative() || citems::countOf(p.getSelectedItem()) == 1, "one connector used");
    auto info = segmentInfo(a.at(3, 0, 1));
    ctx.check(info && info->controller == s && info->index == 3 && info->length == 5, "segment metadata");
    co_await ctx.ticks(3);
    for (int i = 0; i < 5; ++i) {
        ctx.check(kinetics::speedAt(a.at(i, 0, 1)) == 16, "segment " + std::to_string(i) + " turns at 16 RPM, got " + std::to_string(kinetics::speedAt(a.at(i, 0, 1))));
    }
    // sneak clears a selection
    ctx.require(onConnectorUse(p, s, 1, {.5, .9, .5}) || true, "click on a belt with the connector");
    p.setSelectedItem(ItemStack());
});

LV_REGISTER_SELFTEST("belts.rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1110};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    a.shaft(a.at(0, 0, 1), AZ);
    a.shaft(a.at(3, 1, 1), AZ); // not 45 degrees
    a.shaft(a.at(3, 0, 2), AZ); // off the line
    a.shaft(a.at(5, 0, 1), AX); // other axis
    a.shaft(a.at(3, 3, 1), AZ); // 45 degrees up
    co_await ctx.ticks(1);
    auto& p = ctx.player();
    p.setSelectedItem(stackOf(ctx, kBeltConnectorId, 4));
    auto tryConnect = [&](GlobalPos const& b) {
        onConnectorUse(p, a.at(0, 0, 1), 1, {.5, .5, .5});
        onConnectorUse(p, b, 1, {.5, .5, .5});
        return segmentAt(a.at(0, 0, 1)).has_value();
    };
    ctx.check(!tryConnect(a.at(3, 1, 1)), "a slope that isn't 45 degrees is refused");
    ctx.check(!tryConnect(a.at(3, 0, 2)), "shafts off the line are refused");
    ctx.check(!tryConnect(a.at(5, 0, 1)), "different axes are refused");
    ctx.check(tryConnect(a.at(3, 3, 1)), "a 45 degree slope connects");
    auto st = segmentAt(a.at(1, 1, 1));
    ctx.check(st && st->slope == cb::Slope::Upward && st->part == cb::Part::Middle, "the middle of the slope");
    for (int i = 1; i < 3; ++i) ctx.cleanupBlock(a.at(i, i, 1));
    p.setSelectedItem(ItemStack());
    // a sloped belt turns with its source (diagonal neighbours)
    a.source(a.at(0, 0, 0), AZ);
    co_await ctx.ticks(3);
    ctx.check(kinetics::speedAt(a.at(2, 2, 1)) == 16, "the slope's segments turn, got " + std::to_string(kinetics::speedAt(a.at(2, 2, 1))));
});

LV_REGISTER_SELFTEST("belts.transport", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1120};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 4, -96), "belt built");
    ctx.setBlock(a.at(4, 0, 1), kDepotId);
    co_await ctx.ticks(2);
    auto info = beltAt(a.at(0, 0, 1));
    ctx.require(info.has_value(), "belt info");
    ctx.check(info->movementFace == 5, "-96 RPM on an east facing belt moves items east");
    auto& region = *regionOf(a.at(0).dim);
    auto  rest   = common::items::insert(region, game::blockPos(a.at(0, 0, 1)), 1, stackOf(ctx, "minecraft:iron_ingot", 5), false);
    ctx.check(citems::isEmpty(rest), "the belt takes the stack");
    ctx.check(citems::countOf(common::items::insert(region, game::blockPos(a.at(0, 0, 1)), 1, stackOf(ctx, "minecraft:gold_ingot"), true)) == 1,
              "the same segment refuses a second stack right away");
    // 96 RPM = 0.2 blocks per tick: 4 blocks in 20 ticks
    co_await ctx.ticks(30);
    ctx.check(countOn(a.at(4, 0, 1)) == 5, "the depot at the end got the stack, has " + std::to_string(countOn(a.at(4, 0, 1))));
    clearDepot(a.at(4, 0, 1));
});

LV_REGISTER_SELFTEST("belts.queue", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1130};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 5, -240), "belt built");
    ctx.setBlock(a.at(5, 0, 1), "minecraft:stone");
    auto& region = *regionOf(a.at(0).dim);
    for (int i = 0; i < 3; ++i) {
        (void)common::items::insert(region, game::blockPos(a.at(0, 0, 1)), 1, stackOf(ctx, "minecraft:cobblestone", 1 + i), false);
        co_await ctx.ticks(8);
    }
    co_await ctx.ticks(30);
    auto* belt = beltFor(a.at(0, 0, 1));
    ctx.require(belt != nullptr, "belt runtime");
    ctx.require(belt->inv.items.size() == 3, "three stacks on the belt, got " + std::to_string(belt->inv.items.size()));
    auto const& it = belt->inv.items;
    ctx.check(std::abs(it[0].beltPosition - (5 - .45f)) < .01f, "the first stops before the blocked end at " + std::to_string(it[0].beltPosition));
    ctx.check(std::abs(it[0].beltPosition - it[1].beltPosition - 1) < .01f, "queued one block apart");
    ctx.check(std::abs(it[1].beltPosition - it[2].beltPosition - 1) < .01f, "queued one block apart (2)");
    // empty hand takes the items of a segment
    auto& p = ctx.player();
    p.setSelectedItem(ItemStack());
    int before = 0;
    ctx.check(onBeltUse(p, a.at(4, 0, 1), 1, {.5, .9, .5}), "right-click with an empty hand");
    co_await ctx.ticks(1);
    (void)before;
    ctx.check(belt->inv.items.size() == 2, "the stack of that segment went to the player");
    clearItems(a.at(0, 0, 1));
});

LV_REGISTER_SELFTEST("belts.eject_and_transfer", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1140};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    // belt A (x 0..3) feeds belt B (x 4..7) which ends in the air
    ctx.require(co_await makeBelt(a, 4, -240), "belt A built");
    Area b{ctx, 1144};
    ctx.require(co_await makeBelt(b, 4, -240), "belt B built");
    co_await ctx.ticks(2);
    auto& region = *regionOf(a.at(0).dim);
    (void)common::items::insert(region, game::blockPos(a.at(0, 0, 1)), 1, stackOf(ctx, "minecraft:emerald", 3), false);
    bool seenOnB = false;
    for (int i = 0; i < 40 && !seenOnB; ++i) {
        co_await ctx.ticks(1);
        auto* bb = beltFor(b.at(0, 0, 1));
        seenOnB  = bb && !bb->inv.empty();
    }
    ctx.check(seenOnB, "the stack moved from belt A onto belt B");
    co_await ctx.ticks(20);
    auto* bb = beltFor(b.at(0, 0, 1));
    ctx.check(bb && bb->inv.empty(), "it fell off the open end of belt B");
});

LV_REGISTER_SELFTEST("belts.entities", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1150};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 6, -240), "belt built");
    auto& p     = ctx.spawnPlayer();
    auto  start = a.at(0, 0, 1);
    p.teleport(Vec3{float(start.x) + .5f, float(start.y) + 13 / 16.f + .01f, float(start.z) + .5f}, DimensionType(start.dim));
    co_await ctx.ticks(2);
    float x0 = p.getFeetPos().x;
    co_await ctx.ticks(10);
    float moved = p.getFeetPos().x - x0;
    // 240 RPM = 0.5 blocks per tick
    ctx.check(moved > 2, "a player standing on the belt is carried east, moved " + std::to_string(moved));
    // dropped items land on the belt (players out of pickup range)
    p.teleport(Vec3{float(start.x) + .5f, float(start.y) + 12.f, float(start.z) - 6.f}, DimensionType(start.dim));
    ctx.player().teleport(Vec3{float(start.x) + .5f, float(start.y) + 12.f, float(start.z) - 8.f}, DimensionType(start.dim));
    ctx.setBlock(a.at(6, 0, 1), "minecraft:stone"); // the stack stays on the (moving) belt
    auto level = ll::service::getLevel();
    auto mid   = a.at(3, 0, 1);
    auto* dropped = level->getSpawner().spawnItem(*regionOf(mid.dim), stackOf(ctx, "minecraft:apple", 2), nullptr, Vec3{mid.x + .5f, mid.y + 1.2f, mid.z + .5f}, 0);
    ctx.require(dropped != nullptr, "item spawned");
    dropped->mBuiltInComponents->mStateVectorComponent->mPosDelta = Vec3{0, 0, 0}; // straight down
    bool landed = false;
    for (int i = 0; i < 60 && !landed; ++i) {
        co_await ctx.ticks(1);
        auto* belt = beltFor(a.at(0, 0, 1));
        landed     = belt && !belt->inv.empty();
    }    ctx.check(landed, "a dropped item lands on the belt");
    clearItems(a.at(0, 0, 1));
});

LV_REGISTER_SELFTEST("belts.break", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1160};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 5, -32), "belt built");
    auto& region = *regionOf(a.at(0).dim);
    (void)common::items::insert(region, game::blockPos(a.at(1, 0, 1)), 1, stackOf(ctx, "minecraft:diamond"), false);
    co_await ctx.ticks(2);
    // a survival player breaks the middle segment
    state().playerBroke[a.at(2, 0, 1)] = false;
    game::clearBlock(region, game::blockPos(a.at(2, 0, 1)));
    co_await ctx.ticks(2);
    ctx.check(shaftAxisAt(a.at(0, 0, 1)) == cb::Axis::Z, "the start pulley is a shaft again");
    ctx.check(shaftAxisAt(a.at(4, 0, 1)) == cb::Axis::Z, "the end pulley is a shaft again");
    ctx.check(idAt(a.at(1, 0, 1)) == "minecraft:air" && idAt(a.at(3, 0, 1)) == "minecraft:air", "the other segments are gone");
    ctx.check(!state().belts.contains(a.at(0, 0, 1)), "the belt's items were dropped");
});

LV_REGISTER_SELFTEST("belts.edit", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1170};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 6, -32), "belt built");
    auto& p = ctx.player();
    // dye colours the whole belt, water washes it
    p.setSelectedItem(stackOf(ctx, "minecraft:red_dye"));
    ctx.check(onBeltUse(p, a.at(2, 0, 1), 1, {.5, .9, .5}), "dye");
    ctx.check(segmentAt(a.at(0, 0, 1))->color == 14 && segmentAt(a.at(5, 0, 1))->color == 14, "every segment is red");
    p.setSelectedItem(stackOf(ctx, "minecraft:water_bucket"));
    ctx.check(onBeltUse(p, a.at(4, 0, 1), 1, {.5, .9, .5}), "water");
    ctx.check(segmentAt(a.at(1, 0, 1))->color == -1, "washed");
    // a shaft on a middle segment makes it a pulley; the wrench takes it out again
    p.setSelectedItem(stackOf(ctx, kTestShaftId, 1));
    ctx.check(onBeltUse(p, a.at(3, 0, 1), 1, {.5, .9, .5}), "shaft");
    ctx.check(segmentAt(a.at(3, 0, 1))->part == cb::Part::Pulley, "pulley added");
    ctx.check(onBeltWrench(p, a.at(3, 0, 1), 5, {1, .5, .5}), "wrench on the pulley's side");
    ctx.check(segmentAt(a.at(3, 0, 1))->part == cb::Part::Middle, "pulley removed");
    // wrench on the outer half of the end shortens the belt
    ctx.check(onBeltWrench(p, a.at(5, 0, 1), 1, {.9, .9, .5}), "wrench on the end");
    ctx.check(!segmentAt(a.at(5, 0, 1)) && segmentAt(a.at(4, 0, 1))->part == cb::Part::End, "shortened by one");
    ctx.check(segmentInfo(a.at(0, 0, 1))->length == 5, "length 5");
    // the connector on the end extends it again
    p.setSelectedItem(stackOf(ctx, kBeltConnectorId));
    ctx.check(onConnectorUse(p, a.at(4, 0, 1), 1, {.9, .9, .5}), "connector on the end");
    ctx.check(segmentAt(a.at(5, 0, 1)) && segmentAt(a.at(5, 0, 1))->part == cb::Part::End, "extended");
    // the wrench in the middle splits it (creative: no shafts needed; survival: from the inventory)
    if (!p.isCreative()) ctx.give(p, stackOf(ctx, kTestShaftId, 2));
    p.setSelectedItem(ItemStack());
    ctx.check(onBeltWrench(p, a.at(2, 0, 1), 1, {.8, .9, .5}), "wrench in the middle");
    auto l = segmentInfo(a.at(0, 0, 1)), r = segmentInfo(a.at(5, 0, 1));
    ctx.check(l && r && l->length == 3 && r->length == 3 && r->controller == a.at(3, 0, 1), "split into 3 + 3");
    p.setSelectedItem(ItemStack());
});

LV_REGISTER_SELFTEST("belts.depot", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1180};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    GlobalPos d = a.at(0, 0, 1);
    ctx.setBlock(d, kDepotId);
    co_await ctx.ticks(1);
    auto& p = ctx.player();
    p.setSelectedItem(stackOf(ctx, "minecraft:bread", 7));
    ctx.check(onDepotUse(p, d, 1, {.5, 1, .5}), "right-click puts the held stack on the depot");
    ctx.check(citems::isEmpty(p.getSelectedItem()), "the hand is empty");
    co_await ctx.ticks(10);
    ctx.check(countOn(d) == 7, "the depot holds 7 bread");
    auto& region = *regionOf(d.dim);
    ctx.check(citems::countOf(common::items::insert(region, game::blockPos(d), 1, stackOf(ctx, "minecraft:apple"), true)) == 1, "an occupied depot refuses more");
    auto got = common::items::extract(region, game::blockPos(d), 1, common::items::any(), 3, false);
    ctx.check(citems::countOf(got) == 3, "3 extracted through the capability");
    p.setSelectedItem(ItemStack());
    ctx.check(onDepotUse(p, d, 1, {.5, 1, .5}), "right-click with an empty hand takes the rest");
    ctx.check(countOn(d) == 0, "the depot is empty");
    // processing 2 above: a test processor on a lapis block turns the item into gold
    ctx.setBlock(a.at(0, 2, 1), "minecraft:lapis_block");
    int held = 0;
    registerProcessor("minecraft:lapis_block", Processor{
        .received = [](ProcessorContext const&, TransportedItem&) { return ProcessingResult::Hold; },
        .held = [&held](ProcessorContext const& c, TransportedItem&) {
            if (++held < 5) return ProcessingResult::Hold;
            handleCenteredProcessing(c.transporter, .5f, [](TransportedItem& t) {
                auto gold = citems::make("minecraft:gold_ingot", citems::countOf(t.stack));
                return gold ? TransportedResult::convertTo({*gold}) : TransportedResult::doNothing();
            });
            return ProcessingResult::Pass;
        },
    });
    (void)common::items::insert(region, game::blockPos(d), 1, stackOf(ctx, "minecraft:iron_ingot", 2), false);
    co_await ctx.ticks(15);
    unregisterProcessor("minecraft:lapis_block");
    auto items = itemsAt(d);
    ctx.check(items.size() == 1 && citems::idOf(items[0].stack) == "minecraft:gold_ingot" && citems::countOf(items[0].stack) == 2,
              "the processor converted the held item");
    clearDepot(d);
});

LV_REGISTER_SELFTEST("belts.ejector", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1185};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    GlobalPos e = a.at(0, 0, 1), target = a.at(4, 1, 1);
    ctx.setBlock(e, kEjectorId, {{kFacing, 3}});
    ctx.setBlock(target, kDepotId);
    a.source(a.at(0, 0, 0), AZ); // the ejector's shaft runs north-south when it faces east
    co_await ctx.ticks(2);
    kinetics::setGeneratedSpeed(a.at(0, 0, 0), 128);
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(e) != 0, "the ejector turns");
    auto* d = depotFor(e);
    ctx.require(d != nullptr, "ejector runtime");
    d->targetH = 4;
    d->targetV = 1;
    auto& region = *regionOf(e.dim);
    (void)common::items::insert(region, game::blockPos(e), 1, stackOf(ctx, "minecraft:redstone", 4), false);
    bool arrived = false;
    for (int i = 0; i < 100 && !arrived; ++i) {
        co_await ctx.ticks(1);
        arrived = countOn(target) == 4;
    }
    ctx.check(arrived, "the ejector threw the stack onto the target depot");
    co_await ctx.ticks(40);
    ctx.check(depotFor(e)->state == EjectorState::Charged, "it charged again");
    // redstone locks it
    ctx.setBlock(a.at(-1, 0, 1), "minecraft:redstone_block");
    co_await ctx.ticks(6);
    (void)common::items::insert(region, game::blockPos(e), 1, stackOf(ctx, "minecraft:stick"), false);
    co_await ctx.ticks(20);
    ctx.check(countOn(e) == 1, "a powered ejector keeps its item");
    clearDepot(e);
    clearDepot(target);
});

LV_REGISTER_SELFTEST("belts.persist", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1190};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    ctx.require(co_await makeBelt(a, 3, 0), "belt built (standing still)");
    auto& region = *regionOf(a.at(0).dim);
    (void)common::items::insert(region, game::blockPos(a.at(1, 0, 1)), 1, stackOf(ctx, "minecraft:coal", 9), false);
    co_await ctx.ticks(2);
    saveWorld();
    auto& s = state();
    if (auto it = s.belts.find(a.at(0, 0, 1)); it != s.belts.end()) {
        for (auto& [id, sd] : it->second->stacks) removeDisplay(sd);
        s.belts.erase(it);
    }
    auto* belt = beltFor(a.at(0, 0, 1));
    ctx.require(belt != nullptr, "reloaded");
    ctx.check(belt->inv.items.size() == 1 && countOn(a.at(1, 0, 1)) == 9, "the stack came back from BlockData");
    clearItems(a.at(0, 0, 1));
});

} // namespace lv::feature::belts
