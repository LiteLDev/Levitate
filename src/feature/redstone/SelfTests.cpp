// Selftests of the redstone unit (U23, testPos dx 2300..2399). Each test builds its circuit on
// the ground of the test area, lets the game tick and reads block states (vanilla wire power,
// our blocks' states) back.

#include <algorithm>
#include <string>
#include <vector>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/net/Net.h"
#include "feature/redstone/Internal.h"
#include "feature/redstone/Redstone.h"
#include "feature/redstone/RedstoneLink.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::redstone {

namespace {

using lv::selftest::Context;

GP at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

Block const& blockAt(Context& ctx, GP const& p) { return regionOf(ctx).getBlock(game::blockPos(p)); }

int st(Context& ctx, GP const& p, char const* name) { return state(blockAt(ctx, p), name, -1); }

/// Vanilla redstone dust power at `p`.
int wire(Context& ctx, GP const& p) { return state(blockAt(ctx, p), "redstone_signal", -1); }

bool place(Context& ctx, GP const& p, std::string const& id, std::vector<std::pair<std::string, int>> const& states = {}) {
    return ctx.setBlock(p, id, states);
}

bool placeVanilla(Context& ctx, GP const& p, std::string const& id, Json const& states) {
    auto const* b = vanillaBlock(id, states);
    if (!b) return false;
    ctx.cleanupBlock(p);
    return game::setBlock(regionOf(ctx), game::blockPos(p), *b);
}

ItemStack item(std::string const& id, int count = 1) {
    auto s = common::items::make(id, count);
    return s ? *s : ItemStack{};
}

/// Clicks a face of `p` with the item in slot 0 at face-local `hit` (block coordinates 0..1).
bool click(SimulatedPlayer& sp, GP const& p, int face, Vec3 hit) {
    return sp.simulateUseItemInSlotOnBlock(0, game::blockPos(p), static_cast<ScriptModuleMinecraft::ScriptFacing>(face), hit);
}

/// Puts `stack` into hotbar slot 0 (the slot the clicks use).
void hold(Context& ctx, SimulatedPlayer& sp, ItemStack const& stack) { ctx.give(sp, stack, 0); }

} // namespace

// ---- signals in and out --------------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.output", [](Context& ctx) -> ll::coro::CoroTask<> {
    // our blocks drive vanilla dust through their redstone_producer permutations
    auto p = ctx.testPos(2300);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(game::blockWith(kLever) != nullptr, "levitate:analog_lever is registered");
    ctx.require(place(ctx, p, kLever, {{sFace, 0}, {sFacing, kNorth}, {sLeverState, 9}, {sPower, 9}}), "lever placed");
    place(ctx, at(p, 1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(6);
    ctx.check(wire(ctx, at(p, 1, 0, 0)) == 9, "dust next to a lever at 9 carries 9, got " + std::to_string(wire(ctx, at(p, 1, 0, 0))));
    setStates(regionOf(ctx), game::blockPos(p), {{sLeverState, 4}, {sPower, 4}});
    co_await ctx.ticks(6);
    ctx.check(wire(ctx, at(p, 1, 0, 0)) == 4, "the dust follows the lever down to 4, got " + std::to_string(wire(ctx, at(p, 1, 0, 0))));

    // a diode only powers its front
    auto q = at(p, 0, 0, 3);
    place(ctx, q, kToggleLatch, {{sFacing, kWest}, {sPowering, 1}}); // back west: output east
    place(ctx, at(q, 1, 0, 0), "minecraft:redstone_wire");
    place(ctx, at(q, 0, 0, 1), "minecraft:redstone_wire");
    co_await ctx.ticks(6);
    ctx.check(wire(ctx, at(q, 1, 0, 0)) == 15, "the latch powers the dust in front, got " + std::to_string(wire(ctx, at(q, 1, 0, 0))));
    ctx.check(wire(ctx, at(q, 0, 0, 1)) == 0, "but not the dust beside it, got " + std::to_string(wire(ctx, at(q, 0, 0, 1))));
});

LV_REGISTER_SELFTEST("redstone.input", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2305);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    auto  bp     = game::blockPos(p);
    place(ctx, at(p, -1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(2);
    ctx.check(signalFrom(region, bp, kWest) == 15, "a redstone block powers its neighbour");
    ctx.check(signalFrom(region, bp, kEast) == 0, "nothing from the east");
    // dust: a redstone block, two dust, then our position
    auto q = at(p, 0, 0, 3);
    place(ctx, at(q, -3, 0, 0), "minecraft:redstone_block");
    place(ctx, at(q, -2, 0, 0), "minecraft:redstone_wire");
    place(ctx, at(q, -1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(6);
    ctx.check(signalFrom(region, game::blockPos(q), kWest) == 14, "dust two blocks from the source gives 14, got " + std::to_string(signalFrom(region, game::blockPos(q), kWest)));
    // a repeater only powers what it faces (a repeater set by command stays unpowered, so this
    // only checks the sideways case; see SPEC §8 for the facing case)
    auto r = at(p, 0, 0, 6);
    ctx.require(placeVanilla(ctx, at(r, -1, 0, 0), "minecraft:unpowered_repeater", Json::object()), "repeater placed");
    place(ctx, at(r, -2, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(6);
    ctx.info("repeater: " + circuitDebug(region, game::blockPos(at(r, -1, 0, 0))));
    ctx.check(signalFrom(region, game::blockPos(at(r, -1, 0, 1)), kNorth) == 0, "nothing sideways");
    // a redstone torch standing on the ground powers its sides
    auto l = at(p, 0, 0, 9);
    ctx.require(placeVanilla(ctx, at(l, 0, 0, -1), "minecraft:redstone_torch", {{"torch_facing_direction", "top"}}), "torch placed");
    co_await ctx.ticks(6);
    ctx.info("torch: " + circuitDebug(region, game::blockPos(at(l, 0, 0, -1))));
    ctx.check(signalFrom(region, game::blockPos(l), kNorth) == 15, "a lit torch powers its neighbours");
});

// ---- links -------------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.link", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2310);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto tx = p, rx = at(p, 6, 0, 0), other = at(p, 6, 0, 3);
    ctx.require(place(ctx, tx, kLink, {{sFacing, kUp}}), "transmitter placed");
    place(ctx, rx, kLink, {{sFacing, kUp}, {sReceiver, 1}});
    place(ctx, other, kLink, {{sFacing, kUp}, {sReceiver, 1}});
    co_await ctx.ticks(2);
    for (auto const& gp : {tx, rx}) {
        setFrequency(gp, true, item("minecraft:diamond"));
        setFrequency(gp, false, item("minecraft:gold_ingot"));
    }
    setFrequency(other, true, item("minecraft:emerald"));
    // an analog source: our lever at 11 next to the transmitter
    place(ctx, at(tx, -1, 0, 0), kLever, {{sFace, 0}, {sFacing, kNorth}, {sLeverState, 11}, {sPower, 11}});
    place(ctx, at(rx, 1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(8);
    ctx.check(st(ctx, tx, sPowered) == 1, "the transmitter shows its input");
    ctx.check(st(ctx, rx, sPower) == 11, "the receiver carries the analog strength 11, got " + std::to_string(st(ctx, rx, sPower)));
    ctx.check(wire(ctx, at(rx, 1, 0, 0)) == 11, "dust next to the receiver gets 11, got " + std::to_string(wire(ctx, at(rx, 1, 0, 0))));
    ctx.check(st(ctx, other, sPower) == 0, "another frequency stays off");
    // switch the input off
    setStates(regionOf(ctx), game::blockPos(at(tx, -1, 0, 0)), {{sLeverState, 0}, {sPower, 0}});
    co_await ctx.ticks(6);
    ctx.check(st(ctx, rx, sPower) == 0, "the receiver follows the transmitter off");
    // mode toggle keeps the frequencies
    toggleLinkMode(regionOf(ctx), tx);
    co_await ctx.ticks(2);
    ctx.check(st(ctx, tx, sReceiver) == 1, "toggled to receiver");
    auto [a, b] = frequencies(tx);
    ctx.check(common::items::idOf(a) == "minecraft:diamond" && common::items::idOf(b) == "minecraft:gold_ingot", "frequencies kept");
});

LV_REGISTER_SELFTEST("redstone.link_api", [](Context& ctx) {
    // the contract for other units: a transmitter node powers a receiver block
    using namespace lv::feature::redstone;
    auto key = link::keyOf(item("minecraft:apple"), item("minecraft:stick"));
    ctx.check(key.first == "minecraft:apple" && key.last == "minecraft:stick", "frequency keys are item ids");
    int  got = -1;
    auto rx  = link::add({.pos = ctx.testPos(2315), .key = key, .listening = true, .onReceive = [&](int p) { got = p; }});
    ctx.check(got == 0, "a new receiver gets the network power at once");
    auto tx = link::add({.pos = ctx.testPos(2317), .key = key, .transmit = 13});
    ctx.check(got == 13, "a transmitter in range powers it");
    ctx.check(link::received(rx) == 13 && link::anyPower(key), "received / anyPower");
    auto far = ctx.testPos(2315);
    far.x += 300;
    link::move(tx, far);
    // Create re-evaluates around the node that changed: a receiver out of its range keeps its
    // last value until something within its own range changes
    ctx.check(link::powerAt(ctx.testPos(2315), key) == 0, "out of range (256 blocks) after moving away");
    link::setTransmit(tx, 15);
    ctx.check(got == 13, "a transmitter out of range doesn't reach it");
    link::move(tx, ctx.testPos(2317));
    ctx.check(got == 15, "back in range");
    link::remove(tx);
    ctx.check(got == 0, "removing the transmitter in range turns it off");
    link::remove(rx);
});

LV_REGISTER_SELFTEST("redstone.link_click", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2320);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    ctx.require(place(ctx, p, kLink, {{sFacing, kUp}}), "link placed");
    co_await ctx.ticks(2);
    hold(ctx, sp, item("minecraft:redstone"));
    // the first slot is at z 10.5 px on an upward link, the second at 5.5 px
    click(sp, p, kUp, Vec3{0.5f, 3.f / 16, 11.f / 16});
    co_await ctx.ticks(6);
    auto [a, b] = frequencies(p);
    ctx.check(common::items::idOf(a) == "minecraft:redstone", "clicking the first slot sets frequency #1, got " + common::items::idOf(a));
    ctx.check(common::items::isEmpty(b), "the second slot stays empty");
    ctx.check(common::items::countOf(sp.getInventory().getItem(0)) == 1, "the item is not used up");
    // empty-hand clicks (a SimulatedPlayer can't use an empty slot on a block: call the click
    // handler the GameMode hook uses)
    ItemStack none;
    Click     clear{sp, p, kUp, {0.5, 3.0 / 16, 11.0 / 16}, none, false};
    ctx.check(use(clear, blockAt(ctx, p), Kind::Link), "an empty-hand click on a slot is used");
    ctx.check(common::items::isEmpty(frequencies(p).first), "an empty hand clears the slot");
    Click toggle{sp, p, kUp, {0.2, 3.0 / 16, 0.2}, none, true};
    ctx.check(use(toggle, blockAt(ctx, p), Kind::Link), "sneak + empty hand is used");
    co_await ctx.ticks(2);
    ctx.check(st(ctx, p, sReceiver) == 1, "sneak-click switches to receiver");
});

LV_REGISTER_SELFTEST("redstone.link_survival", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2325);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, "minecraft:stone");
    place(ctx, at(p, 1, 0, 0), kLink, {{sFacing, kEast}});
    co_await ctx.ticks(3);
    ctx.require(game::blockIdAt(regionOf(ctx), game::blockPos(at(p, 1, 0, 0))) == kLink, "link on the stone");
    place(ctx, p, "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(game::blockIdAt(regionOf(ctx), game::blockPos(at(p, 1, 0, 0))) != kLink, "the link pops off without its support");
});

// ---- analog lever ------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.analog_lever", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2330);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    hold(ctx, sp, item("minecraft:stick")); // Create: any item but the wrench works the same as an empty hand
    ctx.require(place(ctx, p, kLever, {{sFace, 0}, {sFacing, kNorth}}), "lever placed");
    place(ctx, at(p, 1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(2);
    for (int i = 0; i < 3; ++i) {
        click(sp, p, kUp, Vec3{0.5f, 0.2f, 0.5f});
        co_await ctx.ticks(10);
    }
    ctx.check(st(ctx, p, sLeverState) == 3, "three clicks: state 3, got " + std::to_string(st(ctx, p, sLeverState)));
    ctx.check(st(ctx, p, sPower) == 0, "the output waits 15 ticks after the last click");
    co_await ctx.ticks(16);
    ctx.check(st(ctx, p, sPower) == 3, "then it follows, got " + std::to_string(st(ctx, p, sPower)));
    co_await ctx.ticks(4);
    ctx.check(wire(ctx, at(p, 1, 0, 0)) == 3, "dust gets 3");
    // sneaking with an item in hand does nothing (vanilla secondary use), with an empty hand it lowers
    ItemStack stick = item("minecraft:stick"), none;
    ctx.check(!use(Click{sp, p, kUp, {0.5, 0.2, 0.5}, stick, true}, blockAt(ctx, p), Kind::Lever), "sneak + item passes");
    ctx.check(use(Click{sp, p, kUp, {0.5, 0.2, 0.5}, none, true}, blockAt(ctx, p), Kind::Lever), "sneak + empty hand is used");
    ctx.check(st(ctx, p, sLeverState) == 2, "sneak-click lowers it");
    leverUse(regionOf(ctx), p, blockAt(ctx, p), true);
    leverUse(regionOf(ctx), p, blockAt(ctx, p), true);
    leverUse(regionOf(ctx), p, blockAt(ctx, p), true);
    ctx.check(st(ctx, p, sLeverState) == 0, "never below 0");
});

// ---- brass diodes ------------------------------------------------------------------------------

namespace {
/// Builds a diode facing east (back west) with a redstone block switched by `power`, counts the
/// ticks its output is on during `ticks` ticks.
ll::coro::CoroTask<> diodeRun(Context& ctx, GP p, char const* id, int delay, std::vector<bool> input, int ticks, std::vector<bool>& out) {
    place(ctx, p, id, {{sFacing, kWest}});
    co_await ctx.ticks(2);
    setDiodeDelay(p, delay);
    auto src = at(p, -1, 0, 0);
    for (int t = 0; t < ticks; ++t) {
        bool want = input.empty() ? false : input[std::min<std::size_t>(t, input.size() - 1)];
        bool now  = game::blockIdAt(regionOf(ctx), game::blockPos(src)) == "minecraft:redstone_block";
        if (want != now) place(ctx, src, want ? "minecraft:redstone_block" : "minecraft:air");
        co_await ctx.ticks(1);
        out.push_back(st(ctx, p, sPowering) == 1);
    }
}
int countOn(std::vector<bool> const& v) {
    int n = 0;
    for (bool b : v) n += b;
    return n;
}
} // namespace

LV_REGISTER_SELFTEST("redstone.pulse_repeater", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2335);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    std::vector<bool> out;
    co_await diodeRun(ctx, p, kPulseRepeater, 10, {true}, 40, out);
    ctx.check(countOn(out) == 2, "one pulse of two ticks for a held input, got " + std::to_string(countOn(out)));
    int first = -1;
    for (int i = 0; i < int(out.size()); ++i)
        if (out[i] && first < 0) first = i;
    ctx.check(first >= 9 && first <= 13, "the pulse comes after the delay (10 + input delay), at " + std::to_string(first));
    place(ctx, at(p, 1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("redstone.pulse_extender", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2340);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    std::vector<bool> in(2, true);
    in.resize(60, false);
    std::vector<bool> out;
    co_await diodeRun(ctx, p, kPulseExtender, 20, in, 60, out);
    int n = countOn(out);
    ctx.check(n >= 20 && n <= 25, "a short input is stretched to the delay, on for " + std::to_string(n));
    ctx.check(!out.back(), "and ends");
});

LV_REGISTER_SELFTEST("redstone.pulse_timer", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2345);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    std::vector<bool> out;
    co_await diodeRun(ctx, p, kPulseTimer, 10, {false}, 50, out);
    int n = countOn(out);
    ctx.check(n >= 8 && n <= 12, "two-tick pulses every 10 ticks: on for " + std::to_string(n));
    std::vector<bool> held;
    auto q = at(p, 0, 0, 3);
    co_await diodeRun(ctx, q, kPulseTimer, 10, {true}, 30, held);
    ctx.check(countOn(std::vector<bool>(held.begin() + 4, held.end())) == 0, "a powered timer stays off");
    ctx.check(diodeDelay(p, Kind::PulseTimer) == 10, "delay stored");
    (void)dataFor(at(p, 0, 0, 6)); // a fresh timer defaults to 20 ticks
    ctx.check(diodeDelay(at(p, 0, 0, 6), Kind::PulseTimer) == 20, "default timer delay 20");
    common::blockdata::erase(at(p, 0, 0, 6));
});

LV_REGISTER_SELFTEST("redstone.diode_controls", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2350);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    hold(ctx, sp, item("minecraft:stick")); // Create: any item but the wrench works the same as an empty hand
    place(ctx, p, kPulseExtender, {{sFacing, kWest}});
    place(ctx, at(p, 1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(3);
    // a click beside the value box inverts it: an idle inverted extender emits
    click(sp, p, kUp, Vec3{0.05f, 2.f / 16, 0.05f});
    co_await ctx.ticks(6);
    ctx.check(st(ctx, p, sInverted) == 1, "click toggles inverted");
    ctx.check(wire(ctx, at(p, 1, 0, 0)) == 15, "an idle inverted diode emits 15");
    // the value box (Create's board: ticks / seconds / minutes)
    ctx.require(common::valuebox::apply(sp, p, "delay", {1, 5}), "delay set through the value box");
    ctx.check(diodeDelay(p, Kind::PulseExtender) == 100, "5 seconds = 100 ticks");
    ctx.require(common::valuebox::apply(sp, p, "delay", {0, 0}), "the smallest delay");
    ctx.check(diodeDelay(p, Kind::PulseExtender) == 2, "at least 2 ticks");
    ctx.require(common::valuebox::apply(sp, p, "delay", {2, 60}), "the largest delay");
    ctx.check(diodeDelay(p, Kind::PulseExtender) == 72000, "60 minutes");
});

// ---- latches -----------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.powered_latch", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2355);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, kPoweredLatch, {{sFacing, kWest}});
    co_await ctx.ticks(2);
    place(ctx, at(p, -1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, sPowering) == 1, "back input sets");
    place(ctx, at(p, -1, 0, 0), "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, sPowering) == 1, "and it stays set");
    place(ctx, at(p, 0, 0, 1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, sPowering) == 0, "a side input resets");
    place(ctx, at(p, 0, 0, 1), "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(st(ctx, p, sPowering) == 0, "still reset");
});

LV_REGISTER_SELFTEST("redstone.toggle_latch", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2360);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    hold(ctx, sp, item("minecraft:stick")); // Create: any item but the wrench works the same as an empty hand
    place(ctx, p, kToggleLatch, {{sFacing, kWest}});
    co_await ctx.ticks(2);
    for (int i = 0; i < 2; ++i) {
        place(ctx, at(p, -1, 0, 0), "minecraft:redstone_block");
        co_await ctx.ticks(4);
        ctx.check(st(ctx, p, sPowering) == (i == 0 ? 1 : 0), "rising edge " + std::to_string(i + 1) + " toggles");
        place(ctx, at(p, -1, 0, 0), "minecraft:air");
        co_await ctx.ticks(4);
    }
    click(sp, p, kUp, Vec3{0.5f, 2.f / 16, 0.5f});
    co_await ctx.ticks(2);
    ctx.check(st(ctx, p, sPowering) == 1, "a click toggles it too");
});

// ---- detectors ---------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.smart_observer", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2365);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    place(ctx, at(p, 1, 0, 0), "minecraft:chest");
    place(ctx, p, kSmartObserver, {{sFacing, kEast}, {sTarget, 0}}); // looks east at the chest
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 0, "an empty chest: off");
    auto rest = common::items::insert(region, game::blockPos(at(p, 1, 0, 0)), -1, item("minecraft:diamond", 3), false);
    ctx.require(common::items::isEmpty(rest), "diamonds in the chest");
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 1, "items in the chest: on");
    common::filterslot::set(p, item("minecraft:gold_ingot"));
    co_await ctx.ticks(12);
    ctx.check(st(ctx, p, sPowered) == 0, "a gold filter ignores diamonds (off after 6 ticks)");
    common::filterslot::set(p, item("minecraft:diamond"));
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 1, "a diamond filter sees them");
    // the observed block itself
    auto q = at(p, 0, 0, 3);
    place(ctx, at(q, 1, 0, 0), "minecraft:gold_block");
    place(ctx, q, kSmartObserver, {{sFacing, kEast}, {sTarget, 0}});
    common::filterslot::set(q, item("minecraft:gold_block"));
    co_await ctx.ticks(3);
    ctx.check(st(ctx, q, sPowered) == 1, "the observed block matches the filter");
    // funnel notification (U12 calls it)
    auto f = at(p, 0, 0, 6);
    place(ctx, at(f, 1, 0, 0), "minecraft:stone");
    place(ctx, f, kSmartObserver, {{sFacing, kEast}, {sTarget, 0}});
    co_await ctx.ticks(2);
    observer::onFunnelTransfer(region, game::blockPos(at(f, 1, 0, 0)), item("minecraft:apple"));
    co_await ctx.ticks(1);
    ctx.check(st(ctx, f, sPowered) == 1, "a funnel transfer pulses it");
    co_await ctx.ticks(8);
    ctx.check(st(ctx, f, sPowered) == 0, "for 4 ticks");
});

LV_REGISTER_SELFTEST("redstone.threshold_switch", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2370);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    auto  chest  = at(p, 1, 0, 0);
    place(ctx, chest, "minecraft:chest");
    place(ctx, p, kThresholdSwitch, {{sFacing, kEast}, {sTarget, 0}});
    co_await ctx.ticks(3);
    configureThreshold(p, 10, 20, false, false);
    (void)common::items::insert(region, game::blockPos(chest), -1, item("minecraft:cobblestone", 25), false);
    co_await ctx.ticks(16);
    auto const* d = dataOf(p);
    ctx.require(d != nullptr, "switch data");
    ctx.check(d->value("current", -1) == 25, "25 items counted, got " + std::to_string(d->value("current", -1)));
    ctx.check(d->value("max", 0) == 27 * 64, "a chest holds 27 stacks");
    ctx.check(st(ctx, p, sPowered) == 1, "on at or above 20");
    ctx.check(st(ctx, p, sLevel) == 1, "level 1 of 5");
    // take 10 out: 15 is between the thresholds, stays on
    (void)common::items::extract(region, game::blockPos(chest), -1, common::items::any(), 10, false);
    co_await ctx.ticks(16);
    ctx.check(st(ctx, p, sPowered) == 1, "hysteresis keeps it on at 15");
    (void)common::items::extract(region, game::blockPos(chest), -1, common::items::any(), 10, false);
    co_await ctx.ticks(16);
    ctx.check(st(ctx, p, sPowered) == 0, "off at or below 10");
    configureThreshold(p, 10, 20, true, false);
    co_await ctx.ticks(2);
    ctx.check(st(ctx, p, sPowered) == 1, "inverted");
    // the filter limits what is counted
    common::filterslot::set(p, item("minecraft:dirt"));
    co_await ctx.ticks(12);
    ctx.check(dataOf(p)->value("current", -1) == 0, "a dirt filter counts no cobblestone");
});

LV_REGISTER_SELFTEST("redstone.threshold_screen", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2375);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& player = ctx.player();
    place(ctx, at(p, 1, 0, 0), "minecraft:barrel");
    place(ctx, p, kThresholdSwitch, {{sFacing, kEast}, {sTarget, 0}});
    co_await ctx.ticks(3);
    net::setTestCompanion(player, true);
    (void)net::takeTestMessages(player);
    openThresholdScreen(player, p);
    auto msgs  = net::takeTestMessages(player);
    bool found = false;
    for (auto const& m : msgs) {
        if (m.channel != "screen.open") continue;
        found = true;
        ctx.check(m.payload["type"] == "redstone.threshold_switch", "screen type");
        ctx.check(m.payload["data"]["type"] == "item", "the barrel is an item target");
        ctx.check(m.payload["data"]["onAbove"] == 128 && m.payload["data"]["offBelow"] == 64, "Create's defaults 128 / 64");
    }
    ctx.check(found, "a native screen was opened for the companion");
    net::injectFromClient(
        player,
        "screen.action",
        {{"type", "redstone.threshold_switch"}, {"context", {{"pos", {p.dim, p.x, p.y, p.z}}}}, {"action", {{"offBelow", 64}, {"onAbove", 640}, {"inverted", false}, {"inStacks", true}}}}
    );
    ctx.check(dataOf(p) && dataOf(p)->value("onAbove", 0) == 640 && dataOf(p)->value("inStacks", false), "the screen's settings are stored");
    net::setTestCompanion(player, false);
});

// ---- contacts ----------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.contact", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2380);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, kContact, {{sFacing, kEast}});
    co_await ctx.ticks(2);
    ctx.check(st(ctx, p, sPowered) == 0, "alone: off");
    place(ctx, at(p, 1, 0, 0), kContact, {{sFacing, kWest}});
    place(ctx, at(p, -1, 0, 0), "minecraft:redstone_wire");
    co_await ctx.ticks(6);
    ctx.check(st(ctx, p, sPowered) == 1 && st(ctx, at(p, 1, 0, 0), sPowered) == 1, "facing pair: both on");
    ctx.check(wire(ctx, at(p, -1, 0, 0)) == 15, "dust behind it is powered");
    place(ctx, at(p, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 0, "partner gone: off");
    place(ctx, at(p, 1, 0, 0), kContact, {{sFacing, kNorth}});
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 0, "a partner facing elsewhere doesn't count");
});

// ---- placement by a player ---------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(2385);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{p.x - 2.5f, float(p.y), p.z + 0.5f}, DimensionType(p.dim));
    co_await ctx.ticks(2);
    sp.simulateLookAt(Vec3{p.x + 4.5f, float(p.y), p.z + 0.5f}, sim::LookDuration::Instant); // looking east
    co_await ctx.ticks(2);
    hold(ctx, sp, item(kPulseRepeater));
    ctx.cleanupBlock(p);
    click(sp, at(p, 0, -1, 0), kUp, Vec3{0.5f, 1.f, 0.5f});
    co_await ctx.ticks(2);
    ctx.require(game::blockIdAt(regionOf(ctx), game::blockPos(p)) == kPulseRepeater, "a pulse repeater was placed");
    ctx.check(st(ctx, p, sFacing) == kWest, "its back faces the player (west), got " + std::string(dirName(st(ctx, p, sFacing))));
    // a link faces away from the clicked face (on top of a block: up; the default state is down)
    auto s = at(p, 0, 0, 2);
    hold(ctx, sp, item(kLink));
    ctx.cleanupBlock(s);
    co_await ctx.ticks(10); // the simulated player's use cooldown
    click(sp, at(s, 0, -1, 0), kUp, Vec3{0.5f, 1.f, 0.5f});
    co_await ctx.ticks(2);
    ctx.require(game::blockIdAt(regionOf(ctx), game::blockPos(s)) == kLink, "a link was placed on the ground");
    ctx.check(st(ctx, s, sFacing) == kUp, "facing the clicked face, got " + std::string(dirName(st(ctx, s, sFacing))));
});

LV_REGISTER_SELFTEST("redstone.persistence", [](Context& ctx) -> ll::coro::CoroTask<> {
    // tracked blocks and their settings are kept in the world (BlockData + the tracked list)
    auto p = ctx.testPos(2390);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    place(ctx, p, kPulseTimer, {{sFacing, kWest}});
    co_await ctx.ticks(2);
    setDiodeDelay(p, 77);
    ctx.check(runtimeAt(p) != nullptr && runtimeAt(p)->kind == Kind::PulseTimer, "tracked after placement");
    ctx.check(diodeDelay(p, Kind::PulseTimer) == 77, "delay kept in BlockData");
    place(ctx, p, "minecraft:air");
    co_await ctx.ticks(2);
    ctx.check(runtimeAt(p) == nullptr, "untracked after removal");
    ctx.check(dataOf(p) == nullptr, "its data is gone with it");
});

} // namespace lv::feature::redstone
