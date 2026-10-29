// Ticking behaviour of the transmission blocks (U6): redstone input (clutch, gearshift, adjustable
// chain gearshift, sequenced gearshift), comparator output (gauges, sequenced gearshift), chain
// drive shapes following their neighbours and the speed controller's cog bracket.
//
// Custom Bedrock blocks get no redstone neighbour updates, so the blocks that read redstone are
// tracked (saved in the world DB) and read the power of their neighbours every tick through the
// shared redstone input. Comparators read our blocks through the shared comparator output registry.

#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/Redstone.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::transmission {

namespace {

constexpr char kDbTracked[] = "lv:transmission:tracked";

struct Tracked {
    std::set<GlobalPos> positions;
    bool                dirty = false;
};
Tracked& tracked() {
    static Tracked t;
    return t;
}

bool needsTicking(std::string_view id) {
    return readsRedstone(id) || hasComparatorOutput(id) || id == kSpeedController;
}

} // namespace

void track(GlobalPos const& pos) {
    if (tracked().positions.insert(pos).second) tracked().dirty = true;
}

void untrack(GlobalPos const& pos) {
    if (tracked().positions.erase(pos)) tracked().dirty = true;
    gaugeForget(pos);
    sequencerForget(pos);
}

int comparatorSignal(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return -1;
    auto id = idAt(*region, pos);
    if (isGauge(id)) return ct::gaugeComparatorSignal(dialTarget(pos));
    if (id == kSequencedGearshift) return stateAt(*region, pos, kSeqState, 0); // Create: the STATE value
    return -1;
}

void tickBlocks() {
    auto& t = tracked();
    std::vector<GlobalPos> gone;
    for (auto const& pos : std::vector<GlobalPos>(t.positions.begin(), t.positions.end())) {
        auto* region = regionOf(pos.dim);
        if (!region) continue;
        auto bp = game::blockPos(pos);
        if (!region->hasChunksAt(bp, 0, false)) continue;
        auto id = idAt(*region, pos);
        if (!needsTicking(id)) {
            gone.push_back(pos);
            continue;
        }
        if (id == kClutch || id == kGearshift) {
            bool powered = neighbourSignal(*region, pos) > 0;
            if (powered != (stateAt(*region, pos, kPowered, 0) != 0)) {
                setStates(*region, pos, {{kPowered, powered ? 1 : 0}});
                log().info("transmission: {} at {} {}", id, pos.key(), powered ? "powered" : "unpowered");
            }
        } else if (id == kChainGearshift) {
            int signal = neighbourSignal(*region, pos);
            if (signal != stateAt(*region, pos, kSignal, 0)) {
                setStates(*region, pos, {{kSignal, signal}});
                log().info("transmission: adjustable chain gearshift at {} signal {}", pos.key(), signal);
            }
        } else if (id == kSequencedGearshift) {
            sequencerRedstone(*region, pos, neighbourSignal(*region, pos) > 0);
            sequencerTick(*region, pos);
        } else if (isGauge(id)) {
            gaugeTick(*region, pos);
        } else if (id == kSpeedController) {
            // Create's hasBracket: a dedicated large cogwheel with a horizontal axis on top
            GlobalPos above   = stepPos(pos, Face::Up);
            auto      aboveId = idAt(*region, above);
            bool bracket = (aboveId == kLargeCogwheel) && stateAt(*region, above, kAxis, 1) != 1;
            if (bracket != (stateAt(*region, pos, kBracket, 0) != 0)) setStates(*region, pos, {{kBracket, bracket ? 1 : 0}});
        }
    }
    for (auto const& p : gone) untrack(p);
}

void loadTracked() {
    auto& t = tracked();
    t.positions.clear();
    if (auto j = game::db::load(kDbTracked); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) t.positions.insert(GlobalPos{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
    t.dirty = false;
}

void saveTracked() {
    auto& t = tracked();
    if (!t.dirty || !game::db::available()) return;
    auto j = nlohmann::json::array();
    for (auto const& p : t.positions) j.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbTracked, j);
    t.dirty = false;
}

void clearTracked() {
    tracked().positions.clear();
    tracked().dirty = false;
}

// Comparator output (gauges, sequenced gearshift) through the shared registry.
void installComparatorOutput() {
    for (auto id : {kSpeedometer, kStressometer, kSequencedGearshift}) {
        common::redstone::registerAnalog(std::string(id), [](BlockSource& region, BlockPos const& pos, Block const&) -> std::optional<int> {
            int s = comparatorSignal(game::globalPos(region, pos));
            if (s < 0) return std::nullopt;
            return s;
        });
    }
}

void uninstallComparatorOutput() {
    for (auto id : {kSpeedometer, kStressometer, kSequencedGearshift}) common::redstone::unregisterAnalog(std::string(id));
}

/// Whether a block change at `pos` (from `before` to `after`) concerns the unit.
void onBlockChanged(BlockSource& region, GlobalPos const& pos, std::string const& before, std::string const& after,
                    std::vector<std::function<void()>>& later) {
    if (needsTicking(after)) track(pos);
    else if (needsTicking(before)) untrack(pos);
    int dim = pos.dim;
    // chain drives follow their neighbours (Create's updateShape)
    if (isChainDrive(before) || isChainDrive(after)) {
        if (isChainDrive(after)) {
            later.push_back([dim, pos] {
                if (auto* r = regionOf(dim)) updateChainShape(*r, pos);
            });
        }
        for (Face f : ck::kFaces) {
            GlobalPos n = stepPos(pos, f);
            later.push_back([dim, n] {
                if (auto* r = regionOf(dim)) updateChainShape(*r, n);
            });
        }
    }
    (void)region;
}

} // namespace lv::feature::transmission
