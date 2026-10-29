// Redstone input for custom blocks (see Redstone.h): the game's circuit graph, read per neighbour.

#include "feature/common/Redstone.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/redstone/circuit/CircuitSceneGraph.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"
#include "mc/world/redstone/circuit/components/BaseCircuitComponent.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"
#include "mc/world/redstone/circuit/components/ComparatorCapacitor.h"

#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::common::redstone {

namespace {

/// FacingID of the step from `a` to the adjacent `b` (-1 if not adjacent).
int facingOf(BlockPos const& a, BlockPos const& b) {
    int dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    if (std::abs(dx) + std::abs(dy) + std::abs(dz) != 1) return -1;
    if (dy == -1) return 0;
    if (dy == 1) return 1;
    if (dz == -1) return 2;
    if (dz == 1) return 3;
    if (dx == -1) return 4;
    return 5;
}

BlockPos step(BlockPos const& p, int facing) {
    static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    return {p.x + dx[facing], p.y + dy[facing], p.z + dz[facing]};
}

BaseCircuitComponent* componentAt(BlockSource& region, BlockPos const& pos) {
    CircuitSystem* system = region.getDimension().mCircuitSystem.get();
    if (!system) return nullptr;
    auto& all = system->mSceneGraph->mAllComponents;
    auto  it  = all.find(pos);
    return it == all.end() ? nullptr : it->second.get();
}

std::string stringState(Block const& block, char const* name) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states") || !nbt["states"].contains(std::string_view(name))) return {};
    auto const& v = nbt["states"][std::string_view(name)];
    return v.hold(Tag::Type::String) ? std::string(v.get<StringTag>()) : std::string{};
}

/// FacingID of a direction name ("down", "up", "north", "south", "west", "east"), -1 if none.
int facingByName(std::string const& n) {
    static constexpr char const* names[] = {"down", "up", "north", "south", "west", "east"};
    for (int i = 0; i < 6; ++i)
        if (n == names[i]) return i;
    return -1;
}

/// The block a torch stands on / hangs on (Bedrock's torch_facing_direction names the side the
/// torch leans towards: the support is on the opposite side; "top" stands on the block below).
BlockPos torchSupport(Block const& torch, BlockPos const& at) {
    auto t = stringState(torch, "torch_facing_direction");
    if (t == "north") return step(at, 3);
    if (t == "south") return step(at, 2);
    if (t == "west") return step(at, 5);
    if (t == "east") return step(at, 4);
    return step(at, 0);
}

} // namespace

int signalFrom(BlockSource& region, BlockPos const& from, BlockPos const& to) {
    int towards = facingOf(from, to);
    if (towards < 0 || !region.hasChunksAt(from, 0, false)) return 0;
    auto const& block = region.getBlock(from);
    if (block.isAir()) return 0;
    auto const& id = block.getTypeName();
    if (id == "minecraft:redstone_block") return 15;
    auto* comp = componentAt(region, from);
    if (!comp) return std::clamp(game::intState(block, "redstone_signal").value_or(0), 0, 15);
    int strength = std::clamp(comp->getStrength(), 0, 15);
    if (strength == 0) return 0;
    switch (comp->getCircuitComponentType()) {
    case CircuitComponentType::RedstoneTorchCapacitor:
        return torchSupport(block, from) == to ? 0 : strength; // a torch never powers its support
    case CircuitComponentType::RepeaterCapacitor:
    case CircuitComponentType::ComparatorCapacitor:
        // they only power the block in front of them (the component's direction is its input side)
        return comp->getDirection() == (towards ^ 1) ? strength : 0;
    case CircuitComponentType::ConsumerComponent:
    case CircuitComponentType::PistonConsumer:
        return 0; // lamps, pistons, ... don't pass power on
    case CircuitComponentType::TransporterComponent: {
        // dust powers the block under it and the sides it points into: a side counts unless the
        // dust connects sideways to it (a line along the other axis, a bend, a cross)
        if (towards == 0) return strength;
        if (towards == 1) return 0;
        for (int side : {2, 3, 4, 5}) {
            if ((side >> 1) == (towards >> 1)) continue; // same axis as the target
            auto* n = componentAt(region, step(from, side));
            if (!n) continue;
            auto t = n->getCircuitComponentType();
            if (t != CircuitComponentType::PoweredBlockComponent && t != CircuitComponentType::ConsumerComponent
                && t != CircuitComponentType::PistonConsumer)
                return 0;
        }
        return strength;
    }
    default:
        if (id == "minecraft:observer") {
            // an observer powers only the block behind its face
            int face = game::intState(block, "facing_direction").value_or(-1);
            if (face < 0) face = facingByName(stringState(block, "minecraft:facing_direction"));
            return face >= 0 && face < 6 && step(from, face ^ 1) == to ? strength : 0;
        }
        return strength; // conducting blocks, wire, levers, buttons, plates, observers, ...
    }
}

int power(BlockSource& region, BlockPos const& pos) {
    int best = 0;
    for (int f = 0; f < 6 && best < 15; ++f) best = std::max(best, signalFrom(region, step(pos, f), pos));
    return best;
}

bool powered(BlockSource& region, BlockPos const& pos) { return power(region, pos) > 0; }

// ---- watches -----------------------------------------------------------------------------------

namespace {
struct Watch {
    GlobalPos pos;
    WatchFn   fn;
    int       last = -1;
};
struct Watches {
    std::map<WatchId, Watch> list;
    WatchId                  next = 0;
    std::uint64_t            ticks = 0;
};
Watches& watches() {
    static Watches w;
    return w;
}

void poll() {
    auto& w = watches();
    if (w.list.empty() || ++w.ticks % kPollTicks != 0) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    std::vector<std::pair<WatchId, int>> changed;
    for (auto& [id, watch] : w.list) {
        auto dim = level->getDimension(watch.pos.dim).lock();
        if (!dim) continue;
        auto& region = dim->getBlockSourceFromMainChunkSource();
        auto  bp     = game::blockPos(watch.pos);
        if (!region.hasChunksAt(bp, 0, false)) continue;
        int p = power(region, bp);
        if (p != watch.last) changed.emplace_back(id, p);
    }
    for (auto const& [id, p] : changed) { // callbacks may watch / unwatch
        auto it = w.list.find(id);
        if (it == w.list.end()) continue;
        it->second.last = p;
        auto fn  = it->second.fn;
        auto pos = it->second.pos;
        try {
            fn(pos, p);
        } catch (std::exception const& e) {
            log().error("redstone watch at {} failed: {}", pos.key(), e.what());
        }
    }
}
} // namespace

WatchId watch(GlobalPos const& pos, WatchFn fn) {
    auto& w = watches();
    auto  id = ++w.next;
    w.list.emplace(id, Watch{pos, std::move(fn), -1});
    return id;
}

void unwatch(WatchId id) { watches().list.erase(id); }

// ---- comparator output -------------------------------------------------------------------------

namespace {
std::unordered_map<std::string, AnalogFn>& analogs() {
    static std::unordered_map<std::string, AnalogFn> m;
    return m;
}

/// Comparators we gave an analog input (circuit system, position): released once they stop
/// reading a registered block.
std::set<std::pair<void const*, std::tuple<int, int, int>>>& fedComparators() {
    static std::set<std::pair<void const*, std::tuple<int, int, int>>> s;
    return s;
}

std::unordered_map<CircuitSystem const*, int>& circuitDims() {
    static std::unordered_map<CircuitSystem const*, int> m;
    return m;
}

BlockSource* regionOfCircuit(CircuitSystem const& system) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto& cache = circuitDims();
    auto  it    = cache.find(&system);
    if (it == cache.end()) {
        for (int d = 0; d < 3; ++d) {
            auto dim = level->getDimension(d).lock();
            if (dim && dim->mCircuitSystem.get() == &system) {
                it = cache.emplace(&system, d).first;
                break;
            }
        }
        if (it == cache.end()) return nullptr;
    }
    auto dim = level->getDimension(it->second).lock();
    return dim ? &dim->getBlockSourceFromMainChunkSource() : nullptr;
}

/// Java's isRedstoneConductor, near enough: a solid, full block.
bool conductor(Block const& b) {
    return !b.isAir() && b._isSolid() && b.canProvideFullSupport(1) && b.canProvideFullSupport(0);
}

int clamp15(int v) { return std::clamp(v, 0, 15); }
} // namespace

void registerAnalog(std::string const& blockId, AnalogFn fn) { analogs()[blockId] = std::move(fn); }
void unregisterAnalog(std::string const& blockId) { analogs().erase(blockId); }

std::optional<int> analogOutput(BlockSource& region, BlockPos const& pos) {
    auto& m = analogs();
    if (m.empty() || pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight() || !region.hasChunksAt(pos, 0, false))
        return std::nullopt;
    auto const& block = region.getBlock(pos);
    auto        it    = m.find(block.getTypeName());
    if (it == m.end() || !it->second) return std::nullopt;
    try {
        if (auto v = it->second(region, pos, block)) return clamp15(*v);
    } catch (std::exception const& e) {
        log().error("redstone: analog output at {} {} {} failed: {}", pos.x, pos.y, pos.z, e.what());
    }
    return std::nullopt;
}

std::optional<int> comparatorRear(Block const& b) {
    auto const& id = b.getTypeName();
    if (id != "minecraft:unpowered_comparator" && id != "minecraft:powered_comparator") return std::nullopt;
    // Bedrock's cardinal direction of a comparator points from its output to its input (rear)
    int f = facingByName(stringState(b, "minecraft:cardinal_direction"));
    if (f < 2) {
        static constexpr int byDirection[] = {3, 4, 2, 5}; // legacy "direction": south, west, north, east
        f = byDirection[std::clamp(game::intState(b, "direction").value_or(0), 0, 3)];
    }
    return f;
}

std::optional<int> comparatorInput(BlockSource& region, BlockPos const& at) {
    if (analogs().empty() || !region.hasChunksAt(at, 0, false)) return std::nullopt;
    auto rear = comparatorRear(region.getBlock(at));
    if (!rear) return std::nullopt;
    BlockPos behind = step(at, *rear);
    if (auto v = analogOutput(region, behind)) return v;
    if (!region.hasChunksAt(behind, 0, false) || !conductor(region.getBlock(behind))) return std::nullopt;
    return analogOutput(region, step(behind, *rear));
}

int fractionToRedstoneLevel(double fraction) {
    if (!(fraction > 0)) return 0;
    return clamp15(static_cast<int>(std::floor(fraction * 14 + 1)));
}

LL_TYPE_INSTANCE_HOOK(
    RedstoneComparatorHook,
    ll::memory::HookPriority::Normal,
    ComparatorCapacitor,
    &ComparatorCapacitor::$evaluate,
    bool,
    CircuitSystem&  system,
    BlockPos const& pos
) {
    if (!analogs().empty()) {
        std::pair<void const*, std::tuple<int, int, int>> key{&system, {pos.x, pos.y, pos.z}};
        std::optional<int>                                 v;
        if (auto* region = regionOfCircuit(system)) v = comparatorInput(*region, pos);
        if (v) {
            mRearAnalogStrength = *v;
            mHasAnalogBeenSet   = true;
            fedComparators().insert(key);
        } else if (fedComparators().erase(key) > 0) {
            mRearAnalogStrength = 0; // no longer reads one of our blocks: forget what we gave it
            mHasAnalogBeenSet   = false;
        }
    }
    return origin(system, pos);
}

class RedstoneModule {
public:
    void install() {
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { poll(); });
        RedstoneComparatorHook::hook();
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldClosed([] {
                watches().list.clear();
                fedComparators().clear();
                circuitDims().clear();
            });
        }
    }
    void uninstall() {
        RedstoneComparatorHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mTick);
    }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace lv::feature::common::redstone

LV_REGISTER_MODULE_ORDERED(lv::feature::common::redstone::RedstoneModule, lv::ModuleOrder::Services);
