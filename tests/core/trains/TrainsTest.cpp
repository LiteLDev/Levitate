#include <gtest/gtest.h>

#include <map>

#include "core/tracks/CurveStore.h"
#include "core/tracks/Graph.h"
#include "core/tracks/Placement.h"
#include "core/trains/Assembly.h"
#include "core/trains/Motion.h"
#include "core/trains/Physics.h"
#include "core/trains/Schedule.h"

using namespace lv::core::trains;
namespace tk = lv::core::tracks;

namespace {

class World : public tk::TrackWorld {
public:
    std::map<std::pair<int, tk::BlockPos3>, tk::Shape> shapes;
    tk::CurveStore                                     curves;

    std::optional<tk::TrackBlockView> trackAt(int dim, tk::BlockPos3 const& pos) const override {
        auto it = shapes.find({dim, pos});
        if (it == shapes.end()) return std::nullopt;
        tk::TrackBlockView v;
        v.shape  = it->second;
        v.curves = curves.curvesAt(dim, pos);
        return v;
    }
    void place(tk::Railways& r, tk::BlockPos3 p, tk::Shape s) {
        shapes[{0, p}] = s;
        r.onRailAdded(*this, 0, p);
    }
    void remove(tk::Railways& r, tk::BlockPos3 p) {
        auto view = trackAt(0, p);
        shapes.erase({0, p});
        if (view) r.onRailRemoved(*this, 0, p, *view);
    }
};

/// A straight east-west line x = 0..n-1 at z = 0.
void lineX(World& w, tk::Railways& r, int n, int z = 0) {
    for (int x = 0; x < n; ++x) w.place(r, {x, 0, z}, tk::Shape::XO);
}

/// The position at (x + .5, 0, z + .5) travelling east.
TrackPos eastAt(tk::Railways& r, double x, double z = 0.5) {
    auto g = r.nearestEdge(0, {x, 0, z});
    EXPECT_TRUE(g);
    TrackPos p = fromGraph(r, *g);
    auto     a = p.from.location(), b = p.to.location();
    if (b.x < a.x) p = reverse(r, p);
    return p;
}

class FakeDriver : public Driver {
public:
    StartResult next = StartResult::Navigating;
    std::vector<std::string> statuses;
    std::string title;
    double      throttle = 1;
    bool        nav      = false;
    int         delayCalls = 0;
    long long   items    = 0;
    int         players  = 0;
    bool        powered  = false;

    StartResult startDestination(std::string const&) override {
        if (next == StartResult::Navigating) nav = true;
        return next;
    }
    StartResult packages = StartResult::NoTarget;
    StartResult startPackages(bool, std::string const&) override { return packages; }
    void        rename(std::string const& t) override { title = t; }
    void        setThrottle(double t) override { throttle = t; }
    bool        navigating() const override { return nav; }
    bool        derailed() const override { return false; }
    std::int64_t dayTime() const override { return 0; }
    long long   itemCount(Json const&, bool) const override { return items; }
    long long   fluidAmount(Json const&) const override { return 0; }
    bool        linkPowered(Json const&) const override { return false; }
    int         seatedPlayers() const override { return players; }
    int         idleTicks() const override { return 0; }
    bool        stationPowered() const override { return powered; }
    bool        stationUnloaded() const override { return false; }
    void        status(std::string const& key, std::vector<std::string> const&, bool) override { statuses.push_back(key); }
};

} // namespace

// ---- physics ------------------------------------------------------------------------------------

TEST(TrainPhysics, ConfigUnitsAreBlocksPerTick) {
    SpeedConfig cfg;
    auto        s = statsOf(cfg, false);
    EXPECT_DOUBLE_EQ(s.maxSpeed, 1.4);
    EXPECT_DOUBLE_EQ(s.maxTurnSpeed, 0.7);
    EXPECT_DOUBLE_EQ(s.acceleration, 3.0 / 400);
    auto p = statsOf(cfg, true);
    EXPECT_DOUBLE_EQ(p.maxSpeed, 2.0);
    EXPECT_DOUBLE_EQ(p.maxTurnSpeed, 1.0);
}

TEST(TrainPhysics, ApproachAndCoast) {
    EXPECT_DOUBLE_EQ(approach(0, 1, 0.1, 1), 0.1);
    EXPECT_DOUBLE_EQ(approach(0.95, 1, 0.1, 1), 1);
    EXPECT_DOUBLE_EQ(approach(1, 0, 0.1, 2), 0.8);
    EXPECT_DOUBLE_EQ(passiveSlowdown(0.05, 0.1), 0);
    EXPECT_DOUBLE_EQ(passiveSlowdown(-0.5, 0.1), -0.4);
    EXPECT_DOUBLE_EQ(derailedSlowdown(0.9), 0.3);
    EXPECT_NEAR(brakingDistance(1.4, 0.0075), 130.666, 1e-3);
}

TEST(TrainPhysics, NavigationAcceleratesBrakesAndSnaps) {
    Stats s = statsOf({}, false);
    // far away: accelerate
    auto far = navigate(0, s, {500, -1, 1, 1});
    EXPECT_NEAR(far.speed, s.acceleration, 1e-12);
    // inside the braking distance: slow down
    auto brake = navigate(1.4, s, {100, -1, 1, 1});
    EXPECT_LT(brake.speed, 1.4);
    // last 10 blocks: clamp towards topSpeed * d / 10
    auto approachStep = navigate(1.0, s, {5, -1, 1, 1});
    EXPECT_LT(approachStep.speed, 1.0);
    // final approach: snap onto the station
    auto snap = navigate(0.3, s, {0.01, -1, 1, 1});
    EXPECT_TRUE(snap.finalApproach);
    EXPECT_NEAR(snap.speed, 0.26, 1e-9);
    // a curve ahead holds the turning speed
    auto curve = navigate(1.0, s, {500, 1, 1, 1});
    EXPECT_LT(curve.speed, 1.0);
    // a simulated run arrives without overshooting more than the snap
    double speed = 0, left = 200;
    int    ticks = 0;
    while (left > 0 && ticks < 5000) {
        auto step = navigate(speed, s, {left, -1, 1, 1});
        speed     = step.speed;
        left -= speed;
        ++ticks;
        if (step.finalApproach) break;
    }
    EXPECT_GT(left, -0.3);
    EXPECT_LT(ticks, 1000);
}

TEST(TrainPhysics, ManualDriving) {
    Stats s = statsOf({}, false);
    EXPECT_DOUBLE_EQ(manualTargetSpeed(1, s, 0.75, 1, false, false), 1.4 * 0.75);
    EXPECT_DOUBLE_EQ(manualTargetSpeed(-1, s, 0.75, 1, false, true), -1.4 * 0.75 / 4);
    EXPECT_DOUBLE_EQ(manualTargetSpeed(1, s, 0.75, 1, true, false), 0.7);
    EXPECT_DOUBLE_EQ(manualTargetSpeed(0, s, 0.75, 1, false, false), 0);
    // braking against the motion: twice the acceleration
    EXPECT_NEAR(manualStep(1.0, -1.0, s), 1.0 - 2 * s.acceleration, 1e-12);
    EXPECT_NEAR(manualStep(1.0, 0, s), 1.0 - s.acceleration, 1e-12);
}

TEST(TrainPhysics, HitsAndCollisions) {
    EXPECT_FALSE(entityHit({0.3, 0, 0}, {}, false));
    auto hit = entityHit({1, 0, 0}, {}, false);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->damage, 16);
    EXPECT_LE(hit->motion.length(), 3 + 1e-9);
    auto monster = entityHit({1, 0, 0}, {}, true);
    EXPECT_EQ(monster->damage, 32);
    EXPECT_FALSE(collisionExplosion(0.1, 0.05));
    EXPECT_DOUBLE_EQ(*collisionExplosion(1, 1), 5);
    EXPECT_DOUBLE_EQ(*collisionExplosion(0.2, 0.1), 0.9);
    EXPECT_DOUBLE_EQ(crashBounce(1.2), -0.5);
    EXPECT_DOUBLE_EQ(crashBounce(-0.2), 0.2);
}

// ---- assembly -----------------------------------------------------------------------------------

TEST(TrainAssembly, BogeyRules) {
    EXPECT_TRUE(canPlaceBogey(0, {}));
    EXPECT_FALSE(canPlaceBogey(2, {}));
    EXPECT_FALSE(canPlaceBogey(3, {})) << "Create: the first bogey goes to the station marker";
    EXPECT_TRUE(canPlaceBogey(3, {0}));
    EXPECT_FALSE(canPlaceBogey(2, {0}));
    EXPECT_FALSE(canPlaceBogey(5, {3}));
    EXPECT_TRUE(canPlaceBogey(6, {3}));
    EXPECT_EQ(checkBogeys({}, 20)->key, "create.train_assembly.no_bogeys");
    EXPECT_EQ(checkBogeys({1, 5}, 20)->key, "create.train_assembly.frontmost_bogey_at_station");
    EXPECT_EQ(checkBogeys({0, 2}, 20)->key, "create.train_assembly.bogeys_too_close");
    EXPECT_FALSE(checkBogeys({0, 3, 8}, 20));
    EXPECT_EQ(checkBogeys({0, 3, 8}, 2)->key, "create.train_assembly.too_many_bogeys");
}

TEST(TrainAssembly, CarriagesFromTouchedBogeys) {
    std::vector<int> idx{0, 4, 9};
    // carriage 1 holds bogeys 0 and 1, carriage 2 holds bogey 2
    auto layout = planCarriages(
        idx,
        [](int i) { return i == 0 ? std::vector<int>{0, 1} : std::vector<int>{i}; },
        [](int) { return 5; }
    );
    ASSERT_TRUE(std::holds_alternative<Layout>(layout));
    auto const& l = std::get<Layout>(layout);
    ASSERT_EQ(l.carriages.size(), 2u);
    EXPECT_EQ(l.bogeySpacing[0], 4);
    EXPECT_EQ(l.bogeySpacing[1], 0);
    EXPECT_EQ(l.carriageSpacing[1], 5);

    auto empty = planCarriages(idx, [](int i) { return std::vector<int>{i}; }, [](int i) { return i == 1 ? 1 : 3; });
    ASSERT_TRUE(std::holds_alternative<AssemblyError>(empty));
    EXPECT_EQ(std::get<AssemblyError>(empty).key, "create.train_assembly.nothing_attached");
    auto three = planCarriages(idx, [](int) { return std::vector<int>{0, 1, 2}; }, [](int) { return 9; });
    EXPECT_EQ(std::get<AssemblyError>(three).key, "create.train_assembly.too_many_bogeys");
    auto skip = planCarriages(idx, [](int i) { return i == 0 ? std::vector<int>{0, 2} : std::vector<int>{i}; }, [](int) { return 9; });
    EXPECT_EQ(std::get<AssemblyError>(skip).key, "create.train_assembly.not_connected_in_order");

    bool fw = false, bw = false;
    EXPECT_FALSE(checkControls({1, -1}, fw, bw));
    EXPECT_TRUE(fw && bw);
    EXPECT_EQ(checkControls({0}, fw, bw)->key, "create.train_assembly.sideways_controls");
}

// ---- schedules ----------------------------------------------------------------------------------

TEST(TrainSchedule, JsonRoundTripAndDefaults) {
    Schedule s;
    s.entries.push_back(Entry::makeDefault());
    s.entries[0].instruction.data["Text"] = "Station *";
    Entry rename;
    rename.instruction = defaultInstruction("create:rename");
    s.entries.push_back(rename);
    s.cyclic = false;
    auto back = Schedule::fromJson(s.toJson());
    ASSERT_EQ(back.entries.size(), 2u);
    EXPECT_EQ(back.entries[0].instruction.id, "levitate:destination");
    EXPECT_EQ(back.entries[0].conditions.size(), 1u);
    EXPECT_EQ(back.entries[0].conditions[0][0].id, "levitate:delay");
    EXPECT_EQ(back.entries[1].instruction.id, "levitate:rename");
    EXPECT_FALSE(back.cyclic);
    EXPECT_FALSE(supportsConditions("levitate:throttle"));
    EXPECT_EQ(waitTicks(defaultCondition(ids::kDelay).data), 100);
}

TEST(TrainSchedule, StationFilters) {
    EXPECT_TRUE(stationMatches("Station A", "Station A"));
    EXPECT_FALSE(stationMatches("Station A", "Station AB"));
    EXPECT_TRUE(stationMatches("Station *", "Station AB"));
    EXPECT_TRUE(stationMatches("*A*", "xxAyy"));
    EXPECT_FALSE(stationMatches("*A*", "xxByy"));
    EXPECT_TRUE(stationMatches("a.b", "a.b"));
    EXPECT_FALSE(stationMatches("a.b", "axb")); // '.' is literal
    EXPECT_EQ(filterTitle(" Station * "), "Station");
}

TEST(TrainSchedule, TimeOfDayAndThresholds) {
    Json d = defaultCondition(ids::kTimeOfDay).data;
    d["Rotation"] = 0; // every day
    d["Hour"]     = 8; // 8:00 = day time 2000
    EXPECT_EQ(timeOfDayTarget(d), 2000);
    EXPECT_TRUE(timeOfDayReached(d, 2000));
    EXPECT_TRUE(timeOfDayReached(d, 2040 + 24000));
    EXPECT_FALSE(timeOfDayReached(d, 2041));
    EXPECT_FALSE(timeOfDayReached(d, 1999));
    EXPECT_TRUE(compareThreshold(0, 11, 10));
    EXPECT_FALSE(compareThreshold(0, 10, 10));
    EXPECT_TRUE(compareThreshold(1, 9, 10));
    EXPECT_TRUE(compareThreshold(2, 10, 10));
    auto eta = etaOf(100);
    EXPECT_EQ(eta.kind, EtaText::Kind::Now);
    EXPECT_EQ(etaOf(20000).kind, EtaText::Kind::Unknown);
    auto m = etaOf(2500);
    EXPECT_EQ(m.kind, EtaText::Kind::Minutes);
    EXPECT_EQ(m.value, 2);
    auto sec = etaOf(400);
    EXPECT_EQ(sec.kind, EtaText::Kind::Seconds);
    EXPECT_EQ(sec.value, 30);
}

TEST(TrainSchedule, RuntimeRunsEntriesAndConditions) {
    Schedule s;
    auto     e = Entry::makeDefault();
    e.instruction.data["Text"] = "A";
    e.conditions[0][0].data["Value"]    = 3;
    e.conditions[0][0].data["TimeUnit"] = 0; // 3 ticks
    s.entries.push_back(e);
    Entry throttle;
    throttle.instruction = defaultInstruction(ids::kThrottle);
    throttle.instruction.data["Value"] = 50;
    s.entries.push_back(throttle);
    s.cyclic = false;

    Runtime    rt;
    FakeDriver d;
    rt.setSchedule(s, false);
    rt.tick(d);
    EXPECT_EQ(rt.state, RuntimeState::InTransit);
    EXPECT_TRUE(d.nav);
    d.nav = false;
    rt.destinationReached();
    EXPECT_EQ(rt.state, RuntimeState::PostTransit);
    for (int i = 0; i < 4; ++i) rt.tick(d);
    EXPECT_EQ(rt.state, RuntimeState::PostTransit) << "3 tick delay not over yet";
    rt.tick(d);
    EXPECT_EQ(rt.state, RuntimeState::PreTransit);
    EXPECT_EQ(rt.currentEntry, 1);
    rt.tick(d); // throttle
    EXPECT_DOUBLE_EQ(d.throttle, 0.5);
    rt.tick(d); // end of a non cyclic schedule
    EXPECT_TRUE(rt.completed);
    EXPECT_TRUE(rt.paused);
}

TEST(TrainSchedule, ColumnsAreAlternativesAndFailuresWait) {
    Schedule s;
    auto     e = Entry::makeDefault();
    e.conditions.clear();
    auto powered = defaultCondition(ids::kPowered);
    auto players = defaultCondition(ids::kPlayerCount);
    players.data["Count"] = 2;
    e.conditions.push_back({powered});
    e.conditions.push_back({players});
    s.entries.push_back(e);
    Runtime    rt;
    FakeDriver d;
    rt.setSchedule(s, false);
    d.next = StartResult::NoPath;
    rt.tick(d);
    ASSERT_EQ(d.statuses.size(), 1u);
    EXPECT_EQ(d.statuses[0], "no_path");
    for (int i = 0; i < Runtime::kInterval; ++i) rt.tick(d);
    EXPECT_EQ(d.statuses.size(), 1u) << "waits 40 ticks before retrying";
    d.next = StartResult::AlreadyThere;
    rt.tick(d);
    EXPECT_EQ(d.statuses.back(), "navigation_success");
    EXPECT_EQ(rt.state, RuntimeState::PostTransit);
    rt.tick(d);
    rt.tick(d);
    EXPECT_EQ(rt.state, RuntimeState::PostTransit);
    d.players = 2; // the second column completes
    rt.tick(d);
    rt.tick(d);
    EXPECT_EQ(rt.state, RuntimeState::PreTransit);
    EXPECT_EQ(rt.currentEntry, 1);
    auto back = Runtime::fromJson(rt.toJson());
    EXPECT_EQ(back.currentEntry, 1);
    EXPECT_TRUE(back.hasSchedule);
}

TEST(TrainSchedule, PackageInstructionsWithoutPackagesSkipTheirEntry) {
    // Create's DeliverPackagesInstruction / FetchPackagesInstruction: nothing to do skips the entry
    // with its conditions (Fetch waits the retry interval first)
    Schedule s;
    Entry    deliver;
    deliver.instruction = defaultInstruction(ids::kPackageDelivery);
    deliver.conditions  = {{defaultCondition(ids::kPowered)}};
    s.entries.push_back(deliver);
    s.entries.push_back(Entry::makeDefault());
    Runtime    rt;
    FakeDriver d;
    rt.setSchedule(s, false);
    d.packages = StartResult::Skip;
    rt.tick(d);
    EXPECT_EQ(rt.currentEntry, 1);
    EXPECT_EQ(rt.state, RuntimeState::PreTransit);
    EXPECT_TRUE(d.statuses.empty());
    rt.currentEntry = 0;
    d.packages      = StartResult::SkipCooldown;
    rt.tick(d);
    EXPECT_EQ(rt.currentEntry, 1);
    EXPECT_EQ(rt.cooldown, Runtime::kInterval);
}

TEST(TrainSchedule, PredictionsAccumulateLearnedTransitsAndStays) {
    // Create's ScheduleRuntime.submitPredictions
    Schedule s;
    auto     stop = [](std::string const& to, int delayTicks) {
        auto e                              = Entry::makeDefault();
        e.instruction.data["Text"]          = to;
        e.conditions[0][0].data["Value"]    = delayTicks;
        e.conditions[0][0].data["TimeUnit"] = 0;
        return e;
    };
    s.entries = {stop("A", 100), stop("B", 40), stop("C*", 20)};
    Runtime rt;
    rt.setSchedule(s, false);
    rt.predictionTicks = {300, 500, -1};
    // standing at A (entry 0 reached)
    rt.state = RuntimeState::PostTransit;
    Runtime::PredictionInput in;
    in.currentStation = "Station A";
    auto p            = rt.predictions(in);
    ASSERT_EQ(p.size(), 3u);
    EXPECT_EQ(p[0].destination, "Station A");
    EXPECT_EQ(p[0].ticks, 0);
    EXPECT_EQ(p[0].title, "B") << "the next destination's filter is the title";
    EXPECT_EQ(p[1].destination, "B");
    EXPECT_EQ(p[1].ticks, 100 + 500) << "the stay at A, then B's learned transit";
    EXPECT_EQ(p[2].destination, "C*");
    EXPECT_EQ(p[2].ticks, -1) << "C's transit is not known yet";
    EXPECT_EQ(p[2].title, "A");

    // travelling to A: half the plain estimate and half the learned time, scaled by the distance left
    rt.state                 = RuntimeState::InTransit;
    in                       = {};
    in.navDestination        = "Station A";
    in.distanceToDestination = 50;
    in.distanceStartedAt     = 100;
    in.speed                 = 1;
    p                        = rt.predictions(in);
    ASSERT_GE(p.size(), 2u);
    EXPECT_EQ(p[0].ticks, (100 + 150) / 2);
    EXPECT_EQ(p[1].ticks, 125 + 100 + 500);

    // a non-cyclic schedule stops at its end
    s.cyclic = false;
    rt.setSchedule(s, false);
    rt.predictionTicks = {300, 500, 200};
    rt.currentEntry    = 1;
    rt.state           = RuntimeState::PostTransit;
    in                 = {};
    in.currentStation  = "Station B";
    p                  = rt.predictions(in);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[1].destination, "C*");
    EXPECT_EQ(p[1].ticks, 40 + 200);
}

// ---- motion -------------------------------------------------------------------------------------

TEST(TrainMotion, MildStraightSlopesDontSlowTrains) {
    // Create's Navigation turn listener ignores straight curves on a mild slope
    tk::CurveSpec ramp;
    ramp.starts[0]  = {0.5, 0, 1};
    ramp.starts[1]  = {0.5, 2, 11};
    ramp.axes[0]    = tk::Vec3d{0, 0.2, 1}.normalize();
    ramp.axes[1]    = tk::Vec3d{0, -0.2, -1}.normalize();
    ramp.normals[0] = ramp.normals[1] = {0, 1, 0};
    EXPECT_FALSE(slowsForCurve(tk::Curve(ramp)));
    auto steep      = ramp;
    steep.starts[1] = {0.5, 4, 11};
    EXPECT_TRUE(slowsForCurve(tk::Curve(steep))) << "too steep";
    tk::CurveSpec turn;
    turn.starts[0]  = {0.5, 0, 1};
    turn.starts[1]  = {5, 0, 5.5};
    turn.axes[0]    = {0, 0, 1};
    turn.axes[1]    = {1, 0, 0};
    turn.normals[0] = turn.normals[1] = {0, 1, 0};
    EXPECT_TRUE(slowsForCurve(tk::Curve(turn)));
}

TEST(TrainMotion, WalksAStraightLineAndStopsAtTheEnd) {
    World        w;
    tk::Railways r;
    lineX(w, r, 40);
    TrackPos p = eastAt(r, 2.5);
    auto     step = walk(r, p, 10, {}, nullptr);
    EXPECT_FALSE(step.blocked);
    auto pos = positionOf(r, step.loc);
    ASSERT_TRUE(pos);
    EXPECT_NEAR(pos->x, 12.5, 1e-6);
    auto end = walk(r, step.loc, 100, {}, nullptr);
    EXPECT_TRUE(end.blocked);
    EXPECT_NEAR(positionOf(r, end.loc)->x, 40, 1e-6);
}

TEST(TrainMotion, BodyPointsFollowAndReverse) {
    World        w;
    tk::Railways r;
    lineX(w, r, 60);
    Body b;
    b.front   = eastAt(r, 20.5);
    b.offsets = {0, 2, 6, 8};
    auto pts  = b.points(r);
    ASSERT_EQ(pts.size(), 4u);
    EXPECT_NEAR(positionOf(r, pts[3])->x, 12.5, 1e-6);
    b.move(r, 5, {});
    EXPECT_NEAR(positionOf(r, b.front)->x, 25.5, 1e-6);
    b.move(r, -10, {});
    EXPECT_NEAR(positionOf(r, b.front)->x, 15.5, 1e-6);
    // backing into the dead end stops the back point at x = 0
    auto res = b.move(r, -20, {});
    EXPECT_TRUE(res.blocked);
    EXPECT_NEAR(positionOf(r, b.front)->x, 8, 1e-6);
    auto j = Body::fromJson(b.toJson());
    EXPECT_EQ(j.front, b.front);
}

TEST(TrainMotion, PathsChooseJunctionsAndFollowersTakeTheSameWay) {
    World        w;
    tk::Railways r;
    // a line along z from z = 0 to 24 at x = 0 and a curve leaving it at z = 3 towards +x
    tk::PlacementRequest req;
    req.from   = tk::selectEnd({0, 0, 2}, tk::Shape::ZO, {0, 0, 1});
    req.pos2   = {10, 0, 12};
    req.shape2 = tk::Shape::XO;
    req.look   = {1, 0, 0};
    auto plan  = tk::planConnection(req, tk::Shape::ZO);
    ASSERT_TRUE(plan.valid) << plan.message;
    ASSERT_TRUE(plan.curve);
    w.curves.add(0, *plan.curve);
    for (int z = 0; z <= 24; ++z) w.place(r, {0, 0, z}, tk::Shape::ZO);
    for (auto const& p : plan.straight)
        if (!w.trackAt(0, p.pos)) w.place(r, p.pos, p.shape);
    r.onRailAdded(w, 0, {0, 0, 2});
    ASSERT_EQ(r.graphs().size(), 1u);

    auto g = r.nearestEdge(0, {0.5, 0, 0.5});
    ASSERT_TRUE(g);
    TrackPos start = fromGraph(r, *g);
    if (start.to.location().z < start.from.location().z) start = reverse(r, start);

    // destination: the far end of the branch, travelling east
    auto dest = r.nearestEdge(0, {12.0, 0, plan.straight.back().pos.z + 0.5});
    ASSERT_TRUE(dest);
    TrackPos d = fromGraph(r, *dest);
    if (d.to.location().x < d.from.location().x) d = reverse(r, d);
    auto path = findPath(r, start, {d});
    ASSERT_TRUE(path);
    EXPECT_GE(distanceToCurve(r, start, *path, 100), 0);

    Body b;
    b.front   = start;
    b.offsets = {0, 2};
    Path nav  = *path;
    for (int i = 0; i < 40; ++i) b.move(r, 0.5, nav.chooser());
    auto front = positionOf(r, b.front);
    ASSERT_TRUE(front);
    EXPECT_GT(front->x, 3) << "the front took the branch";
    auto back = b.pointAt(r, 2);
    ASSERT_TRUE(back);
    EXPECT_GT(positionOf(r, *back)->x, 1) << "the back point followed it";

    // straight on without a path: stays on the line
    Body s;
    s.front   = start;
    s.offsets = {0};
    s.move(r, 15, {});
    EXPECT_NEAR(positionOf(r, s.front)->x, 0.5, 1e-6);
    EXPECT_NEAR(yawOf({1, 0, 0}), 0, 1e-9);
    EXPECT_NEAR(yawOf({0, 0, -1}), 90, 1e-9);
}

TEST(TrainMotion, LostWhenTheTrackIsRemoved) {
    World        w;
    tk::Railways r;
    lineX(w, r, 20);
    Body b;
    b.front   = eastAt(r, 10.5);
    b.offsets = {0, 2};
    w.remove(r, {12, 0, 0});
    auto res = b.move(r, 0.1, {});
    EXPECT_TRUE(res.lost);
}
