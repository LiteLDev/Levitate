// Kinetic network on the server: keeps the solver (src/core/kinetics) in step with the world,
// applies its effects (breaking blocks, listeners, sync), persists it in the world DB and loads
// the stress table from `config/kinetics.json`.

#include "feature/kinetics/Kinetics.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/dimension/Dimension.h"

#include "core/kinetics/SyncCodec.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/kinetics/KineticsInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("kinetics", lv::feature::kinetics::KineticsConfig);

namespace lv::feature::kinetics {

namespace k = core::kinetics;

namespace {

constexpr char const* kChunkPrefix = "lv:kinetics:c:";
/// Ticks between two world checks of the same kinetic block (block still there?).
constexpr int kWorldCheckTicks = 100;

struct ChunkKey {
    int dim, cx, cz;
    friend auto operator<=>(ChunkKey const&, ChunkKey const&) = default;
};
ChunkKey chunkOf(GlobalPos const& p) { return {p.dim, k::sync::chunkOf(p.x), k::sync::chunkOf(p.z)}; }
std::string dbKey(ChunkKey const& c) {
    return std::string(kChunkPrefix) + std::to_string(c.dim) + ":" + std::to_string(c.cx) + ":" + std::to_string(c.cz);
}

/// The stress table value of `key` (config `kinetics.json`), else `fallback`.
double tableValue(std::map<std::string, double> const& table, std::string const& key, double fallback) {
    auto it = table.find(key);
    return it == table.end() ? fallback : it->second;
}

k::SolverConfig solverConfig() {
    auto const&     cfg = config::get<KineticsConfig>();
    k::SolverConfig sc;
    sc.maxRotationSpeed    = cfg.maxRotationSpeed;
    sc.disableStress       = cfg.disableStress;
    sc.validationFrequency = std::max(1, cfg.kineticValidationFrequency);
    sc.impact              = [](KineticSpec const& spec, BlockState const&) {
        return tableValue(config::get<KineticsConfig>().impact, spec.stressKey, spec.impact);
    };
    sc.capacity = [](KineticSpec const& spec, BlockState const&) {
        return tableValue(config::get<KineticsConfig>().capacity, spec.stressKey, spec.capacity);
    };
    return sc;
}

struct Pending {
    GlobalPos    pos;
    Block const* block; ///< the new block (registry objects live for the process)
};

struct Listener {
    ListenerId                               id;
    std::string                              blockId;
    std::function<void(SpeedChange const&)>  speed;
    std::function<void(BreakEvent const&)>   brk;
};

std::string posText(GlobalPos const& p) {
    return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z);
}

} // namespace

class KineticsModule {
public:
    static KineticsModule& get() {
        static KineticsModule instance;
        return instance;
    }

    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mSolver.setConfig(solverConfig());
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([this](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { onTick(); }));
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldReady([this] { loadWorld(); });
            game::db::onWorldSave([this] { saveDirty(); });
            game::db::onWorldClosed([this] { closeWorld(); });
            config::onReload([this] { mSolver.setConfig(solverConfig()); });
        }
        mInstalled = true;
        if (game::db::available()) loadWorld(); // BDS: the world exists before mods are enabled
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        if (mLoaded) saveDirty();
        mInstalled = false;
    }

    k::Solver& solver() {
        flush();
        return mSolver;
    }

    // ---- world changes ------------------------------------------------------------------------------------
    void onBlockChanged(ll::event::BlockChangedEvent& ev) {
        if (!mLoaded || ev.layer() != 0) return;
        auto& region = ev.blockSource();
        if (region.getLevel().isClientSide()) return;
        auto const& now  = ev.newBlock();
        auto const& prev = ev.previousBlock();
        if (!detail::specOfBlock(now) && !detail::specOfBlock(prev)) return;
        mQueue.push_back(Pending{game::globalPos(region, ev.pos()), &now});
    }

    void flush() {
        if (mFlushing || mQueue.empty()) return;
        mFlushing  = true;
        auto queue = std::move(mQueue);
        mQueue.clear();
        // one change per position (the game may report a change more than once): the latest wins
        std::map<GlobalPos, Block const*> latest;
        std::vector<GlobalPos>            order;
        for (auto const& p : queue) {
            auto [it, fresh] = latest.insert_or_assign(p.pos, p.block);
            if (fresh) order.push_back(p.pos);
        }
        for (auto const& pos : order) {
            auto const* block = latest[pos];
            // a block the network broke is still in the world until the next tick: don't re-add it
            if (auto b = mBreakPending.find(pos); b != mBreakPending.end()) {
                if (block && block->getTypeName() == b->second) continue;
                mBreakPending.erase(b);
            }
            apply(pos, block);
        }
        mFlushing = false;
        collect();
    }

    void apply(GlobalPos const& pos, Block const* block) {
        auto const* spec = block ? detail::specOfBlock(*block) : nullptr;
        if (!spec) {
            mSolver.remove(pos);
        } else {
            auto snap = detail::snapshot(*block, *spec);
            if (mSolver.contains(pos)) mSolver.update(pos, std::move(snap));
            else mSolver.add(pos, std::move(snap));
        }
        mDirtyChunks.insert(chunkOf(pos));
    }

    void refresh(GlobalPos const& pos) {
        auto* region = detail::regionOf(pos.dim);
        if (!region || !region->hasChunksAt(game::blockPos(pos), 0, false)) return;
        apply(pos, &region->getBlock(game::blockPos(pos)));
        collect();
    }

    /// Takes the solver's effects after an operation: breaks, changes, network events.
    void collect() {
        for (auto& d : mSolver.takeDestroyed()) {
            log().info("kinetics: {} at {} broke ({})", d.state.id, posText(d.pos), k::reasonName(d.reason));
            mDirtyChunks.insert(chunkOf(d.pos));
            mBreaks.push_back(d);
            mBreakPending[d.pos] = d.state.id;
            BreakEvent ev{d.pos, d.state.id, k::reasonName(d.reason)};
            auto       listeners = mListenerList; // callbacks may add or remove listeners
            for (auto const& l : listeners)
                if (l.brk) l.brk(ev);
        }
        for (auto const& e : mSolver.takeNetworkEvents()) {
            if (e.overstressed) {
                log().info("kinetics: network {} overstressed ({} / {} su)", posText(e.id), std::lround(e.stress), std::lround(e.capacity));
            } else {
                log().info("kinetics: network {} no longer overstressed", posText(e.id));
            }
            overstressEffect(e.id, e.overstressed);
        }
        auto changes = mSolver.takeChanges();
        if (changes.empty()) return;
        for (auto const& c : changes) mDirtyChunks.insert(chunkOf(c.pos));
        detail::syncChanges(changes);
        mPendingChanges.insert(mPendingChanges.end(), changes.begin(), changes.end());
    }

    /// Create's KineticEffectHandler: smoke from every block of a network that becomes
    /// overstressed, a puff of cloud when it recovers.
    void overstressEffect(GlobalPos const& id, bool overstressed) {
        auto level = ll::service::getLevel();
        if (!level) return;
        auto members = mSolver.members(id);
        if (members.size() > 256) members.resize(256);
        for (auto const& p : members) {
            auto dim = level->getDimension(p.dim).lock();
            if (!dim) continue;
            int count = overstressed ? 5 : 2;
            for (int i = 0; i < count; ++i) {
                float dx = .2f + .6f * static_cast<float>(std::abs(i * 37 + p.x * 13) % 10) / 10.f;
                float dz = .2f + .6f * static_cast<float>(std::abs(i * 53 + p.z * 7) % 10) / 10.f;
                Vec3  at{static_cast<float>(p.x) + dx, static_cast<float>(p.y) + .5f + .1f * i, static_cast<float>(p.z) + dz};
                level->spawnParticleEffect(overstressed ? "minecraft:basic_smoke_particle" : "minecraft:white_smoke_particle", at, dim.get());
            }
        }
    }

    // ---- tick -------------------------------------------------------------------------------------------------
    void onTick() {
        if (!mLoaded) return;
        flush();
        mSolver.tick();
        collect();
        breakBlocks();
        checkWorld();
        dispatch();
        detail::syncTick();
    }

    void breakBlocks() {
        if (mBreaks.empty()) return;
        auto level = ll::service::getLevel();
        if (!level) return;
        std::vector<k::Destroyed> later;
        for (auto const& d : mBreaks) {
            auto* region = detail::regionOf(d.pos.dim);
            auto  bp     = game::blockPos(d.pos);
            if (!region || !region->hasChunksAt(bp, 0, false)) {
                if (later.size() < 4096) later.push_back(d);
                continue;
            }
            mBreakPending.erase(d.pos);
            if (region->getBlock(bp).getTypeName() != d.state.id) continue;
            BlockChangeContext ctx{};
            level->destroyBlock(*region, bp, true, ctx);
        }
        mBreaks = std::move(later);
    }

    /// Spreads a world check of every kinetic block over `kWorldCheckTicks`: a block that is gone
    /// (or became another block) without a change event is re-read.
    void checkWorld() {
        if (mCheckCursor >= mCheckOrder.size()) {
            mCheckOrder.clear();
            mSolver.forEachNode([this](NodeInfo const& n) { mCheckOrder.push_back(n.pos); });
            mCheckCursor = 0;
        }
        std::size_t count = (mCheckOrder.size() + kWorldCheckTicks - 1) / kWorldCheckTicks;
        bool        any   = false;
        for (std::size_t i = 0; i < count && mCheckCursor < mCheckOrder.size(); ++i) {
            auto const& pos = mCheckOrder[mCheckCursor++];
            auto        n   = mSolver.node(pos);
            if (!n) continue;
            auto* region = detail::regionOf(pos.dim);
            auto  bp     = game::blockPos(pos);
            if (!region || !region->hasChunksAt(bp, 0, false)) continue;
            auto const& block = region->getBlock(bp);
            auto const* spec  = detail::specOfBlock(block);
            if (!spec || detail::snapshot(block, *spec) != n->state) {
                apply(pos, &block);
                any = true;
            }
        }
        if (any) collect();
    }

    void dispatch() {
        if (mPendingChanges.empty()) return;
        auto changes = std::move(mPendingChanges);
        mPendingChanges.clear();
        if (mListenerList.empty()) return;
        auto listeners = mListenerList; // callbacks may add or remove listeners
        for (auto const& c : changes) {
            SpeedChange sc;
            sc.pos             = c.pos;
            sc.prevSpeed       = c.prevSpeed;
            sc.speed           = c.speed;
            sc.prevTheoretical = c.prevTheoretical;
            sc.theoretical     = c.theoretical;
            sc.source          = c.source;
            sc.added           = c.added;
            sc.removed         = c.removed;
            sc.blockId         = c.id;
            for (auto const& l : listeners) {
                if (!l.speed) continue;
                if (!l.blockId.empty() && l.blockId != sc.blockId) continue;
                l.speed(sc);
            }
        }
    }

    // ---- persistence ---------------------------------------------------------------------------------------------
    void loadWorld() {
        if (!mInstalled) return;
        detail::clearBlockCaches(); // block types are registered again for every world
        mSolver.clear();
        mSolver.setConfig(solverConfig());
        mQueue.clear();
        mBreaks.clear();
        mBreakPending.clear();
        mDirtyChunks.clear();
        mPendingChanges.clear();
        mCheckOrder.clear();
        mCheckCursor = 0;
        std::vector<std::string> keys;
        game::db::forEachKey(kChunkPrefix, [&](std::string_view suffix) { keys.emplace_back(suffix); });
        std::size_t count = 0;
        for (auto const& suffix : keys) {
            auto data = game::db::load(kChunkPrefix + suffix);
            if (!data || !data->is_array()) continue;
            for (auto const& j : *data) {
                try {
                    mSolver.restore(j.get<k::NodeRecord>());
                    ++count;
                } catch (std::exception const& e) {
                    log().warn("kinetics: skipped an unreadable block record in {}: {}", suffix, e.what());
                }
            }
        }
        mSolver.finishRestore();
        mLoaded = true;
        collect();
        mPendingChanges.clear();
        detail::syncReset();
        log().info("kinetics: loaded {} kinetic blocks", count);
    }

    void saveDirty() {
        if (!mLoaded || mDirtyChunks.empty()) return;
        std::map<ChunkKey, nlohmann::json> out;
        for (auto const& c : mDirtyChunks) out[c] = nlohmann::json::array();
        mSolver.forEachNode([&](NodeInfo const& n) {
            auto it = out.find(chunkOf(n.pos));
            if (it != out.end()) it->second.push_back(mSolver.record(n.pos));
        });
        for (auto const& [c, arr] : out) {
            if (arr.empty()) game::db::erase(dbKey(c));
            else game::db::save(dbKey(c), arr);
        }
        mDirtyChunks.clear();
    }

    void closeWorld() {
        mLoaded = false;
        detail::clearBlockCaches();
        mSolver.clear();
        mQueue.clear();
        mBreaks.clear();
        mBreakPending.clear();
        mDirtyChunks.clear();
        mPendingChanges.clear();
        mCheckOrder.clear();
        mCheckCursor = 0;
        detail::syncReset();
    }

    void markDirty(GlobalPos const& pos) { mDirtyChunks.insert(chunkOf(pos)); }

    // ---- listeners -----------------------------------------------------------------------------------------------
    ListenerId addListener(Listener l) {
        l.id = ++mNextListener;
        mListenerList.push_back(std::move(l));
        return mNextListener;
    }
    void removeListener(ListenerId id) {
        std::erase_if(mListenerList, [id](Listener const& l) { return l.id == id; });
    }

    [[nodiscard]] bool loaded() const { return mLoaded; }

private:
    KineticsModule() : mSolver([](std::string_view id) { return specOf(id); }) {}

    k::Solver                           mSolver;
    std::vector<ll::event::ListenerPtr> mListeners;
    std::vector<Pending>                mQueue;
    std::vector<k::Destroyed>           mBreaks;
    std::map<GlobalPos, std::string>    mBreakPending;
    std::set<ChunkKey>                  mDirtyChunks;
    std::vector<k::Change>              mPendingChanges;
    std::vector<GlobalPos>              mCheckOrder;
    std::size_t                         mCheckCursor = 0;
    std::vector<Listener>               mListenerList;
    ListenerId                          mNextListener = 0;
    bool                                mLoaded       = false;
    bool                                mInstalled    = false;
    bool                                mFlushing     = false;
};

// ---- public API ---------------------------------------------------------------------------------------------------

namespace {
KineticsModule& mod() { return KineticsModule::get(); }
} // namespace

float speedAt(GlobalPos const& pos) {
    auto n = mod().solver().node(pos);
    return n ? n->speed : 0.f;
}

float theoreticalSpeedAt(GlobalPos const& pos) {
    auto n = mod().solver().node(pos);
    return n ? n->theoreticalSpeed : 0.f;
}

std::optional<NodeInfo> nodeAt(GlobalPos const& pos) { return mod().solver().node(pos); }

std::optional<NetworkInfo> networkAt(GlobalPos const& pos) { return mod().solver().networkAt(pos); }

std::vector<GlobalPos> networkMembers(GlobalPos const& pos) {
    auto& s = mod().solver();
    auto  n = s.node(pos);
    if (!n || !n->network) return {};
    return s.members(*n->network);
}

bool isOverstressed(GlobalPos const& pos) {
    auto n = mod().solver().node(pos);
    return n && n->overstressed;
}

SpeedLevel speedLevelOf(float speed) {
    auto const& cfg = config::get<KineticsConfig>();
    return k::speedLevel(speed, cfg.mediumSpeed, cfg.fastSpeed);
}

double stressImpactOf(std::string_view id) {
    auto const* spec = specOf(id);
    return tableValue(config::get<KineticsConfig>().impact, spec ? spec->stressKey : k::defaultStressKey(id), spec ? spec->impact : 0.0);
}

double stressCapacityOf(std::string_view id) {
    auto const* spec = specOf(id);
    return tableValue(config::get<KineticsConfig>().capacity, spec ? spec->stressKey : k::defaultStressKey(id), spec ? spec->capacity : 0.0);
}

void setGeneratedSpeed(GlobalPos const& pos, float rpm) {
    mod().solver().setGeneratedSpeed(pos, rpm);
    mod().markDirty(pos);
    mod().collect();
}

void setStressImpact(GlobalPos const& pos, std::optional<double> suPerRpm) {
    mod().solver().setImpactOverride(pos, suPerRpm);
    mod().markDirty(pos);
    mod().collect();
}

void setStressCapacity(GlobalPos const& pos, std::optional<double> suPerRpm) {
    mod().solver().setCapacityOverride(pos, suPerRpm);
    mod().markDirty(pos);
    mod().collect();
}

void refresh(GlobalPos const& pos) {
    mod().flush();
    mod().refresh(pos);
}

ListenerId onSpeedChanged(std::string blockId, std::function<void(SpeedChange const&)> fn) {
    return mod().addListener(Listener{0, std::move(blockId), std::move(fn), {}});
}

ListenerId onBreak(std::function<void(BreakEvent const&)> fn) {
    return mod().addListener(Listener{0, {}, {}, std::move(fn)});
}

void removeListener(ListenerId id) { mod().removeListener(id); }

namespace detail {

core::kinetics::Solver& solver() { return mod().solver(); }

void flush() { mod().flush(); }

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

void saveNow() {
    mod().flush();
    mod().saveDirty();
}

void reloadFromDb() {
    mod().flush();
    mod().saveDirty();
    mod().loadWorld();
}

} // namespace detail

} // namespace lv::feature::kinetics

LV_REGISTER_MODULE(lv::feature::kinetics::KineticsModule);
