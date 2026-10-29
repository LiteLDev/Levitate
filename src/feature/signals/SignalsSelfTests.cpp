// Selftests of the signals unit (U28, testPos dx 2800..2899; tests use distinct dz rows).
//
// Trains are simulated through the contract (feature/tracks/SignalsExt.h): a "train" is a
// stretch of track reported with setTrainExtent, and checkAhead is what a train asks before
// entering a section.

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/redstone/circuit/CircuitSceneGraph.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"
#include "mc/world/redstone/circuit/components/BaseCircuitComponent.h"

#include "core/tracks/Shape.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/signals/SignalsInternal.h"
#include "feature/tracks/SignalsExt.h"
#include "feature/tracks/Tracks.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
using lv::core::GlobalPos;
namespace sg  = lv::feature::signals;
namespace api = lv::feature::tracks::signals;
namespace tr  = lv::feature::tracks;
namespace ct  = lv::core::tracks;
namespace cs  = lv::core::signals;

GlobalPos at(GlobalPos p, int dx, int dy = 0, int dz = 0) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

std::vector<std::pair<std::string, int>> trackStates(ct::Shape shape) {
    int s = int(shape);
    return {{std::string(tr::kShapeState), s % 16}, {std::string(tr::kShapeHiState), s / 16}, {std::string(tr::kTurnState), 0}};
}

/// An east-west track from `from` over `length` blocks.
void lineX(Context& ctx, GlobalPos from, int length) {
    for (int i = 0; i < length; ++i) ctx.setBlock(at(from, i), std::string(tr::kTrackId), trackStates(ct::Shape::XO));
}

/// The graph location at the centre of the track block `track`, travelling east (dir +1) or west.
std::optional<ct::GraphLocation> loc(GlobalPos track, int dir = 1) {
    return tr::targetAt(track, Vec3{float(dir), 0, 0});
}

/// A signal block at `pos` bound to the track block `track` for trains going east / west.
bool signalAt(Context& ctx, GlobalPos pos, GlobalPos track, int dir) {
    ctx.setBlock(pos, std::string(api::kSignalId));
    sg::Target t;
    t.dim   = track.dim;
    t.track = {track.x, track.y, track.z};
    t.dir   = {double(dir), 0, 0};
    return sg::addBlock(sg::Kind::Signal, pos, t);
}

/// A simulated train from track block `tail` to track block `head` (both on the line, east-bound).
bool train(std::string const& id, GlobalPos tail, GlobalPos head, int dir = 1) {
    auto a = loc(tail, dir), b = loc(head, dir);
    return a && b && api::setTrainExtent(id, *a, *b);
}

/// What a train at the track block `from` heading to the track block `to` asks (Create's
/// navigation): the path between them and the signal check with `lookahead`.
cs::Authority ask(std::string const& id, GlobalPos from, GlobalPos to, int dir, double lookahead) {
    auto a = loc(from, dir), b = loc(to, dir);
    if (!a || !b) return {false, 0, "no track"};
    std::vector<ct::PathStep> path;
    if (a->node1 != b->node1 || a->node2 != b->node2) {
        ct::PathQuery q;
        q.start        = *a;
        q.destinations = {*b};
        if (auto r = tr::railways().findPath(q)) path = r->edges;
    }
    return api::checkAhead(id, *a, path, lookahead);
}

int lampAt(GlobalPos pos) {
    auto* region = sg::regionOf(pos.dim);
    if (!region) return -1;
    return lv::game::intState(region->getBlock(lv::game::blockPos(pos)), api::kLampState).value_or(-1);
}

std::string idAt(GlobalPos pos) {
    auto* region = sg::regionOf(pos.dim);
    return region ? lv::game::blockIdAt(*region, lv::game::blockPos(pos)) : "";
}

int lamp(cs::SignalState s) { return api::lampValue(s); }

/// A comparator whose input side is `towards` (Bedrock's cardinal direction points to the input).
void placeComparator(Context& ctx, GlobalPos pos, std::string const& towards) {
    ctx.cleanupBlock(pos);
    auto nbt = CompoundTag::fromSnbt(
        R"({"name":"minecraft:unpowered_comparator","states":{"minecraft:cardinal_direction":")" + towards
        + R"(","output_lit_bit":0b,"output_subtract_bit":0b}})"
    );
    if (!nbt) return ctx.fail("comparator snbt");
    auto block = Block::tryGetFromRegistry(*nbt);
    if (!block) return ctx.fail("comparator block");
    auto* region = sg::regionOf(pos.dim);
    if (region) lv::game::setBlock(*region, lv::game::blockPos(pos), *block.as_ptr());
}

} // namespace

LV_REGISTER_SELFTEST("signals.blocks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = ctx.testPos(2800, 0);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(lv::game::blockWith(api::kSignalId) != nullptr, "levitate:track_signal is registered");
    ctx.require(lv::game::blockWith(api::kObserverId) != nullptr, "levitate:track_observer is registered");
    for (int type : {0, 1}) {
        for (int l : {0, 1, 2, 3}) {
            ctx.require(
                ctx.setBlock(pos, std::string(api::kSignalId), {{std::string(api::kTypeState), type}, {std::string(api::kLampState), l}}),
                "set a signal"
            );
            co_await ctx.ticks(1);
            ctx.check(lampAt(pos) == l, "lamp state " + std::to_string(l) + " reads back");
        }
    }
    // an unbound signal shows no light
    ctx.setBlock(pos, std::string(api::kSignalId));
    co_await ctx.ticks(2);
    ctx.check(lampAt(pos) == 0, "a fresh signal is unlit");
    ctx.check(api::signalState(pos) == cs::SignalState::Invalid, "a fresh signal is not bound");
});

LV_REGISTER_SELFTEST("signals.place_with_item", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(2800, 10);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    lineX(ctx, base, 20);
    auto item = lv::feature::common::items::make(api::kSignalId, 4);
    ctx.require(item.has_value(), "the signal item exists");
    ctx.give(sp, *item, 0);
    sp.teleport(Vec3{base.x + 2.5f, float(base.y), base.z + 2.5f}, DimensionType(0));
    co_await ctx.ticks(2);
    auto use = [&](GlobalPos p) {
        return sp.simulateUseItemInSlotOnBlock(0, lv::game::blockPos(p), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.0f, 0.5f});
    };
    auto look = [&](GlobalPos p) {
        sp.simulateLookAt(Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, sim::LookDuration::Continuous);
    };
    std::string uuid = sp.getUuid().asString();

    // without a selection nothing is placed
    look(at(base, 8, -1, 2));
    co_await ctx.ticks(2);
    co_await ctx.ticks(8);
    use(at(base, 4, -1, 2));
    co_await ctx.ticks(3);
    ctx.check(idAt(at(base, 4, 0, 2)) != api::kSignalId, "no signal without a selected track");

    // select the track looking east, then place next to it
    look(at(base, 12, 0, 0));
    co_await ctx.ticks(2);
    co_await ctx.ticks(8);
    bool used = use(at(base, 6));
    co_await ctx.ticks(3);
    ctx.info(std::string("track click returned ") + (used ? "true" : "false") + ", block " + idAt(at(base, 6)));
    ctx.check(sg::state().selections.contains(uuid), "clicking the track selects it");
    ctx.cleanupBlock(at(base, 6, 0, 2));
    co_await ctx.ticks(8);
    use(at(base, 6, -1, 2));
    co_await ctx.ticks(4);
    GlobalPos placed = at(base, 6, 0, 2);
    ctx.require(idAt(placed) == api::kSignalId, "the signal is placed");
    ctx.check(!sg::state().selections.contains(uuid), "placing uses up the selection");
    auto it = sg::state().blocks.find(placed);
    ctx.require(it != sg::state().blocks.end() && !it->second.point.empty(), "the signal is bound to the track");
    co_await ctx.ticks(3);
    ctx.check(api::signalState(placed) == cs::SignalState::Green, "a free section shows green");
    ctx.check(lampAt(placed) == lamp(cs::SignalState::Green), "the lamp block state is green");

    // the same spot facing the other way joins the boundary on its other side
    look(at(base, 0, 0, 0));
    co_await ctx.ticks(2);
    co_await ctx.ticks(8);
    use(at(base, 6));
    co_await ctx.ticks(3);
    ctx.check(sg::state().selections.contains(uuid), "the other direction of the same spot can be selected");
    ctx.cleanupBlock(at(base, 6, 0, -2));
    co_await ctx.ticks(8);
    use(at(base, 6, -1, -2));
    co_await ctx.ticks(4);
    auto other = sg::state().blocks.find(at(base, 6, 0, -2));
    ctx.require(other != sg::state().blocks.end(), "the second signal is placed");
    ctx.check(other->second.point == it->second.point && other->second.side != it->second.side, "both signals share one boundary");

    // too far away
    look(at(base, 12, 0, 0));
    co_await ctx.ticks(2);
    co_await ctx.ticks(8);
    use(at(base, 10));
    co_await ctx.ticks(3);
    sp.teleport(Vec3{base.x + 2.5f, float(base.y), base.z + 20.5f}, DimensionType(0));
    co_await ctx.ticks(2);
    co_await ctx.ticks(8);
    use(at(base, 2, -1, 19));
    co_await ctx.ticks(3);
    ctx.check(idAt(at(base, 2, 0, 19)) != api::kSignalId, "no signal 16 blocks or more from the track");

    // sneak-click clears the selection
    sp.setSneaking(true);
    co_await ctx.ticks(8);
    use(at(base, 2, -1, 19));
    co_await ctx.ticks(2);
    sp.setSneaking(false);
    ctx.check(!sg::state().selections.contains(uuid), "sneak-clicking clears the selection");
});

LV_REGISTER_SELFTEST("signals.sections", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(2800, 30);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    lineX(ctx, base, 40);
    co_await ctx.ticks(2);
    GlobalPos s1 = at(base, 10, 0, 1), s2 = at(base, 25, 0, 1), s2back = at(base, 25, 0, -1);
    ctx.require(signalAt(ctx, s1, at(base, 10), 1), "signal 1 bound");
    ctx.require(signalAt(ctx, s2, at(base, 25), 1), "signal 2 bound");
    ctx.require(signalAt(ctx, s2back, at(base, 25), -1), "signal 2 (west-bound) bound");
    co_await ctx.ticks(2);
    auto middle = api::sectionAt(*loc(at(base, 17)));
    ctx.check(middle != api::kNoGroup, "the middle of the line is a signal section");
    ctx.check(api::sectionAt(*loc(at(base, 3))) != middle, "the signals split the line");
    ctx.check(api::signalState(s1) == cs::SignalState::Green, "signal 1 green");

    // a comparator east of signal 1 reading it
    GlobalPos comp = at(base, 11, 0, 1);
    placeComparator(ctx, comp, "west");
    co_await ctx.ticks(4);
    ctx.check(idAt(comp) == "minecraft:unpowered_comparator", "a green signal gives no comparator signal");

    // a train in the middle section
    ctx.require(train("selftest-a", at(base, 14), at(base, 20)), "train reported");
    co_await ctx.ticks(3);
    ctx.check(api::signalState(s1) == cs::SignalState::Red, "signal 1 red behind the train");
    ctx.check(lampAt(s1) == lamp(cs::SignalState::Red), "the lamp shows red");
    ctx.check(api::signalState(s2back) == cs::SignalState::Red, "signal 2 into the occupied section red");
    ctx.check(api::signalState(s2) == cs::SignalState::Green, "signal 2 ahead green");
    co_await ctx.ticks(6);
    ctx.info("comparator now " + idAt(comp));
    ctx.check(idAt(comp) == "minecraft:powered_comparator", "a red signal powers a comparator");

    // the train leaves
    api::removeTrain("selftest-a");
    co_await ctx.ticks(3);
    ctx.check(api::signalState(s1) == cs::SignalState::Green, "signal 1 green again");

    // redstone forces red
    GlobalPos rs = at(base, 25, 0, 2);
    ctx.setBlock(rs, "minecraft:redstone_block");
    co_await ctx.ticks(10);
    ctx.check(api::signalState(s2) == cs::SignalState::Red, "a powered signal is red");
    auto stop = ask("selftest-b", at(base, 20), at(base, 38), 1, 20);
    ctx.check(!stop.clear && std::abs(stop.stopDistance - 5) < 0.01, "a train stops at the powered signal");
    ctx.setBlock(rs, "minecraft:air");
    co_await ctx.ticks(10);
    ctx.check(api::signalState(s2) == cs::SignalState::Green, "the signal is released");
    api::removeTrain("selftest-b");
});

LV_REGISTER_SELFTEST("signals.reservation", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(2800, 40);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    lineX(ctx, base, 40);
    co_await ctx.ticks(2);
    GlobalPos east = at(base, 10, 0, 1), west = at(base, 25, 0, -1);
    ctx.require(signalAt(ctx, east, at(base, 10), 1), "east-bound signal bound");
    ctx.require(signalAt(ctx, west, at(base, 25), -1), "west-bound signal bound");
    co_await ctx.ticks(2);
    // train A east-bound at x 2..5, train B west-bound at x 30..34
    ctx.require(train("selftest-a", at(base, 2), at(base, 5)), "train A");
    ctx.require(train("selftest-b", at(base, 34), at(base, 30), -1), "train B");
    auto a = ask("selftest-a", at(base, 5), at(base, 38), 1, 16);
    ctx.check(a.clear && a.reserved.size() == 1, "train A may enter and reserves the section");
    auto b = ask("selftest-b", at(base, 30), at(base, 1), -1, 16);
    ctx.check(!b.clear && std::abs(b.stopDistance - 5) < 0.01, "train B waits at its signal");
    co_await ctx.ticks(2);
    ctx.check(api::signalState(east) == cs::SignalState::Green, "the signal train A was let through stays green");
    ctx.check(api::signalState(west) == cs::SignalState::Red, "the other signal shows red");
    // A enters: occupied; leaves: B may go
    ctx.require(train("selftest-a", at(base, 9), at(base, 14)), "train A enters");
    co_await ctx.ticks(2);
    ctx.check(api::signalState(east) == cs::SignalState::Red, "red once the train is in");
    api::removeTrain("selftest-a");
    auto b2 = ask("selftest-b", at(base, 30), at(base, 1), -1, 16);
    ctx.check(b2.clear, "train B may go once the section is free");
    api::removeTrain("selftest-b");
});

LV_REGISTER_SELFTEST("signals.cross_signal", [](Context& ctx) -> ll::coro::CoroTask<> {
    // a crossing at x+12: the east-west line and a north-south line through it
    auto base = ctx.testPos(2800, 60);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    lineX(ctx, base, 25);
    for (int dz = -10; dz <= 10; ++dz)
        if (dz != 0) ctx.setBlock(at(base, 12, 0, dz), std::string(tr::kTrackId), trackStates(ct::Shape::ZO));
    ctx.setBlock(at(base, 12), std::string(tr::kTrackId), trackStates(ct::Shape::CR_O));
    co_await ctx.ticks(3);
    GlobalPos entry = at(base, 8, 0, 1), exitSig = at(base, 17, 0, 1);
    ctx.require(signalAt(ctx, entry, at(base, 8), 1), "signal before the crossing bound");
    ctx.require(signalAt(ctx, exitSig, at(base, 17), 1), "signal after the crossing bound");
    // the north-south line gets a boundary on each side of the crossing too
    GlobalPos ns1 = at(base, 13, 0, -5), ns2 = at(base, 13, 0, 5);
    ctx.setBlock(ns1, std::string(api::kSignalId));
    ctx.setBlock(ns2, std::string(api::kSignalId));
    sg::Target t1{0, {base.x + 12, base.y, base.z - 5}, {0, 0, 1}};
    sg::Target t2{0, {base.x + 12, base.y, base.z + 5}, {0, 0, -1}};
    t1.dim = t2.dim = base.dim;
    ctx.require(sg::addBlock(sg::Kind::Signal, ns1, t1) && sg::addBlock(sg::Kind::Signal, ns2, t2), "north-south signals bound");
    // the wrench switches the signal before the crossing to a cross signal
    {
        auto const* handler = lv::feature::goggles::wrench::handlerOf(api::kSignalId);
        ctx.require(handler && handler->onWrenched, "the signal is wrenchable");
        auto* rg = sg::regionOf(entry.dim);
        lv::feature::goggles::wrench::Context wc{ctx.player(), *rg, entry, rg->getBlock(lv::game::blockPos(entry)), 2};
        handler->onWrenched(wc);
    }
    ctx.require(sg::state().blocks.at(entry).type == 1, "the wrench switches it to a cross signal");
    co_await ctx.ticks(3);
    ctx.check(api::signalState(entry) == cs::SignalState::Green, "cross signal green");
    auto* region = sg::regionOf(entry.dim);
    ctx.check(lv::game::intState(region->getBlock(lv::game::blockPos(entry)), api::kTypeState).value_or(0) == 1, "the block shows the cross type");

    // a train standing after the exit signal: the cross signal turns yellow
    ctx.require(train("selftest-parked", at(base, 19), at(base, 23)), "parked train");
    co_await ctx.ticks(3);
    ctx.check(api::signalState(exitSig) == cs::SignalState::Red, "exit signal red");
    ctx.check(api::signalState(entry) == cs::SignalState::Yellow, "cross signal yellow");
    ctx.check(lampAt(entry) == lamp(cs::SignalState::Yellow), "the lamp shows yellow");
    auto wait = ask("selftest-t", at(base, 3), at(base, 23), 1, 16);
    ctx.check(!wait.clear && std::abs(wait.stopDistance - 5) < 0.01, "a train waits at the cross signal, not in the crossing");
    api::removeTrain("selftest-parked");
    auto go = ask("selftest-t", at(base, 3), at(base, 23), 1, 16);
    ctx.check(go.clear && go.reserved.size() == 2, "with the exit free it reserves the crossing and the section after it");
    co_await ctx.ticks(2);
    ctx.check(api::signalState(ns1) == cs::SignalState::Red, "the crossing line's signal shows the reservation");
    api::removeTrain("selftest-t");
    co_await ctx.ticks(2);
    ctx.check(api::signalState(ns1) == cs::SignalState::Green, "released again");
});

LV_REGISTER_SELFTEST("signals.observer", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(2800, 80);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    lineX(ctx, base, 20);
    co_await ctx.ticks(2);
    GlobalPos obs  = at(base, 8, 0, 2);
    GlobalPos wire = at(base, 9, 0, 2);
    ctx.setBlock(at(base, 9, -1, 2), "minecraft:stone");
    ctx.setBlock(wire, "minecraft:redstone_wire");
    ctx.setBlock(obs, std::string(api::kObserverId));
    sg::Target t{base.dim, {base.x + 8, base.y, base.z}, {1, 0, 0}};
    ctx.require(sg::addBlock(sg::Kind::Observer, obs, t), "observer bound");
    co_await ctx.ticks(3);
    ctx.check(!api::observerPowered(obs), "no train: off");
    ctx.require(train("selftest-o", at(base, 5), at(base, 11)), "train over the observer");
    co_await ctx.ticks(4);
    ctx.check(api::observerPowered(obs), "a train passing powers it");
    ctx.check(api::observedTrain(obs) == "selftest-o", "it knows the train (display link source)");
    auto* region = sg::regionOf(obs.dim);
    ctx.check(lv::game::intState(region->getBlock(lv::game::blockPos(obs)), api::kPoweredState).value_or(0) == 1, "powered block state");
    co_await ctx.ticks(2);
    auto strength = [&]() -> int {
        auto* circuit = region->getDimension().mCircuitSystem.get();
        if (!circuit) return -1;
        auto const& all = circuit->mSceneGraph->mAllComponents;
        auto        it  = all.find(lv::game::blockPos(obs));
        return it == all.end() || !it->second ? -1 : it->second->getStrength();
    };
    ctx.check(strength() == 15, "it is a redstone source of strength 15 (" + std::to_string(strength()) + ")");
    // (the wire's block state follows on the client's integrated server; BDS doesn't update wire
    // states in the selftest area without a real player, so it is only reported)
    ctx.info("wire next to it: " + std::to_string(lv::game::intState(region->getBlock(lv::game::blockPos(wire)), "redstone_signal").value_or(-1)));
    // the train moves on: stays on for 8 ticks (Create's keepAlive), then off
    ctx.require(train("selftest-o", at(base, 12), at(base, 18)), "train moved on");
    co_await ctx.ticks(3);
    ctx.check(api::observerPowered(obs), "still on right after the train");
    co_await ctx.ticks(8);
    ctx.check(!api::observerPowered(obs), "off after 8 ticks");
    co_await ctx.ticks(1);
    ctx.check(strength() <= 0, "no redstone power when off");

    // a filter: only trains carrying the item trigger it
    auto dirt = lv::feature::common::items::make("minecraft:dirt");
    ctx.require(dirt.has_value(), "dirt item");
    lv::feature::common::filterslot::set(obs, *dirt, "filter");
    // the test train's cargo: nothing, then dirt (other trains keep the real provider)
    auto previous = sg::state().cargo;
    auto carries  = std::make_shared<bool>(false);
    api::setCargoProvider([previous, carries](std::string const& id, api::ItemTest const& item, api::FluidTest const& fluid) {
        if (id != "selftest-o") return previous ? previous(id, item, fluid) : false;
        auto d = lv::feature::common::items::make("minecraft:dirt");
        return *carries && d && item(*d);
    });
    ctx.require(train("selftest-o", at(base, 5), at(base, 11)), "train back over the observer");
    co_await ctx.ticks(3);
    ctx.check(!api::observerPowered(obs), "a filter ignores trains without the cargo");
    *carries = true;
    co_await ctx.ticks(3);
    ctx.check(api::observerPowered(obs), "a train carrying the filtered item triggers it");
    api::setCargoProvider(previous);
    api::removeTrain("selftest-o");
});

LV_REGISTER_SELFTEST("signals.track_removed", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = ctx.testPos(2800, 90);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    lineX(ctx, base, 20);
    co_await ctx.ticks(2);
    GlobalPos sig = at(base, 6, 0, 1);
    ctx.require(signalAt(ctx, sig, at(base, 6), 1), "signal bound");
    co_await ctx.ticks(2);
    ctx.check(api::signalState(sig) == cs::SignalState::Green, "green");
    // remove the targeted track: unbound (no light)
    ctx.setBlock(at(base, 6), "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(api::signalState(sig) == cs::SignalState::Invalid, "unbound without its track");
    ctx.check(lampAt(sig) == 0, "the lamp goes dark");
    // the track comes back: bound again
    ctx.setBlock(at(base, 6), std::string(tr::kTrackId), trackStates(ct::Shape::XO));
    co_await ctx.ticks(4);
    ctx.check(api::signalState(sig) == cs::SignalState::Green, "bound again with the track back");
    // breaking the signal removes its boundary
    ctx.setBlock(sig, "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(api::sectionAt(*loc(at(base, 10))) == api::kNoGroup, "no sections without signals");
});
