// Selftests of the building unit (U10, testPos dx 1000..1099).

#include <functional>
#include <optional>

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockProperty.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/building/Building.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
namespace b = lv::feature::building;
using lv::core::GlobalPos;
using Facing = ScriptModuleMinecraft::ScriptFacing;

GlobalPos at(GlobalPos const& p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockPos  bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
std::string id(GlobalPos const& p) { return b::idAt(p); }
int st(GlobalPos const& p, char const* name) {
    auto const* blk = b::blockAt(p);
    return blk ? b::stateOf(*blk, name, -1) : -1;
}
SimulatedPlayer& simOf(Context& ctx) { return static_cast<SimulatedPlayer&>(ctx.player()); }
void stand(SimulatedPlayer& sp, GlobalPos const& p) { sp.teleport(Vec3{p.x + .5f, float(p.y), p.z + .5f}, DimensionType(p.dim)); }
void look(SimulatedPlayer& sp, GlobalPos const& p) { sp.simulateLookAt(Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, sim::LookDuration::Instant); }
Vec3 faceHit(Facing f) {
    switch (f) {
    case Facing::Down: return {.5f, 0.f, .5f};
    case Facing::Up: return {.5f, 1.f, .5f};
    case Facing::North: return {.5f, .5f, 0.f};
    case Facing::South: return {.5f, .5f, 1.f};
    case Facing::West: return {0.f, .5f, .5f};
    default: return {1.f, .5f, .5f};
    }
}
bool use(SimulatedPlayer& sp, GlobalPos const& p, Facing f) { return sp.simulateUseItemInSlotOnBlock(0, bp(p), f, faceHit(f)); }
bool use(SimulatedPlayer& sp, GlobalPos const& p, Facing f, Vec3 hit) {
    return sp.simulateUseItemInSlotOnBlock(0, bp(p), f, hit);
}
int countOf(Player& p, std::string const& item) {
    int  n   = 0;
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& it = inv.getItem(i);
        if (!it.isNull() && it.getTypeName() == item) n += it.mCount;
    }
    return n;
}
/// Uses the item in slot 0 on `p` until `done()` (a SimulatedPlayer's use is sometimes dropped).
ll::coro::CoroTask<bool> useUntil(Context& ctx, SimulatedPlayer& sp, GlobalPos p, Facing f, std::function<bool()> done, std::optional<Vec3> hit = std::nullopt) {
    for (int i = 0; i < 4; ++i) {
        if (hit) use(sp, p, f, *hit);
        else use(sp, p, f);
        co_await ctx.ticks(6);
        if (done()) co_return true;
    }
    co_return false;
}
void clearSlot0(Context& ctx, Player& p) { ctx.give(p, ItemStack{}, 0); }

} // namespace

LV_REGISTER_SELFTEST("building.registered", [](Context& ctx) {
    std::string missing;
    for (auto const* info : b::allBlocks())
        if (!lv::game::blockWith(info->id)) missing += " " + info->id;
    ctx.require(missing.empty(), "every building block is registered (missing:" + missing + ")");
    ctx.check(b::allBlocks().size() == 132, "132 building blocks (" + std::to_string(b::allBlocks().size()) + ")");
});

LV_REGISTER_SELFTEST("building.casing_connected_textures", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1000);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:andesite_casing");
    ctx.setBlock(at(p, 1, 0, 0), "levitate:andesite_casing");
    ctx.setBlock(at(p, 0, 1, 0), "levitate:brass_casing");
    co_await ctx.ticks(3);
    ctx.check(st(p, "levitate:c_east") == 1, "casing connects to the casing east of it");
    ctx.check(st(at(p, 1, 0, 0), "levitate:c_west") == 1, "and that one back");
    ctx.check(st(p, "levitate:c_up") == 0, "no connection to another casing type");
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(st(p, "levitate:c_east") == 0, "the connection goes with the neighbour");
    // windows connect vertically only
    ctx.setBlock(at(p, 0, 0, 2), "levitate:oak_window");
    ctx.setBlock(at(p, 0, 1, 2), "levitate:oak_window");
    co_await ctx.ticks(3);
    ctx.check(st(at(p, 0, 0, 2), "levitate:c_up") == 1 && st(at(p, 0, 1, 2), "levitate:c_down") == 1, "stacked windows connect");
});

LV_REGISTER_SELFTEST("building.pane_connections", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1005);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:framed_glass_pane");
    ctx.setBlock(at(p, 0, 0, -1), "minecraft:stone");
    ctx.setBlock(at(p, 1, 0, 0), "levitate:copper_bars");
    co_await ctx.ticks(3);
    ctx.check(st(p, "levitate:north") == 1, "pane attaches to a sturdy block");
    ctx.check(st(p, "levitate:east") == 1, "pane attaches to bars");
    ctx.check(st(p, "levitate:south") == 0 && st(p, "levitate:west") == 0, "no arms towards air");
    ctx.check(st(at(p, 1, 0, 0), "levitate:west") == 1, "bars attach to the pane");
    auto const* blk   = b::blockAt(p);
    auto        boxes = blk ? b::collisionBoxes(*b::infoOf(*blk), *blk, false, false) : std::nullopt;
    ctx.check(boxes && boxes->size() == 3, "pane collision: post and two arms");
});

LV_REGISTER_SELFTEST("building.stairs_shape", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1010);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:copper_shingle_stairs", {{"levitate:facing", b::North}});
    co_await ctx.ticks(2);
    ctx.check(st(p, "levitate:shape") == 0, "a lone stair is straight");
    auto const* blk   = b::blockAt(p);
    auto        boxes = blk ? b::collisionBoxes(*b::infoOf(*blk), *blk, false, false) : std::nullopt;
    ctx.check(boxes && boxes->size() == 3, "straight stair collision: slab and two quarters");
    // a stair in front facing east: outer right (Java StairBlock.getStairsShape)
    ctx.setBlock(at(p, 0, 0, -1), "levitate:copper_shingle_stairs", {{"levitate:facing", b::East}});
    co_await ctx.ticks(3);
    ctx.check(st(p, "levitate:shape") == 4, "outer right corner (" + std::to_string(st(p, "levitate:shape")) + ")");
    ctx.setBlock(at(p, 0, 0, -1), "minecraft:air");
    ctx.setBlock(at(p, 0, 0, 1), "levitate:copper_shingle_stairs", {{"levitate:facing", b::West}});
    co_await ctx.ticks(3);
    ctx.check(st(p, "levitate:shape") == 1, "inner left corner (" + std::to_string(st(p, "levitate:shape")) + ", back facing "
                                                 + std::to_string(st(at(p, 0, 0, 1), "levitate:facing")) + ")");
});

LV_REGISTER_SELFTEST("building.slab_merge", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1015);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    ctx.setBlock(at(p, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(p, "levitate:copper_tile_slab");
    ctx.give(sp, b::makeItem("levitate:copper_tile_slab", 4), 0);
    stand(sp, at(p, 2, 0, 0));
    look(sp, p);
    co_await ctx.ticks(2);
    int before = countOf(sp, "levitate:copper_tile_slab");
    use(sp, p, Facing::Up, Vec3{.5f, .5f, .5f});
    co_await ctx.ticks(2);
    ctx.check(st(p, "levitate:double") == 1, "a slab on a bottom slab makes a double slab");
    if (!sp.isCreative()) ctx.check(before - countOf(sp, "levitate:copper_tile_slab") == 1, "one slab used");
    clearSlot0(ctx, sp);
});

LV_REGISTER_SELFTEST("building.copper_wax_and_scrape", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1020);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    ctx.setBlock(p, "levitate:exposed_copper_shingles");
    stand(sp, at(p, 2, 0, 0));
    look(sp, p);
    co_await ctx.ticks(2);
    ctx.give(sp, b::makeItem("minecraft:honeycomb", 1), 0);
    co_await ctx.ticks(2);
    co_await useUntil(ctx, sp, p, Facing::Up, [&] { return id(p) == "levitate:waxed_exposed_copper_shingles"; });
    ctx.check(id(p) == "levitate:waxed_exposed_copper_shingles", "honeycomb waxes (" + id(p) + ")");
    ctx.give(sp, b::makeItem("minecraft:iron_axe", 1), 0);
    co_await ctx.ticks(6);
    co_await useUntil(ctx, sp, p, Facing::Up, [&] { return id(p) != "levitate:waxed_exposed_copper_shingles"; });
    ctx.check(id(p) == "levitate:exposed_copper_shingles", "the axe removes the wax (" + id(p) + ")");
    co_await useUntil(ctx, sp, p, Facing::Up, [&] { return id(p) != "levitate:exposed_copper_shingles"; });
    ctx.check(id(p) == "levitate:copper_shingles", "the axe scrapes one stage (" + id(p) + ")");
    clearSlot0(ctx, sp);
});

LV_REGISTER_SELFTEST("building.copper_weathering", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1025);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:copper_tile_stairs", {{"levitate:facing", b::East}});
    co_await ctx.ticks(1);
    auto never = [] { return 0.99f; };
    auto always = [] { return 0.f; };
    ctx.check(!b::copperRandomTick(p, never), "no change on an unlucky tick");
    ctx.check(b::copperRandomTick(p, always), "a lucky tick ages the block");
    ctx.check(id(p) == "levitate:exposed_copper_tile_stairs" && st(p, "levitate:facing") == b::East,
              "aged, states kept (" + id(p) + ", facing " + std::to_string(st(p, "levitate:facing")) + ")");
    // a less weathered block nearby stops it (Java ChangeOverTimeBlock)
    ctx.setBlock(at(p, 2, 0, 0), "levitate:copper_tiles");
    co_await ctx.ticks(1);
    ctx.check(!b::copperRandomTick(p, always), "a fresher neighbour keeps it");
    ctx.setBlock(at(p, 0, 0, 3), "levitate:waxed_copper_tiles");
    ctx.check(!b::copperRandomTick(at(p, 0, 0, 3), always), "waxed copper doesn't age");
});

LV_REGISTER_SELFTEST("building.door_place_and_double", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1030);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    for (int x = -1; x <= 2; ++x) ctx.setBlock(at(p, x, -1, 0), "minecraft:stone");
    for (int x = -1; x <= 2; ++x) ctx.cleanupBlock(at(p, x, 1, 0));
    stand(sp, at(p, 0, 0, 3));
    look(sp, at(p, 0, 0, -6)); // looking north
    co_await ctx.ticks(3);
    ctx.give(sp, b::makeItem("levitate:train_door", 2), 0);
    co_await useUntil(ctx, sp, at(p, 0, -1, 0), Facing::Up, [&] { return id(p) == "levitate:train_door"; });
    ctx.require(id(p) == "levitate:train_door" && id(at(p, 0, 1, 0)) == "levitate:train_door", "door placed with both halves (" + id(p) + ")");
    ctx.check(st(p, "levitate:facing") == b::North, "door faces the way the player looks");
    ctx.check(st(at(p, 0, 1, 0), "levitate:upper") == 1, "upper half");
    co_await ctx.ticks(6);
    co_await useUntil(ctx, sp, at(p, 1, -1, 0), Facing::Up, [&] { return id(at(p, 1, 0, 0)) == "levitate:train_door"; });
    ctx.require(id(at(p, 1, 0, 0)) == "levitate:train_door", "second door placed");
    ctx.check(st(p, "levitate:hinge") != st(at(p, 1, 0, 0), "levitate:hinge"), "the second door mirrors the first (double door)");
    // any item that isn't sneaking-used opens the door (Java useWithoutItem)
    ctx.give(sp, b::makeItem("minecraft:stick", 1), 0);
    co_await ctx.ticks(6);
    co_await useUntil(ctx, sp, p, Facing::South, [&] { return st(p, "levitate:open") == 1; });
    ctx.check(st(p, "levitate:open") == 1 && st(at(p, 0, 1, 0), "levitate:open") == 1, "clicking opens both halves");
    ctx.check(st(at(p, 1, 0, 0), "levitate:open") == 1, "the double door opens too");
    // breaking the lower half removes the upper half
    ctx.setBlock(p, "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(id(at(p, 0, 1, 0)) != "levitate:train_door", "upper half removed with the lower half");
});

LV_REGISTER_SELFTEST("building.door_redstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1035);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:brass_door", {{"levitate:facing", b::South}});
    ctx.setBlock(at(p, 0, 1, 0), "levitate:brass_door", {{"levitate:facing", b::South}, {"levitate:upper", 1}});
    co_await ctx.ticks(2);
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(5);
    ctx.check(st(p, "levitate:open") == 1 && st(p, "levitate:powered") == 1, "redstone opens the door");
    ctx.check(st(at(p, 0, 1, 0), "levitate:open") == 1, "both halves");
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(5);
    ctx.check(st(p, "levitate:open") == 0, "losing power closes it");
    // trapdoor
    ctx.setBlock(at(p, 0, 0, 2), "levitate:train_trapdoor", {{"levitate:facing", b::North}});
    co_await ctx.ticks(2);
    ctx.setBlock(at(p, 1, 0, 2), "minecraft:redstone_block");
    co_await ctx.ticks(5);
    ctx.check(st(at(p, 0, 0, 2), "levitate:open") == 1, "redstone opens the trapdoor");
    ctx.check(b::useTrapdoor(nullptr, at(p, 0, 0, 2)) && st(at(p, 0, 0, 2), "levitate:open") == 0, "using it closes it again");
});

LV_REGISTER_SELFTEST("building.scaffolding", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1040);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    ctx.setBlock(at(p, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(p, "levitate:copper_scaffolding");
    for (int y = 1; y <= 3; ++y) ctx.cleanupBlock(at(p, 0, y, 0));
    stand(sp, at(p, 2, 0, 0));
    look(sp, p);
    co_await ctx.ticks(2);
    ctx.give(sp, b::makeItem("levitate:copper_scaffolding", 4), 0);
    co_await ctx.ticks(2);
    co_await useUntil(ctx, sp, p, Facing::East, [&] { return id(at(p, 0, 1, 0)) == "levitate:copper_scaffolding"; });
    co_await useUntil(ctx, sp, p, Facing::East, [&] { return id(at(p, 0, 2, 0)) == "levitate:copper_scaffolding"; });
    ctx.check(id(at(p, 0, 1, 0)) == "levitate:copper_scaffolding" && id(at(p, 0, 2, 0)) == "levitate:copper_scaffolding",
              "clicking scaffolding stacks it on top of the column");
    ctx.check(st(p, "levitate:bottom") == 0 && st(at(p, 0, 2, 0), "levitate:bottom") == 0, "supported scaffolding is not a bottom piece");
    ctx.setBlock(at(p, 1, 2, 0), "levitate:copper_scaffolding");
    co_await ctx.ticks(3);
    ctx.check(st(at(p, 1, 2, 0), "levitate:bottom") == 1, "floating scaffolding is a bottom piece");
    auto type = BlockType::tryGetFromRegistry(HashedString("levitate:copper_scaffolding"));
    ctx.check(type && (static_cast<std::uint64_t>(static_cast<BlockProperty>(type->mProperties)) & static_cast<std::uint64_t>(BlockProperty::Climbable)) != 0,
              "scaffolding is climbable");
    clearSlot0(ctx, sp);
});

LV_REGISTER_SELFTEST("building.ladder", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1045);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    for (int y = 0; y <= 2; ++y) ctx.setBlock(at(p, 0, y, -1), "minecraft:stone");
    for (int y = 1; y <= 2; ++y) ctx.cleanupBlock(at(p, 0, y, 0));
    ctx.cleanupBlock(p);
    stand(sp, at(p, 0, 0, 3));
    look(sp, at(p, 0, 0, -1));
    co_await ctx.ticks(3);
    ctx.give(sp, b::makeItem("levitate:brass_ladder", 4), 0);
    co_await useUntil(ctx, sp, at(p, 0, 0, -1), Facing::South, [&] { return id(p) == "levitate:brass_ladder"; });
    ctx.require(id(p) == "levitate:brass_ladder", "ladder placed on the wall (" + id(p) + ")");
    ctx.check(st(p, "levitate:facing") == b::South, "it faces away from the wall");
    auto type = BlockType::tryGetFromRegistry(HashedString("levitate:brass_ladder"));
    ctx.check(type && (static_cast<std::uint64_t>(static_cast<BlockProperty>(type->mProperties)) & static_cast<std::uint64_t>(BlockProperty::Climbable)) != 0,
              "ladders are climbable");
    // remove the wall: the ladder pops off
    ctx.setBlock(at(p, 0, 0, -1), "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(id(p) != "levitate:brass_ladder", "a ladder without a wall pops off");
    clearSlot0(ctx, sp);
});

LV_REGISTER_SELFTEST("building.girder", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1050);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:metal_girder", {{"levitate:axis", 1}});
    ctx.setBlock(at(p, 1, 0, 0), "levitate:metal_girder", {{"levitate:axis", 0}, {"levitate:x", 1}});
    co_await ctx.ticks(3);
    ctx.check(st(p, "levitate:x") == 1, "a beam along x joins the pole next to it");
    ctx.check(st(p, "levitate:z") == 0, "no beam along z");
    ctx.check(lv::kinetics::specOf(b::kGirderShaftId) != nullptr, "the encased girder is a kinetic shaft");
    ctx.setBlock(at(p, 0, 0, 2), b::kGirderShaftId, {{"levitate:axis", 2}});
    ctx.setBlock(at(p, 0, 1, 2), "levitate:metal_girder", {{"levitate:axis", 1}});
    co_await ctx.ticks(3);
    ctx.check(st(at(p, 0, 0, 2), "levitate:top") == 1, "the encased girder attaches to the girder above");
    if (lv::game::blockWith(b::kShaftId)) {
        auto& sp = simOf(ctx);
        ctx.setBlock(at(p, 0, 0, 4), "levitate:metal_girder", {{"levitate:axis", 1}});
        stand(sp, at(p, 2, 0, 4));
        look(sp, at(p, 0, 0, 4));
        co_await ctx.ticks(2);
        ctx.give(sp, b::makeItem(b::kShaftId, 1), 0);
        use(sp, at(p, 0, 0, 4), Facing::East);
        co_await ctx.ticks(2);
        ctx.check(id(at(p, 0, 0, 4)) == b::kGirderShaftId, "a shaft goes into the girder");
        clearSlot0(ctx, sp);
    } else {
        ctx.info("levitate:shaft not registered (U6): shaft insertion skipped");
    }
});

LV_REGISTER_SELFTEST("building.placard", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1055);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    ctx.setBlock(at(p, 0, 0, -1), "minecraft:stone");
    ctx.setBlock(p, "levitate:placard", {{"levitate:facing", b::South}});
    stand(sp, at(p, 0, 0, 2));
    look(sp, p);
    co_await ctx.ticks(2);
    ctx.give(sp, b::makeItem("minecraft:diamond", 2), 0);
    co_await useUntil(ctx, sp, p, Facing::South, [&] { return b::placardItem(p).has_value(); });
    ctx.check(b::placardItem(p) == std::optional<std::string>("minecraft:diamond"), "the placard takes the item");
    co_await ctx.ticks(6);
    co_await useUntil(ctx, sp, p, Facing::South, [&] { return st(p, "levitate:powered") == 1; });
    ctx.check(st(p, "levitate:powered") == 1, "a matching item powers the placard");
    co_await ctx.ticks(24);
    ctx.check(st(p, "levitate:powered") == 0, "the pulse ends");
    int before = countOf(sp, "minecraft:diamond");
    ctx.check(b::onAttack(sp, p), "punching takes the item");
    ctx.check(!b::placardItem(p) && countOf(sp, "minecraft:diamond") == before + 1, "the item is back in the inventory");
    clearSlot0(ctx, sp);
});

LV_REGISTER_SELFTEST("building.copycat", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1060);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    ctx.setBlock(p, "levitate:copycat_step", {{"levitate:facing", b::North}});
    stand(sp, at(p, 2, 0, 0));
    look(sp, p);
    co_await ctx.ticks(2);
    ctx.give(sp, b::makeItem("minecraft:oak_planks", 2), 0);
    co_await useUntil(ctx, sp, p, Facing::East, [&] { return b::copycatMaterial(p).has_value(); });
    ctx.check(b::copycatMaterial(p) == std::optional<std::string>("minecraft:oak_planks"), "the copycat takes the material");
    ctx.give(sp, b::makeItem("minecraft:chest", 1), 0);
    ctx.check(!b::useCopycat(sp, b::makeItem("minecraft:chest", 1), p, b::East), "containers are refused");
    ctx.setBlock(at(p, 0, 0, 2), "levitate:copycat_panel");
    ctx.check(!b::useCopycat(sp, b::makeItem("minecraft:chest", 1), at(p, 0, 0, 2), b::East), "containers are refused on an empty copycat");
    clearSlot0(ctx, sp);
});

LV_REGISTER_SELFTEST("building.bracket", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(1065);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = simOf(ctx);
    // a bracket is never placed on its own
    ctx.setBlock(at(p, 0, -1, 0), "minecraft:stone");
    ctx.check(b::useBracket(sp, b::makeItem("levitate:metal_bracket", 1), at(p, 0, -1, 0), b::Up), "the bracket item is handled");
    co_await ctx.ticks(1);
    ctx.check(id(p) != "levitate:metal_bracket" && !b::bracketAt(at(p, 0, -1, 0)), "nothing is placed on a plain block");
    if (!lv::game::blockWith(b::kShaftId)) {
        ctx.info("levitate:shaft not registered (U6): bracketing skipped");
        co_return;
    }
    ctx.setBlock(p, b::kShaftId);
    ctx.check(b::useBracket(sp, b::makeItem("levitate:metal_bracket", 1), p, b::North) && b::bracketAt(p), "a bracket goes onto a shaft");
});

// U59: tool requirements and copycat material rules (dx 5920..5939)
#include "feature/building/HarvestTests.inc"
