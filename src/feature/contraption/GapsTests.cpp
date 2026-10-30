// Selftests of U38's contraption engine additions (dx 3800..3809 in U38's row, see U38Test.h):
// discard, carried block states and door movement, sticker redstone watch, paused actors.

#include "ll/api/service/Bedrock.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/contraption/Internal.h"
#include "feature/contraption/U38Test.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::contraption {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;
namespace bd = ::lv::feature::common::blockdata;

GP      at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
cc::Pos ccp(GP const& p) { return {p.x, p.y, p.z}; }

ll::coro::CoroTask<GP> area(Context& ctx, int n) {
    auto p = ctx.testPos(3800 + n, kU38Dz);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    co_return p;
}

std::string idAt(Context& ctx, GP const& p) { return game::blockIdAt(ctx.player().getDimensionBlockSource(), game::blockPos(p)); }

std::int64_t carriedState(Contraption const& c, cc::Pos const& local, std::string const& key) {
    for (auto const& b : c.blocks()) {
        if (b.local != local) continue;
        auto it = b.states.find(key);
        if (it == b.states.end()) return -1;
        auto const* v = std::get_if<std::int64_t>(&it->second);
        return v ? *v : -1;
    }
    return -1;
}

struct Guard {
    std::vector<Id> ids;
    ~Guard() {
        for (auto id : ids)
            if (find(id)) disassemble(id);
    }
};

} // namespace

// R5: Create's discard: the contraption goes, nothing is placed or dropped
LV_REGISTER_SELFTEST("contraption.discard", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 0);
    ctx.setBlock(p, "minecraft:gold_block");
    AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    auto r          = assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    ctx.check(idAt(ctx, p) == "minecraft:air", "the block was lifted");
    ctx.check(discard(*r.id), "discarded");
    co_await ctx.ticks(2);
    ctx.check(!find(*r.id), "the contraption is gone");
    ctx.check(idAt(ctx, p) == "minecraft:air", "nothing was placed");
    ctx.check(!discard(*r.id), "an unknown id is refused");
});

// R14: the sticker reacts to redstone through the shared watch (no polling of its own)
LV_REGISTER_SELFTEST("contraption.sticker_watch", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 2);
    ctx.setBlock(p, std::string(kSticker), {{"levitate:facing", 1}, {"levitate:extended", 0}});
    detail::trackSticker(p.dim, ccp(p));
    co_await ctx.ticks(4);
    ctx.check(game::intState(ctx.player().getDimensionBlockSource().getBlock(game::blockPos(p)), "levitate:extended").value_or(-1) == 0, "unpowered: retracted");
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(5);
    ctx.check(game::intState(ctx.player().getDimensionBlockSource().getBlock(game::blockPos(p)), "levitate:extended").value_or(-1) == 1, "powered: extended");
    ctx.setBlock(at(p, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(5);
    ctx.check(game::intState(ctx.player().getDimensionBlockSource().getBlock(game::blockPos(p)), "levitate:extended").value_or(-1) == 0, "retracted again");
});

// R142 / R106: carried states change (setBlockStates); doors close on a contraption without door
// control and open at an elevator floor whose contact allows them
LV_REGISTER_SELFTEST("contraption.doors", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto  p = co_await area(ctx, 4);
    Guard g;
    std::vector<std::pair<std::string, int>> lower{{"levitate:facing", 2}, {"levitate:upper", 0}, {"levitate:open", 1}};
    std::vector<std::pair<std::string, int>> upper{{"levitate:facing", 2}, {"levitate:upper", 1}, {"levitate:open", 1}};
    ctx.setBlock(p, "minecraft:stone");
    ctx.setBlock(at(p, 0, 1, 0), "levitate:train_door", lower);
    ctx.setBlock(at(p, 0, 2, 0), "levitate:train_door", upper);
    AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    auto r          = assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.ids.push_back(*r.id);
    ctx.require(find(*r.id)->blocks().size() == 3, "stone and both door halves");
    co_await ctx.ticks(4);
    auto* c = find(*r.id);
    ctx.require(c != nullptr, "still there");
    ctx.check(carriedState(*c, {0, 1, 0}, "levitate:open") == 0 && carriedState(*c, {0, 2, 0}, "levitate:open") == 0, "no door control: the door closed");
    ctx.check(setBlockStates(*r.id, {0, 0, 0}, {{"levitate:open", std::int64_t{1}}}) == false, "a state the block doesn't have changes nothing");
    discard(*r.id);

    // an elevator cabin standing at a floor whose contact opens every door
    auto q = at(p, 3, 0, 0);
    ctx.setBlock(q, "minecraft:stone");
    ctx.setBlock(at(q, 0, 1, 0), "levitate:train_door", {{"levitate:facing", 2}, {"levitate:upper", 0}, {"levitate:open", 0}});
    ctx.setBlock(at(q, 0, 2, 0), "levitate:train_door", {{"levitate:facing", 2}, {"levitate:upper", 1}, {"levitate:open", 0}});
    GP anchor = at(q, 0, 4, 0), contact = at(q, -1, 0, 0);
    ctx.setBlock(anchor, "minecraft:stone");
    ctx.setBlock(contact, "minecraft:stone");
    bd::edit(anchor)["pistons"] = {{"arrived", true}, {"col", {{"x", contact.x}, {"z", contact.z}, {"side", 2}}}, {"ey", double(q.y)}, {"cy", 0}};
    bd::edit(contact)["pistons"] = {{"door", 0}};
    AssembleOptions e;
    e.anchor = anchor;
    e.start  = {ccp(q)};
    e.kind   = "elevator";
    auto r2  = assemble(e);
    ctx.require(r2.ok(), "cabin assembled: " + r2.errorText);
    g.ids.push_back(*r2.id);
    cc::Pos doorLocal = ccp(at(q, 0, 1, 0)) - ccp(anchor);
    co_await ctx.ticks(4);
    c = find(*r2.id);
    ctx.require(c != nullptr, "cabin there");
    ctx.check(carriedState(*c, doorLocal, "levitate:open") == 1, "the elevator door opened at the floor");
    // "keep doors closed"
    bd::edit(contact)["pistons"]["door"] = 5;
    co_await ctx.ticks(3);
    c = find(*r2.id);
    ctx.check(c && carriedState(*c, doorLocal, "levitate:open") == 0, "closed again by the contact's door control");
    discard(*r2.id);
    (void)bd::take(anchor);
    (void)bd::take(contact);
});

// R109: paused actors don't tick while the contraption moves
LV_REGISTER_SELFTEST("contraption.actors_paused", [](Context& ctx) -> ll::coro::CoroTask<> {
    static int ticks = 0;
    static bool registered = false;
    if (!registered) {
        registered = true;
        MovementBehaviour b;
        b.tick = [](MovementContext&) { ++ticks; };
        registerActor("minecraft:sponge", std::move(b));
    }
    auto p = co_await area(ctx, 6);
    ctx.setBlock(p, "minecraft:sponge");
    AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    auto  r         = assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    Guard g;
    g.ids.push_back(*r.id);
    auto* c = find(*r.id);
    Motion m;
    m.velocity = {0, 0.05, 0};
    m.ticks    = 20;
    c->setMotion(m);
    c->setActorsPaused(true);
    ticks = 0;
    co_await ctx.ticks(5);
    ctx.check(ticks == 0, "no actor tick while paused: " + std::to_string(ticks));
    if ((c = find(*r.id))) c->setActorsPaused(false);
    co_await ctx.ticks(5);
    ctx.check(ticks > 0, "actors tick again");
    if ((c = find(*r.id))) c->stop();
    co_await ctx.ticks(2);
    discard(*r.id);
});

} // namespace lv::feature::contraption
