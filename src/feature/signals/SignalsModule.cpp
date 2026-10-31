// Signals unit (U28): train signals, signal sections and track observers.
//
// Create's SignalBlock / SignalBlockEntity / SignalBoundary / TrackObserver over the U5 track
// graph: blocks are bound to edge points (types "levitate:signal" / "levitate:observer"), the
// pure section logic lives in core/signals, trains use feature/tracks/SignalsExt.h.
//
// Persistence: the blocks (position, target, bound point, side, cached type / power) in
// "lv:signals:blocks"; the edge points are stored with the track graph by U5; observer filters
// in BlockData (U3 filter slot "filter").

#include <algorithm>
#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/SetTitlePacket.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/platform/UUID.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"

#include "core/tracks/Shape.h"
#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/common/ValueBox.h"
#include "feature/display/Display.h"
#include "feature/net/Net.h"
#include "feature/signals/SignalsInternal.h"
#include "feature/tracks/Tracks.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::signals {

namespace {
constexpr char const* kIndexKey = "lv:signals:blocks";
constexpr int         kObserverHold = 8; ///< Create's TrackObserver.keepAlive
} // namespace

State& state() {
    static State s;
    return s;
}

// ---- helpers ------------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

std::optional<Kind> kindOf(Block const& block) {
    std::string const& id = block.getTypeName();
    if (id == api::kSignalId) return Kind::Signal;
    if (id == api::kObserverId) return Kind::Observer;
    return std::nullopt;
}

void actionbar(Player& player, std::string const& key, std::string const& fallback, char colour) {
    std::string text = std::string("\xC2\xA7") + colour + trFor(player, key, fallback);
    SetTitlePacket pkt(SetTitlePacketPayload::TitleType::Actionbar, text, std::nullopt);
    pkt.sendTo(player);
}

void playSound(Player& player, std::string const& name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    Vec3  pos   = player.getPosition();
    pkt.mName   = name;
    pkt.mPos    = BlockPos{int(pos.x * 8), int(pos.y * 8), int(pos.z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(player);
}

namespace {

Vec3 toVec3(ct::Vec3d const& v) { return Vec3{float(v.x), float(v.y), float(v.z)}; }

/// The edge of a graph location (graph, edge, node locations).
struct EdgeView {
    ct::Graph const*  graph = nullptr;
    ct::Edge const*   edge  = nullptr;
    ct::NodeLocation  n1, n2;
    double            length = 0;
};
std::optional<EdgeView> viewOf(ct::GraphLocation const& loc) {
    auto& rail = tracks::railways();
    auto* g    = rail.graph(loc.graph);
    if (!g) return std::nullopt;
    auto const* e  = g->edge(loc.node1, loc.node2);
    auto const* a  = g->node(loc.node1);
    auto const* b  = g->node(loc.node2);
    if (!e || !a || !b) return std::nullopt;
    return EdgeView{g, e, a->location, b->location, g->edgeLength(*e)};
}

/// The graph location of an edge point (travelling node1 -> node2).
std::optional<ct::GraphLocation> locationOf(ct::EdgePoint const& p) {
    auto& rail = tracks::railways();
    auto* g    = rail.graphAt(p.node1);
    if (!g) return std::nullopt;
    auto const* a = g->locateNode(p.node1);
    auto const* b = g->locateNode(p.node2);
    if (!a || !b || !g->edge(a->id, b->id)) return std::nullopt;
    return ct::GraphLocation{g->id(), a->id, b->id, p.position};
}

std::string pointIdFor(Kind kind, GlobalPos const& pos) {
    return std::string(kind == Kind::Signal ? "signal:" : "observer:") + pos.key();
}

nlohmann::json vecJson(ct::Vec3d const& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
ct::Vec3d      jsonVec(nlohmann::json const& j) {
    return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

nlohmann::json recJson(BlockRec const& r) {
    return {
        {"k",  r.kind == Kind::Signal ? "signal" : "observer"                   },
        {"p",  nlohmann::json::array({r.pos.dim, r.pos.x, r.pos.y, r.pos.z})    },
        {"t",
         {{"d", r.target.dim},
          {"b", nlohmann::json::array({r.target.track.x, r.target.track.y, r.target.track.z})},
          {"v", vecJson(r.target.dir)},
          {"c", r.target.curve},
          {"a", vecJson(r.target.at)}}                                          },
        {"pt", r.point                                                          },
        {"s",  r.side                                                           },
        {"ty", r.type                                                           },
        {"pw", r.powered                                                        },
    };
}

std::optional<BlockRec> jsonRec(nlohmann::json const& j) {
    try {
        BlockRec r;
        r.kind      = j.at("k").get<std::string>() == "signal" ? Kind::Signal : Kind::Observer;
        auto p      = j.at("p");
        r.pos       = {p.at(0).get<int>(), p.at(1).get<int>(), p.at(2).get<int>(), p.at(3).get<int>()};
        auto t      = j.at("t");
        r.target.dim = t.at("d").get<int>();
        auto b      = t.at("b");
        r.target.track = {b.at(0).get<int>(), b.at(1).get<int>(), b.at(2).get<int>()};
        r.target.dir   = jsonVec(t.at("v"));
        r.target.curve = t.value("c", false);
        r.target.at    = jsonVec(t.at("a"));
        r.point        = j.value("pt", std::string());
        r.side         = j.value("s", 1);
        r.type         = j.value("ty", 0);
        r.powered      = j.value("pw", false);
        return r;
    } catch (...) {
        return std::nullopt;
    }
}

void loadWorld() {
    auto& s = state();
    s.blocks.clear();
    s.watches.clear(); // (the redstone watches are dropped with the world)
    s.observers.clear();
    s.intents.clear();
    s.tasks.clear();
    s.system      = cs::SignalSystem{};
    s.seenVersion = ~0ull;
    s.lastOverlay.clear();
    if (auto j = game::db::load(kIndexKey)) {
        for (auto const& jr : j->value("blocks", nlohmann::json::array()))
            if (auto r = jsonRec(jr)) s.blocks[r->pos] = *r;
    }
    s.loaded     = true;
    s.indexDirty = false;
    log().info("signals: loaded {} signal / observer block(s)", s.blocks.size());
}

void saveWorld() {
    auto& s = state();
    if (!s.loaded || !s.indexDirty || !game::db::available()) return;
    nlohmann::json list = nlohmann::json::array();
    for (auto const& [pos, r] : s.blocks) list.push_back(recJson(r));
    game::db::save(kIndexKey, {{"version", 1}, {"blocks", list}});
    s.indexDirty = false;
}

/// Binds a block to its target: joins a signal boundary within 1/4 block or adds a new point.
bool bind(BlockRec& rec) {
    auto& rail = tracks::railways();
    rec.point.clear();
    auto loc = resolve(rec.target);
    if (!loc) return false;
    auto view = viewOf(*loc);
    if (!view) return false;
    for (auto const* p : rail.pointsOn(view->n1, view->n2)) {
        double other = p->node1 == view->n1 ? p->position : view->length - p->position;
        double d     = std::abs(other - loc->position);
        if (d > .75) continue;
        if (rec.kind == Kind::Signal && p->type == api::kSignalPointType && d < .25) {
            rec.point = p->id;
            rec.side  = p->node1 == view->n1 ? 1 : 0;
            // the side keeps one signal type (Create's SignalBoundary.types)
            for (auto const& [pos, other] : state().blocks)
                if (pos != rec.pos && other.point == rec.point && other.side == rec.side) rec.type = other.type;
            return true;
        }
        return false; // occupied by another point
    }
    ct::EdgePoint np;
    np.id          = pointIdFor(rec.kind, rec.pos);
    np.type        = std::string(rec.kind == Kind::Signal ? api::kSignalPointType : api::kObserverPointType);
    np.node1       = view->n1;
    np.node2       = view->n2;
    np.position    = loc->position;
    np.facingNode2 = true;
    np.data        = {{"block", nlohmann::json::array({rec.pos.dim, rec.pos.x, rec.pos.y, rec.pos.z})}};
    if (!rail.addPoint(np)) return false;
    rec.point = np.id;
    rec.side  = 1;
    return true;
}

/// Boundaries and observers for the section logic, from the edge points and the blocks.
void rebuildSystem() {
    auto& s    = state();
    auto& rail = tracks::railways();
    std::map<std::string, std::array<cs::BoundarySide, 2>> sides;
    for (auto const& [pos, r] : s.blocks) {
        if (r.kind != Kind::Signal || r.point.empty()) continue;
        auto& side     = sides[r.point][size_t(r.side & 1)];
        side.hasSignal = true;
        side.type      = cs::SignalType(r.type & 1);
        side.forcedRed = side.forcedRed || r.powered;
    }
    std::vector<cs::Boundary> boundaries;
    for (auto const* p : rail.points(std::string(api::kSignalPointType))) {
        cs::Boundary b;
        b.id       = p->id;
        b.node1    = p->node1;
        b.node2    = p->node2;
        b.position = p->position;
        if (auto it = sides.find(p->id); it != sides.end()) b.sides = it->second;
        boundaries.push_back(std::move(b));
    }
    std::vector<cs::Observer> observers;
    for (auto const* p : rail.points(std::string(api::kObserverPointType))) observers.push_back({p->id, p->node1, p->node2, p->position});
    s.system.rebuild(rail, std::move(boundaries), std::move(observers));
}

/// Applies changed redstone / type of signal blocks without a full rebuild.
void applySides() {
    auto& s = state();
    std::map<std::string, std::array<cs::BoundarySide, 2>> sides;
    for (auto const& b : s.system.boundaries()) sides[b.id] = {};
    for (auto const& [pos, r] : s.blocks) {
        if (r.kind != Kind::Signal || r.point.empty() || !sides.contains(r.point)) continue;
        auto& side     = sides[r.point][size_t(r.side & 1)];
        side.hasSignal = true;
        side.type      = cs::SignalType(r.type & 1);
        side.forcedRed = side.forcedRed || r.powered;
    }
    for (auto const& [id, sd] : sides) s.system.setSides(id, sd);
}

Block const* signalBlock(int type, cs::SignalState lamp, bool powered) {
    return game::blockWith(
        api::kSignalId,
        {{api::kTypeState, type & 1}, {api::kLampState, api::lampValue(lamp)}, {api::kPoweredState, powered ? 1 : 0}}
    );
}

/// The lamp, type and power of loaded signal blocks; the power of observers.
void pushBlockStates() {
    auto& s = state();
    for (auto& [pos, r] : s.blocks) {
        auto* region = regionOf(pos.dim);
        if (!region) continue;
        BlockPos bp = game::blockPos(pos);
        if (!region->hasChunksAt(bp, 0, false)) continue;
        Block const& block = region->getBlock(bp);
        if (kindOf(block) != r.kind) continue;
        if (r.kind == Kind::Signal) {
            // Create's SignalBlockEntity.enterState: after a green or yellow light a signal turns
            // red only 15 ticks later (the train that took the section passes it first)
            auto target = lampOf(r);
            if (r.redHold > 0) --r.redHold;
            if (r.shown != target && !(target == cs::SignalState::Red && r.redHold > 0)) {
                r.shown   = target;
                r.redHold = target == cs::SignalState::Green || target == cs::SignalState::Yellow ? 15 : 0;
            }
            // SignalRenderer: an invalid signal flashes its red lamp (3 of every 40 ticks)
            auto lamp = r.shown == cs::SignalState::Invalid && s.tick % 40 < 3 ? cs::SignalState::Red : r.shown;
            if (game::intState(block, api::kLampState).value_or(-1) == api::lampValue(lamp)
                && game::intState(block, api::kTypeState).value_or(-1) == r.type
                && game::intState(block, api::kPoweredState).value_or(-1) == int(r.powered))
                continue;
            if (auto const* b = signalBlock(r.type, lamp, r.powered)) game::setBlock(*region, bp, *b);
        } else {
            bool on = false;
            if (auto it = s.observers.find(r.point); it != s.observers.end()) on = it->second.activated > 0;
            if (game::intState(block, api::kPoweredState).value_or(-1) != int(on)) {
                if (auto const* b = game::blockWith(api::kObserverId, {{api::kPoweredState, on ? 1 : 0}})) {
                    game::setBlock(*region, bp, *b);
                    log().info("signals: track observer at {} {}", pos.key(), on ? "powered" : "unpowered");
                    display::notifyGatherers(pos); // Create's TrackObserverBlockEntity: refresh the links
                }
            }
            // the powered state's redstone producer (minecraft:redstone_producer): make sure the
            // circuit component follows the state change
            if (auto* circuit = region->getDimension().mCircuitSystem.get()) circuit->setStrength(bp, on ? 15 : 0);
        }
    }
}

/// Create's SignalBlock.neighborChanged: the POWERED state follows the redstone input.
/// Create's SignalBlock.neighborChanged: one redstone watch per signal block (common::redstone).
void watchRedstone() {
    auto& s = state();
    for (auto& [pos, r] : s.blocks) {
        if (r.kind != Kind::Signal || s.watches.contains(pos)) continue;
        s.watches[pos] = common::redstone::watch(pos, [](GlobalPos const& at, int) {
            auto& st     = state();
            auto  it     = st.blocks.find(at);
            auto* region = regionOf(at.dim);
            if (it == st.blocks.end() || it->second.kind != Kind::Signal || !region) return;
            BlockPos bp = game::blockPos(at);
            if (!region->hasChunksAt(bp, 0, false) || kindOf(region->getBlock(bp)) != Kind::Signal) return;
            bool powered = poweredAt(*region, bp);
            if (powered == it->second.powered) {
                it->second.unpowerAt = 0;
                return;
            }
            if (!powered) {
                // Create's SignalBlock.neighborChanged: losing power is applied 4 ticks later
                if (!it->second.unpowerAt) it->second.unpowerAt = st.tick + 4;
                return;
            }
            it->second.powered   = powered;
            it->second.unpowerAt = 0;
            st.indexDirty        = true;
            st.sidesDirty        = true;
            log().info("signals: signal at {} forced red by redstone", at.key());
        });
    }
}

/// Create's SignalBlock.tick: a scheduled loss of power applies when the input is still off.
void applyUnpowering() {
    auto& s = state();
    for (auto& [pos, r] : s.blocks) {
        if (r.kind != Kind::Signal || !r.unpowerAt || s.tick < r.unpowerAt) continue;
        r.unpowerAt  = 0;
        auto* region = regionOf(pos.dim);
        if (!region || !region->hasChunksAt(game::blockPos(pos), 0, false)) continue;
        if (!r.powered || poweredAt(*region, game::blockPos(pos))) continue;
        r.powered    = false;
        s.indexDirty = true;
        s.sidesDirty = true;
        log().info("signals: signal at {} released by redstone", pos.key());
    }
}

/// Create's TrackObserver.tick / Train.tick: observers stay on 8 ticks after a matching train.
void tickObservers() {
    auto& s = state();
    for (auto& [pos, r] : s.blocks) {
        if (r.kind != Kind::Observer || r.point.empty()) continue;
        auto& run    = s.observers[r.point];
        auto  trains = s.system.trainsOver(r.point);
        bool  match  = false;
        if (!trains.empty()) {
            ItemStack filter = common::filterslot::get(pos, "filter");
            if (common::items::isEmpty(filter)) {
                match     = true;
                run.train = trains.front();
            } else if (s.cargo) {
                auto itemTest  = common::filter::predicate(filter);
                auto fluidTest = [&](common::fluids::FluidStack const& f) { return common::filter::matchesFluid(filter, f); };
                for (auto const& t : trains) {
                    if (s.cargo(t, itemTest, fluidTest)) {
                        match     = true;
                        run.train = t;
                        break;
                    }
                }
            }
        }
        if (match) run.activated = kObserverHold;
        else if (run.activated > 0) --run.activated;
        if (run.activated == 0) run.train.clear();
    }
}

// ---- client overlay (Create's TrackTargetingBehaviour track overlay) ---------------------------

nlohmann::json overlay() {
    auto&          s    = state();
    auto&          rail = tracks::railways();
    nlohmann::json list = nlohmann::json::array();
    std::set<std::pair<std::string, int>> done;
    for (auto const& [pos, r] : s.blocks) {
        if (r.point.empty() || !done.insert({r.point, r.side}).second) continue;
        auto const* p = rail.point(r.point);
        if (!p) continue;
        auto loc = locationOf(*p);
        if (!loc) continue;
        auto view = viewOf(*loc);
        auto at   = rail.positionOf(*loc);
        if (!view || !at) continue;
        ct::Vec3d ahead = view->graph->edgePositionAt(*view->edge, std::min(view->length, p->position + 0.25));
        ct::Vec3d back  = view->graph->edgePositionAt(*view->edge, std::max(0.0, p->position - 0.25));
        ct::Vec3d dir   = (ahead - back).normalize();
        if (r.kind == Kind::Signal && r.side == 0) dir = -dir;
        int stateValue = r.kind == Kind::Signal ? int(shownOf(r)) : 0;
        if (r.kind == Kind::Observer) {
            auto it    = s.observers.find(r.point);
            stateValue = it != s.observers.end() && it->second.activated > 0 ? 1 : 0;
        }
        list.push_back({
            {"d", p->node1.dim                                                         },
            {"p", nlohmann::json::array({std::round(at->x * 100) / 100, std::round(at->y * 100) / 100, std::round(at->z * 100) / 100})},
            {"v", nlohmann::json::array({std::round(dir.x * 100) / 100, std::round(dir.z * 100) / 100})},
            {"k", r.kind == Kind::Signal ? 0 : 1                                       },
            {"s", stateValue                                                           },
        });
    }
    return list;
}

void sendOverlay(Player* only) {
    auto& s     = state();
    auto  list  = overlay();
    auto  dump  = list.dump();
    if (!only && dump == s.lastOverlay) return;
    if (!only) s.lastOverlay = dump;
    auto level = ll::service::getLevel();
    if (!level) return;
    nlohmann::json msg = {{"markers", list}};
    int            sent = 0;
    auto send = [&](Player& p) {
        if (net::isCompanion(p) && net::send(p, "signals.overlay", msg)) ++sent;
    };
    if (only) send(*only);
    else level->forEachPlayer([&](Player& p) {
        send(p);
        return true;
    });
    if (sent > 0 && !s.loggedSync) {
        s.loggedSync = true;
        log().info("signals: sent {} track marker(s) to companions", list.size());
    }
}

} // namespace

// ---- targets ------------------------------------------------------------------------------------

std::optional<ct::GraphLocation> resolve(Target const& t) {
    if (!t.curve) return tracks::targetAt({t.dim, t.track.x, t.track.y, t.track.z}, toVec3(t.dir));
    auto loc = tracks::locate(t.dim, toVec3(t.at), 1.0);
    if (!loc) return std::nullopt;
    auto view = viewOf(*loc);
    if (!view) return std::nullopt;
    ct::Vec3d ahead = view->graph->edgePositionAt(*view->edge, std::min(view->length, loc->position + 0.1));
    ct::Vec3d back  = view->graph->edgePositionAt(*view->edge, std::max(0.0, loc->position - 0.1));
    if ((ahead - back).dot(t.dir) < 0) return ct::GraphLocation{loc->graph, loc->node2, loc->node1, view->length - loc->position};
    return loc;
}

Overlap check(Target const& t, Kind kind) {
    if (!t.curve) {
        auto* region = regionOf(t.dim);
        if (!region) return Overlap::NoTrack;
        auto shape = tracks::shapeAt(*region, BlockPos{t.track.x, t.track.y, t.track.z});
        if (!shape) return Overlap::NoTrack;
        if (ct::isJunction(*shape)) return Overlap::Junction;
    }
    auto loc = resolve(t);
    if (!loc) return Overlap::NoTrack;
    auto view = viewOf(*loc);
    if (!view) return Overlap::NoTrack;
    for (auto const* p : tracks::railways().pointsOn(view->n1, view->n2)) {
        double other = p->node1 == view->n1 ? p->position : view->length - p->position;
        double d     = std::abs(loc->position - other);
        if (d > .75) continue;
        if (kind == Kind::Signal && p->type == api::kSignalPointType && d < .25) continue;
        return Overlap::Occupied;
    }
    return Overlap::Valid;
}

std::optional<Target> targetOfClick(Player& player, BlockPos const& clicked) {
    auto& region = player.getDimensionBlockSource();
    if (!region.hasChunksAt(clicked, 0, false)) return std::nullopt;
    Block const& block = region.getBlock(clicked);
    Vec3         look  = player.getViewVector();
    ct::Vec3d    l{look.x, look.y, look.z};
    Target       t;
    t.dim   = int(player.getDimensionId());
    t.track = {clicked.x, clicked.y, clicked.z};
    if (tracks::isTrack(block)) {
        auto shape = tracks::shapeAt(region, clicked);
        if (!shape) return std::nullopt;
        // Create: the direction along the nearest track axis the player looks along
        t.dir = ct::nearestTrackAxis(*shape, l);
        t.at  = ct::BlockPos3{clicked.x, clicked.y, clicked.z}.bottomCenter();
        return t;
    }
    if (tracks::isFakeTrack(block)) {
        t.curve = true;
        t.dir   = l;
        t.at    = ct::BlockPos3{clicked.x, clicked.y, clicked.z}.bottomCenter();
        return t;
    }
    return std::nullopt;
}

// ---- blocks -------------------------------------------------------------------------------------

bool addBlock(Kind kind, GlobalPos const& pos, Target const& target) {
    auto&    s = state();
    BlockRec rec;
    rec.kind   = kind;
    rec.pos    = pos;
    rec.target = target;
    if (auto* region = regionOf(pos.dim)) {
        BlockPos bp = game::blockPos(pos);
        if (region->hasChunksAt(bp, 0, false)) {
            Block const& b = region->getBlock(bp);
            rec.type       = game::intState(b, api::kTypeState).value_or(0);
            rec.powered    = kind == Kind::Signal && poweredAt(*region, bp);
        }
    }
    bool ok        = bind(rec);
    s.blocks[pos]  = rec;
    s.indexDirty   = true;
    // Create's TrackTargetingBehaviour keeps the target relative to the block: a contraption that
    // moves the block (and its track) carries it in the BlockData
    auto& d = common::blockdata::edit(pos)["signals"];
    d       = {
        {"o", nlohmann::json::array({target.track.x - pos.x, target.track.y - pos.y, target.track.z - pos.z})},
        {"v", nlohmann::json::array({target.dir.x, target.dir.y, target.dir.z})},
        {"c", target.curve},
        {"a", nlohmann::json::array({target.at.x - pos.x, target.at.y - pos.y, target.at.z - pos.z})},
    };
    if (ok) log().info("signals: {} at {} bound to {} (side {})", kind == Kind::Signal ? "signal" : "observer", pos.key(), rec.point, rec.side);
    else log().info("signals: {} at {} could not be bound", kind == Kind::Signal ? "signal" : "observer", pos.key());
    return ok;
}

void removeBlock(GlobalPos const& pos) {
    auto& s  = state();
    auto  it = s.blocks.find(pos);
    if (it == s.blocks.end()) return;
    std::string point = it->second.point;
    s.blocks.erase(it);
    if (auto w = s.watches.find(pos); w != s.watches.end()) {
        common::redstone::unwatch(w->second);
        s.watches.erase(w);
    }
    s.indexDirty = true;
    if (!point.empty()) {
        bool used = std::any_of(s.blocks.begin(), s.blocks.end(), [&](auto const& e) { return e.second.point == point; });
        if (!used) {
            tracks::railways().removePoint(point);
            s.observers.erase(point);
        } else {
            s.sidesDirty = true;
        }
    }
    log().info("signals: block at {} removed", pos.key());
}

bool cycleType(GlobalPos const& pos) {
    auto& s  = state();
    auto  it = s.blocks.find(pos);
    if (it == s.blocks.end() || it->second.kind != Kind::Signal || it->second.point.empty()) return false;
    int next = it->second.type == 0 ? 1 : 0;
    // Create's SignalBoundary.cycleSignalType: every signal on that side switches
    for (auto& [p, r] : s.blocks)
        if (r.point == it->second.point && r.side == it->second.side) r.type = next;
    s.sidesDirty = true;
    s.indexDirty = true;
    log().info("signals: signal at {} switched to {}", pos.key(), next == 1 ? "cross_signal" : "entry_signal");
    return true;
}

cs::SignalState lampOf(BlockRec const& rec) {
    if (rec.point.empty()) return cs::SignalState::Invalid;
    return state().system.state(rec.point, rec.side);
}

cs::SignalState shownOf(BlockRec const& rec) { return rec.shown; }

bool poweredAt(BlockSource& region, BlockPos const& pos) {
    // Create's hasNeighborSignal, read from the circuit graph by the shared redstone input (track
    // observers are registered there as signal sources, see SignalsModule::install)
    return common::redstone::powered(region, pos);
}

// ---- tick ---------------------------------------------------------------------------------------

void refresh() {
    auto& s    = state();
    auto& rail = tracks::railways();
    if (rail.version() != s.seenVersion) {
        // blocks whose point disappeared (edge removed or rebuilt) bind again
        for (auto& [pos, r] : s.blocks) {
            if (!r.point.empty() && rail.point(r.point)) continue;
            bool was = !r.point.empty();
            if (bind(r)) log().info("signals: block at {} re-bound to {}", pos.key(), r.point);
            else if (was) log().info("signals: block at {} lost its track", pos.key());
            s.indexDirty = true;
        }
        // points nobody uses any more
        std::set<std::string> used;
        for (auto const& [pos, r] : s.blocks)
            if (!r.point.empty()) used.insert(r.point);
        std::vector<std::string> stale;
        for (auto const* p : rail.points(std::string(api::kSignalPointType)))
            if (!used.contains(p->id)) stale.push_back(p->id);
        for (auto const* p : rail.points(std::string(api::kObserverPointType)))
            if (!used.contains(p->id)) stale.push_back(p->id);
        for (auto const& id : stale) rail.removePoint(id);
        rebuildSystem();
        s.seenVersion = rail.version();
        s.sidesDirty  = false;
    } else if (s.sidesDirty) {
        applySides();
        s.sidesDirty = false;
    }
}

void runTick() {
    auto& s = state();
    ++s.tick;
    auto tasks = std::move(s.tasks);
    s.tasks.clear();
    for (auto& fn : tasks) fn();
    if (!s.loaded) return;
    s.intents.clear();
    watchRedstone();
    applyUnpowering();
    refresh();
    tickObservers();
    s.system.updateStates();
    pushBlockStates();
    if (s.tick % 10 == 0) sendOverlay(nullptr);
}

namespace {

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    if (ev.layer() != 0) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    auto& s = state();
    auto before = kindOf(ev.previousBlock());
    auto  after  = kindOf(ev.newBlock());
    if (before == after) return; // our own state updates, or unrelated blocks
    GlobalPos pos = game::globalPos(region, ev.pos());
    if (before) {
        s.tasks.push_back([pos] { removeBlock(pos); });
    }
    if (after) {
        // placed by a player holding a selection (SignalsHooks.cpp records it)
        auto it = s.intents.find(pos);
        if (it == s.intents.end()) return;
        auto [uuid, sel] = it->second;
        s.intents.erase(it);
        Kind kind = *after;
        s.tasks.push_back([pos, kind, uuid, sel] {
            bool ok = addBlock(kind, pos, sel.target);
            if (auto level = ll::service::getLevel()) {
                if (auto* p = level->getPlayer(mce::UUID::fromString(uuid))) {
                    // Create clears the selection of the stack it was placed from
                    if (ok) {
                        if (auto held = selectionOf(p->getSelectedItem()); held && held->item == sel.item) setHeldSelection(*p, std::nullopt);
                    }
                    if (ok) actionbar(*p, "create.track_target.success", "Successfully bound to targeted track", 'a');
                    else actionbar(*p, "create.track_target.invalid", "Cannot target this track here", 'c');
                }
            }
        });
    }
}

} // namespace

class SignalsModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        if (game::db::available() && !state().loaded) loadWorld();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runTick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        net::onCompanionJoined([](Player& p) { sendOverlay(&p); });
        // a signal / observer moved by a contraption binds to the track at the same offset
        common::blockdata::onPut([](GlobalPos const& pos, nlohmann::json const& doc) {
            if (!doc.contains("signals") || !doc["signals"].is_object()) return;
            auto const& d = doc["signals"];
            state().tasks.push_back([pos, d] {
                auto* region = regionOf(pos.dim);
                if (!region || !region->hasChunksAt(game::blockPos(pos), 0, false)) return;
                auto kind = kindOf(region->getBlock(game::blockPos(pos)));
                if (!kind) return;
                auto v = [&](char const* k) {
                    auto a = d.value(k, nlohmann::json::array({0, 0, 0}));
                    return ct::Vec3d{a[0].get<double>(), a[1].get<double>(), a[2].get<double>()};
                };
                Target t;
                t.dim   = pos.dim;
                auto o  = v("o");
                t.track = {pos.x + int(std::lround(o.x)), pos.y + int(std::lround(o.y)), pos.z + int(std::lround(o.z))};
                t.dir   = v("v");
                t.curve = d.value("c", false);
                auto a  = v("a");
                t.at    = {pos.x + a.x, pos.y + a.y, pos.z + a.z};
                bool ok = addBlock(*kind, pos, t);
                log().info("signals: moved {} at {} {}", *kind == Kind::Signal ? "signal" : "observer", pos.key(), ok ? "bound again" : "has no track at its target");
            });
        });
        common::blockdata::onRotate("signals", [](nlohmann::json& d, int axis, int quarters) {
            if (axis != 1 || !d.is_object()) return; // contraptions turn signals about y only
            for (char const* k : {"o", "v", "a"}) {
                if (!d.contains(k) || !d[k].is_array() || d[k].size() < 3) continue;
                double x = d[k][0].get<double>(), z = d[k][2].get<double>();
                for (int q = ((quarters % 4) + 4) % 4; q > 0; --q) {
                    double nx = z, nz = -x; // right-hand rule about +y
                    x = nx;
                    z = nz;
                }
                d[k][0] = x;
                d[k][2] = z;
            }
        });
        // Create's TrackObserverBlockEntity filter (the slot on top of the block)
        common::filterslot::registerSlot(
            std::string(api::kObserverId),
            {.id     = "filter",
             .centre = [](Block const&, int face) -> std::optional<core::common::Vec> {
                 if (face != 1) return std::nullopt;
                 return core::common::Vec{0.5, 1.0, 0.5};
             },
             .scale     = 0.5,
             .onChanged = [](GlobalPos const& pos) { log().info("signals: track observer filter at {} changed", pos.key()); }}
        );
        // Create's ObservedTrainNameSource: the name of the train the observer sees
        display::registerSource(
            std::string(api::kObserverId),
            display::Source{
                .id           = "observed_train_name",
                .name         = "create.display_source.observed_train_name",
                .nameFallback = "Detected Train Name",
                .text         = [](display::Context& ctx, display::TargetStats const&) -> display::Lines {
                    std::string train = api::observedTrain(ctx.source);
                    if (train.empty()) return {""};
                    auto const& names = state().trainName;
                    std::string name  = names ? names(train) : std::string();
                    // SingleLineDisplaySource with allowsLabeling: "<label> <name>"
                    auto label = ctx.config.value("Label", std::string());
                    return {label.empty() ? name : label + " " + name};
                },
                .options      = {{"Label", "create.display_source.label", "Attached Label", {}, {}, ""}},
                .refreshTicks = 400, // Create's ObservedTrainNameSource.getPassiveRefreshTicks
            }
        );
        installHooks();
        // Create's TrackObserverBlock.getSignal: 15 to every side while powered
        common::redstone::registerEmitter(std::string{api::kObserverId}, [](BlockSource&, BlockPos const&, Block const& b, int) {
            return game::intState(b, api::kPoweredState).value_or(0) ? 15 : 0;
        });
        // Create's SignalBlock.getAnalogOutputSignal: 15 while red
        common::redstone::registerAnalog(std::string{api::kSignalId}, [](BlockSource& region, BlockPos const& pos, Block const&) -> std::optional<int> {
            auto const& blocks = state().blocks;
            auto        it     = blocks.find(game::globalPos(region, pos));
            if (it == blocks.end() || it->second.kind != Kind::Signal) return 0;
            return shownOf(it->second) == cs::SignalState::Red ? 15 : 0;
        });
    }

    void uninstall() {
        uninstallHooks();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        saveWorld();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

namespace {
struct WorldHooks {
    WorldHooks() {
        game::db::onWorldReady([] { loadWorld(); });
        game::db::onWorldSave([] { saveWorld(); });
        game::db::onWorldClosed([] {
            auto& s  = state();
            s.loaded = false;
            s.blocks.clear();
            s.watches.clear();

            s.observers.clear();
                    s.intents.clear();
            s.tasks.clear();
            s.system = cs::SignalSystem{};
        });
    }
} const gWorldHooks;
} // namespace

} // namespace lv::feature::signals

LV_REGISTER_MODULE(lv::feature::signals::SignalsModule);

// ---- contract (feature/tracks/SignalsExt.h) -----------------------------------------------------

namespace lv::feature::tracks::signals {

namespace sg = lv::feature::signals;

void setTrainSpans(std::string const& trainId, std::vector<Span> const& spans) {
    sg::refresh();
    sg::state().system.setTrainSpans(trainId, spans);
}

bool setTrainExtent(
    std::string const&                 trainId,
    core::tracks::GraphLocation const& trailing,
    core::tracks::GraphLocation const& leading,
    double                             maxLength
) {
    auto& rail = railways();
    auto* g    = rail.graph(trailing.graph);
    if (!g || leading.graph != trailing.graph) return false;
    auto nodeLoc = [&](core::tracks::NodeId id) { return g->node(id)->location; };
    if (!g->node(trailing.node1) || !g->node(trailing.node2) || !g->node(leading.node1) || !g->node(leading.node2)) return false;
    std::vector<Span> spans;
    if (trailing.node1 == leading.node1 && trailing.node2 == leading.node2 && trailing.position <= leading.position + 1e-9) {
        spans.push_back({nodeLoc(trailing.node1), nodeLoc(trailing.node2), trailing.position, leading.position});
    } else {
        core::tracks::PathQuery q;
        q.start        = trailing;
        q.destinations = {leading};
        q.maxDistance  = maxLength;
        auto path      = rail.findPath(q);
        if (!path) return false;
        auto const* e0 = g->edge(trailing.node1, trailing.node2);
        if (!e0) return false;
        spans.push_back({nodeLoc(trailing.node1), nodeLoc(trailing.node2), trailing.position, g->edgeLength(*e0)});
        for (size_t i = 0; i < path->edges.size(); ++i) {
            auto const& st = path->edges[i];
            auto const* e  = g->edge(st.node1, st.node2);
            if (!e) return false;
            bool last = i + 1 == path->edges.size();
            spans.push_back({nodeLoc(st.node1), nodeLoc(st.node2), 0.0, last ? leading.position : g->edgeLength(*e)});
        }
    }
    setTrainSpans(trainId, spans);
    return true;
}

void removeTrain(std::string const& trainId) { sg::state().system.removeTrain(trainId); }

Authority checkAhead(
    std::string const&                         trainId,
    core::tracks::GraphLocation const&         front,
    std::vector<core::tracks::PathStep> const& path,
    double                                     lookahead
) {
    sg::refresh();
    return sg::state().system.checkAhead(railways(), trainId, front, path, lookahead);
}

void releaseReservations(std::string const& trainId) { sg::state().system.releaseReservations(trainId); }

std::vector<Crossing> signalsAhead(
    core::tracks::GraphLocation const&         front,
    std::vector<core::tracks::PathStep> const& path,
    double                                     maxDistance
) {
    sg::refresh();
    return sg::state().system.boundariesAlong(railways(), front, path, maxDistance);
}

double pathPenalty(std::string const& trainId, core::tracks::Graph const& graph, core::tracks::Edge const& edge) {
    auto const* a = graph.node(edge.node1);
    auto const* b = graph.node(edge.node2);
    if (!a || !b) return 0;
    return sg::state().system.edgePenalty(a->location, b->location, trainId, kRedSignalPenalty, kRedstoneRedSignalPenalty);
}

SignalType boundaryType(std::string const& boundaryId, int side) {
    auto const* b = sg::state().system.boundary(boundaryId);
    return b && side >= 0 && side < 2 ? b->sides[size_t(side)].type : SignalType::Entry;
}

core::tracks::EdgeCost
pathCost(std::string const& trainId, core::tracks::Graph const& graph, core::tracks::Edge const& edge, double& redWeight) {
    auto const* a = graph.node(edge.node1);
    auto const* b = graph.node(edge.node2);
    if (!a || !b) return {};
    return sg::state().system.edgeCost(a->location, b->location, trainId, redWeight, kRedstoneRedSignalPenalty);
}

GroupId sectionAt(core::tracks::GraphLocation const& loc) {
    sg::refresh();
    return sg::state().system.groupAt(railways(), loc);
}

std::vector<std::string> trainsIn(GroupId section) { return sg::state().system.trainsIn(section); }

SignalState signalState(core::GlobalPos const& pos) {
    auto& s  = sg::state();
    auto  it = s.blocks.find(pos);
    if (it == s.blocks.end() || it->second.kind != sg::Kind::Signal) return SignalState::Invalid;
    return sg::shownOf(it->second);
}

bool observerPowered(core::GlobalPos const& pos) {
    auto& s  = sg::state();
    auto  it = s.blocks.find(pos);
    if (it == s.blocks.end() || it->second.kind != sg::Kind::Observer) return false;
    auto o = s.observers.find(it->second.point);
    return o != s.observers.end() && o->second.activated > 0;
}

void setCargoProvider(CargoProvider provider) { sg::state().cargo = std::move(provider); }

void setTrainNameProvider(NameProvider provider) { sg::state().trainName = std::move(provider); }

std::string observedTrain(core::GlobalPos const& pos) {
    auto& s  = sg::state();
    auto  it = s.blocks.find(pos);
    if (it == s.blocks.end() || it->second.kind != sg::Kind::Observer) return {};
    auto o = s.observers.find(it->second.point);
    return o == s.observers.end() || o->second.activated <= 0 ? std::string() : o->second.train;
}

} // namespace lv::feature::tracks::signals
