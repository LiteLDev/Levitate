// Tracks unit: module wiring (events, world lifecycle, persistence) and the public API.

#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/platform/UUID.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/tracks/Tracks.h"
#include "feature/tracks/TracksInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("tracks", lv::feature::tracks::TracksConfig);

namespace lv::feature::tracks {

namespace {
std::vector<std::function<void()>>& graphListeners() {
    static std::vector<std::function<void()>> v;
    return v;
}
} // namespace

State& state() {
    static State s;
    return s;
}

TracksConfig const& config() { return lv::config::get<TracksConfig>(); }

ct::BlockPos3 toCore(BlockPos const& p) { return {p.x, p.y, p.z}; }
BlockPos      toGame(ct::BlockPos3 const& p) { return BlockPos{p.x, p.y, p.z}; }

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

std::optional<ct::Shape> shapeOf(Block const& block) {
    if (!isTrack(block)) return std::nullopt;
    auto lo = game::intState(block, kShapeState);
    if (!lo) return std::nullopt;
    return ct::shapeFromInt(*lo + 16 * game::intState(block, kShapeHiState).value_or(0));
}

Block const* trackBlock(ct::Shape shape, bool turn) {
    int s = int(shape);
    return game::blockWith(kTrackId, {{kShapeState, s % 16}, {kShapeHiState, s / 16}, {kTurnState, turn ? 1 : 0}});
}

bool setBlockQuiet(BlockSource& region, ct::BlockPos3 const& pos, Block const& block) {
    ++state().editing;
    bool ok = game::setBlock(region, toGame(pos), block);
    --state().editing;
    return ok;
}

std::optional<ct::TrackBlockView> GameWorld::trackAt(int dim, ct::BlockPos3 const& pos) const {
    auto* region = regionOf(dim);
    if (!region) return std::nullopt;
    BlockPos bp = toGame(pos);
    if (bp.y < region->getMinHeight() || bp.y >= region->getMaxHeight()) return std::nullopt;
    if (!region->hasChunksAt(bp, 0, false)) return std::nullopt;
    auto shape = shapeOf(region->getBlock(bp));
    if (!shape) return std::nullopt;
    return viewOf(dim, pos, *shape);
}

GameWorld const& world() {
    static GameWorld w;
    return w;
}

ct::TrackBlockView viewOf(int dim, ct::BlockPos3 const& pos, ct::Shape shape) {
    ct::TrackBlockView v;
    v.shape  = shape;
    v.curves = state().curves.curvesAt(dim, pos);
    if (auto it = state().portals.find({dim, pos}); it != state().portals.end()) {
        v.portalLink = std::pair{it->second.dim, it->second.pos};
    }
    return v;
}

void markCurveChunkDirty(int dim, ct::BlockPos3 const& primaryPos) {
    auto [cx, cz] = ct::CurveStore::chunkOf(primaryPos);
    state().dirtyCurveChunks.insert({dim, cx, cz});
}

void fireGraphChanged() {
    auto& s = state();
    if (s.rail.version() == s.notifiedVersion) return;
    s.notifiedVersion = s.rail.version();
    for (auto const& fn : graphListeners()) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().error("tracks: graph listener failed: {}", e.what());
        }
    }
}

// ---- persistence --------------------------------------------------------------------------------

namespace {
constexpr char const* kCurvePrefix  = "lv:tracks:curves:";
constexpr char const* kGraphPrefix  = "lv:tracks:graph:";
constexpr char const* kPortalsKey   = "lv:tracks:portals";

std::string curveKey(int dim, int cx, int cz) {
    return kCurvePrefix + std::to_string(dim) + ":" + std::to_string(cx) + ":" + std::to_string(cz);
}

void resolveTypes() {
    auto& s      = state();
    auto  typeOf = [](std::string_view id) -> BlockType const* {
        auto const* b = game::blockWith(id);
        return b ? &b->getBlockType() : nullptr;
    };
    s.trackType  = typeOf(kTrackId);
    s.fakeType   = typeOf(kFakeTrackId);
    s.portalType = typeOf("minecraft:portal");
    if (!s.trackType || !s.fakeType) log().warn("tracks: levitate:track is not registered (behavior pack missing?)");
}
} // namespace

void loadWorld() {
    auto& s = state();
    s.rail.clear();
    s.curves.clear();
    s.portals.clear();
    s.selections.clear();
    s.dirtyCurveChunks.clear();
    s.savedGraphDims.clear();
    resolveTypes();

    std::vector<std::string> curveKeys, graphKeys;
    game::db::forEachKey(kCurvePrefix, [&](std::string_view suffix) { curveKeys.emplace_back(suffix); });
    game::db::forEachKey(kGraphPrefix, [&](std::string_view suffix) { graphKeys.emplace_back(suffix); });
    size_t curves = 0;
    for (auto const& suffix : curveKeys) {
        int dim = std::atoi(suffix.c_str());
        auto j  = game::db::load(kCurvePrefix + suffix);
        if (!j) continue;
        for (auto const& jc : j->value("curves", nlohmann::json::array())) {
            if (auto c = ct::Curve::fromJson(jc)) {
                s.curves.add(dim, *c);
                ++curves;
            }
        }
    }
    for (auto const& suffix : graphKeys) {
        if (auto j = game::db::load(kGraphPrefix + suffix)) {
            s.rail.loadJson(*j);
            s.savedGraphDims.insert(std::atoi(suffix.c_str()));
        }
    }
    s.rail.finishLoading();
    if (auto j = game::db::load(kPortalsKey)) {
        for (auto const& e : *j) {
            if (!e.is_array() || e.size() < 8) continue;
            s.portals[{e[0].get<int>(), {e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}}] =
                PortalLink{e[4].get<int>(), {e[5].get<int>(), e[6].get<int>(), e[7].get<int>()}};
        }
    }
    s.savedGraphVersion = s.rail.version();
    s.notifiedVersion   = s.rail.version();
    size_t nodes        = 0;
    for (auto* g : s.rail.graphs()) nodes += g->nodeCount();
    log().info("tracks: loaded {} curves, {} graphs, {} nodes", curves, s.rail.graphs().size(), nodes);
}

void saveWorld() {
    auto& s = state();
    if (!game::db::available()) return;
    for (auto const& [dim, cx, cz] : s.dirtyCurveChunks) {
        auto list = s.curves.inChunk(dim, cx, cz);
        if (list.empty()) {
            game::db::erase(curveKey(dim, cx, cz));
            continue;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (auto const& c : list) arr.push_back(c->toJson());
        game::db::save(curveKey(dim, cx, cz), {{"curves", arr}});
    }
    s.dirtyCurveChunks.clear();

    if (s.rail.version() != s.savedGraphVersion) {
        auto dims = s.rail.dimensions();
        for (int dim : dims) game::db::save(kGraphPrefix + std::to_string(dim), s.rail.toJson(dim));
        for (int dim : s.savedGraphDims)
            if (!dims.contains(dim)) game::db::erase(kGraphPrefix + std::to_string(dim));
        s.savedGraphDims    = dims;
        s.savedGraphVersion = s.rail.version();
    }

    if (s.portalsDirty) {
        nlohmann::json arr = nlohmann::json::array();
        for (auto const& [key, link] : s.portals) {
            auto const& [dim, p] = key;
            arr.push_back({dim, p.x, p.y, p.z, link.dim, link.pos.x, link.pos.y, link.pos.z});
        }
        game::db::save(kPortalsKey, arr);
        s.portalsDirty = false;
    }
}

// ---- public API ---------------------------------------------------------------------------------

ct::Railways&         railways() { return state().rail; }
ct::CurveStore const& curves() { return state().curves; }

bool isTrack(Block const& block) {
    auto const* t = state().trackType;
    return t ? &block.getBlockType() == t : block.getTypeName() == kTrackId;
}

bool isFakeTrack(Block const& block) {
    auto const* t = state().fakeType;
    return t ? &block.getBlockType() == t : block.getTypeName() == kFakeTrackId;
}

std::optional<ct::Shape> shapeAt(BlockSource& region, BlockPos const& pos) {
    if (!region.hasChunksAt(pos, 0, false)) return std::nullopt;
    return shapeOf(region.getBlock(pos));
}

std::optional<ct::GraphLocation> locate(int dim, Vec3 const& pos, double maxDistance) {
    return state().rail.nearestEdge(dim, {pos.x, pos.y, pos.z}, maxDistance);
}

std::optional<ct::GraphLocation> targetAt(core::GlobalPos const& pos, Vec3 const& facing) {
    auto& rail = state().rail;
    auto  view = world().trackAt(pos.dim, {pos.x, pos.y, pos.z});
    if (!view) return std::nullopt;
    ct::BlockPos3 bp{pos.x, pos.y, pos.z};
    ct::Vec3d     centre = bp.bottomCenter() + ct::Vec3d{0, ct::isSlope(view->shape) ? .5 : 0, 0};
    auto          loc    = rail.nearestEdge(pos.dim, centre, 0.75);
    if (!loc) return std::nullopt;
    ct::Graph* g = rail.graph(loc->graph);
    auto const* e = g ? g->edge(loc->node1, loc->node2) : nullptr;
    if (!e) return std::nullopt;
    ct::Vec3d dir = g->edgeDirection(*e, true);
    if (dir.dot({facing.x, facing.y, facing.z}) < 0) {
        loc = ct::GraphLocation{loc->graph, loc->node2, loc->node1, g->edgeLength(*e) - loc->position};
    }
    return loc;
}

void onGraphChanged(std::function<void()> fn) { graphListeners().push_back(std::move(fn)); }

int dismantleCurves(Player& player, core::GlobalPos const& pos) {
    auto list = state().curves.curvesAt(pos.dim, {pos.x, pos.y, pos.z});
    for (auto const& c : list) destroyCurve(pos.dim, *c, false, &player);
    return int(list.size());
}

// ---- module -------------------------------------------------------------------------------------

namespace {

std::pair<int, ct::BlockPos3> keyOf(BlockSource const& region, BlockPos const& pos) {
    return {int(region.getDimensionId()), toCore(pos)};
}
/// A track a player just placed: Create's getStateForPlacement (shape from the look direction,
/// ascending towards a solid neighbour) and canSurvive (not on top of another track).
void fixPlacedShape(int dim, ct::BlockPos3 pos, PlaceIntent const& intent) {
    auto* region = regionOf(dim);
    if (!region) return;
    auto current = shapeAt(*region, toGame(pos));
    if (!current) return;
    if (shapeAt(*region, toGame(pos.below()))) {
        auto const* air = game::blockWith("minecraft:air");
        state().noDrops.insert({dim, pos});
        if (air) game::setBlock(*region, toGame(pos), *air);
        if (auto level = ll::service::getLevel()) {
            if (auto* p = level->getPlayer(mce::UUID::fromString(intent.uuid))) {
                ItemStack back;
                back.reinit(kTrackId, 1, 0);
                if (!p->isCreative()) p->add(back);
                p->sendInventory(false);
            }
        }
        return;
    }
    auto sturdy = [&](int dx, int dz) {
        BlockPos side = toGame(pos.offset(dx, 0, dz));
        BlockPos top  = toGame(pos.offset(dx, 1, dz));
        if (!region->hasChunksAt(side, 0, false)) return false;
        return region->getBlock(side).canProvideFullSupport(1) && !region->getBlock(top).canProvideFullSupport(0);
    };
    auto shape = ct::placementShape(intent.look, intent.yaw, sturdy);
    if (shape == *current) return;
    if (auto const* b = trackBlock(shape, false)) game::setBlock(*region, toGame(pos), *b);
}

void runTick() {
    auto& s = state();
    ++s.tick;
    auto fixes = std::move(s.shapeFixes);
    s.shapeFixes.clear();
    for (auto& fn : fixes) fn();
    auto tasks = std::move(s.tasks);
    s.tasks.clear();
    for (auto& fn : tasks) fn();
    auto added = std::move(s.railAdded);
    s.railAdded.clear();
    for (auto const& [dim, pos] : added) onTrackPlaced(dim, pos);
    s.noDrops.clear();
    s.playerBroke.clear();
    s.placeIntents.clear();
    fireGraphChanged();
}

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    if (ev.layer() != 0) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    auto&        s          = state();
    Block const& before     = ev.previousBlock();
    Block const& after      = ev.newBlock();
    bool         wasTrack   = isTrack(before);
    bool         isNowTrack = isTrack(after);
    bool         wasFake    = isFakeTrack(before);
    bool         isNowFake  = isFakeTrack(after);
    bool wasPortal = s.portalType && &before.getBlockType() == s.portalType && &after.getBlockType() != s.portalType;
    if (!wasTrack && !isNowTrack && !wasFake && !wasPortal) return;
    auto key = keyOf(region, ev.pos());
    // the game reports some changes twice: handle each change once
    static std::tuple<uint64_t, std::pair<int, ct::BlockPos3>, Block const*, Block const*> last{};
    auto current = std::tuple{s.tick, key, &before, &after};
    if (current == last) return;
    last = current;

    if (wasTrack) {
        auto oldShape = shapeOf(before);
        auto newShape = isNowTrack ? shapeOf(after) : std::nullopt;
        if (!isNowTrack && oldShape) {
            bool drops = !s.noDrops.contains(key);
            onTrackRemoved(key.first, key.second, *oldShape, drops);
            return;
        }
        if (isNowTrack && oldShape && newShape && *oldShape != *newShape) {
            // a track placed this tick isn't in the graph yet: just (re)queue it
            if (!s.railAdded.contains(key))
                s.rail.onRailRemoved(world(), key.first, key.second, viewOf(key.first, key.second, *oldShape));
            s.railAdded.insert(key);
        }
        return;
    }
    if (isNowTrack) {
        if (s.editing == 0) {
            if (auto it = s.placeIntents.find(key); it != s.placeIntents.end()) {
                s.shapeFixes.push_back([key, intent = it->second] { fixPlacedShape(key.first, key.second, intent); });
                s.placeIntents.erase(it);
            }
        }
        s.railAdded.insert(key);
        return;
    }
    if (wasFake && !isNowFake && s.editing == 0) {
        auto through = s.curves.curvesThrough(key.first, key.second);
        if (through.empty()) return;
        if (s.playerBroke.contains(key)) {
            bool drops = !s.noDrops.contains(key);
            for (auto const& c : through) destroyCurve(key.first, *c, drops, nullptr);
            log().info("tracks: curve broken at {}", key.second.str());
        } else {
            s.tasks.push_back([key] { restoreFakeTracks(key.first, key.second); });
        }
        return;
    }
    if (wasPortal) {
        s.tasks.push_back([key] { onPortalRemoved(key.first, key.second); });
    }
}

} // namespace

class TracksModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        // BDS: the world was loaded before the mods were enabled
        if (game::db::available() && !state().trackType) loadWorld();

        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runTick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerJoinEvent>([](ll::event::PlayerJoinEvent& ev) {
            syncAllTo(ev.self());
        }));
        installHooks();
    }

    void uninstall() {
        uninstallHooks();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

namespace {
/// World lifecycle hooks, registered once at static init (before the world DB fires them).
struct WorldHooks {
    WorldHooks() {
        game::db::onWorldReady([] { loadWorld(); });
        game::db::onWorldSave([] { saveWorld(); });
        game::db::onWorldClosed([] {
            auto& s = state();
            s.rail.clear();
            s.curves.clear();
            s.portals.clear();
            s.selections.clear();
            s.shapeFixes.clear();
            s.tasks.clear();
            s.railAdded.clear();
            s.placeIntents.clear();
            s.trackType = s.fakeType = s.portalType = nullptr;
        });
    }
} const gWorldHooks;
} // namespace

} // namespace lv::feature::tracks

LV_REGISTER_MODULE(lv::feature::tracks::TracksModule);
