// Trains unit: config, world helpers, persistence, block change watching and the module.

#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "core/tracks/Shape.h"
#include "feature/contraption/Contraption.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/tracks/SignalsExt.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("trains", lv::feature::trains::Config);

namespace lv::feature::trains {

namespace {
constexpr char kStationsKey[] = "lv:trains:stations";
constexpr char kTrainsKey[]   = "lv:trains:trains";
} // namespace

State& state() {
    static State s;
    return s;
}

Config const& config() { return lv::config::get<Config>(); }

ct::SpeedConfig speedConfig() {
    auto const&     c = config();
    ct::SpeedConfig s;
    s.trainTopSpeed               = c.trainTopSpeed;
    s.trainTurningTopSpeed        = c.trainTurningTopSpeed;
    s.trainAcceleration           = c.trainAcceleration;
    s.poweredTrainTopSpeed        = c.poweredTrainTopSpeed;
    s.poweredTrainTurningTopSpeed = c.poweredTrainTurningTopSpeed;
    s.poweredTrainAcceleration    = c.poweredTrainAcceleration;
    s.manualTrainSpeedModifier    = c.manualTrainSpeedModifier;
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

std::string idAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return {};
    return game::blockIdAt(*region, game::blockPos(p));
}

std::optional<int> intStateAt(GlobalPos const& p, char const* stateName) {
    auto* region = regionOf(p.dim);
    if (!region) return std::nullopt;
    auto bp = game::blockPos(p);
    if (!region->hasChunksAt(bp, 0, false)) return std::nullopt;
    return game::intState(region->getBlock(bp), stateName);
}

bool setBlockAt(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states) {
    auto* region = regionOf(p.dim);
    auto const* b = game::blockWith(id, states);
    if (!region || !b) return false;
    return game::setBlock(*region, game::blockPos(p), *b);
}

void actionBar(Player& player, std::string const& text) { contraption::ext::actionBar(player, text); }

void playSound(int dim, ct::Vec3d const& pos, std::string const& sound, float volume, float pitch) {
    contraption::ext::playSound(dim, {pos.x, pos.y, pos.z}, sound, volume, pitch);
}

Player* playerByUuid(std::string const& uuid) {
    if (uuid.empty()) return nullptr;
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    return level->getPlayer(mce::UUID::fromString(uuid));
}

std::string uuidOf(Player const& player) { return player.getUuid().asString(); }

void defer(std::function<void()> fn) { state().deferred.push_back(std::move(fn)); }

bool isBogey(std::string_view id) { return id == kSmallBogey || id == kLargeBogey; }

int straightTrackAxis(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return -1;
    auto shape = tracks::shapeAt(*region, game::blockPos(p));
    if (!shape) return -1;
    if (*shape == core::tracks::Shape::XO) return 0;
    if (*shape == core::tracks::Shape::ZO) return 2;
    return -1;
}

void markStationsDirty() { state().stationsDirty = true; }

// ---- persistence --------------------------------------------------------------------------------

void saveAll() {
    if (!game::db::available()) return;
    auto& s = state();
    Json  st = Json::array();
    for (auto const& [k, station] : s.stations) st.push_back(station.toJson());
    game::db::save(kStationsKey, st);
    Json tr = Json::array();
    for (auto const& [id, t] : s.trains)
        if (!t.invalid) tr.push_back(t.toJson());
    game::db::save(kTrainsKey, {{"next", s.nextTrainId}, {"trains", tr}});
    s.stationsDirty = false;
}

void loadAll() {
    auto& s = state();
    s.stations.clear();
    s.trains.clear();
    s.targets.clear();
    s.pendingStations.clear();
    s.deferred.clear();
    s.nextTrainId = 1;
    if (auto j = game::db::load(kStationsKey); j && j->is_array()) {
        for (auto const& e : *j) {
            try {
                auto st = Station::fromJson(e);
                s.stations[st.pos.key()] = st;
            } catch (std::exception const& ex) {
                log().warn("trains: skipped an unreadable station: {}", ex.what());
            }
        }
    }
    if (auto j = game::db::load(kTrainsKey); j && j->is_object()) {
        s.nextTrainId = std::max<std::uint64_t>(1, j->value("next", std::uint64_t{1}));
        for (auto const& e : j->value("trains", Json::array())) {
            try {
                auto t = Train::fromJson(e);
                if (!t.id) continue;
                s.nextTrainId = std::max(s.nextTrainId, t.id + 1);
                s.trains[t.id] = std::move(t);
            } catch (std::exception const& ex) {
                log().warn("trains: skipped an unreadable train: {}", ex.what());
            }
        }
    }
    s.graphVersion = tracks::railways().version();
    refreshStationPoints();
    if (!s.stations.empty() || !s.trains.empty())
        log().info("trains: loaded {} stations, {} trains", s.stations.size(), s.trains.size());
}

// ---- module -------------------------------------------------------------------------------------

namespace {

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    if (ev.layer() != 0) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    std::string prev = ev.previousBlock().getTypeName();
    std::string now  = ev.newBlock().getTypeName();
    if (prev == now) return;
    GlobalPos p = game::globalPos(region, ev.pos());
    if (prev == kStation) {
        defer([p] {
            if (idAt(p) != kStation) removeStation(p);
        });
    }
    if (now == kStation) {
        auto& pend = state().pendingStations;
        if (auto it = pend.find(p); it != pend.end()) {
            auto target = it->second.first;
            auto who    = it->second.second;
            pend.erase(it);
            defer([p, target, who] {
                if (idAt(p) != kStation) return;
                std::string err;
                bool        ok = createStation(p, target.track, target.fx, target.fz, &err);
                if (auto* pl = playerByUuid(who)) {
                    actionBar(*pl, ok ? trFor(*pl, "create.track_target.success", "Successfully bound to targeted track")
                                      : trFor(*pl, err, "Cannot target this track here"));
                }
                if (!ok) {
                    // Create: an unbound station can't exist, the block pops
                    if (auto* region = regionOf(p.dim)) game::clearBlock(*region, game::blockPos(p));
                }
            });
        }
    }
}

void runTick() {
    auto& s = state();
    ++s.tick;
    auto tasks = std::move(s.deferred);
    s.deferred.clear();
    for (auto& fn : tasks) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().error("trains: deferred task failed: {}", e.what());
        }
    }
    s.pendingStations.clear();
    if (tracks::railways().version() != s.graphVersion) {
        s.graphVersion = tracks::railways().version();
        onGraphChanged();
        s.graphVersion = tracks::railways().version(); // re-adding points bumps it
    }
    tickTrains();
    tickControls();
    // stations change rarely: write them soon after a change (trains are saved with the world)
    if (s.stationsDirty && s.tick % 100 == 0) saveAll();
}

} // namespace

class TrainsModule {
public:
    void install() {
        registerStationScreens();
        registerScheduleScreens();
        // carriage blocks: bogeys hang on the block above (Create: their top is sticky)
        contraption::MovementRules bogeyRules;
        bogeyRules.attachedTowards = [](cc::States const&) { return cc::bit(cc::Dir::Up); };
        contraption::registerMovementRules(std::string(kSmallBogey), bogeyRules);
        contraption::registerMovementRules(std::string(kLargeBogey), bogeyRules);
        auto axisHandler = [](cc::States& st, cc::Axis axis, int quarter) {
            if (axis != cc::Axis::Y || (quarter & 1) == 0) return;
            auto it = st.find(kBogeyAxisState);
            if (it == st.end()) return;
            if (auto const* v = std::get_if<std::int64_t>(&it->second)) it->second = std::int64_t(*v == 0 ? 1 : 0);
        };
        contraption::registerRotationHandler(std::string(kSmallBogey), axisHandler);
        contraption::registerRotationHandler(std::string(kLargeBogey), axisHandler);

        // track observers (U28): the train's cargo for their filter, its name for display links
        tracks::signals::setCargoProvider([](std::string const& trainId, tracks::signals::ItemTest const& item, tracks::signals::FluidTest const& fluid) {
            auto* t = findTrain(std::strtoull(trainId.c_str(), nullptr, 10));
            if (!t) return false;
            for (auto const& c : t->carriages) {
                auto* con = contraption::find(c.contraption);
                if (!con) continue;
                auto& st = con->storage();
                for (int i = 0; i < st.slotCount(); ++i)
                    if (!st.slot(i).isNull() && item && item(st.slot(i))) return true;
                auto f = st.drain("", 1 << 30, true);
                if (f.second > 0 && fluid && fluid({f.first, f.second})) return true;
            }
            return false;
        });
        tracks::signals::setTrainNameProvider([](std::string const& trainId) -> std::string {
            auto* t = findTrain(std::strtoull(trainId.c_str(), nullptr, 10));
            return t ? t->name : std::string();
        });
        auto& bus = ll::event::EventBus::getInstance();
        if (game::db::available()) loadAll();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { runTick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            onBlockChanged(ev);
        }));
        installHooks();
    }

    void uninstall() {
        uninstallHooks();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        if (game::db::available()) saveAll();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

namespace {
struct WorldHooks {
    WorldHooks() {
        game::db::onWorldReady([] { loadAll(); });
        game::db::onWorldSave([] { saveAll(); });
        game::db::onWorldClosed([] {
            auto& s = state();
            s.stations.clear();
            s.trains.clear();
            s.targets.clear();
            s.pendingStations.clear();
            s.deferred.clear();
            resetControls();
        });
    }
} const gWorldHooks;
} // namespace

} // namespace lv::feature::trains

LV_REGISTER_MODULE(lv::feature::trains::TrainsModule);
