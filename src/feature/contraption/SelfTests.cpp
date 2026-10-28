// Selftests of the contraption unit (testPos dx 400..499). Each test builds a small structure,
// assembles it with the test anchor (the API used by `/create contraption ...`), moves it and
// checks the world after disassembly.

#include <cmath>
#include <map>

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "ll/api/service/Bedrock.h"

#include "feature/contraption/Internal.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "selftest/SelfTest.h"

namespace lv::feature::contraption {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;

GP at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
cc::Pos ccp(GP const& p) { return {p.x, p.y, p.z}; }

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

/// Places a block with typed states (string / int) and remembers it for cleanup.
bool place(Context& ctx, GP const& p, std::string const& id, cc::States const& states = {}) {
    auto const* block = game::blockWith(id);
    if (!block) return false;
    if (!states.empty()) block = detail::withStates(*block, states);
    ctx.cleanupBlock(p);
    return game::setBlock(regionOf(ctx), game::blockPos(p), *block);
}

std::string idAt(Context& ctx, GP const& p) { return game::blockIdAt(regionOf(ctx), game::blockPos(p)); }

std::string stateStr(Context& ctx, GP const& p, std::string const& key) {
    auto s  = detail::statesOf(regionOf(ctx).getBlock(game::blockPos(p)));
    auto it = s.find(key);
    if (it == s.end()) return {};
    if (auto const* v = std::get_if<std::string>(&it->second)) return *v;
    return std::to_string(std::get<std::int64_t>(it->second));
}

ItemStack makeItem(std::string const& id, int count) {
    CompoundTag tag;
    tag["Name"]   = id;
    tag["Count"]  = static_cast<uchar>(count);
    tag["Damage"] = static_cast<short>(0);
    return ItemStack::fromTag(tag);
}

AssembleResult assembleAt(GP const& p, cc::Axis axis = cc::Axis::Y) {
    AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    o.axis          = axis;
    return assemble(o);
}

/// Moves by `delta` over `ticks` ticks and waits until it has arrived.
ll::coro::CoroTask<> moveBy(Context& ctx, Id id, cc::Vec3d delta, int ticks) {
    if (auto* c = find(id)) {
        Motion m;
        m.velocity = delta * (1.0 / ticks);
        m.ticks    = ticks;
        c->setMotion(m);
    }
    co_await ctx.ticks(ticks + 3);
}

ll::coro::CoroTask<> rotateBy(Context& ctx, Id id, double degrees, int ticks) {
    if (auto* c = find(id)) {
        Motion m;
        m.angularVelocity = degrees / ticks;
        m.ticks           = ticks;
        c->setMotion(m);
    }
    co_await ctx.ticks(ticks + 3);
}

int liveDisplays(Contraption const& c) {
    auto level = ll::service::getLevel();
    int  n     = 0;
    for (auto const& b : c.blocks()) {
        ActorUniqueID uid;
        uid.rawID = b.display;
        if (b.display && level && level->fetchEntity(uid, false)) ++n;
    }
    return n;
}

/// Disassembles whatever a failed test left behind.
struct Guard {
    std::optional<Id> id;
    ~Guard() {
        if (id && find(*id)) disassemble(*id);
    }
};

} // namespace

LV_REGISTER_SELFTEST("contraption.assemble_glue", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(400);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(place(ctx, p, "minecraft:stone"), "stone placed");
    place(ctx, at(p, 1, 0, 0), "minecraft:stone");
    place(ctx, at(p, 1, 1, 0), "minecraft:stone");
    place(ctx, at(p, -1, 0, 0), "minecraft:stone"); // not glued
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 1, 1, 0))));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id    = r.id;
    auto* c = find(*r.id);
    ctx.require(c != nullptr, "contraption exists");
    ctx.check(c->blocks().size() == 3, "3 glued blocks, got " + std::to_string(c->blocks().size()));
    ctx.check(idAt(ctx, p) == "minecraft:air" && idAt(ctx, at(p, 1, 1, 0)) == "minecraft:air", "blocks lifted from the world");
    ctx.check(idAt(ctx, at(p, -1, 0, 0)) == "minecraft:stone", "unglued neighbour stays");
    ctx.check(!detail::glued(p.dim, ccp(p), cc::Dir::East), "glue travels with the contraption");
    co_await ctx.ticks(2);
    ctx.check(liveDisplays(*find(*r.id)) == 3, "one display entity per block");
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, p) == "minecraft:stone" && idAt(ctx, at(p, 1, 1, 0)) == "minecraft:stone", "blocks placed back");
    ctx.check(detail::glued(p.dim, ccp(p), cc::Dir::East), "glue placed back");
    clearGlue(p.dim, ccp(p), 3);
});

LV_REGISTER_SELFTEST("contraption.translate", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(406);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 1, 0, 0), "minecraft:gold_block");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 1, 0, 0))));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    co_await moveBy(ctx, *r.id, {0, 3, 0}, 10);
    auto* c = find(*r.id);
    ctx.require(c != nullptr, "still alive");
    ctx.check(std::abs(c->transform().offset.y - 3) < 1e-6, "moved 3 up: " + std::to_string(c->transform().offset.y));
    // the display entities follow and don't fall
    auto level = ll::service::getLevel();
    for (auto const& b : c->blocks()) {
        ActorUniqueID uid;
        uid.rawID = b.display;
        auto* a   = level->fetchEntity(uid, false);
        ctx.check(a != nullptr, "display alive");
        if (a) ctx.check(std::abs(a->getFeetPos().y - (p.y + 3)) < 0.05, "display at the new height: " + std::to_string(a->getFeetPos().y));
    }
    ctx.cleanupBlock(at(p, 0, 3, 0));
    ctx.cleanupBlock(at(p, 1, 3, 0));
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, at(p, 0, 3, 0)) == "minecraft:stone", "stone placed 3 up");
    ctx.check(idAt(ctx, at(p, 1, 3, 0)) == "minecraft:gold_block", "gold placed 3 up");
    ctx.check(idAt(ctx, p) == "minecraft:air", "old place empty");
    clearGlue(p.dim, ccp(p), 5);
});

LV_REGISTER_SELFTEST("contraption.rotate", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(412);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 1, 0, 0), "minecraft:oak_log", {{"pillar_axis", std::string("x")}});
    place(ctx, at(p, 1, 1, 0), "minecraft:torch", {{"torch_facing_direction", std::string("top")}});
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 1, 0, 0))));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    ctx.check(find(*r.id)->blocks().size() == 3, "the torch hangs on the log");
    co_await rotateBy(ctx, *r.id, 90, 10);
    ctx.check(std::abs(find(*r.id)->transform().angle - 90) < 1e-6, "turned 90 degrees");
    for (auto d : {at(p, 0, 0, -1), at(p, 0, 1, -1)}) ctx.cleanupBlock(d);
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, p) == "minecraft:stone", "pivot block stays");
    ctx.check(idAt(ctx, at(p, 0, 0, -1)) == "minecraft:oak_log", "east block now north");
    ctx.check(stateStr(ctx, at(p, 0, 0, -1), "pillar_axis") == "z", "log axis rotated to z");
    ctx.check(idAt(ctx, at(p, 0, 1, -1)) == "minecraft:torch", "torch placed on the log");
    ctx.check(idAt(ctx, at(p, 1, 0, 0)) == "minecraft:air", "old place empty");
    clearGlue(p.dim, ccp(p), 3);
});

LV_REGISTER_SELFTEST("contraption.rotate_rounding", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(418);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:oak_planks");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 0, 1, 0))));
    Guard g;
    auto  r = assembleAt(p, cc::Axis::X);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    co_await rotateBy(ctx, *r.id, 50, 5);
    ctx.cleanupBlock(at(p, 0, 0, 1));
    ctx.require(disassemble(*r.id), "disassembled");
    // 50 degrees about X rounds to a quarter turn: up becomes south
    ctx.check(idAt(ctx, at(p, 0, 0, 1)) == "minecraft:oak_planks", "rounded to the nearest quarter turn");
    clearGlue(p.dim, ccp(p), 3);
});

LV_REGISTER_SELFTEST("contraption.unmovable", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(424);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:bedrock");
    auto r = assembleAt(p);
    ctx.check(!r.ok() && r.errorKey == "create.gui.assembly.exception.unmovableBlock", "bedrock is unmovable: " + r.errorKey);
    ctx.check(idAt(ctx, p) == "minecraft:bedrock", "world untouched");
    // pushing into an unmovable block fails
    place(ctx, at(p, 0, 0, 1), "minecraft:stone");
    place(ctx, at(p, 0, 0, 2), "minecraft:obsidian");
    place(ctx, at(p, 0, 0, 3), "minecraft:barrier");
    AssembleOptions o;
    o.anchor = at(p, 0, 0, 1);
    o.start  = {ccp(at(p, 0, 0, 1))};
    o.includeAnchor = true;
    o.forced = cc::Dir::South;
    auto f   = assemble(o);
    ctx.check(!f.ok() && f.errorKey == "create.gui.assembly.exception.unmovableBlock", "push into barrier fails");
    // too many blocks
    auto& cfg  = config::mut<Config>();
    int   keep = cfg.maxBlocksMoved;
    cfg.maxBlocksMoved = 1;
    addGlue(p.dim, cc::GlueBox::of(ccp(at(p, 0, 0, 1)), ccp(at(p, 0, 0, 2))));
    auto big = assembleAt(at(p, 0, 0, 1));
    cfg.maxBlocksMoved = keep;
    ctx.check(!big.ok() && big.errorKey == "create.gui.assembly.exception.structureTooLarge", "too large: " + big.errorKey);
    if (big.ok()) disassemble(*big.id);
    clearGlue(p.dim, ccp(p), 4);
});

LV_REGISTER_SELFTEST("contraption.chassis_linear", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(430);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(
        place(ctx, p, std::string(kLinearChassis), {{"levitate:axis", std::int64_t{1}}, {"levitate:sticky_top", std::int64_t{1}}, {"levitate:sticky_bottom", std::int64_t{0}}}),
        "chassis placed (behavior pack loaded)"
    );
    for (int y = 1; y <= 3; ++y) place(ctx, at(p, 0, y, 0), "minecraft:stone");
    place(ctx, at(p, 1, 0, 0), std::string(kSecondaryChassis), {{"levitate:axis", std::int64_t{1}}}); // other kind: not connected
    setChassisRange(p, 2);
    ctx.check(chassisRange(p) == 2, "range set");
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    ctx.check(find(*r.id)->blocks().size() == 3, "chassis + 2 blocks in range, got " + std::to_string(find(*r.id)->blocks().size()));
    ctx.check(idAt(ctx, at(p, 0, 3, 0)) == "minecraft:stone", "block out of range stays");
    ctx.check(idAt(ctx, at(p, 1, 0, 0)) == std::string(kSecondaryChassis), "secondary chassis not connected");
    // the range travels with the chassis
    place(ctx, at(p, 0, 3, 0), "minecraft:air");
    co_await moveBy(ctx, *r.id, {0, 1, 0}, 4);
    for (int y = 1; y <= 3; ++y) ctx.cleanupBlock(at(p, 0, y, 0));
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, at(p, 0, 1, 0)) == std::string(kLinearChassis), "chassis moved up");
    ctx.check(stateStr(ctx, at(p, 0, 1, 0), "levitate:sticky_top") == "1", "sticky face kept");
    ctx.check(chassisRange(at(p, 0, 1, 0)) == 2, "range carried: " + std::to_string(chassisRange(at(p, 0, 1, 0))));
});

LV_REGISTER_SELFTEST("contraption.chassis_radial", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(436);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(place(ctx, p, std::string(kRadialChassis), {{"levitate:axis", std::int64_t{1}}, {"levitate:sticky_east", std::int64_t{1}}}), "radial chassis placed");
    place(ctx, at(p, 1, 0, 0), "minecraft:stone");
    place(ctx, at(p, 1, 0, -1), "minecraft:stone");
    place(ctx, at(p, 2, 0, 0), "minecraft:stone");
    setChassisRange(p, 1);
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    ctx.check(find(*r.id)->blocks().size() == 3, "chassis + disc of radius 1, got " + std::to_string(find(*r.id)->blocks().size()));
    ctx.check(idAt(ctx, at(p, 2, 0, 0)) == "minecraft:stone", "outside the radius stays");
    ctx.require(disassemble(*r.id), "disassembled");
});

LV_REGISTER_SELFTEST("contraption.sticker", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(442);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    ctx.require(place(ctx, p, std::string(kSticker), {{"levitate:facing", std::int64_t{5}}, {"levitate:extended", std::int64_t{0}}}), "sticker placed");
    place(ctx, at(p, 1, 0, 0), "minecraft:stone");
    // power extends it
    place(ctx, at(p, -1, 0, 0), "minecraft:redstone_block");
    detail::updateSticker(region, p.dim, ccp(p));
    ctx.check(stateStr(ctx, p, "levitate:extended") == "1", "powered sticker extends");
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    ctx.check(find(*r.id)->blocks().size() == 2, "the sticker holds the block in front");
    ctx.require(disassemble(*r.id), "disassembled");
    place(ctx, at(p, -1, 0, 0), "minecraft:air");
    detail::updateSticker(region, p.dim, ccp(p));
    ctx.check(stateStr(ctx, p, "levitate:extended") == "0", "unpowered sticker retracts");
    auto r2 = assembleAt(p);
    ctx.check(r2.ok() && find(*r2.id)->blocks().size() == 1, "a retracted sticker holds nothing");
    if (r2.ok()) disassemble(*r2.id);
});

LV_REGISTER_SELFTEST("contraption.storage", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(448);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    place(ctx, p, "minecraft:chest");
    co_await ctx.ticks(1);
    auto container = region.tryGetContainer(game::blockPos(p));
    ctx.require(container.has_value(), "chest has a container");
    container->setItem(3, makeItem("minecraft:apple", 7));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id    = r.id;
    auto& s = find(*r.id)->storage();
    ctx.check(s.slotCount() == 27, "27 mounted slots, got " + std::to_string(s.slotCount()));
    ctx.check(s.slotCount() > 3 && s.slot(3).mCount == 7, "the apples travel with the chest");
    auto rest = s.insertItem(makeItem("minecraft:diamond", 5), false);
    ctx.check(rest.isNull(), "diamonds inserted");
    auto out = s.extractItem([](ItemStack const& i) { return i.getTypeName() == "minecraft:apple"; }, 2, false);
    ctx.check(out.mCount == 2, "extracted 2 apples");
    co_await moveBy(ctx, *r.id, {0, 0, 1}, 4);
    auto q = at(p, 0, 0, 1);
    ctx.cleanupBlock(q);
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, q) == "minecraft:chest", "chest placed");
    auto placed = region.tryGetContainer(game::blockPos(q));
    ctx.require(placed.has_value(), "placed chest has a container");
    int apples = 0, diamonds = 0;
    for (int i = 0; i < placed->getContainerSize(); ++i) {
        auto const& item = placed->getItem(i);
        if (item.isNull()) continue;
        if (item.getTypeName() == "minecraft:apple") apples += item.mCount;
        if (item.getTypeName() == "minecraft:diamond") diamonds += item.mCount;
    }
    ctx.check(apples == 5 && diamonds == 5, "contents written back: apples " + std::to_string(apples) + ", diamonds " + std::to_string(diamonds));
    placed->removeAllItems();
});

LV_REGISTER_SELFTEST("contraption.carry_player", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(454);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz) place(ctx, at(p, dx, 0, dz), "minecraft:stone");
    addGlue(p.dim, cc::GlueBox::of(ccp(at(p, -1, 0, -1)), ccp(at(p, 1, 0, 1))));
    auto& player = ctx.player();
    player.teleport(Vec3{p.x + 0.5f, p.y + 1.0f, p.z + 0.5f}, player.getDimensionId());
    co_await ctx.ticks(5);
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    co_await ctx.ticks(10);
    float before = player.getFeetPos().y;
    ctx.check(std::abs(before - (p.y + 1.0f)) < 0.3f, "the still contraption carries the player: y " + std::to_string(before));
    co_await moveBy(ctx, *r.id, {0, 2, 0}, 16);
    float after = player.getFeetPos().y;
    ctx.check(std::abs(after - (p.y + 3.0f)) < 0.6f, "player lifted with it: y " + std::to_string(after));
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz) ctx.cleanupBlock(at(p, dx, 2, dz));
    ctx.require(disassemble(*r.id), "disassembled");
    clearGlue(p.dim, ccp(p), 4);
});

LV_REGISTER_SELFTEST("contraption.collision", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(460);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:stone");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 0, 1, 0))));
    auto& player = ctx.player();
    player.teleport(Vec3{p.x + 2.5f, float(p.y), p.z + 0.5f}, player.getDimensionId());
    co_await ctx.ticks(5);
    float start = player.getFeetPos().x;
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    co_await ctx.ticks(3);
    // the display entities stand on the ground here: they must never land as blocks
    ctx.check(idAt(ctx, p) == "minecraft:air", "display entities don't land: " + idAt(ctx, p));
    co_await moveBy(ctx, *r.id, {3, 0, 0}, 15);
    ctx.check(idAt(ctx, at(p, 3, 0, 0)) == "minecraft:air", "nothing landed at the target: " + idAt(ctx, at(p, 3, 0, 0)));
    float end = player.getFeetPos().x;
    ctx.check(end > start + 0.5f, "the wall pushed the player: x " + std::to_string(start) + " -> " + std::to_string(end));
    ctx.cleanupBlock(at(p, 3, 0, 0));
    ctx.cleanupBlock(at(p, 3, 1, 0));
    ctx.require(disassemble(*r.id), "disassembled");
    clearGlue(p.dim, ccp(p), 5);
});

LV_REGISTER_SELFTEST("contraption.drop_blocked", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(466);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:oak_planks");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 0, 1, 0))));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    place(ctx, at(p, 0, 1, 0), "minecraft:gold_block"); // occupies the target
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, at(p, 0, 1, 0)) == "minecraft:gold_block", "the world block stays");
    ctx.check(idAt(ctx, p) == "minecraft:stone", "the free place is filled");
    auto& region = regionOf(ctx);
    auto  items  = region.getEntities(AABB{Vec3{p.x - 1.f, p.y - 1.f, p.z - 1.f}, Vec3{p.x + 2.f, p.y + 3.f, p.z + 2.f}}, 0.f, ActorType::ItemEntity);
    ctx.check(!items.empty(), "the blocked block dropped as an item");
    for (auto* a : items) a->remove();
    clearGlue(p.dim, ccp(p), 3);
});

namespace {
struct ActorCounters {
    bool enabled = false;
    int  starts = 0, ticks = 0, visits = 0, stops = 0;
};
ActorCounters gActor;
} // namespace

LV_REGISTER_SELFTEST("contraption.actor", [](Context& ctx) -> ll::coro::CoroTask<> {
    static bool registered = false;
    if (!registered) {
        registered = true;
        // a test actor on lapis blocks (counts only while this test runs)
        registerActor(
            "minecraft:lapis_block",
            MovementBehaviour{
                [](MovementContext&) { gActor.starts += gActor.enabled; },
                [](MovementContext&) { gActor.ticks += gActor.enabled; },
                [](MovementContext&, core::GlobalPos const&) { gActor.visits += gActor.enabled; },
                [](MovementContext&) { gActor.stops += gActor.enabled; },
                nullptr,
            }
        );
    }
    gActor = {true};
    auto p = ctx.testPos(472);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:lapis_block");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 0, 1, 0))));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    co_await moveBy(ctx, *r.id, {3, 0, 0}, 12);
    ctx.check(gActor.starts == 1, "startMoving once: " + std::to_string(gActor.starts));
    ctx.check(gActor.ticks >= 10, "ticked while moving: " + std::to_string(gActor.ticks));
    ctx.check(gActor.visits >= 3, "visited each new block: " + std::to_string(gActor.visits));
    ctx.check(gActor.stops == 1, "stopMoving once: " + std::to_string(gActor.stops));
    gActor.enabled = false;
    ctx.cleanupBlock(at(p, 3, 0, 0));
    ctx.cleanupBlock(at(p, 3, 1, 0));
    ctx.require(disassemble(*r.id), "disassembled");
    clearGlue(p.dim, ccp(p), 5);
});

namespace {
std::map<core::GlobalPos, nlohmann::json> gCarried;
std::vector<core::GlobalPos>              gRestored;
} // namespace

LV_REGISTER_SELFTEST("contraption.data_carrier", [](Context& ctx) -> ll::coro::CoroTask<> {
    registerDataCarrier({
        "contraption_test",
        [](BlockSource&, core::GlobalPos const& p) -> std::optional<nlohmann::json> {
            auto it = gCarried.find(p);
            if (it == gCarried.end()) return std::nullopt;
            auto v = it->second;
            gCarried.erase(it);
            return v;
        },
        [](BlockSource&, core::GlobalPos const& p, nlohmann::json const& v) {
            gCarried[p] = v;
            gRestored.push_back(p);
        },
        nullptr,
    });
    gCarried.clear();
    gRestored.clear();
    auto p = ctx.testPos(478);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    gCarried[p] = {{"v", 42}};
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    ctx.check(gCarried.empty(), "data taken from the old position");
    co_await moveBy(ctx, *r.id, {0, 2, 0}, 6);
    auto q = at(p, 0, 2, 0);
    ctx.cleanupBlock(q);
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(gRestored.size() == 1 && gRestored[0] == q, "restored at the new position");
    ctx.check(gCarried.contains(q) && gCarried[q].value("v", 0) == 42, "data intact");
    registerDataCarrier({"contraption_test", nullptr, nullptr, nullptr});
});

LV_REGISTER_SELFTEST("contraption.persistence", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(484);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:oak_planks");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 0, 1, 0))));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    co_await moveBy(ctx, *r.id, {0, 1, 0}, 4);
    detail::reloadAll();
    auto* c = find(*r.id);
    ctx.require(c != nullptr, "contraption reloaded from the world DB");
    ctx.check(c->blocks().size() == 2, "blocks reloaded");
    ctx.check(std::abs(c->transform().offset.y - 1) < 1e-6, "transform reloaded");
    co_await ctx.ticks(3);
    ctx.check(liveDisplays(*find(*r.id)) == 2, "display entities adopted or respawned");
    ctx.cleanupBlock(at(p, 0, 1, 0));
    ctx.cleanupBlock(at(p, 0, 2, 0));
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, at(p, 0, 2, 0)) == "minecraft:oak_planks", "placed after reload");
    clearGlue(p.dim, ccp(p), 4);
});

LV_REGISTER_SELFTEST("contraption.super_glue", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(490);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    for (int x = 0; x < 3; ++x) place(ctx, at(p, x, 0, 0), "minecraft:stone");
    auto& player = ctx.player();
    auto  glue   = makeItem(std::string(kSuperGlue), 1);
    ctx.require(!glue.isNull(), "super glue item exists (behavior pack loaded)");
    int slot = ctx.give(player, glue, 0);
    ctx.require(slot == 0, "glue given");
    player.setSelectedSlot(0);
    clearGlue(p.dim, ccp(p), 4);
    detail::glueClick(player, ccp(p), false);
    detail::glueClick(player, ccp(at(p, 2, 0, 0)), false);
    auto regions = glueRegions(p.dim);
    bool found   = std::any_of(regions.begin(), regions.end(), [&](cc::GlueBox const& b) {
        return b == cc::GlueBox::of(ccp(p), ccp(at(p, 2, 0, 0)));
    });
    ctx.check(found, "two clicks glue the box");
    // a gap: the corners must be connected
    detail::glueClick(player, ccp(p), false);
    detail::glueClick(player, ccp(at(p, 2, 2, 0)), false);
    ctx.check(glueRegions(p.dim).size() == regions.size(), "unconnected selection refused");
    detail::glueClick(player, ccp(p), true); // sneak: discard
    // too far
    detail::glueClick(player, ccp(p), false);
    detail::glueClick(player, ccp(at(p, 30, 0, 0)), false);
    ctx.check(glueRegions(p.dim).size() == regions.size(), "too large selection refused");
    detail::glueClick(player, ccp(p), true);
    ctx.check(detail::glueAttack(player, ccp(at(p, 1, 0, 0))), "hitting with glue removes the region");
    ctx.check(!detail::glued(p.dim, ccp(p), cc::Dir::East), "region gone");
});

LV_REGISTER_SELFTEST("contraption.chassis_use", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(496);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(place(ctx, p, std::string(kLinearChassis), {{"levitate:axis", std::int64_t{1}}}), "chassis placed");
    auto& player = ctx.player();
    ctx.check(detail::chassisUse(player, p.dim, ccp(p), cc::Dir::Up, "minecraft:slime_ball", false), "slime makes the face sticky");
    ctx.check(stateStr(ctx, p, "levitate:sticky_top") == "1", "top sticky");
    ctx.check(!detail::chassisUse(player, p.dim, ccp(p), cc::Dir::North, "minecraft:slime_ball", false), "side faces aren't glueable");
    ctx.check(detail::chassisUse(player, p.dim, ccp(p), cc::Dir::Up, "minecraft:slime_ball", false), "slime on a sticky face");
    ctx.check(stateStr(ctx, p, "levitate:sticky_bottom") == "1", "makes every face sticky");
    ctx.check(!detail::chassisUse(player, p.dim, ccp(p), cc::Dir::Down, "", false), "empty hand needs sneaking");
    ctx.check(detail::chassisUse(player, p.dim, ccp(p), cc::Dir::Down, "", true), "sneaking empty hand clears");
    ctx.check(stateStr(ctx, p, "levitate:sticky_bottom") == "0", "bottom cleared");
    // placement orientation: axis from the clicked face
    detail::orientPlaced(regionOf(ctx), ccp(p), cc::Dir::East, player);
    ctx.check(stateStr(ctx, p, "levitate:axis") == "0", "clicked east face: axis x");
});

} // namespace lv::feature::contraption
