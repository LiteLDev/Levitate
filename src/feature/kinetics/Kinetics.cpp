// Kinetic network on the server: keeps the solver (src/core/kinetics) in step with the world,
// applies its effects (breaking blocks, listeners, sync), persists it in U3's BlockData
// (`data["kinetics"]` = the block's NodeRecord, written back before every save and before a
// contraption takes the block) and loads the stress table from `config/kinetics.json`.

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
#include "feature/common/BlockData.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/kinetics/KineticsInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("kinetics", lv::feature::kinetics::KineticsConfig);

namespace lv::feature::kinetics {

namespace k  = core::kinetics;
namespace bd = common::blockdata;

namespace {

/// Wave-1 storage (one array of records per chunk), migrated into BlockData on world load.
constexpr char const* kLegacyChunkPrefix = "lv:kinetics:c:";
/// The top-level key of the kinetic record in a BlockData document.
constexpr char const* kDataKey = "kinetics";
/// Ticks between two world checks of the same kinetic block (block still there?).
constexpr int kWorldCheckTicks = 100;

/// The stress table value of `key` (config `kinetics.json`), else `fallback`.
double tableValue(std::map<std::string, double> const& table, std::string const& key, double fallback) {
    auto it = table.find(key);
    return it == table.end() ? fallback : it->second;
}

/// Sequenced gearshift instructions per block that set one (not persisted; cleared per world).
std::map<GlobalPos, SequenceContext>& sequenceContexts() {
    static std::map<GlobalPos, SequenceContext> m;
    return m;
}

/// Config files written before version 2 keep the old, wrong default gantry_shaft impact of 4 (the
/// version merge keeps existing table entries): Create 6.0.10 has none. Reset it once per load.
void fixStaleDefaults() {
    auto& cfg = config::mut<KineticsConfig>();
    if (auto it = cfg.impact.find("gantry_shaft"); it != cfg.impact.end() && it->second == 4.0) {
        it->second = 0.0;
        config::saveAll();
        log().info("kinetics: config gantry_shaft impact 4 (an old default) set to 0 as in Create 6.0.10");
    }
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
        fixStaleDefaults();
        mSolver.setConfig(solverConfig());
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([this](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { onTick(); }));
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldReady([this] { loadWorld(); });
            game::db::onWorldClosed([this] { closeWorld(); });
            config::onReload([this] {
                fixStaleDefaults();
                mSolver.setConfig(solverConfig());
            });
            // BlockData saves (and contraptions take) our records: write them back first
            bd::onWriteBack([this](GlobalPos const* pos) { writeBack(pos); });
            bd::onPut([this](GlobalPos const& pos, nlohmann::json const& doc) { adopt(pos, doc); });
        }
        mInstalled = true;
        if (game::db::available()) loadWorld(); // BDS: the world exists before mods are enabled
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        if (mLoaded) writeBack(nullptr);
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
            if (mSolver.contains(pos)) {
                mSolver.update(pos, std::move(snap));
            } else {
                mSolver.add(pos, std::move(snap));
                // a block a contraption put back brings its record (generated speed, overrides)
                if (auto const* doc = bd::get(pos); doc && doc->contains(kDataKey)) {
                    auto rec = (*doc)[kDataKey];
                    adoptRecord(pos, rec);
                }
            }
        }
        mDirty.insert(pos);
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
            mDirty.insert(d.pos);
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
        for (auto const& c : changes) mDirty.insert(c.pos);
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
    /// Moves the wave-1 records (`lv:kinetics:c:<dim>:<cx>:<cz>`, one array per chunk) into BlockData once.
    void migrateLegacy() {
        std::vector<std::string> keys;
        game::db::forEachKey(kLegacyChunkPrefix, [&](std::string_view suffix) { keys.emplace_back(suffix); });
        if (keys.empty()) return;
        std::size_t count = 0;
        for (auto const& suffix : keys) {
            auto data = game::db::load(kLegacyChunkPrefix + suffix);
            if (!data || !data->is_array()) continue;
            for (auto const& j : *data) {
                try {
                    auto r                    = j.get<k::NodeRecord>();
                    bd::edit(r.pos)[kDataKey] = j;
                    ++count;
                } catch (std::exception const& e) {
                    log().warn("kinetics: skipped an unreadable block record in {}: {}", suffix, e.what());
                }
            }
        }
        bd::flush(); // the records are in the world DB before the old keys go
        for (auto const& suffix : keys) game::db::erase(kLegacyChunkPrefix + suffix);
        log().info("kinetics: moved {} block records from {} chunks into block data", count, keys.size());
    }

    void loadWorld() {
        if (!mInstalled) return;
        mLoaded = false; // no write-backs of the old network while loading
        detail::clearBlockCaches(); // block types are registered again for every world
        mSolver.clear();
        mSolver.setConfig(solverConfig());
        mQueue.clear();
        mBreaks.clear();
        mBreakPending.clear();
        mDirty.clear();
        mPendingChanges.clear();
        mCheckOrder.clear();
        mCheckCursor = 0;
        sequenceContexts().clear();
        migrateLegacy();
        std::size_t count = 0;
        bd::forEach([&](GlobalPos const& pos, nlohmann::json const& doc) {
            auto it = doc.find(kDataKey);
            if (it == doc.end()) return;
            try {
                auto r = it->get<k::NodeRecord>();
                if (r.pos != pos) { // carried here by a contraption: re-attach from scratch
                    r.pos   = pos;
                    r.speed = 0;
                    r.source.reset();
                    r.network.reset();
                }
                mSolver.restore(r);
                ++count;
            } catch (std::exception const& e) {
                log().warn("kinetics: skipped an unreadable block record at {}: {}", pos.key(), e.what());
            }
        });
        mSolver.finishRestore();
        mLoaded = true;
        collect();
        mPendingChanges.clear();
        detail::syncReset();
        log().info("kinetics: loaded {} kinetic blocks", count);
    }

    /// Writes the record of `pos` into its BlockData document (or drops it when the node is gone).
    void writeRecord(GlobalPos const& pos) {
        auto const* doc = bd::get(pos);
        if (mSolver.contains(pos)) {
            nlohmann::json rec = mSolver.record(pos);
            if (doc && doc->contains(kDataKey) && (*doc)[kDataKey] == rec) return;
            bd::edit(pos)[kDataKey] = std::move(rec);
        } else if (doc && doc->contains(kDataKey)) {
            auto rest = *doc;
            rest.erase(kDataKey);
            bd::set(pos, rest.empty() ? nlohmann::json() : std::move(rest));
        }
    }

    /// BlockData write-back: every changed record (nullptr, before a save) or one position
    /// (before a contraption takes the block).
    void writeBack(GlobalPos const* pos) {
        if (!mLoaded) return;
        flush(); // blocks broken since the last tick must not get a record
        if (pos) {
            writeRecord(*pos);
            mDirty.erase(*pos);
            return;
        }
        auto dirty = std::move(mDirty);
        mDirty.clear();
        for (auto const& p : dirty) writeRecord(p);
    }

    /// Takes over the generated speed and stress overrides of a record carried to `pos`.
    void adoptRecord(GlobalPos const& pos, nlohmann::json const& j) {
        try {
            auto r = j.get<k::NodeRecord>();
            if (r.generated != 0) mSolver.setGeneratedSpeed(pos, r.generated);
            if (r.impactOverride) mSolver.setImpactOverride(pos, r.impactOverride);
            if (r.capacityOverride) mSolver.setCapacityOverride(pos, r.capacityOverride);
        } catch (std::exception const& e) {
            log().warn("kinetics: unreadable carried record at {}: {}", pos.key(), e.what());
        }
        mDirty.insert(pos);
    }

    /// BlockData put listener: a contraption placed a block with its document.
    void adopt(GlobalPos const& pos, nlohmann::json const& doc) {
        if (!mLoaded) return;
        auto it = doc.find(kDataKey);
        if (it == doc.end()) return;
        bool known = mSolver.contains(pos);
        flush(); // the block placed just before is added here (and adopts the record itself)
        if (!known && mSolver.contains(pos)) {
            collect();
            return;
        }
        if (!mSolver.contains(pos)) return;
        adoptRecord(pos, *it);
        collect();
    }

    void closeWorld() {
        mLoaded = false;
        sequenceContexts().clear();
        detail::clearBlockCaches();
        mSolver.clear();
        mQueue.clear();
        mBreaks.clear();
        mBreakPending.clear();
        mDirty.clear();
        mPendingChanges.clear();
        mCheckOrder.clear();
        mCheckCursor = 0;
        detail::syncReset();
    }

    void markDirty(GlobalPos const& pos) { mDirty.insert(pos); }

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
    std::set<GlobalPos>                 mDirty; ///< records to write back into BlockData
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

void setSequenceContext(GlobalPos const& pos, std::optional<SequenceContext> context) {
    static std::uint64_t serial = 0;
    if (context) {
        auto& stored  = sequenceContexts()[pos] = *context;
        stored.serial = ++serial;
    } else {
        sequenceContexts().erase(pos);
    }
}

std::optional<SequenceContext> sequenceContextAt(GlobalPos const& pos) {
    auto& contexts = sequenceContexts();
    if (contexts.empty()) return std::nullopt;
    auto& solver = mod().solver();
    auto  here   = solver.node(pos);
    if (!here) return std::nullopt;
    // walk the source chain up to the generator; the nearest block with an instruction wins
    std::optional<NodeInfo> n = here;
    for (int depth = 0; n && depth < 4096; ++depth) {
        if (auto it = contexts.find(n->pos); it != contexts.end()) {
            SequenceContext out = it->second;
            double          at  = std::abs(here->theoreticalSpeed);
            double          src = std::abs(n->theoreticalSpeed);
            out.value           = src > 0 ? std::abs(out.value) * at / src : 0.0;
            return out;
        }
        if (!n->source || *n->source == n->pos) break;
        n = solver.node(*n->source);
    }
    return std::nullopt;
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
    bd::flush(); // writes our records back first
}

void reloadFromDb() {
    saveNow();
    mod().loadWorld();
}

} // namespace detail

} // namespace lv::feature::kinetics

LV_REGISTER_MODULE(lv::feature::kinetics::KineticsModule);
