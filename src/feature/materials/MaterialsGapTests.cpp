// Selftests of the materials unit's U43 additions (testPos dx 4300..4349): wall heights and posts,
// layered / pillar connected textures, tool tags, furnace xp, chromatic compound stack size and
// belt light, block of experience particles.

#include <cmath>
#include <random>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/Container.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/FurnaceBlockActor.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltApi.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/materials/Materials.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::materials {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;

/// testPos puts dx 43xx 170 blocks south, outside the run's ticking area (z +-64): the U43 tests
/// with entities / block entities move 220 blocks north, to the row no unit uses (z -50..-46).
constexpr int kU43Shift = -220;

GP       at(GP const& p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockPos bp(GP const& p) { return BlockPos{p.x, p.y, p.z}; }
BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }
int st(Context& ctx, GP const& p, char const* name) { return game::intState(regionOf(ctx).getBlock(bp(p)), name).value_or(-1); }

} // namespace

LV_REGISTER_SELFTEST("materials.wall_heights", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4300);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto const* wall = game::blockWith("levitate:cut_granite_wall");
    ctx.require(wall != nullptr, "the wall is registered");
    // a straight east-west line: no post, low sides
    ctx.setBlock(at(p, -1, 0, 0), "levitate:cut_granite_wall");
    ctx.setBlock(p, "levitate:cut_granite_wall");
    ctx.setBlock(at(p, 1, 0, 0), "levitate:cut_granite_wall");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, "minecraft:connection_east") == 1 && st(ctx, p, "minecraft:connection_west") == 1, "the walls connect (connection trait)");
    ctx.check(st(ctx, p, "levitate:up") == 0, "a straight wall has no post");
    ctx.check(st(ctx, p, "levitate:tall_east") == 0, "sides are low under air");
    // a full block above: tall sides, still no post (Java: tall line)
    ctx.setBlock(at(p, 0, 1, 0), "minecraft:stone");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, "levitate:tall_east") == 1 && st(ctx, p, "levitate:tall_west") == 1, "a block above makes the sides tall");
    ctx.check(st(ctx, p, "levitate:up") == 0, "a tall straight line has no post");
    // a torch above (WALL_POST_OVERRIDE): low sides, post
    ctx.setBlock(at(p, 0, 1, 0), "minecraft:torch");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, "levitate:tall_east") == 0, "a torch does not cover the sides");
    ctx.check(st(ctx, p, "levitate:up") == 1, "a torch above forces the post");
    // the end of the line: post (sides differ)
    ctx.check(st(ctx, at(p, 1, 0, 0), "levitate:up") == 1, "a wall end has a post");
    // collision 24 px (Java)
    auto boxes = wallCollision(regionOf(ctx).getBlock(bp(p)));
    bool high  = !boxes.empty();
    for (auto const& b : boxes) high = high && b[4] == 24.f;
    ctx.check(high, "wall collision is 24 px high");
});

LV_REGISTER_SELFTEST("materials.layered_connections", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4305);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:layered_granite");
    ctx.setBlock(at(p, 1, 0, 0), "levitate:layered_granite");
    ctx.setBlock(at(p, 0, 0, 1), "levitate:cut_granite");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, "levitate:c_east") == 1, "layered stone joins the same block");
    ctx.check(st(ctx, at(p, 1, 0, 0), "levitate:c_west") == 1, "both ways");
    ctx.check(st(ctx, p, "levitate:c_south") == 0, "not another block");
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, "levitate:c_east") == 0, "the join goes with the neighbour");
});

LV_REGISTER_SELFTEST("materials.pillar_connections", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4310);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    // two vertical columns: 2 high next to 1 high -> no side join (Create: same column extents only)
    ctx.setBlock(p, "levitate:granite_pillar");
    ctx.setBlock(at(p, 0, 1, 0), "levitate:granite_pillar");
    ctx.setBlock(at(p, 1, 0, 0), "levitate:granite_pillar");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, "levitate:c_up") == 1 && st(ctx, at(p, 0, 1, 0), "levitate:c_down") == 1, "a column joins along its axis");
    ctx.check(st(ctx, p, "levitate:c_east") == 0, "columns of different length do not join sideways");
    ctx.setBlock(at(p, 1, 1, 0), "levitate:granite_pillar");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, "levitate:c_east") == 1 && st(ctx, at(p, 0, 1, 0), "levitate:c_east") == 1, "equal columns join into a rectangle");
    ctx.check(st(ctx, at(p, 1, 1, 0), "levitate:c_west") == 1, "both ways");
});

LV_REGISTER_SELFTEST("materials.tool_speeds", [](Context& ctx) {
    auto const* stone = game::blockWith("levitate:cut_granite");
    auto const* ore   = game::blockWith("levitate:zinc_ore");
    auto const* card  = game::blockWith("levitate:cardboard_block");
    ctx.require(stone && ore && card, "blocks registered");
    auto speed = [](char const* tool, Block const& b) {
        auto s = makeStack(tool, 1);
        return s.isNull() || !s.mItem ? 0.f : s.mItem->getDestroySpeed(s, b);
    };
    ctx.check(speed("minecraft:iron_pickaxe", *stone) > 1.f, "a pickaxe mines palette stone fast");
    ctx.check(speed("minecraft:iron_pickaxe", *ore) > 1.f, "an iron pickaxe mines zinc ore fast");
    ctx.check(speed("minecraft:iron_axe", *card) > 1.f, "an axe mines cardboard fast");
    ctx.check(speed("minecraft:iron_axe", *stone) <= 1.f, "an axe is no pickaxe");
});

LV_REGISTER_SELFTEST("materials.furnace_xp", [](Context& ctx) -> ll::coro::CoroTask<> {
    ctx.check(createSmeltingXp("levitate:crushed_raw_iron") == 0.1f, "crushed ores give 0.1");
    ctx.check(createSmeltingXp("levitate:raw_zinc") == 0.7f, "raw zinc gives 0.7");
    ctx.check(createSmeltingXp("levitate:zinc_ore") == 1.0f, "zinc ore gives 1");
    ctx.check(!createSmeltingXp("minecraft:raw_iron").has_value(), "vanilla recipes keep theirs");
    auto p = ctx.testPos(4320, kU43Shift);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "minecraft:blast_furnace");
    co_await ctx.ticks(2);
    auto& region = regionOf(ctx);
    auto* be     = static_cast<FurnaceBlockActor*>(region.getBlockEntity(bp(p)));
    ctx.require(be != nullptr, "the blast furnace has its block entity");
    auto* box = be->getContainer();
    ctx.require(box != nullptr, "the furnace's container");
    box->setItem(0, makeStack("levitate:zinc_ore", 1));
    box->setItem(1, makeStack("minecraft:coal", 1));
    for (int i = 0; i < 160 && box->getItem(2).isNull(); ++i) {
        co_await ctx.ticks(1);
        be = static_cast<FurnaceBlockActor*>(region.getBlockEntity(bp(p)));
        box = be ? be->getContainer() : nullptr;
        if (!box) break;
    }
    ctx.require(box && !box->getItem(2).isNull(), "the ore was smelted");
    ctx.check(box->getItem(2).getTypeName() == "levitate:zinc_ingot", "into a zinc ingot");
    int xp = be->withdrawStoredXPReward();
    ctx.check(xp == 1, "1 xp (Create's zinc_ingot_from_ore), got " + std::to_string(xp));
});

LV_REGISTER_SELFTEST("materials.chromatic_stack_size", [](Context& ctx) {
    auto plain = makeStack("levitate:chromatic_compound", 1);
    ctx.require(!plain.isNull(), "chromatic compound registered");
    ctx.check(plain.getMaxStackSize() == 16, "an empty compound stacks to 16");
    auto charged = plain;
    setChromaticLight(charged, 3);
    ctx.check(charged.getMaxStackSize() == 1, "a charged compound stacks to 1 (Create's bar)");
});

// The client e2e scenarios need a charged compound in the real player's hotbar (the charge bar):
// every non-simulated player gets one with 6 of 10 charges (no player: nothing to do).
LV_REGISTER_SELFTEST("materials.chromatic_bar_item", [](Context& ctx) {
    auto charged = makeStack("levitate:chromatic_compound", 1);
    ctx.require(!charged.isNull(), "chromatic compound registered");
    setChromaticLight(charged, 6);
    int given = 0;
    ctx.player().getLevel().forEachPlayer([&](Player& p) {
        if (p.isSimulated()) return true;
        ItemStack s = charged;
        if (p.add(s)) ++given;
        p.sendInventory(false);
        return true;
    });
    ctx.info("charged compounds given: " + std::to_string(given));
});

LV_REGISTER_SELFTEST("materials.chromatic_belt_light", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4330, kU43Shift);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    if (!game::blockWith(belts::kDepotId)) {
        ctx.info("levitate:depot not registered (U11): skipped");
        co_return;
    }
    // ensureLoaded drops the test player onto the depot: once it lands (a few ticks, later on a
    // busy server) a survival player picks up the compound beside it. A spectator picks up nothing.
    ctx.player().setPlayerGameType(GameType::Spectator);
    ctx.setBlock(p, belts::kDepotId);
    co_await ctx.ticks(2);
    belts::TransportedItem torch;
    torch.stack = makeStack("minecraft:torch", 2);
    auto rest   = common::items::insert(regionOf(ctx), bp(p), 1, torch.stack, false);
    ctx.require(rest.isNull() || rest.mCount == 0, "the depot holds the torches");
    auto& region = regionOf(ctx);
    auto* item   = region.getLevel().getSpawner().spawnItem(region, makeStack("levitate:chromatic_compound", 1), nullptr, Vec3{p.x + 1.6f, p.y + .1f, p.z + .5f}, 0);
    ctx.require(item != nullptr, "the compound is in the world");
    ctx.check(absorbFromTransporter(*item, bp(p)), "the compound takes a light from the depot");
    int left = 0;
    for (auto const& t : belts::itemsAt(p)) left += t.stack.isNull() ? 0 : t.stack.mCount;
    ctx.check(left == 1, "one torch is gone");
    // Read the charged compound now: once it ticks it looks for light again (half the ticks, a
    // random block within 3) and may take the second torch from the depot, a charge of 2.
    int         charged = 0;
    std::string seen;
    AABB box{Vec3{p.x - 2.f, p.y - 1.f, p.z - 2.f}, Vec3{p.x + 3.f, p.y + 4.f, p.z + 3.f}};
    for (auto* a : region.getEntities(box, 0.f, ActorType::ItemEntity)) {
        auto& s = static_cast<ItemActor*>(a)->mItem.get();
        if (!s.isNull()) seen += " " + s.getTypeName() + "x" + std::to_string(s.mCount) + "/" + std::to_string(chromaticLight(s));
        if (!s.isNull() && s.getTypeName() == "levitate:chromatic_compound") {
            charged = std::max(charged, chromaticLight(s));
            a->remove();
        }
    }
    ctx.check(charged == 1, "the compound has one charge:" + seen);
});

LV_REGISTER_SELFTEST("materials.experience_particles", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4335);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:experience_block");
    co_await ctx.ticks(1);
    std::mt19937 rng{7};
    int          n = 0, near = 0;
    for (int i = 0; i < 400; ++i) {
        animateExperienceBlocks(regionOf(ctx), bp(p), rng, [&](Vec3 const& v, Vec3 const& vel) {
            ++n;
            bool inside = std::abs(v.x - (p.x + .5f)) <= .551f && std::abs(v.y - (p.y + .5f)) <= .551f && std::abs(v.z - (p.z + .5f)) <= .551f;
            if (inside && std::abs(vel.x) < .05f) ++near;
        });
    }
    // per tick: 667 samples of +-16 hit the centre with (1/16)^3 each, +-32 with (1/32)^3, 1 in 5 emits
    ctx.check(n > 0, "the block of experience sheds end rod particles (" + std::to_string(n) + ")");
    ctx.check(near == n, "around the block, slow");
});

} // namespace lv::feature::materials
