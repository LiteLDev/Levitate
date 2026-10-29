// Selftests of the actors unit (U22, testPos dx 2200..2299). Each test builds a small glued
// structure carrying an actor, moves it with the contraption test anchor over prepared ground
// and checks the world (and contraption storage) afterwards.

#include <cmath>
#include <string>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/actors/Actors.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

namespace lv::feature::actors {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;
namespace fs = ::lv::feature::common::filterslot;

GP at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
cc::Pos ccp(GP const& p) { return {p.x, p.y, p.z}; }

BlockSource& region(Context& ctx) { return ctx.player().getDimensionBlockSource(); }
std::string  idAt(Context& ctx, GP const& p) { return game::blockIdAt(region(ctx), toBlockPos(p)); }
int          stateAt(Context& ctx, GP const& p, std::string const& s) {
    return game::intState(region(ctx).getBlock(toBlockPos(p)), s).value_or(-1);
}

/// Clears a box to air (and remembers it for cleanup).
void clearBox(Context& ctx, GP const& a, GP const& b) {
    for (int x = a.x; x <= b.x; ++x)
        for (int y = a.y; y <= b.y; ++y)
            for (int z = a.z; z <= b.z; ++z) ctx.setBlock({a.dim, x, y, z}, "minecraft:air");
}

/// Fills a box with a block (cleanup restores air).
void fillBox(Context& ctx, GP const& a, GP const& b, std::string const& id, std::vector<std::pair<std::string, int>> const& states = {}) {
    for (int x = a.x; x <= b.x; ++x)
        for (int y = a.y; y <= b.y; ++y)
            for (int z = a.z; z <= b.z; ++z) ctx.setBlock({a.dim, x, y, z}, id, states);
}

int countIn(Context& ctx, GP const& chest, std::string const& id) {
    auto c = region(ctx).tryGetContainer(toBlockPos(chest));
    if (!c) return -1;
    int n = 0;
    for (int i = 0; i < c->getContainerSize(); ++i) {
        auto const& s = c->getItem(i);
        if (!s.isNull() && s.getTypeName() == id) n += s.mCount;
    }
    return n;
}

void emptyChest(Context& ctx, GP const& chest) {
    if (auto c = region(ctx).tryGetContainer(toBlockPos(chest))) c->removeAllItems();
}

con::AssembleResult assembleAt(GP const& anchor) {
    con::AssembleOptions o;
    o.anchor        = anchor;
    o.includeAnchor = true;
    return con::assemble(o);
}

ll::coro::CoroTask<> moveBy(Context& ctx, con::Id id, cc::Vec3d delta, int ticks, int extraWait = 3) {
    if (auto* c = con::find(id)) {
        con::Motion m;
        m.velocity = delta * (1.0 / ticks);
        m.ticks    = ticks;
        c->setMotion(m);
    }
    co_await ctx.ticks(ticks + extraWait);
}

/// Waits until the contraption's motion has run out (stalls make it take longer).
ll::coro::CoroTask<> waitStopped(Context& ctx, con::Id id, int maxTicks) {
    for (int i = 0; i < maxTicks; ++i) {
        auto* c = con::find(id);
        if (!c || c->motion().ticks <= 0) break;
        co_await ctx.ticks(1);
    }
    co_await ctx.ticks(2);
}

struct Guard {
    std::optional<con::Id> id;
    ~Guard() {
        if (id && con::find(*id)) con::disassemble(*id);
    }
};

Actor* spawnMob(Context& ctx, std::string const& type, GP const& p) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto owner = level->getActorFactory().createSpawnedActor(
        ActorDefinitionIdentifier(type),
        nullptr,
        Vec3{float(p.x + .5), float(p.y), float(p.z + .5)},
        Vec2{0, 0}
    );
    if (!owner) return nullptr;
    return level->addEntity(region(ctx), std::move(owner));
}

/// Structure for the actor tests: actor at `a`, stone above it (anchor), chest beside the stone
/// (on the -x side), glued. The actor faces east (+x).
struct Rig {
    GP actor, anchor, chest;
};
Rig buildRig(Context& ctx, GP const& actorPos, std::string const& actorId) {
    Rig r{actorPos, at(actorPos, 0, 1, 0), at(actorPos, -1, 1, 0)};
    ctx.setBlock(r.actor, actorId, {{"levitate:facing", 5}});
    ctx.setBlock(r.anchor, "minecraft:stone");
    ctx.setBlock(r.chest, "minecraft:chest");
    con::addGlue(actorPos.dim, cc::GlueBox::of(ccp(at(r.actor, -1, 0, 0)), ccp(r.anchor)));
    return r;
}

} // namespace

LV_REGISTER_SELFTEST("actors.harvester", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2200);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 7, 4, 1));
    fillBox(ctx, at(g0, 2, 0, 0), at(g0, 5, 0, 0), "minecraft:farmland");
    ctx.setBlock(at(g0, 2, 1, 0), "minecraft:wheat", {{"growth", 7}});
    ctx.setBlock(at(g0, 3, 1, 0), "minecraft:wheat", {{"growth", 3}});
    ctx.setBlock(at(g0, 4, 1, 0), "minecraft:wheat", {{"growth", 7}});
    auto rig = buildRig(ctx, at(g0, 1, 1, 0), std::string(kHarvester));
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    ctx.check(con::find(*r.id)->blocks().size() == 3, "harvester, stone and chest assembled");
    co_await moveBy(ctx, *r.id, {4, 0, 0}, 20);
    ctx.check(stateAt(ctx, at(g0, 2, 1, 0), "growth") == 0, "mature wheat harvested and replanted");
    ctx.check(stateAt(ctx, at(g0, 3, 1, 0), "growth") == 3, "growing wheat left alone");
    ctx.check(stateAt(ctx, at(g0, 4, 1, 0), "growth") == 0, "second mature wheat harvested");
    auto& st    = con::find(*r.id)->storage();
    int   wheat = 0;
    for (int i = 0; i < st.slotCount(); ++i)
        if (!st.slot(i).isNull() && st.slot(i).getTypeName() == "minecraft:wheat") wheat += st.slot(i).mCount;
    ctx.check(wheat == 2, "the wheat went into contraption storage: " + std::to_string(wheat));
    for (int x = 2; x <= 5; ++x) ctx.cleanupBlock(at(g0, x + 0, 2, 0));
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    ctx.check(idAt(ctx, at(g0, 5, 1, 0)) == kHarvester, "harvester placed back");
    ctx.cleanupBlock(at(g0, 5, 1, 0));
    ctx.cleanupBlock(at(g0, 5, 2, 0));
    ctx.cleanupBlock(at(g0, 4, 2, 0));
    ctx.check(countIn(ctx, at(g0, 4, 2, 0), "minecraft:wheat") == 2, "the wheat is in the chest after disassembly");
    emptyChest(ctx, at(g0, 4, 2, 0));
});

LV_REGISTER_SELFTEST("actors.harvester_backwards", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2210);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -3, 1, -1), at(g0, 3, 4, 1));
    fillBox(ctx, at(g0, -3, 0, 0), at(g0, 0, 0, 0), "minecraft:farmland");
    ctx.setBlock(at(g0, -1, 1, 0), "minecraft:wheat", {{"growth", 7}});
    auto rig = buildRig(ctx, at(g0, 1, 1, 0), std::string(kHarvester));
    rig.chest = rig.chest; // chest at x 0
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    // moving towards its back: inactive (Create's isActive)
    co_await moveBy(ctx, *r.id, {-1, 0, 0}, 10);
    ctx.check(stateAt(ctx, at(g0, -1, 1, 0), "growth") == 7, "no harvest while moving backwards");
    co_await moveBy(ctx, *r.id, {1, 0, 0}, 10);
    ctx.cleanupBlock(rig.actor);
    ctx.cleanupBlock(rig.anchor);
    ctx.cleanupBlock(rig.chest);
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
});

LV_REGISTER_SELFTEST("actors.plough", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2220);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 7, 4, 1));
    fillBox(ctx, at(g0, 2, 0, 0), at(g0, 5, 0, 0), "minecraft:grass_block");
    // water beside the row keeps the new farmland moist: dry farmland without a crop turns back
    // into dirt on a random tick (made the test flaky in long runs)
    for (auto const& p : {at(g0, 2, 0, 1), at(g0, 4, 0, 1), at(g0, 3, 0, 2), at(g0, 3, -1, 1)}) ctx.setBlock(p, "minecraft:stone");
    ctx.setBlock(at(g0, 3, 0, 1), "minecraft:water");
    ctx.setBlock(at(g0, 3, 1, 0), "minecraft:short_grass");
    ctx.setBlock(at(g0, 4, 1, 0), "minecraft:torch");
    auto  rig = buildRig(ctx, at(g0, 1, 1, 0), std::string(kPlough));
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    co_await moveBy(ctx, *r.id, {4, 0, 0}, 20);
    co_await waitStopped(ctx, *r.id, 60);
    ctx.check(idAt(ctx, at(g0, 3, 1, 0)) == "minecraft:air", "grass broken: " + idAt(ctx, at(g0, 3, 1, 0)));
    ctx.check(idAt(ctx, at(g0, 4, 1, 0)) == "minecraft:air", "torch broken: " + idAt(ctx, at(g0, 4, 1, 0)));
    ctx.check(idAt(ctx, at(g0, 2, 0, 0)) == "minecraft:farmland", "ground tilled: " + idAt(ctx, at(g0, 2, 0, 0)));
    ctx.check(idAt(ctx, at(g0, 3, 0, 0)) == "minecraft:farmland", "ground under the grass tilled: " + idAt(ctx, at(g0, 3, 0, 0)));
    auto& st      = con::find(*r.id)->storage();
    int   torches = 0;
    for (int i = 0; i < st.slotCount(); ++i)
        if (!st.slot(i).isNull() && st.slot(i).getTypeName() == "minecraft:torch") torches += st.slot(i).mCount;
    ctx.check(torches == 1, "the torch went into storage: " + std::to_string(torches));
    for (int x = 2; x <= 5; ++x) {
        ctx.cleanupBlock(at(g0, x, 1, 0));
        ctx.cleanupBlock(at(g0, x, 2, 0));
    }
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    emptyChest(ctx, at(g0, 4, 2, 0));
});

LV_REGISTER_SELFTEST("actors.roller_fill", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2230);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 7, 5, 1));
    fillBox(ctx, at(g0, 0, -3, 0), at(g0, 6, 0, 0), "minecraft:dirt");
    clearBox(ctx, at(g0, 3, -1, 0), at(g0, 3, 0, 0)); // a 2 deep hole
    auto rig = buildRig(ctx, at(g0, 1, 2, 0), std::string(kRoller));
    setRollerMode(rig.actor, RollingMode::StraightFill, false);
    fs::set(rig.actor, makeItem("minecraft:stone"), "filter");
    if (auto c = region(ctx).tryGetContainer(toBlockPos(rig.chest))) {
        auto s = makeItem("minecraft:stone", 10);
        c->setItem(0, s);
    }
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    co_await moveBy(ctx, *r.id, {4, 0, 0}, 40);
    co_await waitStopped(ctx, *r.id, 80);
    ctx.check(idAt(ctx, at(g0, 3, 0, 0)) == "minecraft:stone", "hole filled at the top: " + idAt(ctx, at(g0, 3, 0, 0)));
    ctx.check(idAt(ctx, at(g0, 3, -1, 0)) == "minecraft:stone", "and below: " + idAt(ctx, at(g0, 3, -1, 0)));
    ctx.check(idAt(ctx, at(g0, 2, 0, 0)) == "minecraft:dirt", "solid ground left as it is");
    int stone = 0;
    auto& st  = con::find(*r.id)->storage();
    for (int i = 0; i < st.slotCount(); ++i)
        if (!st.slot(i).isNull() && st.slot(i).getTypeName() == "minecraft:stone") stone += st.slot(i).mCount;
    ctx.check(stone == 8, "two stone used from storage, left: " + std::to_string(stone));
    for (int x = 1; x <= 6; ++x)
        for (int y = 2; y <= 3; ++y) ctx.cleanupBlock(at(g0, x, y, 0));
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    emptyChest(ctx, at(g0, 4, 3, 0));
});

LV_REGISTER_SELFTEST("actors.roller_tunnel", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2240);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 7, 5, 1));
    fillBox(ctx, at(g0, 0, -1, 0), at(g0, 6, 0, 0), "minecraft:dirt");
    ctx.setBlock(at(g0, 3, 1, 0), "minecraft:dirt"); // a bump in the path
    auto rig = buildRig(ctx, at(g0, 1, 2, 0), std::string(kRoller));
    ctx.check(rollerMode(rig.actor) == RollingMode::TunnelPave, "default mode: clear blocks and pave");
    fs::set(rig.actor, makeItem("minecraft:cobblestone"), "filter");
    if (auto c = region(ctx).tryGetContainer(toBlockPos(rig.chest))) {
        auto s = makeItem("minecraft:cobblestone", 16);
        c->setItem(0, s);
    }
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    co_await moveBy(ctx, *r.id, {4, 0, 0}, 40);
    co_await waitStopped(ctx, *r.id, 200);
    ctx.check(idAt(ctx, at(g0, 3, 1, 0)) == "minecraft:air", "the bump was cleared: " + idAt(ctx, at(g0, 3, 1, 0)));
    ctx.check(idAt(ctx, at(g0, 3, 0, 0)) == "minecraft:cobblestone", "the path paved: " + idAt(ctx, at(g0, 3, 0, 0)));
    ctx.check(idAt(ctx, at(g0, 2, 0, 0)) == "minecraft:cobblestone", "the path paved before it: " + idAt(ctx, at(g0, 2, 0, 0)));
    for (int x = 1; x <= 6; ++x)
        for (int y = 2; y <= 3; ++y) ctx.cleanupBlock(at(g0, x, y, 0));
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    emptyChest(ctx, at(g0, 4, 3, 0));
});

LV_REGISTER_SELFTEST("actors.roller_settings", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2250);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    auto a = at(g0, 0, 1, 0), b = at(g0, 1, 1, 0), c = at(g0, 2, 1, 0);
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 3, 2, 1));
    ctx.setBlock(a, std::string(kRoller), {{"levitate:facing", 2}});
    ctx.setBlock(b, std::string(kRoller), {{"levitate:facing", 2}});
    ctx.setBlock(c, std::string(kRoller), {{"levitate:facing", 2}});
    setRollerMode(a, RollingMode::WideFill);
    ctx.check(rollerMode(b) == RollingMode::WideFill && rollerMode(c) == RollingMode::WideFill, "the mode is shared along the row");
    auto const* box = common::valuebox::boxAt(region(ctx).getBlock(toBlockPos(a)), 1, {0.5 - 3 / 16.0, 1.0, 0.5 + 3 / 16.0});
    ctx.check(box && box->id == "mode", "mode value box on the top face");
    common::blockdata::erase(a);
    common::blockdata::erase(b);
    common::blockdata::erase(c);
});

LV_REGISTER_SELFTEST("actors.psi", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2260);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 6, 3, 1));
    auto station = at(g0, 4, 1, 0);
    ctx.setBlock(station, std::string(kPsi), {{"levitate:facing", 4}});
    auto  rig = buildRig(ctx, at(g0, 1, 1, 0), std::string(kPsi));
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    co_await moveBy(ctx, *r.id, {1, 0, 0}, 10);
    // the connection cycles while no items move (Create: 14 ticks, then it reconnects)
    for (int i = 0; i < 20 && !psiConnected(station); ++i) co_await ctx.ticks(1);
    ctx.check(psiConnected(station), "the interfaces connected");
    ctx.check(stateAt(ctx, station, "levitate:extended") == 1, "the stationary interface shows it");
    auto& reg  = region(ctx);
    auto  rest = common::items::insert(reg, toBlockPos(station), 4, makeItem("minecraft:diamond", 5), false);
    ctx.check(rest.isNull() || rest.mCount == 0, "items inserted through the interface");
    auto& st       = con::find(*r.id)->storage();
    int   diamonds = 0;
    for (int i = 0; i < st.slotCount(); ++i)
        if (!st.slot(i).isNull() && st.slot(i).getTypeName() == "minecraft:diamond") diamonds += st.slot(i).mCount;
    ctx.check(diamonds == 5, "they are in contraption storage: " + std::to_string(diamonds));
    auto got = common::items::extract(reg, toBlockPos(station), 4, common::items::any(), 2, false);
    ctx.check(!got.isNull() && got.mCount == 2, "items extracted through the interface");
    // redstone disconnects
    ctx.setBlock(at(station, 0, 1, 0), "minecraft:redstone_block");
    co_await ctx.ticks(3);
    ctx.check(!psiConnected(station), "a redstone signal disconnects");
    ctx.setBlock(at(station, 0, 1, 0), "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(psiConnected(station), "connected again without the signal");
    ctx.cleanupBlock(at(g0, 2, 1, 0));
    ctx.cleanupBlock(at(g0, 2, 2, 0));
    ctx.cleanupBlock(at(g0, 1, 2, 0));
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    emptyChest(ctx, at(g0, 1, 2, 0));
    co_await ctx.ticks(3);
    ctx.check(!psiConnected(station), "disconnected once the contraption is gone");
});

LV_REGISTER_SELFTEST("actors.psi_holds", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2270);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 8, 3, 1));
    auto station = at(g0, 5, 1, 0);
    ctx.setBlock(station, std::string(kPsi), {{"levitate:facing", 4}});
    auto  rig = buildRig(ctx, at(g0, 1, 1, 0), std::string(kPsi));
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    // drive past: the moving interface holds the contraption at the station
    if (auto* c = con::find(*r.id)) {
        con::Motion m;
        m.velocity = {0.1, 0, 0};
        m.ticks    = 30;
        c->setMotion(m);
    }
    // wait until it is held at the station (active point 1.85 ahead passes the station's centre)
    for (int i = 0; i < 60; ++i) {
        auto* c = con::find(*r.id);
        if (c && c->transform().offset.x > 2.0 && psiConnected(station)) break;
        co_await ctx.ticks(1);
    }
    ctx.require(psiConnected(station), "connected at the station");
    // a transfer restarts the timeout: the contraption waits psiTimeout ticks after it
    auto rest = common::items::insert(region(ctx), toBlockPos(station), 4, makeItem("minecraft:apple", 1), false);
    ctx.check(rest.isNull() || rest.mCount == 0, "an item went through");
    co_await ctx.ticks(20);
    auto* c = con::find(*r.id);
    ctx.require(c != nullptr, "still there");
    double x = c->transform().offset.x;
    ctx.check(x > 2.0 && x < 2.25, "held in front of the station: offset " + std::to_string(x));
    auto timeout = config::get<Config>().psiTimeout;
    co_await ctx.ticks(timeout + 20);
    c = con::find(*r.id);
    ctx.require(c != nullptr, "still there");
    ctx.check(c->motion().ticks <= 0 && c->transform().offset.x > 2.9, "released and moved on: offset " + std::to_string(c->transform().offset.x));
    for (int dx = 0; dx <= 6; ++dx)
        for (int dy = 1; dy <= 2; ++dy) ctx.cleanupBlock(at(g0, dx, dy, 0));
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    emptyChest(ctx, at(g0, 3, 2, 0));
});

LV_REGISTER_SELFTEST("actors.controls", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2280);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 7, 4, 1));
    fillBox(ctx, at(g0, 2, 0, 0), at(g0, 5, 0, 0), "minecraft:farmland");
    ctx.setBlock(at(g0, 2, 1, 0), "minecraft:wheat", {{"growth", 7}});
    ctx.setBlock(at(g0, 4, 1, 0), "minecraft:wheat", {{"growth", 7}});
    auto rig      = buildRig(ctx, at(g0, 1, 1, 0), std::string(kHarvester));
    auto controls = at(g0, 1, 3, 0); // on top of the anchor
    ctx.setBlock(controls, std::string(kControls), {{"levitate:facing", 5}});
    con::addGlue(g0.dim, cc::GlueBox::of(ccp(rig.anchor), ccp(controls)));
    auto& player = ctx.player();
    controlsClickStationary(player, controls);
    ctx.check(stateAt(ctx, controls, "levitate:disabled") == 1, "stationary controls toggled off");
    Guard guard;
    auto  r = assembleAt(rig.anchor);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    ctx.check(con::find(*r.id)->blocks().size() == 4, "controls assembled with the harvester");
    co_await ctx.ticks(2);
    ctx.check(actorDisabled(*r.id, std::string(kHarvester)), "assembled while disabled: actors start disabled");
    co_await moveBy(ctx, *r.id, {2, 0, 0}, 10);
    ctx.check(stateAt(ctx, at(g0, 2, 1, 0), "growth") == 7, "a disabled harvester leaves the wheat");
    // a click on the moving controls switches every actor on again
    auto* c = con::find(*r.id);
    controlsClickOnContraption(player, *c, cc::Pos{0, 1, 0});
    ctx.check(!actorDisabled(*r.id, std::string(kHarvester)), "toggled on from the contraption");
    co_await moveBy(ctx, *r.id, {2, 0, 0}, 10);
    ctx.check(stateAt(ctx, at(g0, 4, 1, 0), "growth") == 0, "the harvester works again");
    for (int x = 0; x <= 5; ++x)
        for (int y = 1; y <= 3; ++y) ctx.cleanupBlock(at(g0, x, y, 0));
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    ctx.check(stateAt(ctx, at(g0, 5, 3, 0), "levitate:disabled") == 0, "placed controls remember: enabled");
    emptyChest(ctx, at(g0, 4, 2, 0));
});

LV_REGISTER_SELFTEST("actors.seat", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2290);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 4, 3, 2));
    auto seat = at(g0, 0, 1, 0);
    ctx.setBlock(seat, "levitate:white_seat");
    auto& player = ctx.player();
    auto  dye    = makeItem("minecraft:red_dye");
    ctx.check(seatUse(player, seat, dye), "dye used on the seat");
    ctx.check(idAt(ctx, seat) == "levitate:red_seat", "the seat was dyed red: " + idAt(ctx, seat));
    ctx.check(seatUse(player, seat, ItemStack::EMPTY_ITEM()), "sat down");
    co_await ctx.ticks(2);
    auto* vehicle = player.getVehicle();
    ctx.check(vehicle && isSeatEntityId(vehicle->getTypeName()), "riding the seat entity");
    ctx.check(seatedAt(seat) == &player, "seated at the seat");
    // the seat entity goes with its block
    ctx.setBlock(seat, "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(player.getVehicle() == nullptr, "dismounted when the seat is gone");

    // a mob that stands on a free seat sits down
    auto seat2 = at(g0, 2, 1, 1);
    ctx.setBlock(seat2, "levitate:blue_seat");
    auto* pig = spawnMob(ctx, "minecraft:pig", at(seat2, 0, 1, 0));
    ctx.require(pig != nullptr, "pig spawned");
    auto pigId = pig->getOrCreateUniqueID();
    for (int i = 0; i < 40 && !seatedAt(seat2); ++i) co_await ctx.ticks(1);
    auto* sitting = seatedAt(seat2);
    ctx.check(sitting && sitting->getOrCreateUniqueID() == pigId, "the pig sat down");
    if (auto* p = ll::service::getLevel()->fetchEntity(pigId, false)) p->remove();
    co_await ctx.ticks(4);
});

LV_REGISTER_SELFTEST("actors.seat_contraption", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto g0 = ctx.testPos(2295);
    ctx.require(co_await ctx.ensureLoaded(g0), "test area loaded");
    clearBox(ctx, at(g0, -1, 1, -1), at(g0, 4, 4, 1));
    auto stone = at(g0, 0, 1, 0);
    auto seat  = at(g0, 0, 2, 0);
    ctx.setBlock(stone, "minecraft:stone");
    ctx.setBlock(seat, "levitate:white_seat");
    con::addGlue(g0.dim, cc::GlueBox::of(ccp(stone), ccp(seat)));
    auto* pig = spawnMob(ctx, "minecraft:pig", seat);
    ctx.require(pig != nullptr, "pig spawned");
    auto pigId = pig->getOrCreateUniqueID();
    ctx.require(sitDown(*pig, seat), "pig seated");
    co_await ctx.ticks(2);
    Guard guard;
    auto  r = assembleAt(stone);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    guard.id = r.id;
    co_await moveBy(ctx, *r.id, {2, 0, 0}, 10);
    auto* p = ll::service::getLevel()->fetchEntity(pigId, false);
    ctx.require(p != nullptr, "pig still there");
    ctx.check(std::abs(p->getFeetPos().x - (seat.x + 2.5f)) < .3f, "the pig rode along: x " + std::to_string(p->getFeetPos().x));
    ctx.cleanupBlock(at(g0, 2, 1, 0));
    ctx.cleanupBlock(at(g0, 2, 2, 0));
    ctx.require(con::disassemble(*r.id), "disassembled");
    guard.id.reset();
    co_await ctx.ticks(3);
    auto* sitting = seatedAt(at(g0, 2, 2, 0));
    ctx.check(sitting && sitting->getOrCreateUniqueID() == pigId, "the pig sits on the placed seat");
    if (auto* e = ll::service::getLevel()->fetchEntity(pigId, false)) e->remove();
    co_await ctx.ticks(3);
});

} // namespace lv::feature::actors
