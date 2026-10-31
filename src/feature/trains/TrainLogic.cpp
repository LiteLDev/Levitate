// Trains unit: trains on the graph — movement, navigation, schedules (game side), manual
// controls, conductors, fuel, derailment, collisions and damage (Create's Train / Navigation /
// ControlsHandler / ContraptionCollider), plus persistence of trains.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/ActorHurtResult.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/HurtParameters.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/actor/FurnaceBlockActor.h"
#include "mc/world/phys/AABB.h"

#include "feature/advancements/Advancements.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/display/Display.h"
#include "feature/packages/Packages.h"
#include "feature/redstone/RedstoneLink.h"
#include "feature/tracks/SignalsExt.h"
#include "feature/tracks/Tracks.h"
#include "feature/tracks/TrainsExt.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::trains {

namespace tk  = core::tracks;
namespace sig = tracks::signals;
using ct::Vec3d;

/// The train's id towards U28's signals.
std::string signalId(Train const& t) { return std::to_string(t.id); }

namespace {

Json vecJson(cc::Vec3d const& v) { return Json::array({v.x, v.y, v.z}); }
cc::Vec3d vecOf(Json const& j) {
    if (!j.is_array() || j.size() < 3) return {};
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}
Json posJson(cc::Pos const& p) { return Json::array({p.x, p.y, p.z}); }
cc::Pos posOf(Json const& j) {
    if (!j.is_array() || j.size() < 3) return {};
    return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

tk::Railways& rail() { return tracks::railways(); }

double normAngle(double a) {
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

/// Edge point listeners and the pass check (TrainsExt.h).
struct ExtState {
    std::vector<std::function<void(std::uint64_t, tk::EdgePoint const&)>> entered, left;
};
ExtState& ext() {
    static ExtState e;
    return e;
}

} // namespace

// ---- train record -------------------------------------------------------------------------------

ct::Stats Train::stats() const { return ct::statsOf(speedConfig(), fuelTicks > 0); }

Json Train::toJson() const {
    Json cars = Json::array();
    for (auto const& c : carriages) {
        Json locals = Json::array(), controlsJ = Json::array();
        for (auto const& p : c.bogeyLocal) locals.push_back(posJson(p));
        for (auto const& [p, d] : c.controls) controlsJ.push_back({posJson(p), d});
        cars.push_back({
            {"id",       c.contraption },
            {"offsets",  c.bogeyOffsets},
            {"bogeys",   locals        },
            {"sizes",    c.bogeySize   },
            {"origin",   vecJson(c.origin)},
            {"controls", controlsJ     },
            {"fw",       c.forward     },
            {"bw",       c.backward    }
        });
    }
    Json j{
        {"id",       id         },
        {"name",     name       },
        {"owner",    owner      },
        {"dim",      dim        },
        {"body",     body.toJson()},
        {"carriages", cars      },
        {"a",        Json::array({ax, az})},
        {"lift",     lift       },
        {"speed",    speed      },
        {"throttle", throttle   },
        {"derailed", derailed   },
        {"double",   doubleEnded},
        {"fuel",     fuelTicks  },
        {"stallSpeed", speedBeforeStall ? Json(*speedBeforeStall) : Json()},
        {"runtime",  runtime.toJson()},
        {"station",  currentStation},
        {"idle",     idleTicks  },
        {"lastFront", Json::array({lastFront.x, lastFront.y, lastFront.z})},
        {"lastDir",   Json::array({lastDir.x, lastDir.y, lastDir.z})}
    };
    if (nav) {
        j["nav"] = {
            {"station",  nav->stationPos},
            {"path",     nav->path.toJson()},
            {"distance", nav->distance},
            {"behind",   nav->behind},
            {"manual",   nav->manual},
            {"filter",   nav->filter},
            {"total",    nav->total},
            {"anyOf",    nav->anyOf}
        };
    }
    return j;
}

Train Train::fromJson(Json const& j) {
    Train t;
    t.id    = j.value("id", std::uint64_t{0});
    t.name  = j.value("name", std::string());
    t.owner = j.value("owner", std::string());
    t.dim   = j.value("dim", 0);
    t.body  = ct::Body::fromJson(j.value("body", Json::object()));
    for (auto const& cj : j.value("carriages", Json::array())) {
        Carriage c;
        c.contraption  = cj.value("id", std::uint64_t{0});
        c.bogeyOffsets = cj.value("offsets", std::vector<double>{});
        for (auto const& p : cj.value("bogeys", Json::array())) c.bogeyLocal.push_back(posOf(p));
        c.bogeySize = cj.value("sizes", std::vector<int>{});
        c.origin    = vecOf(cj.value("origin", Json::array()));
        for (auto const& e : cj.value("controls", Json::array()))
            if (e.is_array() && e.size() >= 2) c.controls.push_back({posOf(e[0]), e[1].get<int>()});
        c.forward  = cj.value("fw", false);
        c.backward = cj.value("bw", false);
        t.carriages.push_back(std::move(c));
    }
    auto a           = j.value("a", Json::array({1, 0}));
    t.ax             = a[0].get<int>();
    t.az             = a[1].get<int>();
    t.lift           = j.value("lift", 1.0);
    t.speed          = j.value("speed", 0.0);
    t.throttle       = j.value("throttle", 1.0);
    t.derailed       = j.value("derailed", false);
    t.doubleEnded    = j.value("double", false);
    t.fuelTicks      = j.value("fuel", 0);
    if (auto st = j.find("stallSpeed"); st != j.end() && st->is_number()) t.speedBeforeStall = st->get<double>();
    t.runtime        = ct::Runtime::fromJson(j.value("runtime", Json::object()));
    t.currentStation = j.value("station", std::string());
    t.idleTicks      = j.value("idle", 0);
    if (auto lf = j.value("lastFront", Json::array()); lf.is_array() && lf.size() == 3) t.lastFront = {lf[0], lf[1], lf[2]};
    if (auto ld = j.value("lastDir", Json::array()); ld.is_array() && ld.size() == 3) t.lastDir = {ld[0], ld[1], ld[2]};
    if (j.contains("nav")) {
        auto const& n = j["nav"];
        Navigation  nav;
        nav.stationPos = n.value("station", std::string());
        nav.path       = ct::Path::fromJson(n.value("path", Json::object()));
        nav.distance   = n.value("distance", 0.0);
        nav.behind     = n.value("behind", false);
        nav.manual     = n.value("manual", false);
        nav.filter     = n.value("filter", std::string());
        nav.total      = n.value("total", 0.0);
        nav.anyOf      = n.value("anyOf", std::vector<std::string>{});
        t.nav          = std::move(nav);
    }
    return t;
}

Train* findTrain(std::uint64_t id) {
    auto& m  = state().trains;
    auto  it = m.find(id);
    return it == m.end() || it->second.invalid ? nullptr : &it->second;
}

Train* trainOfContraption(std::uint64_t contraptionId) {
    for (auto& [id, t] : state().trains)
        for (auto const& c : t.carriages)
            if (c.contraption == contraptionId && !t.invalid) return &t;
    return nullptr;
}

// ---- carriages ----------------------------------------------------------------------------------

int dimOf(ct::TrackPos const& p) {
    // a portal edge (length 0) links two dimensions: past its start the point is on the other side
    if (p.from.dim == p.to.dim) return p.from.dim;
    return p.pos > 0 ? p.to.dim : p.from.dim;
}

std::vector<CarriagePlace> carriagePlaces(Train const& t, ct::Body const& body) {
    std::vector<CarriagePlace> out;
    auto&                      r   = rail();
    auto                       pts = body.points(r);
    if (pts.size() != body.offsets.size()) return out;
    auto pointAt = [&](double offset) -> std::optional<ct::TrackPos> {
        for (size_t i = 0; i < body.offsets.size(); ++i)
            if (std::abs(body.offsets[i] - offset) < 1e-6) return pts[i];
        return body.pointAt(r, offset);
    };
    double baseYaw = ct::yawOf({double(t.ax), 0, double(t.az)});
    for (auto const& c : t.carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con || c.bogeyOffsets.empty()) continue;
        double o1 = c.bogeyOffsets.front();
        auto   t1 = pointAt(o1);
        auto   p1 = t1 ? ct::positionOf(r, *t1) : std::nullopt;
        if (!p1) continue;
        int dim = dimOf(*t1);
        // the carriage's direction from points in its own dimension (Create cuts a carriage at a
        // portal; here the carriage is where its first bogey is)
        auto sameDimPos = [&](double offset) -> std::optional<Vec3d> {
            auto tp = pointAt(offset);
            if (!tp || dimOf(*tp) != dim) return std::nullopt;
            return ct::positionOf(r, *tp);
        };
        Vec3d dir;
        if (c.bogeyOffsets.size() > 1)
            if (auto p2 = sameDimPos(c.bogeyOffsets.back())) dir = *p2 - *p1;
        if (dir.x * dir.x + dir.z * dir.z < 1e-8) {
            auto a = sameDimPos(o1 - 1), b = sameDimPos(o1 + 1);
            if (a && b) dir = *b - *a;
            else if (a) dir = *p1 - *a;
            else if (b) dir = *b - *p1;
        }
        if (dir.x * dir.x + dir.z * dir.z < 1e-8 && t1->from.dim == t1->to.dim) dir = t1->from.location() - t1->to.location();
        if (dir.x * dir.x + dir.z * dir.z < 1e-8) continue;
        cc::Transform tr = con->transform();
        tr.axis          = cc::Axis::Y;
        tr.pivot         = {0.5, 0, 0.5};
        tr.angle         = normAngle(ct::yawOf(dir) - baseYaw);
        // Create's carriage pitch from its bogeys: tilt about the local horizontal axis across
        // the carriage (it runs along the assembly direction), before the yaw
        auto [tiltAxis, tilt] = cc::pitchTilt(t.ax, t.az, {dir.x, dir.y, dir.z});
        tr.tiltAxis           = tiltAxis;
        tr.tilt               = tilt;

        cc::Vec3d target{p1->x, p1->y + t.lift, p1->z};
        tr.offset = target - tr.origin - tr.pivot;
        out.push_back({c.contraption, tr, dim});
    }
    return out;
}

std::vector<std::pair<std::uint64_t, cc::Transform>> carriageTransforms(Train const& t, ct::Body const& body) {
    std::vector<std::pair<std::uint64_t, cc::Transform>> out;
    for (auto const& p : carriagePlaces(t, body)) out.emplace_back(p.contraption, p.transform);
    return out;
}

void placeCarriages(Train& t) {
    t.spansDirty = true;
    for (auto const& p : carriagePlaces(t, t.body)) {
        auto* con = contraption::find(p.contraption);
        if (!con) continue;
        if (con->dimension() != p.dim) {
            // Create's DimensionalCarriageEntity: the carriage goes through the portal
            log().info("trains: train #{} '{}' carriage #{} passes into dimension {}", t.id, t.name, p.contraption, p.dim);
            contraption::changeDimension(p.contraption, p.dim, p.transform);
        } else {
            con->setTransform(p.transform);
        }
    }
    t.dim = dimOf(t.body.front); // Create's Train.getPresentDimensions: the leading carriage's
}


// ---- status -------------------------------------------------------------------------------------

void statusMessage(Train& t, std::string const& key, std::vector<std::string> const& args, bool good) {
    if (!good && t.lastStatus == key) return; // Create prints a failure once until it changes
    t.lastStatus = key;
    log().info("trains: train #{} '{}' status: {}", t.id, t.name, key);
    auto* owner = playerByUuid(t.owner);
    if (!owner) return;
    std::string header = trFor(*owner, "create.train.status", "<i> Information about Train: {0}", t.name);
    std::string line   = trFor(*owner, "create.train.status." + key, key);
    line               = core::formatMessage(line, args);
    owner->sendMessage("§6" + header);
    owner->sendMessage(std::string(good ? "§a" : "§c") + " - " + line);
}

std::string statusLine(Train const& t) {
    std::string s = "#" + std::to_string(t.id) + " '" + t.name + "', " + std::to_string(t.carriages.size()) + " carriages";
    if (t.derailed) s += ", derailed";
    if (!t.currentStation.empty()) {
        if (auto* st = stationByKey(t.currentStation)) s += ", waiting at " + st->name;
    } else if (t.nav) {
        auto* st = stationByKey(t.nav->stationPos);
        s += ", travelling to " + (st ? st->name : std::string("?")) + " (" + std::to_string(int(t.nav->distance)) + "m away)";
    }
    if (t.runtime.hasSchedule) {
        static char const* states[] = {"PRE TRANSIT", "IN TRANSIT", "POST TRANSIT"};
        s += ", schedule entry " + std::to_string(t.runtime.currentEntry + 1) + " "
           + (t.runtime.paused ? "Paused" : states[int(t.runtime.state)]);
    } else {
        s += ", idle, no schedule";
    }
    s += ", speed " + std::to_string(int(std::round(std::abs(t.speed) * 20))) + " b/s";
    if (auto p = ct::positionOf(rail(), t.body.front))
        s += ", near [" + std::to_string(int(p->x)) + ", " + std::to_string(int(p->y)) + ", " + std::to_string(int(p->z)) + "]";
    return s;
}

// ---- conductors ---------------------------------------------------------------------------------

std::optional<bool> conductorSeatSide(Train const& t, Carriage const& c, cc::Pos const& seat, bool forward) {
    for (auto const& [local, rel] : c.controls)
        if ((rel > 0) == forward && local + cc::Pos{t.ax * rel, 0, t.az * rel} == seat) return true;
    return std::nullopt;
}

bool isConductorSeat(Train const& t, Carriage const& c, cc::Pos const& seat) {
    return conductorSeatSide(t, c, seat, true) || conductorSeatSide(t, c, seat, false);
}

bool hasConductor(Train const& t, bool forward) {
    if (t.testConductor) return true;
    auto level = ll::service::getLevel();
    for (auto const& c : t.carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con) continue;
        for (auto const& [local, rel] : c.controls) {
            if ((rel > 0) != forward) continue;
            // Create's CarriageContraption.inControl: the cell behind controls facing it
            cc::Pos seat = local + cc::Pos{t.ax * rel, 0, t.az * rel};
            // a blaze burner there (Create: a valid conductor while its heat isn't NONE; U14's
            // blaze burner always has a blaze, the empty burner is another block)
            for (auto const& b : con->blocks())
                if (b.local == seat && b.id == kBlazeBurner) return true;
            // Create's checkConductors: a non-player passenger seated on a seat there
            if (!level) continue;
            for (auto const& [at, uid] : con->seated()) {
                if (at != seat) continue;
                ActorUniqueID id;
                id.rawID = uid;
                auto* a  = level->fetchEntity(id, false);
                if (a && a->isAlive() && !a->isPlayer()) return true;
            }
        }
    }
    return false;
}

Train* conductorTrainOf(Actor const& entity) {
    auto uid = const_cast<Actor&>(entity).getOrCreateUniqueID().rawID;
    for (auto& [id, t] : state().trains) {
        if (t.invalid) continue;
        for (auto const& c : t.carriages) {
            auto* con = contraption::find(c.contraption);
            if (!con) continue;
            for (auto const& [at, seated] : con->seated())
                if (seated == uid && isConductorSeat(t, c, at)) return &t;
        }
    }
    return nullptr;
}

Train* seatedTrainOf(Actor const& entity) {
    auto uid = const_cast<Actor&>(entity).getOrCreateUniqueID().rawID;
    for (auto& [id, t] : state().trains) {
        if (t.invalid) continue;
        for (auto const& c : t.carriages) {
            auto* con = contraption::find(c.contraption);
            if (!con) continue;
            for (auto const& [at, seated] : con->seated())
                if (seated == uid) return &t;
        }
    }
    return nullptr;
}

// ---- navigation ---------------------------------------------------------------------------------

void cancelNavigation(Train& t) {
    if (!t.nav) return;
    sig::releaseReservations(signalId(t));
    t.nav.reset();
    t.targetSpeed = 0;
    t.runtime.transitInterrupted();
}

namespace {

/// Create's Train.getNavigationPenalty: what other trains add (halved) to the edges at their ends.
int navigationPenaltyOf(Train const& t) {
    if (!t.controller.empty()) return 200;                      // MANUAL_TRAIN
    if (!t.runtime.hasSchedule || t.runtime.paused) return 700; // IDLE_TRAIN
    if (t.nav && t.signalWaitTicks > 0) return 50 + std::min(t.signalWaitTicks / 20, 1000); // WAITING_TRAIN
    if (t.nav && (t.nav->distance < 50 || t.distanceToSignal < 20)) return 50;              // ARRIVING_TRAIN
    return 25;                                                  // ANY_TRAIN
}

using EdgePenalties = std::map<std::pair<tk::NodeLocation, tk::NodeLocation>, double>;

/// Create's Navigation.search: penalties of the edges the other trains' ends stand on (both ways).
EdgePenalties otherTrainPenalties(Train const& self) {
    EdgePenalties out;
    for (auto const& [id, other] : state().trains) {
        if (id == self.id || other.invalid || other.disassembling) continue;
        double p     = double(navigationPenaltyOf(other) / 2); // integer division in Create
        auto   endAt = [&](ct::TrackPos const& at) {
            out[{at.from, at.to}] += p;
            out[{at.to, at.from}] += p;
        };
        endAt(other.body.front);
        if (auto back = other.body.pointAt(rail(), other.body.length())) endAt(*back);
    }
    return out;
}

tk::EdgeCost navigationCost(Train const& self, tk::Graph const& g, tk::Edge const& e, double& redWeight, EdgePenalties const& trains) {
    auto const* n1 = g.node(e.node1);
    auto const* n2 = g.node(e.node2);
    if (!n1 || !n2) return {};
    auto cost = sig::pathCost(signalId(self), g, e, redWeight); // signals (U28)
    for (auto const* p : rail().pointsOn(n1->location, n2->location)) {
        if (p->type != "levitate:station") continue;
        auto key = p->data.contains("pos") ? GlobalPos{p->data["pos"][0], p->data["pos"][1], p->data["pos"][2], p->data["pos"][3]}.key()
                                           : std::string();
        auto* st = stationByKey(key);
        if (st && st->assembling) return {1e7}; // can't pass through a station in assembly mode
        auto* present = st ? trainAt(*st) : nullptr;
        if (present && present != &self) cost.penalty += 300; // STATION_WITH_TRAIN
        if (present != &self) cost.penalty += 50;             // STATION
    }
    if (auto it = trains.find({n1->location, n2->location}); it != trains.end()) cost.penalty += it->second;
    return cost;
}

/// Path finding costs for the train (Create's Navigation.findPathTo: one signal weight per search).
std::function<tk::EdgeCost(tk::Graph const&, tk::Edge const&)> pathCostFor(Train const& t) {
    auto weight = std::make_shared<double>(sig::redSignalWeight(t.signalWaitTicks));
    auto trains = std::make_shared<EdgePenalties>(otherTrainPenalties(t));
    return [&t, weight, trains](tk::Graph const& g, tk::Edge const& e) { return navigationCost(t, g, e, *weight, *trains); };
}

} // namespace

/// Whether the station's point is on the train's track graph (which may span dimensions).
bool sameGraph(Train const& t, Station const& s) {
    auto p = s.point();
    if (!p) return s.pos.dim == t.dim;
    auto a = ct::resolve(rail(), *p), b = ct::resolve(rail(), t.body.front);
    if (!a || !b) return s.pos.dim == t.dim;
    return a->graph == b->graph;
}

ct::StartResult navigateTo(
    Train&                          t,
    std::string const&              filter,
    bool                            manual,
    std::string const&              onlyStation,
    std::vector<std::string> const& anyOf,
    int                             approachDir
) {
    std::vector<Station*>     candidates;
    std::vector<ct::TrackPos> dests;
    bool                      anyMatch = false;
    for (auto& [key, s] : state().stations) {
        bool match = !onlyStation.empty() ? key == onlyStation
                   : !anyOf.empty()       ? std::find(anyOf.begin(), anyOf.end(), key) != anyOf.end()
                                          : ct::stationMatches(filter, s.name);
        if (!match || !sameGraph(t, s)) continue; // Create: the stations of the train's graph, any dimension
        anyMatch = true;
        if (s.assembling) continue;
        if (key == t.currentStation && std::abs(t.speed) < 1e-6) {
            if (!manual) return ct::StartResult::AlreadyThere;
            continue;
        }
        auto p = s.point();
        if (!p) continue;
        candidates.push_back(&s);
        dests.push_back(*p);
    }
    if (!anyMatch) return ct::StartResult::NoMatch;
    bool paused     = t.runtime.paused || !t.controller.empty();
    bool canForward = approachDir >= 0 && (manual || paused || hasConductor(t, true));
    bool canBack    = t.doubleEnded && approachDir <= 0 && !(manual && !onlyStation.empty() && approachDir == 0)
                 && (manual || paused || hasConductor(t, false));
    if (!canForward && !canBack) {
        statusMessage(t, "missing_driver", {}, false);
        return ct::StartResult::NoDriver;
    }
    if (dests.empty()) return ct::StartResult::NoPath;
    std::optional<ct::Path> fw, bw;
    if (canForward) fw = ct::findPath(rail(), t.body.front, dests, pathCostFor(t));
    if (canBack) {
        // reversing: the back point arrives, travelling against the station's direction
        std::vector<ct::TrackPos> reversed;
        for (auto const& d : dests) reversed.push_back(ct::reverse(rail(), d));
        if (auto back = t.body.pointAt(rail(), t.body.length())) bw = ct::findPath(rail(), ct::reverse(rail(), *back), reversed, pathCostFor(t));
    }
    if (!fw && !bw) return ct::StartResult::NoPath;
    bool useBack = false;
    if (fw && bw) useBack = manual ? bw->distance < fw->distance : bw->cost < fw->cost;
    else useBack = bw.has_value();
    auto& path = useBack ? *bw : *fw;
    Navigation nav;
    nav.stationPos = candidates[path.destination]->pos.key();
    nav.distance   = path.distance;
    nav.behind     = useBack;
    nav.manual     = manual;
    nav.filter     = filter;
    nav.anyOf      = anyOf;
    nav.total      = path.distance; // Create's distanceStartedAt
    nav.path       = std::move(path);
    t.nav          = std::move(nav);
    log().info(
        "trains: train #{} '{}' heading to station '{}' ({} blocks{})",
        t.id,
        t.name,
        candidates[t.nav->path.destination]->name,
        int(t.nav->distance),
        useBack ? ", reversing" : ""
    );
    return ct::StartResult::Navigating;
}

void arriveAt(Train& t, Station& s) {
    t.speed       = 0;
    t.targetSpeed = 0;
    bool wasNav   = t.nav.has_value();
    t.nav.reset();
    sig::releaseReservations(signalId(t));
    t.currentStation = s.pos.key();
    t.idleTicks      = 0;
    if (wasNav) t.runtime.destinationReached();
    // Create's StationBlockEntity: every newly arrived train gets the station's auto-schedule
    applyAutoSchedule(s);
    log().info("trains: train #{} '{}' arrived at station '{}'", t.id, t.name, s.name);
    display::notifyGatherers(s.pos); // Create: arrivals refresh display links
    if (auto* p = playerByUuid(t.controller)) actionBar(*p, trFor(*p, "create.train.arrived_at", "Arrived at {0}", s.name));
}

// ---- schedule driver ----------------------------------------------------------------------------

namespace {

class TrainDriver final : public ct::Driver {
public:
    explicit TrainDriver(Train& t) : mT(t) {}

    ct::StartResult startDestination(std::string const& filter) override { return navigateTo(mT, filter); }
    ct::StartResult startPackages(bool delivery, std::string const& filter) override {
        // Create's DeliverPackagesInstruction: every station with a postbox for any package in
        // cargo is a candidate (the best path wins); FetchPackagesInstruction: every station whose
        // postboxes hold a package for another address matching `filter`
        std::vector<std::string> targets;
        if (delivery) {
            std::string firstAddress;
            bool        anyPackage = false;
            forEachSlot([&](ItemStack const& s) {
                if (s.isNull() || !packages::isPackage(s)) return;
                auto address = packages::addressOf(s);
                if (!anyPackage) firstAddress = address;
                anyPackage = true;
                for (auto const& [key, st] : state().stations)
                    for (auto const& port : packages::postboxesForStation(st.pos))
                        if (packages::matchAddress(address, port.address)) {
                            targets.push_back(key);
                            break;
                        }
            });
            if (!anyPackage) return ct::StartResult::Skip; // nothing to deliver: skip the entry
            if (targets.empty()) {
                statusMessage(mT, "no_package_target", {firstAddress}, false);
                return ct::StartResult::NoDriver;
            }
        } else {
            for (auto const& [key, st] : state().stations) {
                for (auto const& port : packages::postboxesForStation(st.pos)) {
                    for (auto const& item : packages::portInventory(port.pos)) {
                        if (item.isNull() || !packages::isPackage(item)) continue;
                        auto address = packages::addressOf(item);
                        if (packages::matchAddress(address, port.address)) continue; // delivered here
                        if (filter.empty() || packages::matchAddress(address, filter)) targets.push_back(key);
                    }
                }
            }
            if (targets.empty()) return ct::StartResult::SkipCooldown;
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        auto r = navigateTo(mT, "", false, {}, targets);
        return r == ct::StartResult::NoMatch ? ct::StartResult::NoTarget : r;
    }
    void            rename(std::string const& title) override {
        log().info("trains: train #{} '{}' schedule title is now '{}'", mT.id, mT.name, title);
    }
    void setThrottle(double throttle) override { mT.throttle = throttle; }
    bool navigating() const override { return mT.nav.has_value(); }
    bool derailed() const override { return mT.derailed; }

    std::int64_t dayTime() const override {
        auto level = ll::service::getLevel();
        return level ? level->getTime() : 0;
    }
    long long itemCount(Json const& filter, bool stacks) const override {
        ItemStack f     = common::items::fromJson(filter);
        long long total = 0;
        forEachSlot([&](ItemStack const& s) {
            if (s.isNull()) return;
            if (!common::items::isEmpty(f) && !common::filter::matches(f, s)) return;
            if (stacks) total += s.mCount >= s.getMaxStackSize() ? 1 : 0;
            else total += s.mCount;
        });
        return total;
    }
    long long fluidAmount(Json const& filter) const override {
        ItemStack   f     = common::items::fromJson(filter);
        std::string fluid = common::items::isEmpty(f) ? std::string() : common::fluids::fluidInItem(f).fluid;
        long long   total = 0;
        for (auto const& c : mT.carriages)
            if (auto* con = contraption::find(c.contraption)) total += con->storage().drain(fluid, 1 << 30, true).second;
        return total;
    }
    bool linkPowered(Json const& frequency) const override {
        // Create's RedstoneLinkCondition: anything transmits on the pair (hasAnyLoadedPower)
        if (!frequency.is_array() || frequency.size() < 2) return false;
        auto a = common::items::fromJson(frequency[0]), b = common::items::fromJson(frequency[1]);
        return redstone::link::anyPower(redstone::link::keyOf(a, b));
    }
    int  seatedPlayers() const override {
        // Create's Train.countPlayerPassengers: players riding the carriages
        int  n     = 0;
        auto level = ll::service::getLevel();
        if (!level) return 0;
        for (auto const& c : mT.carriages) {
            auto* con = contraption::find(c.contraption);
            if (!con) continue;
            for (auto const& [local, uid] : con->seated()) {
                ActorUniqueID id;
                id.rawID = uid;
                if (level->getPlayer(id)) ++n;
            }
        }
        return n;
    }
    int  idleTicks() const override { return mT.idleTicks; }
    bool stationPowered() const override {
        auto* s = stationByKey(mT.currentStation);
        return s && contraption::ext::powered(s->pos.dim, {s->pos.x, s->pos.y, s->pos.z});
    }
    bool stationUnloaded() const override {
        auto* s = stationByKey(mT.currentStation);
        if (!s) return false;
        auto* region = regionOf(s->pos.dim);
        return !region || !region->hasChunksAt(game::blockPos(s->pos), 0, false);
    }
    void status(std::string const& key, std::vector<std::string> const& args, bool good) override {
        statusMessage(mT, key, args, good);
    }

private:
    template <class F>
    void forEachSlot(F&& fn) const {
        for (auto const& c : mT.carriages) {
            auto* con = contraption::find(c.contraption);
            if (!con) continue;
            auto& st = con->storage();
            for (int i = 0; i < st.slotCount(); ++i) fn(st.slot(i));
        }
    }
    Train& mT;
};

} // namespace

std::string waitingStatusText(Train& t) {
    auto& r = t.runtime;
    if (!r.hasSchedule || r.paused || r.state != ct::RuntimeState::PostTransit) return {};
    int size = int(r.schedule.entries.size());
    if (r.currentEntry == size - 1 && !r.schedule.cyclic) return {}; // Create: nothing on the last stop
    auto const* e = r.entry();
    if (!e || e->conditions.empty() || r.conditionProgress.empty() || r.conditionContext.empty()) return {};
    auto const& column   = e->conditions[0];
    int         progress = r.conditionProgress[0];
    if (progress >= int(column.size())) return {};
    TrainDriver d(t);
    auto        st = ct::waitingStatus(column[size_t(progress)], r.conditionContext[0], d);
    if (st.key.empty()) return {};
    return core::formatMessage(tr(st.key, st.fallback), st.args);
}

std::vector<ct::Runtime::Prediction> predictionsOf(Train& t) {
    if (t.invalid || t.derailed || !t.runtime.hasSchedule || t.runtime.paused) return {};
    ct::Runtime::PredictionInput in;
    if (auto* s = stationByKey(t.currentStation)) in.currentStation = s->name;
    if (t.nav)
        if (auto* s = stationByKey(t.nav->stationPos)) {
            in.navDestination        = s->name;
            in.distanceToDestination = t.nav->distance;
            in.distanceStartedAt     = t.nav->total > 0 ? t.nav->total : t.nav->path.distance;
        }
    auto st  = t.stats();
    in.speed = std::min(t.throttle * st.maxSpeed, (st.maxSpeed + st.maxTurnSpeed) / 2);
    return t.runtime.predictions(in);
}

namespace {

std::size_t cargoHashOf(Train const& t) {
    std::size_t h = 0;
    for (auto const& c : t.carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con) continue;
        auto& st = con->storage();
        for (int i = 0; i < st.slotCount(); ++i) {
            auto const& s = st.slot(i);
            h = h * 31 + std::hash<std::string>{}(s.isNull() ? std::string() : std::string(s.getTypeName())) + (s.isNull() ? 0 : s.mCount);
        }
        auto fl = con->storage().drain("", 1 << 30, true);
        h       = h * 31 + std::size_t(fl.second);
    }
    return h;
}

/// Create's Train.burnFuel: one burnable item from the carriages' chests / barrels, from the
/// back carriage first when reversing; the item's crafting remainder (a lava bucket's bucket) goes
/// back into the carriage. (Vaults never give fuel in Create; Levitate's carriages mount vanilla
/// containers only.)
void burnFuel(Train& t) {
    if (t.fuelTicks > 0) {
        --t.fuelTicks;
        return;
    }
    bool fromBack = t.speed < 0;
    int  n        = int(t.carriages.size());
    for (int index = 0; index < n; ++index) {
        auto& c   = t.carriages[size_t(fromBack ? n - 1 - index : index)];
        auto* con = contraption::find(c.contraption);
        if (!con) continue;
        auto& st   = con->storage();
        auto  item = st.extractItem([](ItemStack const& s) { return FurnaceBlockActor::getBurnDuration(s, 200) > 0; }, 1, false);
        if (item.isNull()) continue;
        t.fuelTicks += int(FurnaceBlockActor::getBurnDuration(item, 200));
        Item const* remainder = item.mItem ? static_cast<Item const*>(item.mItem->mCraftingRemainingItem) : nullptr;
        std::string rest      = remainder ? remainder->mFullName->getString() : std::string();
        if (!rest.empty() && rest.find(':') == std::string::npos) rest = "minecraft:" + rest;
        if (rest.empty() && item.getTypeName() == "minecraft:lava_bucket") rest = "minecraft:bucket";
        if (!rest.empty())
            if (auto back = common::items::make(rest)) st.insertItem(*back, false);
        return;
    }
}

/// Steering at junctions: -1 right, +1 left, 0 straight.
ct::Chooser steering(int steer) {
    return [steer](ct::TrackPos const& at, std::vector<tk::NodeLocation> const& options) -> std::optional<tk::NodeLocation> {
        if (steer == 0 || options.size() < 2) return std::nullopt;
        Vec3d dir  = (at.to.location() - at.from.location()).normalize();
        double best = -1e9;
        std::optional<tk::NodeLocation> pick;
        for (auto const& o : options) {
            Vec3d  d     = (o.location() - at.to.location()).normalize();
            double cross = dir.z * d.x - dir.x * d.z; // > 0: to the left
            double score = cross * steer;
            if (score > best) {
                best = score;
                pick = o;
            }
        }
        return pick;
    };
}

/// Create's signal scan (U28's SignalsExt.h): the distance to the signal the train must wait
/// at, or -1 when the way within `lookahead` is clear. Keeps the train's signal state.
double distanceToBlocker(Train& t, ct::TrackPos const& lead, ct::Path const& path, double lookahead) {
    t.reserving = false;
    auto loc    = ct::resolve(rail(), lead);
    if (!loc || lookahead <= 0) return -1;
    auto* g = rail().graph(loc->graph);
    if (!g) return -1;
    std::vector<tk::PathStep> steps;
    for (size_t i = path.next; i < path.edges.size(); ++i) {
        auto const* a = g->locateNode(path.edges[i].first);
        auto const* b = g->locateNode(path.edges[i].second);
        if (!a || !b) break;
        steps.push_back({a->id, b->id});
    }
    auto way    = sig::checkAhead(signalId(t), *loc, steps, lookahead);
    t.reserving = !way.reserved.empty();
    if (way.clear) return -1;
    t.waitingAtCross = sig::boundaryType(way.waitingFor, way.waitingSide) == sig::SignalType::Cross;
    return std::max(0.0, way.stopDistance);
}

/// Edge points between the train's ends (for TrainsExt enter / leave callbacks).
std::vector<std::string> occupiedPoints(Train const& t) {
    std::vector<std::string> out;
    std::vector<sig::Span>   spans;
    (void)ct::walk(rail(), ct::reverse(rail(), t.body.front), t.body.length(), ct::follow(t.body.route), nullptr, [&](ct::EdgeSpan const& sp) {
        spans.push_back({sp.from, sp.to, sp.start, sp.end});
        double len = ct::edgeLength(rail(), {sp.from, sp.to, 0});
        for (auto const* p : rail().pointsOn(sp.from, sp.to)) {
            double pos = p->node1 == sp.from ? p->position : len - p->position;
            if (pos >= sp.start - 1e-9 && pos <= sp.end + 1e-9) out.push_back(p->id);
        }
    });
    sig::setTrainSpans(signalId(t), spans); // the sections the train occupies (U28)
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---- advancements (Create's Train / CarriageContraptionEntity / ContraptionCollider awards) -------------

/// Create's Train#backwardsDriver: the player who drove the train backwards (slowly) this tick.
std::map<std::uint64_t, std::string>& backwardsDrivers() {
    static std::map<std::uint64_t, std::string> m;
    return m;
}

Player* playerByActorId(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level || !uid) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

/// The signal section under the train's leading point (in the direction it moves).
sig::GroupId leadSection(Train& t) {
    auto lead = t.speed >= 0 ? std::optional<ct::TrackPos>(t.body.front) : t.body.pointAt(rail(), t.body.length());
    if (!lead) return sig::kNoGroup;
    auto loc = ct::resolve(rail(), *lead);
    return loc ? sig::sectionAt(*loc) : sig::kNoGroup;
}

/// Create's CarriageContraptionEntity#addPassenger / AbstractContraptionEntity#getDismountLocationForPassenger:
/// a player who leaves a train seat 5000 blocks or more from where they sat down gets long_travel.
std::map<std::int64_t, Vec3d>& mountedAtMap() {
    static std::map<std::int64_t, Vec3d> m;
    static bool                           hooked = [] {
        game::db::onWorldClosed([] { m.clear(); }); // actor ids belong to one world
        return true;
    }();
    (void)hooked;
    return m;
}

void trackRiders() {
    auto& mountedAt = mountedAtMap();
    std::set<std::int64_t>              seated;
    for (auto const& [id, t] : state().trains) {
        for (auto const& c : t.carriages) {
            auto* con = contraption::find(c.contraption);
            if (!con) continue;
            for (auto const& [local, uid] : con->seated()) {
                auto* p = playerByActorId(uid);
                if (!p) continue;
                seated.insert(uid);
                auto f = p->getPosition();
                mountedAt.try_emplace(uid, Vec3d{f.x, f.y, f.z});
            }
        }
    }
    for (auto it = mountedAt.begin(); it != mountedAt.end();) {
        if (seated.contains(it->first)) {
            ++it;
            continue;
        }
        if (auto* p = playerByActorId(it->first)) {
            auto f = p->getPosition();
            if ((Vec3d{f.x, f.y, f.z} - it->second).length() >= 5000) advancements::award(*p, "long_travel");
        }
        it = mountedAt.erase(it);
    }
}

void crash(Train& t, Vec3d const& where) {
    // Create's Train#crash: the passengers (not the driver) get train_crash, a backwards driver
    // train_crash_backwards
    if (!t.derailed) {
        for (auto const& c : t.carriages) {
            auto* con = contraption::find(c.contraption);
            if (!con) continue;
            for (auto const& [local, uid] : con->seated())
                if (auto* p = playerByActorId(uid); p && uuidOf(*p) != t.controller) advancements::award(*p, "train_crash");
        }
        if (auto it = backwardsDrivers().find(t.id); it != backwardsDrivers().end())
            if (auto* p = playerByUuid(it->second)) advancements::award(*p, "train_crash_backwards");
    }
    cancelNavigation(t);
    t.speed    = ct::crashBounce(t.speed);
    t.derailed = true;
    statusMessage(t, "collision", {}, false);
    if (auto* owner = playerByUuid(t.owner)) {
        static char const* dims[] = {"overworld", "the_nether", "the_end"};
        owner->sendMessage(
            "§c - "
            + trFor(*owner,
                    "create.train.status.collision.where",
                    "In {0} near [{1}]",
                    t.dim >= 0 && t.dim < 3 ? dims[t.dim] : std::to_string(t.dim),
                    std::to_string(int(where.x)) + "," + std::to_string(int(where.y)) + "," + std::to_string(int(where.z)))
        );
    }
}

void collideWithOtherTrains(Train& t) {
    if (t.derailed || std::abs(t.speed) < 1e-6) return;
    auto lead = t.speed > 0 ? std::optional<ct::TrackPos>(t.body.front) : t.body.pointAt(rail(), t.body.length());
    if (!lead) return;
    auto lp = ct::positionOf(rail(), *lead);
    if (!lp) return;
    for (auto& [id, other] : state().trains) {
        if (id == t.id || other.invalid || other.dim != t.dim || other.disassembling) continue;
        auto pts = other.body.points(rail());
        std::vector<Vec3d> ps;
        for (auto const& p : pts)
            if (auto v = ct::positionOf(rail(), p)) ps.push_back(*v);
        bool hit = false;
        for (size_t i = 0; i + 1 < ps.size() && !hit; ++i) {
            Vec3d  a = ps[i], b = ps[i + 1];
            if (std::abs(lp->y - a.y) > 3 && std::abs(lp->y - b.y) > 3) continue;
            Vec3d  ab{b.x - a.x, 0, b.z - a.z};
            double l2 = ab.lengthSqr();
            double f  = l2 > 0 ? std::clamp(((lp->x - a.x) * ab.x + (lp->z - a.z) * ab.z) / l2, 0.0, 1.0) : 0;
            double dx = a.x + ab.x * f - lp->x, dz = a.z + ab.z * f - lp->z;
            hit       = dx * dx + dz * dz < 0.5 * 0.5;
        }
        if (!hit) continue;
        log().info("trains: train #{} '{}' collided with train #{} '{}'", t.id, t.name, other.id, other.name);
        if (auto power = ct::collisionExplosion(t.speed, other.speed)) {
            auto  level  = ll::service::getLevel();
            auto* region = regionOf(t.dim);
            if (level && region)
                level->explode(*region, nullptr, Vec3{float(lp->x), float(lp->y + 1), float(lp->z)}, float(*power), false, false, 0.0f, false);
        }
        crash(t, *lp);
        crash(other, *lp);
        return;
    }
}

std::map<std::int64_t, std::uint64_t>& hitCooldown() {
    static std::map<std::int64_t, std::uint64_t> m;
    if (m.size() > 512) m.clear();
    return m;
}

void damageEntities(Train& t, Vec3d const& motion) {
    if (!config().trainsCauseDamage || motion.length() <= 0.35) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    for (auto const& c : t.carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con || con->blocks().empty()) continue;
        int   dim    = con->dimension(); // a train passing a portal has carriages in two dimensions
        auto* region = regionOf(dim);
        if (!region) continue;
        std::vector<cc::Vec3d> centers;
        cc::Vec3d              lo{1e18, 1e18, 1e18}, hi{-1e18, -1e18, -1e18};
        for (auto const& b : con->blocks()) {
            if (!b.collision) continue;
            auto w = con->toWorld(cc::Vec3d::center(b.local));
            centers.push_back(w);
            lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
            hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
        }
        if (centers.empty()) continue;
        AABB box{Vec3{float(lo.x - 1.5), float(lo.y - 1.5), float(lo.z - 1.5)}, Vec3{float(hi.x + 1.5), float(hi.y + 1.5), float(hi.z + 1.5)}};
        std::vector<Actor*> nearby = region->getEntities(box, 0.0f);
        level->forEachPlayer([&](Player& p) {
            if (int(p.getDimensionId()) == dim && box.contains(p.getFeetPos())) nearby.push_back(&p);
            return true;
        });
        std::unordered_set<Actor*> seen;
        for (auto* a : nearby) {
            if (!a || !seen.insert(a).second || !a->isAlive()) continue;
            auto type = a->getEntityTypeId();
            if (type == ActorType::FallingBlock || type == ActorType::ItemEntity) continue;
            if (!a->hasCategory(ActorCategory::Mob)) continue;
            if (a->isPlayer()) {
                auto& p = static_cast<Player&>(*a);
                if (p.isCreative() || p.isSpectator()) continue;
            }
            auto   f = a->getFeetPos();
            bool   inside = false, standing = false;
            for (auto const& w : centers) {
                if (std::abs(f.x - w.x) < 0.8 && std::abs(f.z - w.z) < 0.8) {
                    if (f.y >= w.y + 0.4) standing = true; // riding on top of the carriage
                    else if (f.y > w.y - 1.6) inside = true;
                }
            }
            if (!inside || standing) continue;
            auto  uid  = a->getOrCreateUniqueID().rawID;
            auto& last = hitCooldown()[uid];
            if (last != 0 && state().tick - last < 10) continue; // invulnerability frames
            auto pd  = a->getPosDelta();
            auto hit = ct::entityHit(motion, {pd.x, pd.y, pd.z}, a->hasCategory(ActorCategory::Monster));
            if (!hit) continue;
            ActorDamageSource source(SharedTypes::Legacy::ActorDamageCause::EntityAttack, "");
            HurtParameters    params{};
            last = state().tick;
            a->hurt(source, float(hit->damage), params);
            // Create's ContraptionCollider: the driver of a train that ran someone over
            if (!a->isAlive() || a->getHealth() <= 0)
                if (auto* driver = playerByUuid(t.controller)) advancements::award(*driver, "train_roadkill");
            a->lerpMotion(Vec3{float(hit->motion.x), float(hit->motion.y), float(hit->motion.z)});
            playSound(dim, {f.x, f.y, f.z}, "game.player.attack.strong", 1, 0.75f);
            log().info("trains: train #{} '{}' hit {} for {} damage", t.id, t.name, a->getTypeName(), hit->damage);
        }
    }
}

/// Create's Train.mailTransfer: packages addressed to a postbox of the station leave the train;
/// packages waiting in the postboxes for other addresses board it.
void transferMail(Train& t, Station const& st) {
    auto ports = packages::postboxesForStation(st.pos);
    if (ports.empty()) return;
    for (auto& c : t.carriages) {
        auto* con = contraption::find(c.contraption);
        if (!con) continue;
        auto& storage = con->storage();
        for (auto const& port : ports) {
            // unload
            for (int guard = 0; guard < 18; ++guard) {
                auto box = storage.extractItem(
                    [&](ItemStack const& s) { return packages::isPackage(s) && packages::matchAddress(packages::addressOf(s), port.address); },
                    1,
                    true
                );
                if (box.isNull() || !packages::insertIntoPort(port.pos, box, true).isNull()) break;
                box = storage.extractItem(
                    [&](ItemStack const& s) { return packages::isPackage(s) && packages::matchAddress(packages::addressOf(s), port.address); },
                    1,
                    false
                );
                (void)packages::insertIntoPort(port.pos, box, false);
                packages::notifyDelivered(port.pos);
                log().info("trains: train #{} '{}' delivered a package to the postbox at {} {} {}", t.id, t.name, port.pos.x, port.pos.y, port.pos.z);
            }
            // load
            for (int guard = 0; guard < 18; ++guard) {
                auto outgoing = [&](ItemStack const& s) { return packages::isPackage(s) && !packages::matchAddress(packages::addressOf(s), port.address); };
                auto box      = packages::extractFromPort(port.pos, outgoing, true);
                if (box.isNull() || !storage.insertItem(box, true).isNull()) break;
                box = packages::extractFromPort(port.pos, outgoing, false);
                (void)storage.insertItem(box, false);
                log().info("trains: train #{} '{}' picked up a package at {} {} {}", t.id, t.name, port.pos.x, port.pos.y, port.pos.z);
            }
        }
    }
}

} // namespace

bool crossesPortal(ct::Body const& body) {
    std::set<int> dims;
    for (auto const& p : body.points(rail())) {
        dims.insert(p.from.dim);
        dims.insert(p.to.dim);
    }
    return dims.size() > 1;
}

bool overlapsOtherTrain(Train const& t, ct::Body const& body) {
    // Create's TrainRelocator: Train.findCollidingTrain along the relocated train's points
    auto polyline = [](ct::Body const& b) {
        std::vector<Vec3d> out;
        for (auto const& p : b.points(rail()))
            if (auto v = ct::positionOf(rail(), p)) out.push_back(*v);
        return out;
    };
    auto mine = polyline(body);
    if (mine.size() < 2) return false;
    auto close = [](Vec3d const& p, Vec3d const& a, Vec3d const& b) {
        if (std::abs(p.y - a.y) > 3 && std::abs(p.y - b.y) > 3) return false;
        Vec3d  ab{b.x - a.x, 0, b.z - a.z};
        double l2 = ab.lengthSqr();
        double f  = l2 > 0 ? std::clamp(((p.x - a.x) * ab.x + (p.z - a.z) * ab.z) / l2, 0.0, 1.0) : 0;
        double dx = a.x + ab.x * f - p.x, dz = a.z + ab.z * f - p.z;
        return dx * dx + dz * dz < 0.75 * 0.75;
    };
    for (auto const& [id, other] : state().trains) {
        if (id == t.id || other.invalid || other.dim != t.dim) continue;
        auto theirs = polyline(other.body);
        if (theirs.size() < 2) continue;
        for (size_t i = 0; i + 1 < mine.size(); ++i) {
            Vec3d  a = mine[i], b = mine[i + 1];
            double len = (b - a).length();
            int    n   = std::max(1, int(std::ceil(len / 0.5)));
            for (int k = 0; k <= n; ++k) {
                Vec3d p = a + (b - a) * (double(k) / n);
                for (size_t j = 0; j + 1 < theirs.size(); ++j)
                    if (close(p, theirs[j], theirs[j + 1])) return true;
            }
        }
    }
    return false;
}

std::optional<ct::Body> bodyAt(Train const& t, Vec3d const& point, Vec3d const& dir, double maxDistance) {
    auto loc = rail().nearestEdge(t.dim, point, maxDistance);
    if (!loc) return std::nullopt;
    auto p = ct::fromGraph(rail(), *loc);
    auto a = p.from.location(), b = p.to.location();
    if ((b - a).dot(dir) < 0) p = ct::reverse(rail(), p);
    ct::Body body = t.body;
    body.front    = p;
    body.route.clear();
    for (auto const& pt : body.points(rail()))
        if (!ct::resolve(rail(), pt)) return std::nullopt;
    // the whole train must be on track
    if (!body.pointAt(rail(), body.length())) return std::nullopt;
    return body;
}

namespace {

/// Tries to put a train whose track changed back on the graph (Create's reattachToTracks).
bool reattach(Train& t) {
    auto body = bodyAt(t, t.lastFront, t.lastDir, 0.6);
    if (!body) return false;
    t.body = std::move(*body);
    return true;
}
/// Create's Navigation.findNearestApproachable: (station key, name) of the nearest free station within
/// [0.75, max(32, 1.5)] braking distances ahead (`forward`) or behind the train.
std::optional<std::pair<std::string, std::string>> nearestApproachable(Train& t, bool forward, double braking) {
    auto& r     = rail();
    auto  start = forward ? std::optional<ct::TrackPos>(t.body.front)
                          : t.body.pointAt(r, t.body.length()).transform([&](ct::TrackPos const& p) { return ct::reverse(r, p); });
    if (!start) return std::nullopt;
    double minDistance = 0.75 * braking, maxDistance = std::max(32.0, 1.5 * braking);
    std::optional<std::pair<std::string, std::string>> best;
    double                                             bestDistance = 1e18;
    for (auto& [key, st] : state().stations) {
        auto p = st.point();
        if (!p || !sameGraph(t, st) || st.assembling) continue;
        if (auto* present = trainAt(st); present && present != &t) continue;
        auto path = ct::findPath(r, *start, {forward ? *p : ct::reverse(r, *p)}, pathCostFor(t));
        if (!path || path->distance < minDistance || path->distance > maxDistance || path->distance >= bestDistance) continue;
        bestDistance = path->distance;
        best         = std::make_pair(key, st.name);
    }
    return best;
}

/// The manual driving prompt in the action bar (Create's TrainPromptPacket), resent while it shows.
void approachPrompt(Train& t, Player& p, std::string const& text) {
    if (text == t.approachPrompt && state().tick - t.approachPromptTick < 20) return;
    t.approachPrompt     = text;
    t.approachPromptTick = state().tick;
    actionBar(p, text);
}

/// Picks the way to the schedule's stations again (Create: runtime.startCurrentInstruction); keeps
/// the current way when nothing better is found.
void reroute(Train& t) {
    if (!t.nav || t.nav->manual || t.runtime.paused || !t.controller.empty()) return;
    auto old = *t.nav;
    t.nav.reset();
    auto r = navigateTo(t, old.filter, false, old.anyOf.empty() && old.filter.empty() ? old.stationPos : std::string(), old.anyOf);
    if (r != ct::StartResult::Navigating) t.nav = std::move(old);
    else if (t.nav->stationPos != old.stationPos || t.nav->path.edges != old.path.edges)
        log().info("trains: train #{} '{}' found another way", t.id, t.name);
}

void tickTrain(Train& t) {
    auto& r     = rail();
    auto  stats = t.stats();
    auto& s     = state();
    backwardsDrivers().erase(t.id);

    // the graph under the train (and where it is, to find the track again if it changes)
    if (ct::resolve(r, t.body.front)) {
        if (auto p = ct::positionOf(r, t.body.front)) t.lastFront = *p;
        Vec3d d = t.body.front.to.location() - t.body.front.from.location();
        if (d.length() > 1e-6) t.lastDir = d.normalize();
    } else {
        if (t.reattachCooldown-- <= 0) {
            if (reattach(t)) {
                bool was   = t.derailed;
                t.derailed = false;
                t.trackMissingSent = false;
                if (was) statusMessage(t, "back_on_track", {}, true);
                log().info("trains: train #{} '{}' re-attached to the tracks", t.id, t.name);
            } else {
                t.reattachCooldown = 40;
                cancelNavigation(t);
                if (!t.trackMissingSent) {
                    t.trackMissingSent = true;
                    t.derailed         = true;
                    statusMessage(t, "track_missing", {}, false);
                    log().info("trains: train #{} '{}' derailed: the track beneath it is gone", t.id, t.name);
                }
            }
        }
        if (!ct::resolve(r, t.body.front)) {
            t.speed = ct::derailedSlowdown(t.speed);
            return;
        }
    }

    if (t.spansDirty) {
        t.spansDirty = false;
        t.occupied   = occupiedPoints(t);
    }

    // manual control input times out (Create: 30 ticks without a key packet)
    if (!t.controller.empty()) {
        auto* p    = playerByUuid(t.controller);
        auto* ccon = t.controlCarriage < int(t.carriages.size()) ? contraption::find(t.carriages[size_t(t.controlCarriage)].contraption) : nullptr;
        if (!p || int(p->getDimensionId()) != (ccon ? ccon->dimension() : t.dim)) {
            stopControlling(t, false);
        } else if (int(s.tick) - t.lastInput > 30) {
            t.keys = t.steer = 0;
            t.jump = false;
        }
        if (!t.controller.empty()) {
            // must stay within 8 blocks of the controls
            auto* con = t.controlCarriage < int(t.carriages.size()) ? contraption::find(t.carriages[t.controlCarriage].contraption) : nullptr;
            if (con) {
                auto w = con->toWorld(cc::Vec3d::center(t.controlLocal));
                auto f = p->getFeetPos();
                if ((Vec3d{f.x, f.y, f.z} - Vec3d{w.x, w.y, w.z}).length() > 8) stopControlling(t);
            }
        }
    }

    // schedule
    if (!t.derailed) {
        TrainDriver d(t);
        t.runtime.tick(d);
    }

    // mail: packages for the station's postboxes are unloaded, outgoing ones loaded (every 20 ticks)
    if (s.tick % 20 == 0 && !t.currentStation.empty() && std::abs(t.speed) < 1e-6) {
        if (auto* st = stationByKey(t.currentStation)) transferMail(t, *st);
    }
    // cargo inactivity (Create checks every 20 ticks)
    if (s.tick % 20 == 0) {
        auto h = cargoHashOf(t);
        if (h != t.cargoHash) t.idleTicks = 0;
        else t.idleTicks += 20;
        t.cargoHash = h;
    }

    bool  manual = !t.controller.empty();
    auto& keys   = t.keys;
    if (t.derailed) {
        t.speed = ct::derailedSlowdown(t.speed);
    } else if (t.nav) {
        auto& nav = *t.nav;
        if (!nav.manual && !t.runtime.paused && !manual && !hasConductor(t, !nav.behind)) {
            statusMessage(t, hasConductor(t, nav.behind) ? "opposite_driver" : "missing_driver", {}, false);
            cancelNavigation(t);
        } else if (nav.manual && manual && !t.jump) {
            cancelNavigation(t); // releasing space cancels the approach
        } else {
            if (nav.manual && manual) {
                // Create's approach progress: 30 bars, filled from the left as the station nears
                double f        = nav.total > 0 ? nav.distance / nav.total : 0;
                int    progress = int(std::clamp(1 - (1 - f) * (1 - f), 0.0, 1.0) * 30);
                if (auto* p = playerByUuid(t.controller))
                    approachPrompt(t, *p, (progress < 15 ? "§a" : "§6") + std::string(size_t(30 - progress), '|') + (progress == 0 ? "§2" : "§8") + std::string(size_t(progress), '|'));
            }
            if (!t.currentStation.empty()) {
                if (auto* st = stationByKey(t.currentStation)) log().info("trains: train #{} '{}' departing from '{}'", t.id, t.name, st->name);
                t.currentStation.clear();
            }
            burnFuel(t);
            stats = t.stats();
            double dir = nav.behind ? -1 : 1;
            auto   lead = nav.behind ? t.body.pointAt(r, t.body.length()).transform([&](ct::TrackPos const& p) { return ct::reverse(r, p); })
                                     : std::optional<ct::TrackPos>(t.body.front);
            double target  = nav.distance;
            double blocker = -1;
            if (lead) {
                blocker = distanceToBlocker(t, *lead, nav.path, std::min(nav.distance, ct::brakingDistance(t.speed, stats.acceleration) + 4));
                // Create waits in front of the signal (no overshoot past it)
                if (blocker >= 0) target = std::min(target, std::max(0.0, blocker - 0.75));
            }
            if (blocker >= 0) {
                t.distanceToSignal = blocker;
            } else {
                t.distanceToSignal = 1e18;
                t.signalWaitTicks  = 0;
                t.waitingAtCross   = false;
            }
            if (blocker >= 0 && blocker < 1.0) {
                t.speed = 0; // holding at the point
                ++t.signalWaitTicks;
                // Create's Train.updateNavigationTarget: a train waiting at a cross signal looks
                // for another way now and then (the red signal penalty grows with the wait)
                if (t.waitingAtCross && t.signalWaitTicks % 100 == 50 && std::fmod(nav.distance, 100) <= 20 && !t.reserving)
                    reroute(t);
                return;
            }
            ct::NavigationInput in;
            in.distanceToTarget    = std::max(0.0, target);
            in.distanceToNextCurve = lead ? ct::distanceToCurve(r, *lead, nav.path, ct::brakingDistance(t.speed, stats.acceleration) + 20) : -1;
            in.throttle            = t.throttle;
            in.direction           = dir;
            auto step              = ct::navigate(t.speed, stats, in);
            if (target < nav.distance && step.finalApproach) step.speed = 0; // wait at the signal
            t.speed = step.speed;
            if (nav.distance < -10) cancelNavigation(t);
        }
    } else if (manual) {
        int   dirKeys = keys * t.controlDir;
        auto* driver  = playerByUuid(t.controller);
        if (auto* st = stationByKey(t.currentStation)) {
            if (t.jump && driver) actionBar(*driver, trFor(*driver, "create.train.arrived_at", "Arrived at {0}", st->name));
        } else {
            // Create's Navigation.findNearestApproachable: the nearest station ahead in the direction
            // driven (a double-ended train: either way) between 0.75 and max(32, 1.5) braking distances
            double directed = dirKeys != 0 ? dirKeys : t.speed;
            bool   forward  = !t.doubleEnded || (directed != 0 ? directed > 0 : t.controlDir > 0);
            auto   approach = nearestApproachable(t, forward, ct::brakingDistance(t.speed, stats.acceleration));
            if (approach && t.jump) {
                navigateTo(t, "", true, approach->first, {}, forward ? 1 : -1);
                if (t.nav) t.nav->total = std::max(t.nav->distance, 1e-3);
            } else if (approach && driver) {
                // "Hold [Space] to approach {station}"
                approachPrompt(t, *driver, trFor(*driver, "create.contraption.controls.approach_station", "Hold {0} to approach {1}", "[Space]", approach->second));
            } else if (!t.approachPrompt.empty() && driver) {
                approachPrompt(t, *driver, " "); // Create clears the message
                t.approachPrompt.clear();
            }
        }
        if (dirKeys != 0 && !t.currentStation.empty()) {
            if (auto* st = stationByKey(t.currentStation)) {
                if (auto* p = playerByUuid(t.controller)) actionBar(*p, trFor(*p, "create.train.departing_from", "Departing from {0}", st->name));
                log().info("trains: train #{} '{}' departing from '{}'", t.id, t.name, st->name);
            }
            t.currentStation.clear();
        }
        if (!t.nav) {
            if (dirKeys != 0) burnFuel(t);
            stats      = t.stats();
            bool onTrn = ct::onTurn(r, t.body.front);
            if (auto back = t.body.pointAt(r, t.body.length())) onTrn = onTrn || ct::onTurn(r, *back);
            bool slow     = dirKeys != 0 && dirKeys != t.controlDir; // Create: inverted ^ targetSpeed < 0
            double target = ct::manualTargetSpeed(dirKeys, stats, config().manualTrainSpeedModifier, t.throttle, onTrn, slow);
            auto   sgn    = [](double v) { return v > 0 ? 1.0 : v < 0 ? -1.0 : 0.0; };
            if (slow && std::abs(sgn(target) - sgn(t.speed)) <= 1.5) backwardsDrivers()[t.id] = t.controller;
            t.targetSpeed = target;
            t.speed       = ct::manualStep(t.speed, target, stats);
        }
    } else if (t.speed != 0) {
        t.speed = ct::passiveSlowdown(t.speed, stats.acceleration);
    }

    // Create's Train.tick: a carriage whose actor stalls (a drill breaking a block) stops the whole
    // train; it goes on at its previous speed (at most 1 block per tick) when the stall ends
    bool stalled = std::any_of(t.carriages.begin(), t.carriages.end(), [](Carriage const& c) {
        auto* con = contraption::find(c.contraption);
        return con && con->stalled();
    });
    if (stalled && !t.derailed) {
        if (!t.speedBeforeStall) {
            t.speedBeforeStall = t.speed;
            log().info("trains: train #{} '{}' stalled", t.id, t.name);
        }
        t.speed = 0;
        // the carriages keep asking to move a little, so their actors go on working (the
        // contraption engine holds a stalled contraption where it is)
        if (double dir = *t.speedBeforeStall > 0 ? 1 : *t.speedBeforeStall < 0 ? -1 : 0; dir != 0) {
            ct::Body    probe = t.body;
            ct::Path    way   = t.nav ? t.nav->path : ct::Path{}; // the probe must not advance the path
            ct::Chooser pick  = t.nav ? way.chooser() : ct::Chooser{};
            (void)probe.move(r, dir * 0.05, pick, {});
            for (auto const& [id, tr] : carriageTransforms(t, probe))
                if (auto* con = contraption::find(id)) con->setTransform(tr);
        }
        return;
    }
    if (t.speedBeforeStall) {
        t.speed = std::clamp(*t.speedBeforeStall, -1.0, 1.0);
        t.speedBeforeStall.reset();
        log().info("trains: train #{} '{}' goes on after a stall", t.id, t.name);
    }

    if (std::abs(t.speed) < 1e-9) {
        t.speed = 0;
        return;
    }

    // move
    ct::Chooser choose;
    if (t.nav) choose = t.nav->path.chooser();
    else if (manual) choose = steering(t.steer * (t.speed >= 0 ? 1 : -1) * t.controlDir);
    std::optional<ct::TrackPos> dest;
    Station*                    destStation = nullptr;
    if (t.nav) {
        destStation = stationByKey(t.nav->stationPos);
        if (destStation) dest = destStation->point();
        if (dest && t.nav->behind) dest = ct::reverse(r, *dest);
    }
    bool arrived = false;
    auto before  = ct::positionOf(r, t.body.front);
    auto section = leadSection(t);
    auto res = t.body.move(r, t.speed, choose, [&](ct::EdgeSpan const& sp) {
        if (!dest || arrived) return;
        if (sp.from == dest->from && sp.to == dest->to && dest->pos > sp.start - 1e-9 && dest->pos <= sp.end + 1e-9) arrived = true;
    });
    if (t.nav) {
        // Create's Train.updateNavigationTarget: far from the destination, the way is looked for
        // again every 100 blocks
        double before = t.nav->distance;
        t.nav->distance -= res.moved;
        double now = t.nav->distance;
        if (!t.nav->manual && before > 100 && std::fmod(before, 100) > 20 && std::fmod(now, 100) <= 20 && !t.reserving) reroute(t);
    }
    if (res.lost) {
        t.speed = 0;
        return;
    }
    if (arrived && destStation) arriveAt(t, *destStation);
    if (res.blocked && !arrived) {
        t.speed = 0;
        cancelNavigation(t);
        statusMessage(t, "end_of_track", {}, false);
    }
    auto after = ct::positionOf(r, t.body.front);
    if (before && after) {
        Vec3d d = *after - *before;
        if (d.length() > 1e-6) {
            t.lastDir   = t.speed >= 0 ? d.normalize() : -d.normalize();
            t.lastFront = *after;
        }
    }
    placeCarriages(t);

    // what the train passes: edge point listeners (U28)
    auto occ     = occupiedPoints(t);
    t.spansDirty = false;
    if (occ != t.occupied) {
        for (auto const& id : occ)
            if (!std::binary_search(t.occupied.begin(), t.occupied.end(), id))
                if (auto const* p = r.point(id))
                    for (auto const& fn : ext().entered) fn(t.id, *p);
        for (auto const& id : t.occupied)
            if (!std::binary_search(occ.begin(), occ.end(), id))
                if (auto const* p = r.point(id))
                    for (auto const& fn : ext().left) fn(t.id, *p);
        t.occupied = std::move(occ);
    }
    // Create's Train signal listener: a train without a running schedule entering an occupied section
    if ((!t.runtime.hasSchedule || t.runtime.paused) && !t.controller.empty()) {
        auto now = leadSection(t);
        if (now != sig::kNoGroup && now != section) {
            auto in = sig::trainsIn(now);
            if (std::any_of(in.begin(), in.end(), [&](std::string const& id) { return id != signalId(t); }))
                if (auto* p = playerByUuid(t.controller)) advancements::award(*p, "red_signal");
        }
    }

    collideWithOtherTrains(t);
    if (after && before) damageEntities(t, *after - *before);
}

} // namespace

void tickTrains() {
    auto& m = state().trains;
    std::vector<std::uint64_t> ids;
    for (auto const& [id, t] : m) ids.push_back(id);
    for (auto id : ids) {
        auto it = m.find(id);
        if (it == m.end()) continue;
        Train& t = it->second;
        if (t.invalid) {
            m.erase(it);
            continue;
        }
        if (t.disassembling) continue;
        bool missing = false;
        for (auto const& c : t.carriages) missing = missing || !contraption::find(c.contraption);
        if (missing) {
            log().info("trains: train #{} '{}' lost a carriage and was removed", t.id, t.name);
            for (auto const& c : t.carriages)
                if (contraption::find(c.contraption)) contraption::disassemble(c.contraption);
            t.carriages.clear();
            removeTrain(id);
            continue;
        }
        try {
            tickTrain(t);
        } catch (std::exception const& e) {
            log().error("trains: train #{} tick failed: {}", id, e.what());
        }
    }
    trackRiders();
}

void removeTrain(std::uint64_t id) {
    auto& m  = state().trains;
    auto  it = m.find(id);
    if (it == m.end()) return;
    Train& t = it->second;
    if (!t.controller.empty()) stopControlling(t, false);
    // Create's /create train remove: the carriage entities are discarded, nothing is placed
    for (auto const& c : t.carriages) contraption::discard(c.contraption);
    t.carriages.clear();
    for (auto const& pid : t.occupied)
        if (auto const* p = rail().point(pid))
            for (auto const& fn : ext().left) fn(t.id, *p);
    sig::removeTrain(signalId(t));
    t.invalid = true;
}

void onGraphChanged() {
    refreshStationPoints();
    for (auto& [id, t] : state().trains) {
        if (t.invalid || !t.nav) continue;
        auto key    = t.nav->stationPos;
        bool manual = t.nav->manual;
        t.nav.reset();
        if (navigateTo(t, "", manual, key) != ct::StartResult::Navigating) {
            t.targetSpeed = 0;
            t.runtime.transitInterrupted();
        }
    }
}

// ---- manual controls ----------------------------------------------------------------------------

void startControlling(Player& player, Train& t, int carriage, cc::Pos local, int direction) {
    if (t.derailed) return;
    for (auto& [id, other] : state().trains)
        if (other.controller == uuidOf(player)) stopControlling(other, false);
    if (t.runtime.hasSchedule && !t.runtime.paused) {
        statusMessage(t, "paused_for_manual", {}, true);
        t.runtime.paused = true;
        cancelNavigation(t);
    }
    t.controller      = uuidOf(player);
    t.controlDir      = direction >= 0 ? 1 : -1;
    t.controlCarriage = carriage;
    t.controlLocal    = local;
    t.keys = t.steer = 0;
    t.jump           = false;
    t.lastInput      = int(state().tick);
    actionBar(player, trFor(player, "create.contraption.controls.start_controlling", "Now controlling: {0}", t.name));
    log().info("trains: {} is controlling train #{} '{}'", player.getRealName(), t.id, t.name);
    controlsChanged(t, &player, true);
}

void stopControlling(Train& t, bool message) {
    if (t.controller.empty()) return;
    auto* p = playerByUuid(t.controller);
    if (p && message) actionBar(*p, trFor(*p, "create.contraption.controls.stop_controlling", "Stopped controlling contraption"));
    log().info("trains: {} stopped controlling train #{} '{}'", p ? p->getRealName() : std::string("?"), t.id, t.name);
    t.controller.clear();
    t.keys = t.steer = 0;
    t.jump = false;
    if (t.nav && t.nav->manual) cancelNavigation(t);
    controlsChanged(t, p, false);
}

void setInput(Player& player, int throttle, int steer, bool jump, bool sneak) {
    auto uuid = uuidOf(player);
    for (auto& [id, t] : state().trains) {
        if (t.controller != uuid) continue;
        if (sneak) {
            stopControlling(t);
            return;
        }
        t.keys      = throttle;
        t.steer     = steer;
        t.jump      = jump;
        t.lastInput = int(state().tick);
        return;
    }
}

bool tryControls(Player& player, double maxDistance) {
    auto  eye  = player.getPosition(); // eye height (getHeadPos drifts with the body yaw of real players)
    auto  look = player.getViewVector();
    Vec3d o{eye.x, eye.y, eye.z}, d{look.x, look.y, look.z};
    d              = d.normalize();
    Train* best    = nullptr;
    int    bestCar = 0, bestDir = 1;
    cc::Pos bestLocal;
    double bestT   = std::min(maxDistance, 5.5);
    for (auto& [id, t] : state().trains) {
        if (t.invalid) continue;
        for (size_t c = 0; c < t.carriages.size(); ++c) {
            auto* con = contraption::find(t.carriages[c].contraption);
            if (!con || con->dimension() != int(player.getDimensionId())) continue;
            for (auto const& [local, rel] : t.carriages[c].controls) {
                auto   w   = con->toWorld(cc::Vec3d::center(local));
                Vec3d  to  = Vec3d{w.x, w.y, w.z} - o;
                double along = to.dot(d);
                if (along < 0 || along > bestT) continue;
                Vec3d off = to - d * along;
                if (off.length() > 0.65) continue;
                bestT     = along;
                best      = &t;
                bestCar   = int(c);
                bestLocal = local;
                bestDir   = rel;
            }
        }
    }
    if (!best) return false;
    // Bedrock repeats a held use: toggle at most once per 10 ticks
    static std::map<std::string, std::uint64_t> lastToggle;
    auto& last = lastToggle[uuidOf(player)];
    if (last != 0 && state().tick - last < 10) return true;
    last = state().tick;
    if (best->controller == uuidOf(player)) stopControlling(*best);
    else startControlling(player, *best, bestCar, bestLocal, bestDir);
    return true;
}

} // namespace lv::feature::trains

// ---- TrainsExt.h --------------------------------------------------------------------------------

namespace lv::feature::tracks::trains {

namespace ft = lv::feature::trains;

std::vector<TrainInfo> all() {
    std::vector<TrainInfo> out;
    for (auto const& [id, t] : ft::state().trains) {
        if (t.invalid) continue;
        TrainInfo i;
        i.id       = t.id;
        i.name     = t.name;
        i.dim      = t.dim;
        i.speed    = t.speed * 20;
        i.derailed = t.derailed;
        i.points   = t.occupied;
        if (auto g = core::trains::resolve(railways(), t.body.front)) i.graph = g->graph;
        if (!t.currentStation.empty())
            if (auto* s = ft::stationByKey(t.currentStation)) i.station = s->name;
        if (t.nav)
            if (auto* s = ft::stationByKey(t.nav->stationPos)) i.destination = s->name;
        i.title = t.runtime.title();
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<StationInfo> stations() {
    std::vector<StationInfo> out;
    for (auto const& [k, s] : ft::state().stations) {
        StationInfo i;
        i.pos  = s.pos;
        i.name = s.name;
        if (auto* t = ft::trainAt(s)) i.presentTrain = t->name;
        for (auto const& [name, ticks] : ft::arrivals(s)) i.arrivals.push_back({name, ticks});
        out.push_back(std::move(i));
    }
    return out;
}

void onPointEntered(std::function<void(std::uint64_t, core::tracks::EdgePoint const&)> fn) { ft::ext().entered.push_back(std::move(fn)); }
void onPointLeft(std::function<void(std::uint64_t, core::tracks::EdgePoint const&)> fn) { ft::ext().left.push_back(std::move(fn)); }

} // namespace lv::feature::tracks::trains

namespace lv::feature::trains {

std::optional<DoorMode> stationDoorModeOf(std::uint64_t contraption) {
    for (auto const& [id, t] : state().trains) {
        if (t.invalid) continue;
        bool mine = std::any_of(t.carriages.begin(), t.carriages.end(), [&](Carriage const& c) { return c.contraption == contraption; });
        if (!mine) continue;
        if (t.currentStation.empty() || std::abs(t.speed) > 1e-6) return std::nullopt;
        auto s = state().stations.find(t.currentStation);
        if (s == state().stations.end()) return std::nullopt;
        return s->second.door;
    }
    return std::nullopt;
}

} // namespace lv::feature::trains
