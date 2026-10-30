// Redstone unit (U23), U49 additions: Create's desk bell and rose quartz lamp.
//
// Desk bell (DeskBellBlock / DeskBellBlockEntity / BellMovementBehaviour): a click rings it (its
// sound for everyone, the bell's wobble in the client mod: `redstone.bell`), powers it for 20 ticks
// (15 to every side, strongly into the block it stands on: BP producer) and rotates with the wrench.
// On a contraption it rings when it starts, stops and turns around (an elevator: when it arrives),
// never on a train.
//
// Rose quartz lamp (RoseQuartzLampBlock): a rising input lights it and turns off every lamp of its
// cluster (lamps connected to it within 16 blocks), a tick later the activated ones stay lit
// (POWERING = ACTIVATE); a falling input only clears POWERED. Lit lamps emit 15 to everything but
// other lamps (BP producer, the shared emitter); a comparator reads 15 minus the distance to the lit
// lamp of the column behind it. The wrench toggles a lamp (Create's getRotatedBlockState). Light 15
// while lit.

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "core/redstone/RoseQuartzLamp.h"
#include "feature/common/BlockData.h"
#include "feature/common/Redstone.h"
#include "feature/contraption/Contraption.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/net/Net.h"
#include "feature/redstone/Internal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::redstone {

namespace {

constexpr char kBellChannel[] = "redstone.bell";
constexpr char kBellSound[]   = "levitate.desk_bell";

int gRings = 0;

std::string posText(GP const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

bool isLampAt(BlockSource& region, BlockPos const& p) { return loadedAt(region, p) && region.getBlock(p).getTypeName() == kRoseQuartzLamp; }

cr::LampState lampState(Block const& b) {
    return cr::LampState{state(b, sPowered) != 0, state(b, sPowering) != 0, state(b, sActivate) != 0};
}

/// The runtime of a block of the unit at `pos` (tracked now when it is not yet).
Runtime* runtimeFor(GP const& pos, Kind kind) {
    if (auto* rt = runtimeAt(pos); rt && rt->kind == kind) return rt;
    track(pos, kind);
    auto* rt = runtimeAt(pos);
    if (rt) rt->loaded = true;
    return rt;
}

/// Level.scheduleTick(pos, block, 1) unless one is pending (hasScheduledTick).
void scheduleLamp(GP const& pos) {
    auto* rt = runtimeFor(pos, Kind::RoseQuartzLamp);
    if (rt && rt->scheduledAt == 0) rt->scheduledAt = gameTickCounter() + 1;
}

void bellSound(int dim, double x, double y, double z) { playSound(dim, x, y, z, kBellSound, 1.f, 1.f); }

// ---- the bell on a contraption (Create's BellMovementBehaviour) ----------------------------------

bool elevatorArrived(contraption::Contraption const& c) {
    auto const* doc = common::blockdata::get(c.anchor());
    if (!doc || !doc->contains("pistons")) return false;
    return (*doc)["pistons"].value("arrived", false);
}

void movingBellSound(contraption::MovementContext const& m) {
    bellSound(m.worldPos.dim, std::floor(m.position.x) + .5, std::floor(m.position.y) + .5, std::floor(m.position.z) + .5);
    log().info("contraption #{}: desk bell rings", m.contraption.id());
}

bool bellActive(contraption::MovementContext const& m) { return m.contraption.kind() != "train"; }

} // namespace

// ---- desk bell -------------------------------------------------------------------------------------

void ringDeskBell(BlockSource& region, GP const& pos, Player* player) {
    auto bp = game::blockPos(pos);
    bellSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5);
    setStates(region, bp, {{sPowered, 1}});
    // DeskBellBlockEntity.ding: unpressed 20 ticks later
    if (auto* rt = runtimeFor(pos, Kind::DeskBell)) rt->scheduledAt = gameTickCounter() + cr::kDeskBellPulse;
    if (auto level = ll::service::getLevel()) {
        Json msg{{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}};
        level->forEachPlayer([&](Player& p) {
            if (static_cast<int>(p.getDimensionId()) == pos.dim && net::isCompanion(p)) net::send(p, kBellChannel, msg);
            return true;
        });
    }
    ++gRings;
    log().info("desk bell at {} rung{}", posText(pos), player ? " by " + player->getRealName() : std::string());
}

int deskBellRings() { return gRings; }

void tickDeskBell(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    if (rt.scheduledAt == 0 || gameTickCounter() < rt.scheduledAt) return;
    rt.scheduledAt = 0;
    if (state(block, sPowered)) setStates(region, game::blockPos(pos), {{sPowered, 0}}); // DeskBellBlock.unPress
}

// ---- rose quartz lamp ------------------------------------------------------------------------------

void tickRoseQuartzLamp(BlockSource& region, GP const& pos, Block const& block, Runtime& rt) {
    auto bp = game::blockPos(pos);
    // the scheduled tick
    if (rt.scheduledAt != 0 && gameTickCounter() >= rt.scheduledAt) {
        rt.scheduledAt = 0;
        auto s         = lampState(block);
        auto next      = cr::lampTick(s);
        if (!(next == s)) setStates(region, bp, {{sPowering, next.powering ? 1 : 0}, {sActivate, next.activate ? 1 : 0}});
    }
    // neighborChanged: the input (other lamps send nothing into a lamp)
    auto const& now   = region.getBlock(bp);
    auto        s     = lampState(now);
    bool        input = lampInput(region, bp) > 0;
    switch (cr::lampNeighbourChanged(s, input)) {
    case cr::LampChange::None: break;
    case cr::LampChange::Unpower: setStates(region, bp, {{sPowered, 0}}); break;
    case cr::LampChange::Activate: {
        auto cluster = cr::lampCluster({pos.x, pos.y, pos.z}, [&](cr::LampPos const& p) { return isLampAt(region, BlockPos{p[0], p[1], p[2]}); });
        for (auto const& p : cluster) {
            setStates(region, BlockPos{p[0], p[1], p[2]}, {{sPowering, 0}});
            scheduleLamp(GP{pos.dim, p[0], p[1], p[2]});
        }
        setStates(region, bp, {{sPowered, 1}, {sPowering, 1}, {sActivate, 1}});
        scheduleLamp(pos);
        log().info("rose quartz lamp at {} activated ({} lamps in its cluster)", posText(pos), cluster.size());
        break;
    }
    }
}

int lampInput(BlockSource& region, BlockPos const& pos) {
    // Level.hasNeighborSignal. A solid block the circuit powers counts with what its other
    // neighbours (but lamps) send into it: Bedrock lets a lit lamp's producer power the blocks
    // around it, which in Java (weak signals) would not light the lamp again.
    int best = 0;
    for (int d = 0; d < 6 && best < 15; ++d) {
        auto n = step(pos, d);
        if (!loadedAt(region, n) || isLampAt(region, n)) continue;
        if (common::redstone::isPoweredBlock(region, n)) {
            for (int e = 0; e < 6; ++e) {
                auto m = step(n, e);
                // solid blocks never power each other (Java: no direct signal)
                if (m == pos || isLampAt(region, m) || common::redstone::isPoweredBlock(region, m)) continue;
                best = std::max(best, common::redstone::signalFrom(region, m, n));
            }
        } else {
            best = std::max(best, common::redstone::signalFrom(region, n, pos));
        }
    }
    return best;
}

int lampSignal(BlockSource& region, BlockPos const& at, Block const& block, int towards) {
    auto to = step(at, towards);
    if (loadedAt(region, to)) {
        auto const& receiver = region.getBlock(to);
        if (receiver.getTypeName() == kRoseQuartzLamp) return 0;
        if (common::redstone::comparatorRear(receiver)) {
            // getDistanceToPowered along the column away from the comparator
            int column = opposite(towards);
            return cr::lampDistanceToPowered([&](int i) {
                auto o = offsetOf(column);
                BlockPos p{at.x + o[0] * i, at.y + o[1] * i, at.z + o[2] * i};
                if (!isLampAt(region, p)) return -1;
                return state(region.getBlock(p), sPowering) ? 1 : 0;
            });
        }
    }
    return state(block, sPowering) ? 15 : 0;
}

std::optional<int> lampComparatorOutput(BlockSource& region, BlockPos const& pos) {
    for (int d = kNorth; d <= kEast; ++d) {
        auto c = step(pos, d);
        if (!loadedAt(region, c)) continue;
        auto rear = common::redstone::comparatorRear(region.getBlock(c));
        if (!rear || *rear != opposite(d)) continue;
        return lampSignal(region, pos, region.getBlock(pos), d);
    }
    return std::nullopt;
}

// ---- registration ------------------------------------------------------------------------------------

void registerBellAndLamp() {
    common::redstone::registerEmitter(kRoseQuartzLamp, [](BlockSource& region, BlockPos const& at, Block const& block, int towards) {
        return lampSignal(region, at, block, towards);
    });
    common::redstone::registerAnalog(kRoseQuartzLamp, [](BlockSource& region, BlockPos const& pos, Block const&) {
        return lampComparatorOutput(region, pos);
    });
    // WrenchableDirectionalBlock: the wrench turns the bell; the lamp's getRotatedBlockState
    // cycles POWERING
    goggles::wrench::registerBlock(kDeskBell);
    goggles::wrench::registerBlock(
        kRoseQuartzLamp,
        goggles::wrench::Handler{
            .rotate = [](Block const& block, int) -> Block const* { return withStates(block, {{sPowering, state(block, sPowering) ? 0 : 1}}); },
        }
    );
    contraption::registerActor(
        kDeskBell,
        contraption::MovementBehaviour{
            .startMoving =
                [](contraption::MovementContext& m) {
                    if (!bellActive(m) || m.contraption.kind() == "elevator") return;
                    movingBellSound(m); // Create: firstMovement
                },
            .tick =
                [](contraption::MovementContext& m) {
                    if (!bellActive(m)) return;
                    if (m.contraption.kind() == "elevator") {
                        // rings once the cabin arrives at its floor
                        if (!elevatorArrived(m.contraption)) m.data["moved"] = true;
                        else if (m.data.value("moved", false)) {
                            movingBellSound(m);
                            m.data.erase("moved");
                        }
                        return;
                    }
                    // onSpeedChanged: turning back (or starting again) rings
                    auto last = m.data.value("motion", Json());
                    Json now  = Json::array({m.motion.x, m.motion.y, m.motion.z});
                    if (last.is_array() && last.size() == 3 && last != now) {
                        double dot = last[0].get<double>() * m.motion.x + last[1].get<double>() * m.motion.y + last[2].get<double>() * m.motion.z;
                        if (dot <= 0 && m.motion.length() > 1e-6) movingBellSound(m);
                    }
                    m.data["motion"] = now;
                },
            .visitNewPosition = nullptr,
            .stopMoving =
                [](contraption::MovementContext& m) {
                    if (bellActive(m)) movingBellSound(m);
                },
            .isActive       = nullptr,
            .tickWhileStill = true,
        }
    );
}

} // namespace lv::feature::redstone
