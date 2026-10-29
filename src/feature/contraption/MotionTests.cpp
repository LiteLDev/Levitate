// Selftests of contraption motion on clients (U34, testPos dx 3400..3499): the transform sent to
// companions, the display entities' spawn height and the respawn steps for vanilla viewers.

#include <cmath>
#include <set>

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"

#include "ll/api/service/Bedrock.h"

#include "feature/contraption/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::contraption {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;

GP      at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
cc::Pos ccp(GP const& p) { return {p.x, p.y, p.z}; }

bool place(Context& ctx, GP const& p, std::string const& id) {
    auto const* block = game::blockWith(id);
    if (!block) return false;
    ctx.cleanupBlock(p);
    return game::setBlock(ctx.player().getDimensionBlockSource(), game::blockPos(p), *block);
}

/// A glued pair (stone at `p`, gold east of it) assembled with the test anchor.
std::optional<Id> assemblePair(Context& ctx, GP const& p) {
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 1, 0, 0), "minecraft:gold_block");
    addGlue(p.dim, cc::GlueBox::of(ccp(p), ccp(at(p, 1, 0, 0))));
    AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    auto r          = assemble(o);
    return r.id;
}

::Actor* display(std::int64_t id) {
    auto level = ll::service::getLevel();
    if (!level || !id) return nullptr;
    ActorUniqueID uid;
    uid.rawID = id;
    return level->fetchEntity(uid, false);
}

struct Cleanup {
    std::optional<Id> id;
    GP                glueAt;
    Player*           companion = nullptr;
    ~Cleanup() {
        detail::forceVanillaViewers(false);
        if (companion) net::setTestCompanion(*companion, false);
        if (id && find(*id)) disassemble(*id);
        clearGlue(glueAt.dim, ccp(glueAt), 6);
    }
};

} // namespace

// Companions get the transform with the displays and every change of it, once per tick.
LV_REGISTER_SELFTEST("contraptionmotion.sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3400);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& player = ctx.player();
    Cleanup guard;
    guard.glueAt = p;
    net::setTestCompanion(player, true);
    guard.companion = &player;
    co_await ctx.ticks(2);
    (void)net::takeTestMessages(player);

    guard.id = assemblePair(ctx, p);
    ctx.require(guard.id.has_value(), "assembled");
    co_await ctx.ticks(2);
    bool sawBlocks = false;
    for (auto const& m : net::takeTestMessages(player)) {
        if (m.channel != "contraption.blocks" || m.payload.value("id", Id{0}) != *guard.id) continue;
        auto t    = m.payload.value("transform", nlohmann::json());
        sawBlocks = t.is_array() && t.size() == 8 && std::abs(t[3].get<double>() - p.y) < 1e-6;
    }
    ctx.check(sawBlocks, "contraption.blocks carries the transform");

    Motion mo;
    mo.velocity = {0, 0.2, 0};
    mo.ticks    = 10;
    find(*guard.id)->setMotion(mo);
    co_await ctx.ticks(13);
    int    updates = 0;
    double lastY   = 0;
    for (auto const& m : net::takeTestMessages(player)) {
        if (m.channel != "contraption.motion") continue;
        for (auto const& e : m.payload.value("m", nlohmann::json::array())) {
            if (!e.is_array() || e.size() != 9 || e[0].get<Id>() != *guard.id) continue;
            ++updates;
            lastY = e[4].get<double>();
        }
    }
    ctx.check(updates == 10, "one motion update per moving tick: " + std::to_string(updates));
    ctx.check(std::abs(lastY - (p.y + 2)) < 1e-6, "the last update is the final place: " + std::to_string(lastY));
    for (int dx : {0, 1}) ctx.cleanupBlock(at(p, dx, 2, 0));
});

// Display entities stand at their block's place from the moment they are spawned: a vanilla
// client shows each one where it was spawned (respawn steps), the client mod moves it from there.
LV_REGISTER_SELFTEST("contraptionmotion.spawn_height", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3410);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    Cleanup guard;
    guard.glueAt = p;
    guard.id     = assemblePair(ctx, p);
    ctx.require(guard.id.has_value(), "assembled");
    auto* c = find(*guard.id);
    ctx.require(c && c->blocks().size() == 2, "two blocks");
    for (auto const& b : c->blocks()) {
        auto* a = display(b.display);
        ctx.check(a != nullptr, "display spawned");
        if (!a) continue;
        auto feet = a->getFeetPos();
        ctx.check(std::abs(feet.y - p.y) < 1e-3, "spawned with its feet on the block's bottom: " + std::to_string(feet.y));
        ctx.check(std::abs(feet.x - (p.x + b.local.x + 0.5)) < 1e-3, "spawned at the block's centre");
    }
    co_return;
});

// With a vanilla viewer, moving displays are spawned again as they go (half a block at a
// time, at most every other tick), and end exactly where the structure stops.
LV_REGISTER_SELFTEST("contraptionmotion.vanilla_steps", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3420);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    Cleanup guard;
    guard.glueAt = p;
    guard.id     = assemblePair(ctx, p);
    ctx.require(guard.id.has_value(), "assembled");

    // without a vanilla viewer the displays only move
    std::set<std::int64_t> first;
    for (auto const& b : find(*guard.id)->blocks()) first.insert(b.display);
    Motion mo;
    mo.velocity = {0, 0.1, 0};
    mo.ticks    = 20;
    find(*guard.id)->setMotion(mo);
    co_await ctx.ticks(23);
    std::set<std::int64_t> after;
    for (auto const& b : find(*guard.id)->blocks()) after.insert(b.display);
    ctx.check(after == first, "companions only: the same display entities");

    detail::forceVanillaViewers(true);
    std::set<std::int64_t> seen = after;
    find(*guard.id)->setMotion(mo); // two more blocks up, 0.1 per tick
    for (int i = 0; i < 20 + 12; ++i) {
        co_await ctx.ticks(1);
        for (auto const& b : find(*guard.id)->blocks()) seen.insert(b.display);
    }
    auto respawns = static_cast<int>(seen.size() - after.size());
    // 2 blocks of 2 each: a step every 5 ticks (0.5 block) -> about 4 per block
    ctx.check(respawns >= 6 && respawns <= 12, "displays spawned again in steps: " + std::to_string(respawns));
    for (auto const& b : find(*guard.id)->blocks()) {
        auto* a = display(b.display);
        ctx.check(a != nullptr, "display alive");
        if (a) ctx.check(std::abs(a->getFeetPos().y - (p.y + 4)) < 1e-3, "display at the final place: " + std::to_string(a->getFeetPos().y));
    }
    detail::forceVanillaViewers(false);
    for (int dx : {0, 1}) ctx.cleanupBlock(at(p, dx, 4, 0));
    ctx.require(disassemble(*guard.id), "disassembled");
    ctx.check(game::blockIdAt(ctx.player().getDimensionBlockSource(), game::blockPos(at(p, 0, 4, 0))) == "minecraft:stone", "placed 4 up");
});

} // namespace lv::feature::contraption
