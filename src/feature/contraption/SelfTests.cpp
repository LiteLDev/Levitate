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
#include "mc/world/actor/item/ItemActor.h"
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
#include <algorithm>
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"

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
    place(ctx, at(p, 0, 0, 2), "minecraft:oak_planks"); // (obsidian is unmovable by default since U53)
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
    // Create's addBlocksToWorld: an occupied place is cleared (the world block drops) and the
    // carried block is placed; only an unbreakable block keeps its place (the carried one drops)
    auto p = ctx.testPos(466);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    auto  dropsNear = [&](GP const& c) {
        return region.getEntities(AABB{Vec3{c.x - 1.f, c.y - 1.f, c.z - 1.f}, Vec3{c.x + 2.f, c.y + 3.f, c.z + 2.f}}, 0.f, ActorType::ItemEntity);
    };
    for (auto* a : dropsNear(p)) a->remove();
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:oak_planks");
    place(ctx, at(p, 0, 2, 0), "minecraft:oak_planks");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 0, 2, 0))));
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    place(ctx, at(p, 0, 1, 0), "minecraft:gold_block"); // a breakable block in the way
    place(ctx, at(p, 0, 2, 0), "minecraft:bedrock");    // an unbreakable one
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(idAt(ctx, at(p, 0, 1, 0)) == "minecraft:oak_planks", "the carried block replaces the world block: " + idAt(ctx, at(p, 0, 1, 0)));
    ctx.check(idAt(ctx, at(p, 0, 2, 0)) == "minecraft:bedrock", "an unbreakable block stays");
    ctx.check(idAt(ctx, p) == "minecraft:stone", "the free place is filled");
    co_await ctx.ticks(2);
    bool gold = false, planks = false;
    for (auto* a : dropsNear(p)) {
        auto id = static_cast<ItemActor*>(a)->item().getTypeName();
        gold    = gold || id == "minecraft:gold_block";
        planks  = planks || id == "minecraft:oak_planks";
        a->remove();
    }
    ctx.check(gold, "the replaced world block dropped");
    ctx.check(planks, "the carried block blocked by bedrock dropped");
    clearGlue(p.dim, ccp(p), 3);
    place(ctx, at(p, 0, 2, 0), "minecraft:air");
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

// Selftests of U53 (the contraption audit fixes; testPos dx 5300..5399): movement settings, sticky
// neighbours, Create's brittle / empty-collider blocks, running bearings and tracks, actors that
// aren't brittle, chassis glue towards a connected chassis, single-block and off-hand super glue,
// lever rotation and stabilized sub-contraptions.
namespace lv::feature::contraption {
namespace u53_tests {


namespace {

using lv::selftest::Context;
using GP     = core::GlobalPos;
namespace bd = ::lv::feature::common::blockdata;

GP      at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
cc::Pos ccp(GP const& p) { return {p.x, p.y, p.z}; }

ll::coro::CoroTask<GP> area(Context& ctx, int n) {
    auto p = ctx.testPos(5300 + n);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    co_return p;
}

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

bool place(Context& ctx, GP const& p, std::string const& id, cc::States const& states = {}) {
    auto const* block = game::blockWith(id);
    if (!block) return false;
    if (!states.empty()) block = detail::withStates(*block, states);
    ctx.cleanupBlock(p);
    return game::setBlock(regionOf(ctx), game::blockPos(p), *block);
}

std::string idAt(Context& ctx, GP const& p) { return game::blockIdAt(regionOf(ctx), game::blockPos(p)); }

AssembleResult assembleAt(GP const& p) {
    AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    return assemble(o);
}

bool carries(Contraption const& c, cc::Pos const& local) {
    return std::any_of(c.blocks().begin(), c.blocks().end(), [&](CarriedBlock const& b) { return b.local == local; });
}

struct Guard {
    std::vector<Id> ids;
    ~Guard() {
        for (auto id : ids)
            if (find(id)) disassemble(id);
    }
};

} // namespace

// C3 / M52 / M53: Create's ContraptionMovementSetting defaults and config keys
LV_REGISTER_SELFTEST("contraption.u53_movement_settings", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 0);
    ctx.check(movementSetting("minecraft:obsidian") == MovementSetting::Unmovable, "obsidian unmovable by default");
    ctx.check(movementSetting("minecraft:crying_obsidian") == MovementSetting::Unmovable, "crying obsidian unmovable");
    ctx.check(movementSetting("minecraft:respawn_anchor") == MovementSetting::Unmovable, "respawn anchor unmovable");
    ctx.check(movementSetting("minecraft:reinforced_deepslate") == MovementSetting::Unmovable, "reinforced deepslate unmovable");
    ctx.check(movementSetting("minecraft:mob_spawner") == MovementSetting::NoPickup, "spawners: no pickup");
    ctx.check(movementSetting("minecraft:budding_amethyst") == MovementSetting::NoPickup, "budding amethyst: no pickup");
    place(ctx, p, "minecraft:obsidian");
    auto r = assembleAt(p);
    ctx.check(!r.ok() && r.errorKey == "create.gui.assembly.exception.unmovableBlock", "obsidian refuses to assemble: " + r.errorKey);
    if (r.ok()) disassemble(*r.id);
    auto& cfg  = config::mut<Config>();
    auto  keep = cfg.movableObsidian;
    cfg.movableObsidian = "MOVABLE";
    auto moved = assembleAt(p);
    cfg.movableObsidian = keep;
    ctx.check(moved.ok(), "movableObsidian = MOVABLE moves it: " + moved.errorText);
    if (moved.ok()) disassemble(*moved.id);
    ctx.check(idAt(ctx, p) == "minecraft:obsidian", "placed back");
});

// C11: a slime block next to a moved block is pulled (either block sticky)
LV_REGISTER_SELFTEST("contraption.u53_slime_neighbour", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 3);
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 1, 0, 0), "minecraft:slime");
    place(ctx, at(p, 2, 0, 0), "minecraft:oak_planks");
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.ids.push_back(*r.id);
    auto* c = find(*r.id);
    ctx.check(c && carries(*c, {1, 0, 0}), "the slime block next to the stone moves");
    ctx.check(c && carries(*c, {2, 0, 0}), "and drags its own neighbour");
    co_return;
});

// C12: Create's #create:movable_empty_collider / #create:brittle blocks travel on contraptions
LV_REGISTER_SELFTEST("contraption.u53_empty_colliders", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 6);
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 1, 0, 0), "minecraft:web");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 1, 0, 0))));
    auto info = detail::describe(regionOf(ctx), p.dim, ccp(at(p, 1, 0, 0)));
    ctx.check(info.necessary, "a cobweb is moved although it has no collision shape");
    Guard g;
    auto  r = assembleAt(p);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.ids.push_back(*r.id);
    auto* c = find(*r.id);
    ctx.check(c && carries(*c, {1, 0, 0}), "the glued cobweb moves");
    clearGlue(p.dim, ccp(p), 3);
});

// C4: a running bearing and tracks are unmovable (isMovementAllowedFallback)
LV_REGISTER_SELFTEST("contraption.u53_running_bearing", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 9);
    ctx.require(place(ctx, p, "levitate:mechanical_bearing", {{"levitate:facing", std::int64_t{1}}}), "bearing placed");
    bd::edit(p)["bearings"] = {{"running", true}};
    auto info = detail::describe(regionOf(ctx), p.dim, ccp(p));
    ctx.check(!info.movable, "a running bearing is unmovable");
    ctx.check(cc::has(info.pulls, cc::Dir::Up), "a carried bearing takes its front along");
    ctx.check(cc::has(info.notSupportive, cc::Dir::Up), "nothing else sticks to its front");
    bd::edit(p)["bearings"] = {{"running", false}};
    ctx.check(detail::describe(regionOf(ctx), p.dim, ccp(p)).movable, "a stopped bearing moves");
    bd::erase(p);
    place(ctx, at(p, 1, 0, 0), "levitate:track");
    if (idAt(ctx, at(p, 1, 0, 0)) == "levitate:track") {
        ctx.check(!detail::describe(regionOf(ctx), p.dim, ccp(at(p, 1, 0, 0))).movable, "tracks never move");
    }
    co_return;
});

// C13: harvesters, ploughs and rollers aren't brittle; harvesters carry nothing on their front
LV_REGISTER_SELFTEST("contraption.u53_actor_rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 12);
    for (auto const* id : {"levitate:mechanical_harvester", "levitate:mechanical_plough", "levitate:mechanical_roller"}) {
        ctx.require(place(ctx, p, id, {{"levitate:facing", std::int64_t{2}}}), std::string(id) + " placed");
        auto info = detail::describe(regionOf(ctx), p.dim, ccp(p));
        ctx.check(!info.brittle, std::string(id) + " isn't brittle");
        ctx.check(info.necessary, std::string(id) + " is moved");
        bool roller = std::string_view(id) == "levitate:mechanical_roller";
        ctx.check(roller ? info.notSupportive == 0 : info.notSupportive == cc::bit(cc::Dir::North), std::string(id) + " supportive sides");
    }
    co_return;
});

// C15: a linear chassis face towards a connected chassis of the same kind and axis can't be glued
LV_REGISTER_SELFTEST("contraption.u53_chassis_glue_side", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 15);
    ctx.require(place(ctx, p, std::string(kLinearChassis), {{"levitate:axis", std::int64_t{1}}}), "chassis placed");
    ctx.require(place(ctx, at(p, 0, 1, 0), std::string(kLinearChassis), {{"levitate:axis", std::int64_t{1}}}), "chassis above");
    auto& player = ctx.player();
    ctx.check(!detail::chassisUse(player, p.dim, ccp(p), cc::Dir::Up, "minecraft:slime_ball", false), "no glue towards the connected chassis");
    ctx.check(detail::chassisUse(player, p.dim, ccp(p), cc::Dir::Down, "minecraft:slime_ball", false), "the free end takes glue");
    co_return;
});

// C14: one block alone is no glue group; super glue in the off hand glues a placed block
LV_REGISTER_SELFTEST("contraption.u53_super_glue", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 18);
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:stone");
    clearGlue(p.dim, ccp(p), 4);
    auto& player = ctx.player();
    auto  glue   = common::items::make(std::string(kSuperGlue));
    ctx.require(glue.has_value(), "super glue item exists");
    ctx.give(player, *glue, 0);
    player.setSelectedSlot(0);
    detail::glueClick(player, ccp(p), false);
    detail::glueClick(player, ccp(p), false);
    ctx.check(glueRegions(p.dim).empty() || !detail::glued(p.dim, ccp(p), cc::Dir::Up), "a single block isn't glued");
    detail::glueClick(player, ccp(p), true);
    // off hand
    player.setSelectedItem(ItemStack{});
    player.setOffhandSlot(*glue);
    ctx.check(detail::glueOnPlace(player, ccp(at(p, 0, 1, 0)), ccp(p)), "off-hand glue applies on placement");
    ctx.check(detail::glued(p.dim, ccp(p), cc::Dir::Up), "the placed block is glued to the one it was placed against");
    player.setOffhandSlot(ItemStack{});
    clearGlue(p.dim, ccp(p), 4);
    co_return;
});

// C10: a lever on a structure turned about a horizontal axis lands on a wall
LV_REGISTER_SELFTEST("contraption.u53_lever_rotation", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 21);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 0, 1, 0), "minecraft:lever", {{"lever_direction", std::string("up_east_west")}});
    Guard g;
    AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    o.axis          = cc::Axis::X;
    auto r          = assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.ids.push_back(*r.id);
    auto* c = find(*r.id);
    ctx.require(c && c->blocks().size() == 2, "stone and lever carried");
    auto t  = c->transform();
    t.angle = 90;
    c->setTransform(t);
    co_await ctx.ticks(2);
    auto lever = find(*r.id) ? find(*r.id)->placement({0, 1, 0}) : GP{};
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.cleanupBlock(lever);
    auto states = detail::statesOf(regionOf(ctx).getBlock(game::blockPos(lever)));
    auto it     = states.find("lever_direction");
    auto value  = it == states.end() ? std::string() : std::get<std::string>(it->second);
    ctx.check(idAt(ctx, lever) == "minecraft:lever", "the lever is placed: " + idAt(ctx, lever));
    ctx.check(value == "south" || value == "north", "turned onto a wall: " + value);
});

// C9: a mechanical bearing on a bearing's structure (same axis) carries a stabilized sub-contraption
LV_REGISTER_SELFTEST("contraption.u53_stabilized", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 24);
    place(ctx, p, "minecraft:stone"); // stands for the parent's bearing (the anchor stays)
    place(ctx, at(p, 0, 1, 0), "minecraft:stone");
    ctx.require(place(ctx, at(p, 1, 1, 0), "levitate:mechanical_bearing", {{"levitate:facing", std::int64_t{1}}}), "sub bearing");
    place(ctx, at(p, 1, 2, 0), "minecraft:oak_planks");
    addGlue(p.dim, cc::GlueBox::of(ccp(at(p, 0, 1, 0)), ccp(at(p, 1, 1, 0))));
    Guard g;
    AssembleOptions o;
    o.anchor = p;
    o.start  = {ccp(at(p, 0, 1, 0))};
    o.axis   = cc::Axis::Y;
    o.kind   = "mechanical_bearing";
    auto r   = assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    Id   child = 0;
    for (auto id : all())
        if (auto* c = find(id); c && c->kind() == "stabilized" && c->anchorData().value("stabilized", nlohmann::json::object()).value("parent", Id{0}) == *r.id) child = id;
    g.ids.push_back(*r.id);
    ctx.require(child != 0, "a stabilized sub-contraption was assembled");
    ctx.check(find(*r.id)->blocks().size() == 2, "the parent holds the stone and the sub bearing");
    ctx.check(find(child)->blocks().size() == 1, "the sub-contraption holds the plank");
    // turn the parent a quarter: the sub-contraption follows its bearing without turning
    Motion m;
    m.angularVelocity = 9;
    m.ticks           = 10;
    find(*r.id)->setMotion(m);
    co_await ctx.ticks(14);
    ctx.require(find(child) != nullptr, "still alive");
    ctx.check(std::abs(find(child)->transform().angle) < 1e-6, "the sub-contraption doesn't turn");
    auto bearingAt = find(*r.id)->placement({1, 1, 0});
    ctx.require(disassemble(*r.id), "disassembled");
    ctx.check(find(child) == nullptr, "the sub-contraption is placed with its parent");
    GP plank = at(bearingAt, 0, 1, 0);
    ctx.cleanupBlock(bearingAt);
    ctx.cleanupBlock(plank);
    ctx.cleanupBlock(at(p, 0, 1, 0));
    ctx.check(idAt(ctx, bearingAt) == "levitate:mechanical_bearing", "the sub bearing is placed: " + idAt(ctx, bearingAt));
    ctx.check(idAt(ctx, plank) == "minecraft:oak_planks", "the plank is on its bearing: " + idAt(ctx, plank));
    clearGlue(p.dim, ccp(p), 4);
});

} // namespace u53_tests
} // namespace lv::feature::contraption
