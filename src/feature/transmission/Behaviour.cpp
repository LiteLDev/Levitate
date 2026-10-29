// Ticking behaviour of the transmission blocks (U6): redstone input (clutch, gearshift, adjustable
// chain gearshift, sequenced gearshift), comparator output (gauges, sequenced gearshift), chain
// drive shapes following their neighbours and the speed controller's cog bracket.
//
// Custom Bedrock blocks get no redstone neighbour updates, so the blocks that read redstone are
// tracked (saved in the world DB) and read the power of their neighbours every tick from the
// dimension's circuit system. Comparators read our blocks through a hook on their evaluation.

#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"
#include "mc/world/redstone/circuit/components/ComparatorCapacitor.h"

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
    /// Comparators reading one of our blocks: (dim, comparator pos) -> signal.
    std::map<GlobalPos, int> comparators;
};
Tracked& tracked() {
    static Tracked t;
    return t;
}

bool needsTicking(std::string_view id) {
    return readsRedstone(id) || hasComparatorOutput(id) || id == kSpeedController;
}

bool solidFull(BlockSource& region, GlobalPos const& p) {
    auto bp = game::blockPos(p);
    if (!region.hasChunksAt(bp, 0, false)) return false;
    return region.getBlock(bp).canProvideFullSupport(1) && region.getBlock(bp).canProvideFullSupport(0);
}

/// A comparator at `p` whose output points along `f` (its rear faces back towards -f).
bool comparatorFacing(BlockSource& region, GlobalPos const& p, Face f) {
    std::string id = idAt(region, p);
    if (id != "minecraft:unpowered_comparator" && id != "minecraft:powered_comparator") return false;
    // minecraft:cardinal_direction: south, west, north, east (the way the placing player looked,
    // which is where the signal goes)
    static constexpr Face kCardinal[] = {Face::South, Face::West, Face::North, Face::East};
    int dir = std::clamp(stateAt(region, p, "minecraft:cardinal_direction", 0), 0, 3);
    return kCardinal[dir] == f;
}

/// Registers the comparators that read `pos` (Java: directly behind, or behind a solid block).
void exposeComparators(BlockSource& region, GlobalPos const& pos, int signal) {
    auto& comps = tracked().comparators;
    for (Face f : {Face::North, Face::South, Face::West, Face::East}) {
        GlobalPos n = stepPos(pos, f);
        if (comparatorFacing(region, n, f)) {
            comps[n] = signal;
            continue;
        }
        if (!solidFull(region, n)) continue;
        GlobalPos n2 = stepPos(pos, f, 2);
        if (comparatorFacing(region, n2, f)) comps[n2] = signal;
    }
}

int dimOfCircuit(CircuitSystem const& system) {
    auto level = ll::service::getLevel();
    if (!level) return -1;
    for (int d = 0; d < 3; ++d) {
        auto dim = level->getDimension(d).lock();
        if (dim && dim->mCircuitSystem.get() == &system) return d;
    }
    return -1;
}

} // namespace

// Comparators: the rear analog input of a comparator behind one of our blocks.
LL_TYPE_INSTANCE_HOOK(
    TransmissionComparatorHook,
    ll::memory::HookPriority::Normal,
    ComparatorCapacitor,
    &ComparatorCapacitor::$evaluate,
    bool,
    CircuitSystem&  system,
    BlockPos const& pos
) {
    auto& comps = tracked().comparators;
    if (!comps.empty()) {
        int dim = dimOfCircuit(system);
        if (auto it = comps.find(GlobalPos{dim, pos.x, pos.y, pos.z}); it != comps.end()) {
            mRearAnalogStrength = it->second;
            mHasAnalogBeenSet   = true;
        }
    }
    return origin(system, pos);
}

void track(GlobalPos const& pos) {
    if (tracked().positions.insert(pos).second) tracked().dirty = true;
}

void untrack(GlobalPos const& pos) {
    if (tracked().positions.erase(pos)) tracked().dirty = true;
    gaugeForget(pos);
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
    t.comparators.clear();
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
        if (hasComparatorOutput(id)) {
            int s = comparatorSignal(pos);
            if (s >= 0) exposeComparators(*region, pos, s);
        }
    }
    for (auto const& p : gone) untrack(p);
}

void loadTracked() {
    auto& t = tracked();
    t.positions.clear();
    t.comparators.clear();
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
    tracked().comparators.clear();
    tracked().dirty = false;
}

void installComparatorHook() { TransmissionComparatorHook::hook(); }
void uninstallComparatorHook() { TransmissionComparatorHook::unhook(); }

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
