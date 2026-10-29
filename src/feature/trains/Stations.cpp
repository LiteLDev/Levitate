// Trains unit: train stations (edge points on the track graph), the assembly area with its
// bogeys, train assembly / disassembly and the station screen (native + form).

#include <algorithm>
#include <cmath>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "core/trains/Assembly.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/contraption/Contraption.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/tracks/Tracks.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::trains {

namespace items   = common::items;
namespace screens = common::screens;
namespace tk      = core::tracks;

namespace {

GlobalPos offsetOf(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }

/// The first `n` characters (code points) of UTF-8 text (Create limits names in characters).
std::string utf8Prefix(std::string const& s, size_t n) {
    size_t i = 0, count = 0;
    while (i < s.size() && count < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t        len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > s.size()) break;
        i += len;
        ++count;
    }
    return s.substr(0, i);
}
GlobalPos posOf(Json const& j) {
    if (!j.is_array() || j.size() < 4) return {};
    return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

/// The track block of assembly index `i` (one block past the station's track, along the
/// assembly direction).
GlobalPos assemblyTrack(Station const& s, int i) { return offsetOf(s.track, -s.fx * (i + 1), 0, -s.fz * (i + 1)); }

int trackAxisOf(Station const& s) { return s.fx != 0 ? 0 : 2; }

bool trackOk(Station const& s) { return straightTrackAxis(s.track) == trackAxisOf(s); }

std::string errorText(Player* p, Json const& e) {
    if (!e.is_object()) return {};
    std::string key = e.value("key", std::string()), fb = e.value("fallback", std::string());
    auto        args = e.value("args", std::vector<std::string>{});
    std::string text(p ? translate(key, fb, localeOf(*p)) : translate(key, fb));
    return core::formatMessage(text, args);
}

void setError(Station& s, ct::AssemblyError const& e) {
    s.lastError = {{"key", e.key}, {"fallback", e.fallback}, {"args", e.args}, {"carriage", e.carriage}};
    log().info("trains: assembly failed at station '{}': {}", s.name, errorText(nullptr, s.lastError));
    markStationsDirty();
}

} // namespace

// ---- station record -----------------------------------------------------------------------------

std::string Station::pointId() const { return "station:" + pos.key(); }

Json Station::toJson() const {
    return {
        {"pos",        posJson(pos)        },
        {"track",      posJson(track)      },
        {"f",          Json::array({fx, fz})},
        {"name",       name                },
        {"door",       int(door)           },
        {"assembling", assembling          },
        {"auto",       autoSchedule        },
        {"last",       lastTrainName       },
        {"error",      lastError           }
    };
}

Station Station::fromJson(Json const& j) {
    Station s;
    s.pos   = posOf(j.at("pos"));
    s.track = posOf(j.at("track"));
    auto f  = j.value("f", Json::array({1, 0}));
    s.fx    = f[0].get<int>();
    s.fz    = f[1].get<int>();
    s.name  = j.value("name", std::string("Track Station"));
    s.door  = DoorMode(std::clamp(j.value("door", 0), 0, 5));
    s.assembling    = j.value("assembling", false);
    s.autoSchedule  = j.value("auto", Json());
    s.lastTrainName = j.value("last", std::string());
    s.lastError     = j.value("error", Json());
    return s;
}

std::optional<ct::TrackPos> Station::point() const {
    auto const* p = tracks::railways().point(pointId());
    if (!p) return std::nullopt;
    return ct::TrackPos{p->node1, p->node2, p->position};
}

Station* stationAt(GlobalPos const& pos) { return stationByKey(pos.key()); }

Station* stationByKey(std::string const& key) {
    auto& m  = state().stations;
    auto  it = m.find(key);
    return it == m.end() ? nullptr : &it->second;
}

namespace {
/// Adds the station's edge point when it is missing or moved. Returns false if the track isn't
/// on the graph.
bool addPoint(Station const& s) {
    auto loc = tracks::targetAt(s.track, Vec3{float(s.fx), 0, float(s.fz)});
    if (!loc) return false;
    auto& rail = tracks::railways();
    auto* g    = rail.graph(loc->graph);
    if (!g) return false;
    auto const* n1 = g->node(loc->node1);
    auto const* n2 = g->node(loc->node2);
    if (!n1 || !n2) return false;
    if (auto const* existing = rail.point(s.pointId())) {
        if (existing->node1 == n1->location && existing->node2 == n2->location && std::abs(existing->position - loc->position) < 1e-6
            && existing->data.value("name", std::string()) == s.name)
            return true;
    }
    tk::EdgePoint p;
    p.id          = s.pointId();
    p.type        = "levitate:station";
    p.node1       = n1->location;
    p.node2       = n2->location;
    p.position    = loc->position;
    p.facingNode2 = true;
    p.data        = {{"name", s.name}, {"pos", posJson(s.pos)}};
    return rail.addPoint(std::move(p));
}
} // namespace

bool createStation(GlobalPos const& pos, GlobalPos const& track, int fx, int fz, std::string* errorKey) {
    auto* region = regionOf(track.dim);
    if (!region) return false;
    auto shape = tracks::shapeAt(*region, game::blockPos(track));
    if (!shape) {
        if (errorKey) *errorKey = "create.track_target.invalid";
        return false;
    }
    if (tk::isJunction(*shape)) {
        if (errorKey) *errorKey = "create.track_target.no_junctions";
        return false;
    }
    Station s;
    s.pos   = pos;
    s.track = track;
    s.fx    = fx;
    s.fz    = fz;
    if (!addPoint(s)) {
        if (errorKey) *errorKey = "create.track_target.invalid";
        return false;
    }
    state().stations[pos.key()] = s;
    markStationsDirty();
    log().info("trains: station placed at {} {} {} on the track at {} {} {}", pos.x, pos.y, pos.z, track.x, track.y, track.z);
    return true;
}

void removeStation(GlobalPos const& pos) {
    auto& m  = state().stations;
    auto  it = m.find(pos.key());
    if (it == m.end()) return;
    tracks::railways().removePoint(it->second.pointId());
    for (auto& [id, t] : state().trains) {
        if (t.nav && t.nav->stationPos == pos.key()) cancelNavigation(t);
        if (t.currentStation == pos.key()) t.currentStation.clear();
    }
    log().info("trains: station '{}' removed", it->second.name);
    m.erase(it);
    markStationsDirty();
}

void refreshStationPoints() {
    for (auto& [k, s] : state().stations) addPoint(s);
}

Train* trainAt(Station const& s) {
    for (auto& [id, t] : state().trains)
        if (!t.invalid && t.currentStation == s.pos.key()) return &t;
    return nullptr;
}

// ---- assembly area ------------------------------------------------------------------------------

std::map<int, int> bogeysOf(Station const& s, int* trackLength) {
    std::map<int, int> out;
    int                axis = trackAxisOf(s);
    int                len  = 0;
    for (int i = 0; i < config().maxAssemblyLength; ++i) {
        auto t = assemblyTrack(s, i);
        if (straightTrackAxis(t) != axis) break;
        ++len;
        auto above = offsetOf(t, 0, 1, 0);
        auto id    = idAt(above);
        if (!isBogey(id)) continue;
        int baxis = intStateAt(above, kBogeyAxisState).value_or(0) == 0 ? 0 : 2;
        if (baxis != axis) continue;
        out[i] = id == kLargeBogey ? 1 : 0;
    }
    if (trackLength) *trackLength = len;
    return out;
}

std::optional<int> assemblyIndex(Station const& s, GlobalPos const& track) {
    if (!s.assembling || track.dim != s.track.dim || track.y != s.track.y) return std::nullopt;
    int dx = track.x - s.track.x, dz = track.z - s.track.z;
    int i  = -(dx * s.fx + dz * s.fz) - 1; // along the assembly direction
    if (i < 0) return std::nullopt;
    if (assemblyTrack(s, i) != track) return std::nullopt;
    int len = 0;
    bogeysOf(s, &len);
    if (i >= len) return std::nullopt;
    return i;
}

std::string useCasingOnTrack(Player* player, GlobalPos const& track, bool hasCasing) {
    Station*           station = nullptr;
    std::optional<int> index;
    for (auto& [k, s] : state().stations) {
        if (auto i = assemblyIndex(s, track)) {
            station = &s;
            index   = i;
            break;
        }
    }
    if (!station) return {};
    auto tr = [&](std::string_view key, std::string_view fb) {
        return player ? trFor(*player, key, fb) : std::string(translate(key, fb));
    };
    if (!hasCasing) return tr("create.train_assembly.requires_casing", "Use Train Casing to create bogeys on tracks");
    auto             bogeys = bogeysOf(*station);
    std::vector<int> existing;
    for (auto const& [i, size] : bogeys) existing.push_back(i);
    // clicking at / next to a bogey cycles its size
    for (auto const& [i, size] : bogeys) {
        if (std::abs(i - *index) > 2) continue;
        auto at = offsetOf(assemblyTrack(*station, i), 0, 1, 0);
        setBlockAt(at, size == 0 ? kLargeBogey : kSmallBogey, {{kBogeyAxisState, trackAxisOf(*station) == 0 ? 0 : 1}});
        log().info("trains: bogey at {} {} {} is now {}", at.x, at.y, at.z, size == 0 ? "large" : "small");
        return tr("create.bogey.style.updated_style_and_size", "Updated style and size");
    }
    if (!ct::canPlaceBogey(*index, existing)) return "!";
    auto  above  = offsetOf(track, 0, 1, 0);
    auto* region = regionOf(track.dim);
    if (!region || !game::replaceable(*region, game::blockPos(above))) {
        // Create breaks the block above (with drops); unbreakable blocks stop it
        auto const& b = region ? &region->getBlock(game::blockPos(above)) : nullptr;
        if (!b || b->getBlockType().getDestroySpeed() < 0) return "!";
        game::clearBlock(*region, game::blockPos(above));
    }
    setBlockAt(above, kSmallBogey, {{kBogeyAxisState, trackAxisOf(*station) == 0 ? 0 : 1}});
    log().info("trains: bogey created at {} {} {} (assembly index {} of station '{}')", above.x, above.y, above.z, *index, station->name);
    return "+" + tr("create.train_assembly.bogey_created", "Bogey created. Click again to cycle type");
}

void setAssembling(Station& s, bool on, Player* by) {
    if (on == s.assembling) return;
    if (on) {
        if (!trackOk(s)) return;
        if (auto* t = trainAt(s)) {
            if (!disassemble(s, by)) return;
            (void)t;
        }
        // trains heading here look for another way
        for (auto& [id, t] : state().trains)
            if (t.nav && t.nav->stationPos == s.pos.key()) cancelNavigation(t);
    }
    s.assembling = on;
    s.lastError  = nullptr;
    setBlockAt(s.pos, kStation, {{kAssemblingState, on ? 1 : 0}});
    markStationsDirty();
    log().info("trains: station '{}' {} assembly mode", s.name, on ? "entered" : "left");
}

std::optional<std::uint64_t> assemble(Station& s, Player* by) {
    auto fail = [&](ct::AssemblyError const& e) -> std::optional<std::uint64_t> {
        setError(s, e);
        return std::nullopt;
    };
    if (!trackOk(s)) return fail({"create.station.no_assembly_diagonal", "Cannot build trains", {}});
    auto point = s.point();
    if (!point) return fail({"create.gui.assembly.exception", "This Contraption was unable to assemble:", {}});
    auto bogeys = bogeysOf(s);
    std::vector<int> indices;
    for (auto const& [i, size] : bogeys) indices.push_back(i);
    if (auto e = ct::checkBogeys(indices, config().maxBogeyCount)) return fail(*e);

    int    dx = -s.fx, dz = -s.fz; // assembly direction
    auto   bogeyPos = [&](int k) { return offsetOf(assemblyTrack(s, indices[k]), 0, 1, 0); };
    auto   cpos     = [](GlobalPos const& p) { return cc::Pos{p.x, p.y, p.z}; };
    int    n        = int(indices.size());
    std::vector<cc::AssemblyResult> found(n);
    std::vector<bool>               ran(n, false);
    auto options = [&](int k) {
        contraption::AssembleOptions o;
        auto                         b = bogeyPos(k);
        o.anchor        = b;
        o.start         = {cpos(b), cpos(offsetOf(b, 0, 1, 0))};
        o.kind          = "train";
        o.includeAnchor = true;
        o.pivot         = {0.5, 0, 0.5};
        return o;
    };
    std::optional<ct::AssemblyError> dryError;
    auto structure = [&](int k) -> cc::AssemblyResult const& {
        if (!ran[k]) {
            found[k] = contraption::ext::dryRun(options(k));
            ran[k]   = true;
            if (!found[k].ok() && !dryError) {
                auto e   = contraption::ext::errorOf(found[k]);
                dryError = ct::AssemblyError{e.key, e.fallback, e.args};
            }
        }
        return found[k];
    };
    auto layoutOr = ct::planCarriages(
        indices,
        [&](int k) {
            std::vector<int> held;
            auto const&      r = structure(k);
            for (int j = 0; j < n; ++j) {
                auto b = cpos(bogeyPos(j));
                if (std::any_of(r.blocks.begin(), r.blocks.end(), [&](cc::Pos const& q) { return q == b; })) held.push_back(j);
            }
            return held;
        },
        [&](int k) { return int(structure(k).blocks.size()); }
    );
    if (dryError) return fail(*dryError);
    if (auto const* e = std::get_if<ct::AssemblyError>(&layoutOr)) return fail(*e);
    auto const& layout = std::get<ct::Layout>(layoutOr);

    // what the carriages hold: no tracks or stations; controls along the track
    bool anyForward = false, anyBackward = false;
    std::vector<std::vector<std::pair<cc::Pos, int>>> controlsOf(layout.carriages.size());
    std::vector<std::pair<bool, bool>>                dirsOf(layout.carriages.size());
    for (size_t c = 0; c < layout.carriages.size(); ++c) {
        int  first = layout.carriages[c].bogeys.front();
        auto base  = cpos(bogeyPos(first));
        std::vector<int> facings;
        for (auto const& p : structure(first).blocks) {
            GlobalPos   gp{s.pos.dim, p.x, p.y, p.z};
            std::string id = idAt(gp);
            if (id == tracks::kTrackId || id == tracks::kFakeTrackId || id == kStation) {
                return fail({"create.gui.assembly.exception.unmovableBlock",
                             "Unmovable Block ({3}) at [{0},{1},{2}]",
                             {std::to_string(p.x), std::to_string(p.y), std::to_string(p.z), id},
                             int(c) + 1});
            }
            if (id != kControls) continue;
            int  f   = intStateAt(gp, "levitate:facing").value_or(2);
            auto off = cc::offset(cc::Dir(std::clamp(f, 0, 5)));
            int  rel = off.x == dx && off.z == dz ? 1 : (off.x == -dx && off.z == -dz ? -1 : 0);
            facings.push_back(rel);
            controlsOf[c].push_back({p - base, rel});
        }
        bool fw = false, bw = false;
        if (auto e = ct::checkControls(facings, fw, bw)) {
            auto err     = *e;
            err.carriage = int(c) + 1;
            return fail(err);
        }
        dirsOf[c] = {fw, bw};
        anyForward |= fw;
        anyBackward |= bw;
    }
    if (!anyForward) {
        return fail({"create.train_assembly.no_controls",
                     "Attach at least one forward-facing Train Controls block (Are you missing Super Glue?)",
                     {}});
    }

    // assemble every carriage (undo on failure)
    Train t;
    t.id = state().nextTrainId;
    for (size_t c = 0; c < layout.carriages.size(); ++c) {
        auto const& plan  = layout.carriages[c];
        int         first = plan.bogeys.front();
        auto        r     = contraption::assemble(options(first));
        if (!r.ok()) {
            for (auto const& done : t.carriages) contraption::disassemble(done.contraption);
            return fail({r.errorKey, r.errorText, r.errorArgs, int(c) + 1});
        }
        Carriage car;
        car.contraption = *r.id;
        auto b0         = bogeyPos(first);
        car.origin      = {double(b0.x), double(b0.y), double(b0.z)};
        for (int k : plan.bogeys) {
            car.bogeyOffsets.push_back(1.0 + indices[k]);
            auto b = bogeyPos(k);
            car.bogeyLocal.push_back({b.x - b0.x, b.y - b0.y, b.z - b0.z});
            car.bogeySize.push_back(bogeys[indices[k]]);
        }
        car.controls = controlsOf[c];
        car.forward  = dirsOf[c].first;
        car.backward = dirsOf[c].second;
        if (auto* con = contraption::find(car.contraption)) con->anchorData() = {{"train", t.id}, {"carriage", c}};
        t.carriages.push_back(std::move(car));
    }
    ++state().nextTrainId;
    t.name        = s.lastTrainName.empty() ? std::string(translate("create.train.unnamed", "Unnamed Train")) : s.lastTrainName;
    t.owner       = by ? uuidOf(*by) : std::string();
    t.dim         = s.pos.dim;
    t.ax          = dx;
    t.az          = dz;
    t.doubleEnded = anyBackward;
    t.body.front  = *point;
    std::vector<double> offs{0};
    for (auto const& car : t.carriages)
        for (double o : car.bogeyOffsets) offs.insert(offs.end(), {o - 1, o, o + 1});
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), offs.end());
    t.body.offsets = offs;
    auto pos       = ct::positionOf(tracks::railways(), *point);
    t.lift         = pos ? t.carriages.front().origin.y - pos->y : 1;
    t.currentStation = s.pos.key();
    auto [it, ok]    = state().trains.emplace(t.id, std::move(t));
    Train& train     = it->second;
    s.lastError      = nullptr;
    s.assembling     = false;
    setBlockAt(s.pos, kStation, {{kAssemblingState, 0}});
    markStationsDirty();
    placeCarriages(train);
    log().info(
        "trains: assembled train #{} '{}' with {} carriages ({} bogeys) at station '{}'",
        train.id,
        train.name,
        train.carriages.size(),
        n,
        s.name
    );
    if (by) playSound(s.pos.dim, {s.pos.x + .5, s.pos.y + .5, s.pos.z + .5}, "random.anvil_use", 0.5f, 1.5f);
    saveAll();
    return train.id;
}

bool canDisassemble(Station const& s) {
    auto* t = trainAt(s);
    if (!t || t->disassembling || std::abs(t->speed) > 1e-6 || t->derailed) return false;
    if (!trackOk(s)) return false;
    // the front must stand at the station marker (a train that reversed in has its back there)
    auto p = s.point();
    auto a = p ? ct::positionOf(tracks::railways(), *p) : std::nullopt;
    auto f = ct::positionOf(tracks::railways(), t->body.front);
    if (!a || !f || a->distanceTo(*f) > 0.75) return false;
    for (auto const& c : t->carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con) return false;
        double a = std::fmod(std::abs(con->transform().angle), 90.0);
        if (std::min(a, 90 - a) > 1.0) return false;
    }
    return true;
}

bool disassemble(Station& s, Player* by) {
    if (!canDisassemble(s)) return false;
    Train& t        = *trainAt(s);
    t.disassembling = true;
    if (auto p = s.point()) t.body.front = *p; // exactly at the station marker
    placeCarriages(t);
    auto id         = t.id;
    auto stationKey = s.pos.key();
    std::string byUuid = by ? uuidOf(*by) : std::string();
    // the carriages take the aligned transform on their next tick; place them after that
    defer([id, stationKey, byUuid] {
        defer([id, stationKey, byUuid] {
            auto* t = findTrain(id);
            auto* s = stationByKey(stationKey);
            if (!t) return;
            if (t->runtime.hasSchedule && !t->runtime.isAutoSchedule) {
                auto item = makeScheduleItem(t->runtime.discard());
                if (auto* p = playerByUuid(byUuid)) {
                    if (!p->add(item)) p->drop(item, false);
                    p->sendInventory(false);
                }
            }
            for (auto const& c : t->carriages) contraption::disassemble(c.contraption);
            if (s) {
                s->lastTrainName = t->name;
                markStationsDirty();
            }
            log().info("trains: disassembled train #{} '{}' at station '{}'", t->id, t->name, s ? s->name : std::string("?"));
            t->carriages.clear();
            removeTrain(id);
        });
    });
    return true;
}

std::vector<std::pair<std::string, int>> arrivals(Station const& s) {
    std::vector<std::pair<std::string, int>> out;
    for (auto const& [id, t] : state().trains) {
        if (t.invalid) continue;
        if (t.currentStation == s.pos.key()) out.push_back({t.name, 0});
        else if (t.nav && t.nav->stationPos == s.pos.key())
            out.push_back({t.name, ct::predictTransitTicks(t.nav->distance, t.stats(), t.throttle)});
    }
    std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
    });
    return out;
}

// ---- station screen -----------------------------------------------------------------------------

namespace {

Station* screenStation(Player& p, Json const& ctx) {
    auto pos = posOf(ctx.value("pos", Json()));
    auto* s  = stationAt(pos);
    if (!s || int(p.getDimensionId()) != pos.dim) return nullptr;
    auto feet = p.getFeetPos();
    double dx = feet.x - (pos.x + .5), dy = feet.y - (pos.y + .5), dz = feet.z - (pos.z + .5);
    if (dx * dx + dy * dy + dz * dz > 16 * 16) return nullptr;
    return s;
}

std::string etaString(Player& p, int ticks) {
    auto e = ct::etaOf(ticks);
    switch (e.kind) {
    case ct::EtaText::Kind::Now:
        return trFor(p, "create.display_source.station_summary.now", "now");
    case ct::EtaText::Kind::Minutes:
        return std::to_string(e.value) + trFor(p, "create.display_source.station_summary.minutes", " min");
    case ct::EtaText::Kind::Seconds:
        return trFor(p, "create.display_source.station_summary.seconds", "{0}s", e.value);
    default:
        return " ~ ";
    }
}

Json stationData(Player& p, Station& s) {
    int  len    = 0;
    auto bogeys = bogeysOf(s, &len);
    Json d;
    d["name"]       = s.name;
    d["assembling"] = s.assembling;
    d["trackOk"]    = trackOk(s);
    d["diagonal"]   = straightTrackAxis(s.track) < 0;
    d["bogeys"]     = bogeys.size();
    d["trackLength"] = len;
    d["door"]       = int(s.door);
    d["auto"]       = !s.autoSchedule.is_null();
    d["error"]      = s.lastError.is_object() ? Json{{"text", errorText(&p, s.lastError)}, {"carriage", s.lastError.value("carriage", 0)}} : Json();
    if (auto* t = trainAt(s)) {
        d["train"] = {
            {"id",             t->id                                   },
            {"name",           t->name                                 },
            {"canDisassemble", canDisassemble(s)                       },
            {"schedule",       t->runtime.hasSchedule                  },
            {"autoSchedule",   t->runtime.isAutoSchedule               },
            {"carriages",      t->carriages.size()                     }
        };
    } else {
        // Create: a train navigating here within 30 blocks is "imminent"
        for (auto& [id, t] : state().trains)
            if (t.nav && t.nav->stationPos == s.pos.key() && t.nav->distance < 30)
                d["incoming"] = {{"id", t.id}, {"name", t.name}, {"distance", t.nav->distance}};
    }
    Json arr = Json::array();
    for (auto const& [name, ticks] : arrivals(s)) arr.push_back({{"name", name}, {"eta", etaString(p, ticks)}});
    d["arrivals"] = arr;
    return d;
}

void renameStationForm(Player& player, GlobalPos pos);
void renameTrainForm(Player& player, GlobalPos pos);

void stationAction(Player& p, Json const& ctx, Json const& action) {
    auto* s = screenStation(p, ctx);
    if (!s) return;
    std::string op  = action.value("op", std::string());
    GlobalPos   pos = s->pos;
    if (op == "rename") {
        if (action.contains("text")) {
            std::string name = utf8Prefix(action.value("text", std::string()), 25);
            if (!name.empty()) {
                s->name = name;
                addPoint(*s);
                markStationsDirty();
                log().info("trains: station renamed to '{}'", name);
            }
        } else {
            renameStationForm(p, pos);
        }
    } else if (op == "create") {
        setAssembling(*s, true, &p);
    } else if (op == "cancel") {
        setAssembling(*s, false, &p);
    } else if (op == "assemble") {
        if (s->assembling) assemble(*s, &p);
    } else if (op == "disassemble") {
        disassemble(*s, &p);
    } else if (op == "door") {
        s->door = DoorMode(std::clamp(action.value("mode", 0), 0, 5));
        markStationsDirty();
    } else if (op == "train_name") {
        if (auto* t = trainAt(*s)) {
            if (action.contains("text")) {
                std::string name = utf8Prefix(action.value("text", std::string()), 35);
                if (!name.empty()) {
                    log().info("trains: train #{} renamed to '{}'", t->id, name);
                    t->name = name;
                }
            } else {
                renameTrainForm(p, pos);
            }
        }
    } else if (op == "remove_schedule") {
        if (auto* t = trainAt(*s); t && t->runtime.hasSchedule) {
            bool autoS = t->runtime.isAutoSchedule;
            auto sch   = t->runtime.discard();
            cancelNavigation(*t);
            if (!autoS) {
                auto item = makeScheduleItem(sch);
                if (!p.add(item)) p.drop(item, false);
                p.sendInventory(false);
            }
            actionBar(p, autoS ? trFor(p, "create.schedule.auto_removed_from_train", "Auto-Schedule discarded")
                               : trFor(p, "create.schedule.removed_from_train", "Schedule retrieved from Train"));
        }
    }
}

std::vector<std::string> doorNames(Player& p) {
    static char const* keys[] = {"all", "north", "east", "south", "west", "none"};
    static char const* fb[]   = {"Open All Doors", "North Side Only", "East Side Only", "South Side Only", "West Side Only", "Keep Doors Closed"};
    std::vector<std::string> out;
    for (int i = 0; i < 6; ++i) out.push_back(trFor(p, std::string("create.contraption.door_control.") + keys[i], fb[i]));
    return out;
}

void renameStationForm(Player& player, GlobalPos pos) {
    auto* s = stationAt(pos);
    if (!s) return;
    ll::form::CustomForm form(s->name);
    form.appendInput("name", trFor(player, "lv.trains.station_name", "Station Name"), "", s->name);
    form.appendDropdown("door", trFor(player, "create.contraption.door_control", "Onboard Door Control"), doorNames(player), size_t(s->door));
    form.sendTo(player, [pos](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        Json ctx{{"pos", posJson(pos)}};
        if (auto it = result->find("name"); it != result->end())
            if (auto const* v = std::get_if<std::string>(&it->second)) stationAction(p, ctx, {{"op", "rename"}, {"text", *v}});
        if (auto it = result->find("door"); it != result->end())
            if (auto const* v = std::get_if<uint64>(&it->second)) stationAction(p, ctx, {{"op", "door"}, {"mode", int(*v)}});
        screens::refresh(p, "trains.station", ctx);
    });
}

void renameTrainForm(Player& player, GlobalPos pos) {
    auto* s = stationAt(pos);
    auto* t = s ? trainAt(*s) : nullptr;
    if (!t) return;
    ll::form::CustomForm form(t->name);
    form.appendInput("name", trFor(player, "lv.trains.train_name", "Train Name"), "", t->name);
    form.sendTo(player, [pos](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        Json ctx{{"pos", posJson(pos)}};
        if (auto it = result->find("name"); it != result->end())
            if (auto const* v = std::get_if<std::string>(&it->second)) stationAction(p, ctx, {{"op", "train_name"}, {"text", *v}});
        screens::refresh(p, "trains.station", ctx);
    });
}

void stationForm(Player& p, Json const& ctx) {
    auto* s = screenStation(p, ctx);
    if (!s) return;
    auto        d   = stationData(p, *s);
    GlobalPos   pos = s->pos;
    std::string content;
    std::vector<std::pair<std::string, Json>> buttons;
    if (s->assembling) {
        size_t n = d["bogeys"].get<size_t>();
        content  = n == 0   ? trFor(p, "create.station.no_bogeys", "No Bogeys")
                 : n == 1 ? trFor(p, "create.station.one_bogey", "1 Bogey")
                          : trFor(p, "create.station.more_bogeys", "{0} Bogeys", n);
        content += "\n" + trFor(p, "create.station.how_to", "Use Train Casing on highlighted Tracks to create bogeys.");
        content += "\n" + trFor(p, "create.station.how_to_1", "Remove bogeys by breaking the block on top.");
        content += "\n" + trFor(p, "create.station.how_to_2", "Build carriages attached to one or two bogeys each.");
        if (d["error"].is_object()) {
            content += "\n\n" + trFor(p, "create.station.failed", "Assembly Failed");
            if (int c = d["error"].value("carriage", 0)) content += "\n" + trFor(p, "create.station.carriage_number", "Carriage {0}:", c);
            content += "\n" + d["error"].value("text", std::string()) + "\n" + trFor(p, "create.station.retry", "Resolve this and retry");
        }
        buttons.push_back({trFor(p, "create.station.assemble_train", "Assemble Train"), {{"op", "assemble"}}});
        buttons.push_back({trFor(p, "create.station.cancel", "Cancel Assembly"), {{"op", "cancel"}}});
    } else if (d.contains("train")) {
        content = d["train"]["name"].get<std::string>();
        buttons.push_back({trFor(p, "lv.trains.rename_train", "Rename Train"), {{"op", "train_name"}}});
        if (d["train"]["canDisassemble"].get<bool>())
            buttons.push_back({trFor(p, "create.station.disassemble_train", "Disassemble Train"), {{"op", "disassemble"}}});
        else
            content += "\n" + trFor(p, "create.station.train_not_aligned", "Cannot disassemble,") + " "
                     + trFor(p, "create.station.train_not_aligned_1", "not all carriages aligned");
        if (d["train"]["schedule"].get<bool>()) {
            bool a = d["train"]["autoSchedule"].get<bool>();
            buttons.push_back({a ? trFor(p, "create.station.remove_auto_schedule", "Discard Auto-Schedule")
                                 : trFor(p, "create.station.remove_schedule", "Retrieve Schedule"),
                               {{"op", "remove_schedule"}}});
        }
    } else {
        content = trFor(p, "create.station.idle", "Station is Idle");
        if (d["trackOk"].get<bool>())
            buttons.push_back({trFor(p, "create.station.create_train", "Create new Train"), {{"op", "create"}}});
        else
            content += "\n" + trFor(p, "create.station.no_assembly_diagonal", "Cannot build trains") + " "
                     + trFor(p, d["diagonal"].get<bool>() ? "create.station.no_assembly_diagonal_1" : "create.station.no_assembly_curve_1",
                             d["diagonal"].get<bool>() ? "on diagonal tracks" : "on curved tracks");
    }
    for (auto const& a : d["arrivals"]) content += "\n" + a["name"].get<std::string>() + "  " + a["eta"].get<std::string>();
    buttons.push_back({trFor(p, "lv.trains.station_settings", "Name & Door Control"), {{"op", "rename"}}});
    ll::form::SimpleForm form(s->name, content);
    for (auto const& [text, act] : buttons) {
        Json a = act;
        form.appendButton(text, [pos, a](Player& pl) {
            Json c{{"pos", posJson(pos)}};
            stationAction(pl, c, a);
        });
    }
    form.sendTo(p);
}

} // namespace

void openStationScreen(Player& player, GlobalPos const& pos) { screens::open(player, "trains.station", {{"pos", posJson(pos)}}); }

void registerStationScreens() {
    screens::registerType({
        .id     = "trains.station",
        .data   = [](Player& p, Json const& ctx) -> std::optional<Json> {
            auto* s = screenStation(p, ctx);
            if (!s) return std::nullopt;
            return stationData(p, *s);
        },
        .action = [](Player& p, Json const& ctx, Json const& action) { stationAction(p, ctx, action); },
        .form   = [](Player& p, Json const& ctx) { stationForm(p, ctx); },
    });
}

} // namespace lv::feature::trains
