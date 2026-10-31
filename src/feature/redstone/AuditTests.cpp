// Selftests of the U58 redstone alignments (testPos dx 5800..5849): the wrench on the unit's
// blocks, funnel transfers seen by smart observers, contacts joining elevator columns, the rope
// pulley as a threshold switch observable and the clipboard copying link / diode settings.

#include <string>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/display/Internal.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/pistons/ElevatorColumns.h"
#include "feature/redstone/Internal.h"
#include "feature/redstone/Redstone.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::redstone {

namespace {

using lv::selftest::Context;
using Facing = ScriptModuleMinecraft::ScriptFacing;
namespace wrench = goggles::wrench;

GP at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

Block const& blockAt(Context& ctx, GP const& p) { return regionOf(ctx).getBlock(game::blockPos(p)); }

std::string idAt(Context& ctx, GP const& p) { return blockAt(ctx, p).getTypeName(); }

int st(Context& ctx, GP const& p, char const* name) { return state(blockAt(ctx, p), name, -1); }

ItemStack item(std::string const& id, int count = 1) { return common::items::make(id, count).value_or(ItemStack{}); }

int countOf(Player& p, std::string const& id) {
    int   n   = 0;
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& s = inv.getItem(i);
        if (!s.isNull() && s.getTypeName() == id) n += s.mCount;
    }
    return n;
}

/// A survival test player holding the wrench in slot 0.
SimulatedPlayer& wrenchPlayer(Context& ctx, bool sneaking) {
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.setPlayerGameType(GameType::Survival);
    ctx.give(sp, item(wrench::kWrenchId), 0);
    sp.setSelectedSlot(0);
    sp.setSneaking(sneaking);
    return sp;
}

/// Clicks the top of `p` with the item in slot 0 (standing beside it, looking down at it).
bool clickTop(SimulatedPlayer& sp, GP const& p) {
    sp.teleport(Vec3{float(p.x) + 0.5f, float(p.y) + 1.f, float(p.z) + 2.5f}, DimensionType(p.dim));
    sp.simulateLookAt(Vec3{float(p.x) + 0.5f, float(p.y) + 1.f, float(p.z) + 0.5f}, sim::LookDuration::Instant);
    return sp.simulateUseItemInSlotOnBlock(0, game::blockPos(p), Facing::Up, Vec3{0.5f, 1.f, 0.5f});
}

} // namespace

// ---- M1: the wrench --------------------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.wrench", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5800);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    // Create's IWrenchable blocks of the unit (the analog lever is not one)
    for (auto kind : {Kind::Link, Kind::PulseRepeater, Kind::PulseExtender, Kind::PulseTimer, Kind::PoweredLatch, Kind::ToggleLatch,
                      Kind::SmartObserver, Kind::ThresholdSwitch, Kind::Contact, Kind::DeskBell, Kind::RoseQuartzLamp})
        ctx.check(wrench::isWrenchable(idOf(kind)), std::string(idOf(kind)) + " is wrenchable");
    ctx.check(!wrench::isWrenchable(kLever), "the analog lever is not wrenchable (AnalogLeverBlock is no IWrenchable)");

    // AbstractDiodeBlock: a top click turns FACING clockwise, a side click does nothing
    auto const* diode = game::blockWith(kPulseRepeater);
    ctx.require(diode != nullptr, "pulse repeater block");
    auto const* north = withStates(*diode, {{sFacing, kNorth}});
    auto const* h     = wrench::handlerOf(kPulseRepeater);
    ctx.require(h && h->rotate, "the diode has a rotation");
    ctx.check(state(*h->rotate(*north, kUp), sFacing) == kEast, "top click: north -> east");
    ctx.check(state(*h->rotate(*north, kDown), sFacing) == kEast, "bottom click: north -> east too");
    ctx.check(h->rotate(*north, kEast) == north, "side click: unchanged");

    // DirectedDirectionalBlock: side clicks turn the target about the clicked axis
    auto const* observer = game::blockWith(kSmartObserver);
    ctx.require(observer != nullptr, "smart observer block");
    auto const* east = withStates(*observer, {{sTarget, 0}, {sFacing, kEast}});
    auto const* r    = rotateDirected(*east, kNorth); // east about z, negative face: up
    ctx.check(state(*r, sTarget) == 2, "north click on an east-facing observer: it looks up (ceiling)");
    r = rotateDirected(*east, kSouth); // positive face: down
    ctx.check(state(*r, sTarget) == 1, "south click: it looks down (floor)");
    r = rotateDirected(*east, kUp);
    ctx.check(state(*r, sTarget) == 0 && state(*r, sFacing) == kSouth, "top click: facing turns east -> south");
    auto const* floor = withStates(*observer, {{sTarget, 1}, {sFacing, kNorth}});
    r = rotateDirected(*floor, kEast); // down about x, positive: down.getClockWise(X) = south
    ctx.check(state(*r, sTarget) == 0 && state(*r, sFacing) == kSouth, "east click on a floor observer: looks south");

    // a real click: the wrench turns a latch, sneak + wrench picks it up
    ctx.setBlock(at(p, 0, -1, 0), "minecraft:stone");
    ctx.require(ctx.setBlock(p, kPoweredLatch, {{sFacing, kNorth}}), "latch placed");
    co_await ctx.ticks(2);
    auto& sp = wrenchPlayer(ctx, false);
    clickTop(sp, p);
    co_await ctx.ticks(2);
    ctx.check(st(ctx, p, sFacing) == kEast, "the wrench turned the latch, facing " + std::to_string(st(ctx, p, sFacing)));
    co_await ctx.ticks(12); // the simulated player's use cooldown
    int before = countOf(sp, kPoweredLatch);
    wrenchPlayer(ctx, true);
    co_await ctx.ticks(2);
    clickTop(sp, p);
    co_await ctx.ticks(2);
    sp.setSneaking(false);
    ctx.check(idAt(ctx, p) == "minecraft:air", "sneak + wrench picked the latch up");
    ctx.check(countOf(sp, kPoweredLatch) == before + 1, "into the inventory");
});

// ---- M2: funnels notify smart observers -------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.funnel_observer", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5805);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    // a chest with an upward funnel on it (machines hand items to it); the observer beside the
    // funnel looks at it
    auto chest  = p;
    auto funnel = at(p, 0, 1, 0);
    auto obs    = at(p, 1, 1, 0);
    ctx.setBlock(chest, "minecraft:chest");
    ctx.require(ctx.setBlock(funnel, "levitate:andesite_funnel", {{"levitate:facing", kUp}}), "funnel placed");
    ctx.setBlock(obs, kSmartObserver, {{sFacing, kWest}, {sTarget, 0}});
    co_await ctx.ticks(3);
    ctx.check(st(ctx, obs, sPowered) == 0, "nothing moved yet: off");
    auto rest = common::items::insert(region, game::blockPos(funnel), -1, item("minecraft:apple", 1), false);
    ctx.check(common::items::isEmpty(rest), "the funnel took the apple");
    co_await ctx.ticks(1);
    ctx.check(st(ctx, obs, sPowered) == 1, "the funnel transfer pulsed the observer");
});

// ---- M7: contacts in an elevator column ------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.contact_elevator", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5810);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    // an elevator contact facing east two blocks up makes the column (x, z, east)
    ctx.require(ctx.setBlock(at(p, 0, 3, 0), kElevatorContact, {{sFacing, kEast}}), "elevator contact placed");
    co_await ctx.ticks(3);
    ctx.check(pistons::elevatorColumnExists(p.dim, p.x, p.z, kEast), "the column exists");
    ctx.check(!pistons::elevatorColumnExists(p.dim, p.x, p.z, kWest), "not on the other side");
    // placement: a contact facing east there becomes an elevator contact
    auto const* contact = game::blockWith(kContact);
    ctx.require(contact != nullptr, "redstone contact block");
    Placement   look{.clickedFace = kUp, .pitch = 0, .yaw = 90, .sneaking = false}; // looking west: faces east
    auto const* placed = placementState(region, game::blockPos(p), *contact, Kind::Contact, look);
    ctx.check(placed && placed->getTypeName() == kElevatorContact, "placed into the column: an elevator contact");
    Placement   away{.clickedFace = kUp, .pitch = 0, .yaw = 0, .sneaking = false}; // looking south: faces north
    placed = placementState(region, game::blockPos(p), *contact, Kind::Contact, away);
    ctx.check(placed && placed->getTypeName() == kContact, "facing out of the column: a redstone contact");
    // the wrench: a contact turned into the column becomes an elevator contact
    ctx.require(ctx.setBlock(p, kContact, {{sFacing, kNorth}}), "contact facing north");
    co_await ctx.ticks(2);
    auto& sp = wrenchPlayer(ctx, false);
    clickTop(sp, p); // north -> east
    co_await ctx.ticks(2);
    ctx.check(idAt(ctx, p) == kElevatorContact, "wrenched into the column: now " + idAt(ctx, p));
});

// ---- M8: what smart observers see --------------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.observer_belt", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5815);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    // a depot (TransportedItemStackHandlerBehaviour): the item at its centre is seen
    auto depot = at(p, 1, 0, 0);
    ctx.require(ctx.setBlock(depot, "levitate:depot"), "depot placed");
    ctx.setBlock(p, kSmartObserver, {{sFacing, kEast}, {sTarget, 0}});
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 0, "an empty depot: off");
    auto rest = common::items::insert(region, game::blockPos(depot), -1, item("minecraft:diamond"), false);
    ctx.require(common::items::isEmpty(rest), "a diamond on the depot");
    co_await ctx.ticks(3);
    ctx.check(st(ctx, p, sPowered) == 1, "the item on the depot: on");
});

// ---- M9: the rope pulley as an observable --------------------------------------------------------------

LV_REGISTER_SELFTEST("redstone.threshold_pulley", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5820);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    auto  pulley = at(p, 1, 2, 0);
    ctx.require(ctx.setBlock(pulley, "levitate:rope_pulley", {{"levitate:axis", 0}}), "rope pulley placed");
    ctx.setBlock(at(p, 0, 2, 0), kThresholdSwitch, {{sFacing, kEast}, {sTarget, 0}});
    co_await ctx.ticks(3);
    updateThresholdLevel(region, at(p, 0, 2, 0), blockAt(ctx, at(p, 0, 2, 0)));
    auto const* d = dataOf(at(p, 0, 2, 0));
    ctx.require(d != nullptr, "the switch measured");
    ctx.check(d->value("type", std::string()) == "custom", "the pulley is a custom observable, got " + d->value("type", std::string()));
    ctx.check(d->value("max", -1) == pulley.y, "max = the pulley's y, got " + std::to_string(d->value("max", -1)));
    ctx.check(d->value("current", -1) == pulley.y, "a retracted rope ends at the pulley, got " + std::to_string(d->value("current", -1)));
    ctx.check(d->value("min", 0) == region.getMinHeight(), "min = the bottom of the world");
});

// ---- M10: the clipboard copies link frequencies and diode inversion --------------------------------------

LV_REGISTER_SELFTEST("redstone.clipboard", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5825);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = regionOf(ctx);
    auto& player = ctx.player();
    ctx.setBlock(at(p, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(at(p, 2, -1, 0), "minecraft:stone");
    ctx.require(ctx.setBlock(p, kLink, {{sFacing, kUp}}), "link a");
    ctx.require(ctx.setBlock(at(p, 2, 0, 0), kLink, {{sFacing, kUp}}), "link b");
    co_await ctx.ticks(2);
    setFrequency(p, true, item("minecraft:diamond"));
    setFrequency(p, false, item("minecraft:emerald"));
    auto copied = display::clipboard::copySettings(region, p);
    ctx.require(copied.is_object() && copied.contains("Frequencies"), "copied under Frequencies");
    ctx.check(display::clipboard::pasteSettings(player, region, at(p, 2, 0, 0), copied, false), "pasted on the other link");
    auto [a, b] = frequencies(at(p, 2, 0, 0));
    ctx.check(common::items::idOf(a) == "minecraft:diamond" && common::items::idOf(b) == "minecraft:emerald", "both frequencies pasted");
    // diodes: "Block" -> Inverted, onto any brass diode
    auto r = at(p, 0, 0, 2), e = at(p, 2, 0, 2);
    ctx.setBlock(at(r, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(at(e, 0, -1, 0), "minecraft:stone");
    ctx.setBlock(r, kPulseRepeater, {{sFacing, kNorth}, {sInverted, 1}});
    ctx.setBlock(e, kPulseExtender, {{sFacing, kNorth}, {sInverted, 0}});
    co_await ctx.ticks(2);
    auto diode = display::clipboard::copySettings(region, r);
    ctx.require(diode.is_object() && diode.contains("Block"), "a diode copies under Block");
    ctx.check(display::clipboard::pasteSettings(player, region, e, diode, false), "pasted on an extender");
    ctx.check(st(ctx, e, sInverted) == 1, "the extender is inverted now");
});

} // namespace lv::feature::redstone
