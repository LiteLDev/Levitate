#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <random>

#include "nlohmann/json.hpp"

#include "core/kinetics/Solver.h"

using namespace lv::core::kinetics;
using lv::core::GlobalPos;

namespace {

GlobalPos P(int x, int y, int z) { return GlobalPos{0, x, y, z}; }

BlockState S(std::string id, int axis = 1) {
    BlockState s;
    s.id = std::move(id);
    s.set("levitate:axis", axis);
    return s;
}

constexpr int X = 0, Y = 1, Z = 2;

/// Test blocks: the shapes of Create's transmission blocks plus a motor and a consumer.
class World : public ::testing::Test {
protected:
    World() : solver([this](std::string_view id) { return spec(id); }) {
        specs["shaft"] = specs::shaft();
        specs["cog"]   = specs::cogwheel(false);
        specs["large"] = specs::cogwheel(true);

        auto motor           = specs::generatorShaft(16384);
        specs["motor"]       = motor;
        auto weak            = specs::generatorShaft(8);
        specs["weak_motor"]  = weak;
        auto consumer        = specs::shaft();
        consumer.impact      = 4;
        specs["consumer"]    = consumer;
        auto heavy           = specs::shaft();
        heavy.impact         = 2000;
        specs["heavy"]       = heavy;

        // gearbox: shafts on the four faces not on its axis
        KineticSpec gearbox;
        gearbox.states          = {"levitate:axis"};
        gearbox.rotationAxis    = specs::axisFromState("levitate:axis");
        gearbox.hasShaftTowards = [](BlockState const& s, Face f) { return static_cast<int>(axisOf(f)) != s.get("levitate:axis"); };
        gearbox.axisModifier    = specs::gearboxModifier;
        specs["gearbox"]        = gearbox;

        auto gearshift         = specs::shaft();
        gearshift.states       = {"levitate:axis", "levitate:powered"};
        gearshift.axisModifier = [](NodeView const& v, Face f) {
            return specs::splitShaftModifier(v, f, v.state->get("levitate:powered") != 0, true);
        };
        specs["gearshift"] = gearshift;

        auto clutch         = specs::shaft();
        clutch.states       = {"levitate:axis", "levitate:powered"};
        clutch.axisModifier = [](NodeView const& v, Face f) {
            return specs::splitShaftModifier(v, f, v.state->get("levitate:powered") != 0, false);
        };
        specs["clutch"] = clutch;

        // a 2:1 pair like an adjustable chain gearshift: "pair_a" turns its east "pair_b" twice as fast
        auto pairA      = specs::shaft();
        pairA.propagate = [](NodeView const&, NodeView const& to, Offset d, bool, bool) {
            return (to.state->id == "pair_b" && d == Offset{1, 0, 0}) ? 2.f : 0.f;
        };
        specs["pair_a"] = pairA;
        auto pairB      = specs::shaft();
        pairB.propagate = [](NodeView const&, NodeView const& to, Offset d, bool, bool) {
            return (to.state->id == "pair_a" && d == Offset{-1, 0, 0}) ? .5f : 0.f;
        };
        specs["pair_b"] = pairB;
    }

    KineticSpec const* spec(std::string_view id) {
        auto it = specs.find(std::string(id));
        return it == specs.end() ? nullptr : &it->second;
    }

    void place(GlobalPos p, std::string id, int axis = Y) { ASSERT_TRUE(solver.add(p, S(std::move(id), axis))); }
    void motor(GlobalPos p, float rpm, int axis = Y, std::string id = "motor") {
        place(p, std::move(id), axis);
        solver.setGeneratedSpeed(p, rpm);
    }
    float speed(GlobalPos p) {
        auto n = solver.node(p);
        return n ? n->speed : NAN;
    }
    float theoretical(GlobalPos p) {
        auto n = solver.node(p);
        return n ? n->theoreticalSpeed : NAN;
    }

    /// Network invariants that hold after every operation.
    void checkInvariants() {
        solver.forEachNode([&](NodeInfo const& n) {
            EXPECT_LE(std::abs(n.theoreticalSpeed), solver.config().maxRotationSpeed) << n.pos.key();
            if (n.source) {
                auto src = solver.node(*n.source);
                ASSERT_TRUE(src.has_value()) << "source of " << n.pos.key() << " is gone";
                EXPECT_EQ(src->network, n.network) << n.pos.key();
            }
            if (n.network) {
                auto members = solver.members(*n.network);
                EXPECT_TRUE(std::binary_search(members.begin(), members.end(), n.pos)) << n.pos.key();
            }
            if (!n.source && !n.generator) EXPECT_EQ(n.theoreticalSpeed, 0) << n.pos.key();
        });
    }

    std::map<std::string, KineticSpec> specs;
    Solver                             solver;
};

} // namespace

TEST_F(World, ShaftLineCarriesTheSpeed) {
    motor(P(0, 0, 0), 16, X);
    for (int x = 1; x <= 5; ++x) place(P(x, 0, 0), "shaft", X);
    for (int x = 1; x <= 5; ++x) EXPECT_EQ(speed(P(x, 0, 0)), 16) << x;
    auto n = solver.node(P(3, 0, 0));
    ASSERT_TRUE(n && n->source);
    EXPECT_EQ(*n->source, P(2, 0, 0));
    EXPECT_EQ(*n->network, P(0, 0, 0));
    auto net = solver.networkAt(P(5, 0, 0));
    ASSERT_TRUE(net);
    EXPECT_EQ(net->members, 6);
    EXPECT_EQ(net->sources, 1);
    checkInvariants();
}

TEST_F(World, ShaftsOnlyConnectAlongTheirAxis) {
    motor(P(0, 0, 0), 16, X);
    place(P(1, 0, 0), "shaft", Z); // wrong axis: no shaft end towards the motor
    place(P(0, 1, 0), "shaft", X); // beside the motor's axis
    EXPECT_EQ(speed(P(1, 0, 0)), 0);
    EXPECT_EQ(speed(P(0, 1, 0)), 0);
}

TEST_F(World, PlacingTheSourceLastPowersTheLine) {
    for (int x = 1; x <= 4; ++x) place(P(x, 0, 0), "shaft", X);
    motor(P(0, 0, 0), -32, X);
    for (int x = 1; x <= 4; ++x) EXPECT_EQ(speed(P(x, 0, 0)), -32);
    checkInvariants();
}

TEST_F(World, BreakingAShaftStopsTheRest) {
    motor(P(0, 0, 0), 16, X);
    for (int x = 1; x <= 4; ++x) place(P(x, 0, 0), "shaft", X);
    solver.remove(P(2, 0, 0));
    EXPECT_EQ(speed(P(1, 0, 0)), 16);
    EXPECT_EQ(speed(P(3, 0, 0)), 0);
    EXPECT_EQ(speed(P(4, 0, 0)), 0);
    EXPECT_FALSE(solver.node(P(4, 0, 0))->network);
    place(P(2, 0, 0), "shaft", X);
    EXPECT_EQ(speed(P(4, 0, 0)), 16);
    checkInvariants();
}

TEST_F(World, RemovingTheMotorStopsEverything) {
    motor(P(0, 0, 0), 16, X);
    for (int x = 1; x <= 3; ++x) place(P(x, 0, 0), "shaft", X);
    solver.remove(P(0, 0, 0));
    for (int x = 1; x <= 3; ++x) EXPECT_EQ(speed(P(x, 0, 0)), 0);
    EXPECT_FALSE(solver.networkAt(P(1, 0, 0)));
}

TEST_F(World, MotorSpeedChangesPropagate) {
    motor(P(0, 0, 0), 16, X);
    place(P(1, 0, 0), "shaft", X);
    solver.setGeneratedSpeed(P(0, 0, 0), 64);
    EXPECT_EQ(speed(P(1, 0, 0)), 64);
    solver.setGeneratedSpeed(P(0, 0, 0), -8);
    EXPECT_EQ(speed(P(1, 0, 0)), -8);
    solver.setGeneratedSpeed(P(0, 0, 0), 0);
    EXPECT_EQ(speed(P(1, 0, 0)), 0);
    EXPECT_FALSE(solver.networkAt(P(0, 0, 0)));
}

TEST_F(World, SmallCogsMeshSidewaysInOppositeDirections) {
    motor(P(0, 0, 0), 16, Y);
    place(P(0, 1, 0), "cog", Y);  // on the motor's shaft
    place(P(1, 1, 0), "cog", Y);  // beside it
    place(P(1, 1, 1), "cog", Y);  // beside that one
    place(P(0, 1, 1), "cog", X);  // different axis: no mesh
    EXPECT_EQ(speed(P(0, 1, 0)), 16);
    EXPECT_EQ(speed(P(1, 1, 0)), -16);
    EXPECT_EQ(speed(P(1, 1, 1)), 16);
    EXPECT_EQ(speed(P(0, 1, 1)), 0);
    checkInvariants();
}

TEST_F(World, SmallCogsDoNotMeshDiagonally) {
    motor(P(0, 0, 0), 16, Y);
    place(P(0, 1, 0), "cog", Y);
    place(P(1, 1, 1), "cog", Y);
    EXPECT_EQ(speed(P(1, 1, 1)), 0);
}

TEST_F(World, CogsStackedAlongTheirAxisTurnTogether) {
    motor(P(0, 0, 0), 16, Y);
    place(P(0, 1, 0), "cog", Y);
    place(P(0, 2, 0), "cog", Y);
    EXPECT_EQ(speed(P(0, 2, 0)), 16);
}

TEST_F(World, LargeCogDrivesASmallCogDiagonallyAtTwiceTheSpeed) {
    motor(P(0, 0, 0), 16, Z);
    place(P(0, 0, 1), "large", Z);
    place(P(1, 1, 1), "cog", Z);  // diagonal in the cog's plane
    place(P(-1, 0, 2), "cog", Z); // not in the plane
    EXPECT_EQ(speed(P(0, 0, 1)), 16);
    EXPECT_EQ(speed(P(1, 1, 1)), -32);
    EXPECT_EQ(speed(P(-1, 0, 2)), 0);
    checkInvariants();
}

TEST_F(World, SmallCogDrivesALargeCogAtHalfTheSpeed) {
    motor(P(0, 0, 0), 32, X);
    place(P(1, 0, 0), "cog", X);
    place(P(1, 1, 1), "large", X);
    EXPECT_EQ(speed(P(1, 1, 1)), -16);
    EXPECT_FLOAT_EQ(solver.conveyedSpeed(P(1, 1, 1), P(1, 0, 0)), 32);
}

TEST_F(World, PerpendicularLargeCogsMesh) {
    motor(P(0, 0, 0), 16, Y);
    place(P(0, 1, 0), "large", Y);
    place(P(1, 2, 0), "large", X); // diagonal in the plane of both axes
    EXPECT_EQ(std::abs(speed(P(1, 2, 0))), 16);
    // Create: sourceAxisDiff = dy = 1 (> 0), targetAxisDiff = dx = 1 (> 0) -> same -> modifier 1
    EXPECT_EQ(speed(P(1, 2, 0)), 16);
    place(P(-1, 2, 0), "large", X); // dy = 1 > 0, dx = -1 -> opposite
    EXPECT_EQ(speed(P(-1, 2, 0)), -16);
    checkInvariants();
}

TEST_F(World, ParallelLargeCogsDoNotMesh) {
    motor(P(0, 0, 0), 16, Y);
    place(P(0, 1, 0), "large", Y);
    place(P(1, 1, 1), "large", Y);
    place(P(2, 1, 0), "large", Y);
    EXPECT_EQ(speed(P(1, 1, 1)), 0);
    EXPECT_EQ(speed(P(2, 1, 0)), 0);
}

TEST_F(World, OppositeSourcesBreakTheBlockWhereTheyMeet) {
    motor(P(0, 0, 0), 16, X);
    motor(P(4, 0, 0), -16, X);
    for (int x = 1; x <= 2; ++x) place(P(x, 0, 0), "shaft", X);
    place(P(3, 0, 0), "shaft", X);
    auto destroyed = solver.takeDestroyed();
    ASSERT_EQ(destroyed.size(), 1u);
    EXPECT_EQ(destroyed[0].pos, P(3, 0, 0));
    EXPECT_EQ(destroyed[0].reason, DestroyReason::Conflict);
    EXPECT_FALSE(solver.contains(P(3, 0, 0)));
    EXPECT_EQ(speed(P(2, 0, 0)), 16);
    EXPECT_EQ(speed(P(4, 0, 0)), -16);
    checkInvariants();
}

TEST_F(World, MeshingCogsWithTheSameDirectionConflict) {
    // two motors spin two meshing cogs the same way: the meshing cog that is placed last breaks
    motor(P(0, 0, 0), 16, Y);
    motor(P(1, 0, 0), 16, Y);
    place(P(0, 1, 0), "cog", Y);
    place(P(1, 1, 0), "cog", Y);
    auto destroyed = solver.takeDestroyed();
    ASSERT_FALSE(destroyed.empty());
    EXPECT_EQ(destroyed.back().reason, DestroyReason::Conflict);
    checkInvariants();
}

TEST_F(World, TheFasterSourceOverpowersTheSlowerOne) {
    motor(P(0, 0, 0), 16, X);
    motor(P(3, 0, 0), 64, X, "weak_motor");
    place(P(1, 0, 0), "shaft", X);
    place(P(2, 0, 0), "shaft", X);
    EXPECT_TRUE(solver.takeDestroyed().empty());
    EXPECT_EQ(speed(P(1, 0, 0)), 64);
    // the slow motor turns at the fast one's speed, in the fast one's network, still adding capacity
    auto slow = solver.node(P(0, 0, 0));
    ASSERT_TRUE(slow);
    EXPECT_EQ(slow->theoreticalSpeed, 64);
    EXPECT_EQ(slow->source, P(1, 0, 0));
    auto net = solver.networkAt(P(0, 0, 0));
    ASSERT_TRUE(net);
    EXPECT_EQ(net->id, P(3, 0, 0));
    EXPECT_EQ(net->sources, 2);
    EXPECT_DOUBLE_EQ(net->capacity, 8 * 64 + 16384 * 16);
    checkInvariants();
}

TEST_F(World, OverpoweredGeneratorRestartsWhenTheOtherSourceGoes) {
    motor(P(0, 0, 0), 16, X);
    place(P(1, 0, 0), "shaft", X);
    motor(P(2, 0, 0), 64, X);
    EXPECT_EQ(theoretical(P(0, 0, 0)), 64);
    solver.remove(P(2, 0, 0));
    EXPECT_EQ(theoretical(P(1, 0, 0)), 0);
    solver.tick(); // reActivateSource
    EXPECT_EQ(speed(P(0, 0, 0)), 16);
    EXPECT_EQ(speed(P(1, 0, 0)), 16);
    checkInvariants();
}

TEST_F(World, SpeedAboveTheLimitBreaksTheCog) {
    motor(P(0, 0, 0), 200, Z);
    place(P(0, 0, 1), "large", Z);
    place(P(1, 1, 1), "cog", Z); // would turn at 400 RPM
    auto destroyed = solver.takeDestroyed();
    ASSERT_EQ(destroyed.size(), 1u);
    EXPECT_EQ(destroyed[0].reason, DestroyReason::TooFast);
    checkInvariants();
}

TEST_F(World, AGearRatioLoopBreaks) {
    // a ring of gearboxes with a 2:1 pair: the loop would speed itself up
    place(P(0, 0, 0), "gearbox", Y);
    place(P(1, 0, 0), "pair_a", X);
    place(P(2, 0, 0), "pair_b", X);
    place(P(3, 0, 0), "gearbox", Y);
    place(P(3, 0, 1), "shaft", Z);
    place(P(3, 0, 2), "gearbox", Y);
    place(P(2, 0, 2), "shaft", X);
    place(P(1, 0, 2), "shaft", X);
    place(P(0, 0, 2), "gearbox", Y);
    place(P(0, 0, 1), "shaft", Z);
    motor(P(-1, 0, 0), 16, X);
    auto destroyed = solver.takeDestroyed();
    ASSERT_FALSE(destroyed.empty());
    bool loop = false;
    for (auto const& d : destroyed) loop |= d.reason == DestroyReason::RatioLoop;
    EXPECT_TRUE(loop);
    checkInvariants();
}

TEST_F(World, AGearboxRingWithoutRatioIsStable) {
    place(P(0, 0, 0), "gearbox", Y);
    place(P(1, 0, 0), "shaft", X);
    place(P(2, 0, 0), "shaft", X);
    place(P(3, 0, 0), "gearbox", Y);
    place(P(3, 0, 1), "shaft", Z);
    place(P(3, 0, 2), "gearbox", Y);
    place(P(2, 0, 2), "shaft", X);
    place(P(1, 0, 2), "shaft", X);
    place(P(0, 0, 2), "gearbox", Y);
    place(P(0, 0, 1), "shaft", Z);
    motor(P(-1, 0, 0), 16, X);
    EXPECT_TRUE(solver.takeDestroyed().empty());
    // Create's gearbox: the opposite face turns the other way, perpendicular faces by direction
    EXPECT_EQ(speed(P(0, 0, 0)), 16);
    EXPECT_EQ(speed(P(1, 0, 0)), -16);
    EXPECT_EQ(speed(P(0, 0, 1)), 16);
    for (auto const& p : {P(1, 0, 0), P(2, 0, 0), P(3, 0, 1), P(2, 0, 2), P(1, 0, 2), P(0, 0, 1)}) {
        EXPECT_NE(speed(p), 0) << p.key();
    }
    checkInvariants();
}

TEST_F(World, GearboxTurnsPerpendicularFacesByDirection) {
    motor(P(-1, 0, 0), 16, X); // source on the west
    place(P(0, 0, 0), "gearbox", Y);
    place(P(0, 0, -1), "shaft", Z); // north: negative like west -> reversed
    place(P(0, 0, 1), "shaft", Z);  // south: positive -> same
    place(P(1, 0, 0), "shaft", X);  // east, opposite the source -> reversed
    EXPECT_EQ(speed(P(0, 0, -1)), -16);
    EXPECT_EQ(speed(P(0, 0, 1)), 16);
    EXPECT_EQ(speed(P(1, 0, 0)), -16);
}

TEST_F(World, PoweredGearshiftReversesTheFarSide) {
    motor(P(0, 0, 0), 16, X);
    place(P(1, 0, 0), "gearshift", X);
    place(P(2, 0, 0), "shaft", X);
    EXPECT_EQ(speed(P(2, 0, 0)), 16);
    auto powered = S("gearshift", X);
    powered.set("levitate:powered", 1);
    solver.update(P(1, 0, 0), powered);
    EXPECT_EQ(speed(P(1, 0, 0)), 16);
    EXPECT_EQ(speed(P(2, 0, 0)), -16);
    EXPECT_TRUE(solver.takeDestroyed().empty());
    checkInvariants();
}

TEST_F(World, PoweredClutchDisconnectsTheFarSide) {
    motor(P(0, 0, 0), 16, X);
    place(P(1, 0, 0), "clutch", X);
    place(P(2, 0, 0), "shaft", X);
    EXPECT_EQ(speed(P(2, 0, 0)), 16);
    auto powered = S("clutch", X);
    powered.set("levitate:powered", 1);
    solver.update(P(1, 0, 0), powered);
    EXPECT_EQ(speed(P(1, 0, 0)), 16);
    EXPECT_EQ(speed(P(2, 0, 0)), 0);
    solver.update(P(1, 0, 0), S("clutch", X));
    EXPECT_EQ(speed(P(2, 0, 0)), 16);
    checkInvariants();
}

TEST_F(World, AxisChangeReattaches) {
    motor(P(0, 0, 0), 16, X);
    place(P(1, 0, 0), "shaft", X);
    place(P(2, 0, 0), "shaft", X);
    solver.update(P(1, 0, 0), S("shaft", Z)); // wrenched sideways
    EXPECT_EQ(speed(P(1, 0, 0)), 0);
    EXPECT_EQ(speed(P(2, 0, 0)), 0);
    solver.update(P(1, 0, 0), S("shaft", X));
    EXPECT_EQ(speed(P(2, 0, 0)), 16);
}

TEST_F(World, StressAndOverstress) {
    motor(P(0, 0, 0), 16, X, "weak_motor"); // 8 SU/RPM * 16 = 128 SU
    place(P(1, 0, 0), "consumer", X);       // 4 SU/RPM * 16 = 64 SU
    auto net = solver.networkAt(P(1, 0, 0));
    ASSERT_TRUE(net);
    EXPECT_DOUBLE_EQ(net->stress, 64);
    EXPECT_DOUBLE_EQ(net->capacity, 128);
    EXPECT_FALSE(net->overstressed);
    EXPECT_EQ(stressLevel(net->stress, net->capacity), StressLevel::Low);

    place(P(2, 0, 0), "consumer", X); // 128 of 128: still fine (overstressed when stress > capacity)
    EXPECT_FALSE(solver.networkAt(P(1, 0, 0))->overstressed);
    (void)solver.takeNetworkEvents();
    place(P(3, 0, 0), "consumer", X); // 192 of 128
    net = solver.networkAt(P(1, 0, 0));
    EXPECT_TRUE(net->overstressed);
    EXPECT_EQ(speed(P(2, 0, 0)), 0);
    EXPECT_EQ(theoretical(P(2, 0, 0)), 16);
    EXPECT_EQ(speed(P(0, 0, 0)), 0);
    auto events = solver.takeNetworkEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].overstressed);

    solver.remove(P(3, 0, 0));
    EXPECT_FALSE(solver.networkAt(P(1, 0, 0))->overstressed);
    EXPECT_EQ(speed(P(2, 0, 0)), 16);
    events = solver.takeNetworkEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_FALSE(events[0].overstressed);
}

TEST_F(World, StressScalesWithSpeed) {
    motor(P(0, 0, 0), 32, X, "weak_motor"); // 256 SU
    place(P(1, 0, 0), "consumer", X);       // 4 * 32 = 128
    EXPECT_DOUBLE_EQ(solver.networkAt(P(1, 0, 0))->stress, 128);
    solver.setImpactOverride(P(1, 0, 0), 10.0);
    EXPECT_DOUBLE_EQ(solver.networkAt(P(1, 0, 0))->stress, 320);
    EXPECT_TRUE(solver.networkAt(P(1, 0, 0))->overstressed);
    solver.setImpactOverride(P(1, 0, 0), std::nullopt);
    EXPECT_FALSE(solver.networkAt(P(1, 0, 0))->overstressed);
}

TEST_F(World, DisabledStressNeverOverstresses) {
    SolverConfig cfg;
    cfg.disableStress = true;
    solver.setConfig(cfg);
    motor(P(0, 0, 0), 16, X, "weak_motor");
    place(P(1, 0, 0), "heavy", X);
    EXPECT_FALSE(solver.networkAt(P(1, 0, 0))->overstressed);
    EXPECT_EQ(speed(P(1, 0, 0)), 16);
}

TEST_F(World, ConfigProvidesTheStressTable) {
    SolverConfig cfg;
    cfg.impact = [](KineticSpec const& s, BlockState const& st) { return st.id == "consumer" ? 100.0 : s.impact; };
    solver.setConfig(cfg);
    motor(P(0, 0, 0), 16, X, "weak_motor");
    place(P(1, 0, 0), "consumer", X);
    EXPECT_DOUBLE_EQ(solver.networkAt(P(1, 0, 0))->stress, 1600);
    EXPECT_TRUE(solver.networkAt(P(1, 0, 0))->overstressed);
}

TEST_F(World, FlickeringBreaks) {
    motor(P(0, 0, 0), 16, X);
    place(P(1, 0, 0), "shaft", X);
    bool broke = false;
    for (int i = 0; i < 100 && !broke; ++i) {
        solver.setGeneratedSpeed(P(0, 0, 0), i % 2 ? 16.f : -16.f);
        for (auto const& d : solver.takeDestroyed()) broke |= d.reason == DestroyReason::Flicker;
    }
    EXPECT_TRUE(broke);
    // the flicker score decays by one per tick
    for (int i = 0; i < 400; ++i) solver.tick();
    solver.forEachNode([](NodeInfo const& n) { EXPECT_EQ(n.flicker, 0); });
}

TEST_F(World, ChangesAreReported) {
    motor(P(0, 0, 0), 16, X);
    (void)solver.takeChanges();
    place(P(1, 0, 0), "shaft", X);
    auto changes = solver.takeChanges();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_TRUE(changes[0].added);
    EXPECT_EQ(changes[0].speed, 16);
    EXPECT_EQ(changes[0].source, P(0, 0, 0));
    solver.remove(P(0, 0, 0));
    changes = solver.takeChanges();
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_TRUE(changes[0].removed);
    EXPECT_EQ(changes[1].prevSpeed, 16);
    EXPECT_EQ(changes[1].speed, 0);
    EXPECT_TRUE(solver.takeChanges().empty());
}

TEST_F(World, RecordsRoundTrip) {
    motor(P(0, 0, 0), 16, X, "weak_motor");
    place(P(1, 0, 0), "consumer", X);
    place(P(2, 0, 0), "cog", X);
    place(P(2, 1, 0), "cog", X);
    solver.setImpactOverride(P(1, 0, 0), 2.5);

    std::vector<nlohmann::json> saved;
    solver.forEachNode([&](NodeInfo const& n) { saved.push_back(solver.record(n.pos)); });

    Solver loaded([this](std::string_view id) { return spec(id); });
    for (auto const& j : saved) loaded.restore(j.get<NodeRecord>());
    loaded.finishRestore();
    EXPECT_TRUE(loaded.takeDestroyed().empty());
    EXPECT_EQ(loaded.node(P(2, 1, 0))->speed, -16);
    EXPECT_EQ(loaded.node(P(1, 0, 0))->source, P(0, 0, 0));
    EXPECT_DOUBLE_EQ(loaded.node(P(1, 0, 0))->impact, 2.5);
    auto net = loaded.networkAt(P(2, 1, 0));
    ASSERT_TRUE(net);
    EXPECT_DOUBLE_EQ(net->stress, 2.5 * 16);
    EXPECT_DOUBLE_EQ(net->capacity, 8 * 16);
    // the loaded solver keeps working
    loaded.remove(P(0, 0, 0));
    EXPECT_EQ(loaded.node(P(2, 1, 0))->speed, 0);
}

TEST_F(World, RestoreDropsStaleSources) {
    NodeRecord r;
    r.pos     = P(5, 0, 0);
    r.state   = S("shaft", X);
    r.speed   = 16;
    r.source  = P(4, 0, 0); // never saved
    r.network = P(4, 0, 0);
    solver.restore(r);
    solver.finishRestore();
    EXPECT_EQ(speed(P(5, 0, 0)), 0);
    EXPECT_FALSE(solver.node(P(5, 0, 0))->network);
}

TEST_F(World, ValidationStopsOrphans) {
    NodeRecord r;
    r.pos   = P(5, 0, 0);
    r.state = S("shaft", X);
    r.speed = 16; // no source: Create's validateKinetics zeroes it
    solver.restore(r);
    for (int i = 0; i < 61; ++i) solver.tick();
    EXPECT_EQ(theoretical(P(5, 0, 0)), 0);
}

TEST_F(World, UnknownBlocksAreIgnored) {
    EXPECT_FALSE(solver.add(P(0, 0, 0), S("stone")));
    EXPECT_FALSE(solver.contains(P(0, 0, 0)));
}

TEST_F(World, RandomEditsKeepTheNetworkConsistent) {
    std::mt19937                       rng(20260521);
    std::vector<std::string>           kinds = {"shaft", "cog", "large", "gearbox", "consumer", "motor", "gearshift"};
    std::uniform_int_distribution<int> coord(0, 5), kind(0, static_cast<int>(kinds.size()) - 1), axis(0, 2),
        op(0, 9), rpm(-64, 64);
    for (int i = 0; i < 3000; ++i) {
        GlobalPos p = P(coord(rng), coord(rng), coord(rng));
        int       o = op(rng);
        if (o < 6) {
            auto k = kinds[kind(rng)];
            auto s = S(k, axis(rng));
            if (k == "gearshift") s.set("levitate:powered", axis(rng) == 0);
            solver.add(p, s);
            if (k == "motor") solver.setGeneratedSpeed(p, static_cast<float>(rpm(rng)));
        } else if (o < 8) {
            solver.remove(p);
        } else if (o < 9) {
            solver.setGeneratedSpeed(p, static_cast<float>(rpm(rng)));
        } else {
            solver.tick();
        }
        (void)solver.takeDestroyed();
        (void)solver.takeChanges();
        checkInvariants();
        if (HasFailure()) {
            ADD_FAILURE() << "after step " << i;
            return;
        }
    }
}

// ---- helpers ------------------------------------------------------------------------------------------------

TEST(Kinetics, NearestFaceFollowsCreate) {
    EXPECT_EQ(nearestFace(1, 0, 0), Face::East);
    EXPECT_EQ(nearestFace(0, -3, 0), Face::Down);
    EXPECT_EQ(nearestFace(1, 1, 0), Face::Up);   // tie: first in Direction order
    EXPECT_EQ(nearestFace(-1, 0, -1), Face::North);
    EXPECT_EQ(nearestFace(0, 0, 0), Face::North);
    EXPECT_EQ(opposite(Face::West), Face::East);
    EXPECT_EQ(opposite(Face::Down), Face::Up);
}

TEST(Kinetics, SpeedLevels) {
    EXPECT_EQ(speedLevel(0), SpeedLevel::None);
    EXPECT_EQ(speedLevel(16), SpeedLevel::Slow);
    EXPECT_EQ(speedLevel(-30), SpeedLevel::Medium);
    EXPECT_EQ(speedLevel(100), SpeedLevel::Fast);
    EXPECT_EQ(stressLevel(0, 0), StressLevel::Low);
    EXPECT_EQ(stressLevel(60, 100), StressLevel::Medium);
    EXPECT_EQ(stressLevel(80, 100), StressLevel::High);
    EXPECT_EQ(stressLevel(101, 100), StressLevel::Overstressed);
}

TEST(Kinetics, RotationOffsetsLetCogsMesh) {
    // neighbours in the plane of the axis alternate between 0 and 22.5 degrees
    EXPECT_FLOAT_EQ(rotationOffset(P(0, 5, 0), Axis::Y, false), 22.5f);
    EXPECT_FLOAT_EQ(rotationOffset(P(1, 5, 0), Axis::Y, false), 0.f);
    EXPECT_FLOAT_EQ(rotationOffset(P(-1, 5, 0), Axis::Y, false), 0.f);
    EXPECT_FLOAT_EQ(rotationOffset(P(1, 9, 1), Axis::Y, false), 22.5f);
    EXPECT_FLOAT_EQ(rotationOffset(P(1, 0, 0), Axis::Y, true), 11.25f);
    EXPECT_FLOAT_EQ(rotationOffset(P(1, 1, 0), Axis::Z, true, 5), 27.5f);
    EXPECT_FLOAT_EQ(rotationAngle(20, 60, 0), 360.f * 0); // 60 RPM = one turn per 20 ticks
    EXPECT_FLOAT_EQ(rotationAngle(10, 60, 0), 180.f);
    EXPECT_FLOAT_EQ(rotationAngle(1, 16, 22.5f), 16 * .3f + 22.5f);
}

TEST(Kinetics, DefaultRotorParts) {
    auto       cog = specs::cogwheel(true);
    BlockState s   = S("large", Z);
    std::vector<RotorPart> parts;
    cog.rotorParts(RenderView{&s, 8, std::nullopt}, parts);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].rotorValue, 2);
    EXPECT_EQ(parts[0].axis, Axis::Z);
    EXPECT_TRUE(parts[0].largeCog);
    EXPECT_EQ(defaultStressKey("levitate:mechanical_press"), "mechanical_press");
}
