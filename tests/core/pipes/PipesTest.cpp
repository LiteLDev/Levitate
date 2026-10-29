#include <gtest/gtest.h>

#include <map>

#include "core/pipes/Flow.h"
#include "core/pipes/Rules.h"

using namespace lv::core::pipes;

namespace {

constexpr FaceMask kX = axisMask(Axis::X);

// ---- a small test world ------------------------------------------------------------------------------

struct Block {
    enum Type { Air, Solid, Pipe, Pump, Tank, Water } type = Air;
    FaceMask mask     = 0;
    Face     front    = Face::East;
    float    speed    = 0;
    Fluid    contents;
    int      capacity = 0;
    bool     closed   = false; ///< a closed valve
};

struct TestWorld : Host {
    std::map<GlobalPos, Block>     blocks;
    int                            range = 16;
    std::vector<std::pair<Fluid, Fluid>> collisions;
    std::map<BlockFace, int>       poured; ///< mB pushed out of open ends

    Block const& at(GlobalPos const& p) {
        static Block air;
        auto it = blocks.find(p);
        return it == blocks.end() ? air : it->second;
    }
    bool loaded(GlobalPos const& p) override { return p.y > -64; }
    Kind kind(GlobalPos const& p) override {
        auto t = at(p).type;
        return t == Block::Pipe ? Kind::Pipe : t == Block::Pump ? Kind::Pump : Kind::None;
    }
    bool canHaveFlowToward(GlobalPos const& p, Face f) override {
        auto const& b = at(p);
        if (b.type == Block::Pipe) return has(b.mask, f);
        if (b.type == Block::Pump) return axisOf(b.front) == axisOf(f);
        return false;
    }
    bool  canPullFluidFrom(GlobalPos const& p, Face, Fluid const&) override { return !at(p).closed; }
    Face  pumpFront(GlobalPos const& p) override { return at(p).front; }
    float pumpSpeed(GlobalPos const& p) override { return at(p).speed; }
    int   pumpRange() override { return range; }
    bool  isOpenEnd(GlobalPos const& pipe, Face side) override {
        auto t = at(shifted(pipe, side)).type;
        return t == Block::Air || t == Block::Water;
    }
    bool hasHandler(GlobalPos const& p, Face) override { return at(p).type == Block::Tank; }
    std::vector<Fluid> handlerTanks(GlobalPos const& p, Face) override { return {at(p).contents}; }
    int handlerFill(GlobalPos const& p, Face, Fluid const& f, bool simulate) override {
        auto& b = blocks[p];
        if (b.type != Block::Tank || f.empty()) return 0;
        if (!b.contents.empty() && b.contents.id != f.id) return 0;
        int n = std::min(f.amount, b.capacity - std::max(0, b.contents.amount));
        if (!simulate && n > 0) b.contents = {f.id, std::max(0, b.contents.amount) + n};
        return n;
    }
    Fluid handlerDrain(GlobalPos const& p, Face, Fluid const& req, bool simulate) override {
        auto& b = blocks[p];
        if (b.type != Block::Tank || b.contents.empty()) return {};
        if (!req.id.empty() && req.id != b.contents.id) return {};
        int   n   = std::min(req.amount, b.contents.amount);
        Fluid out = {b.contents.id, n};
        if (!simulate) b.contents.amount -= n;
        if (b.contents.amount <= 0) b.contents = simulate ? b.contents : Fluid{};
        return n > 0 ? out : Fluid{};
    }
    int openEndFill(BlockFace const& face, OpenEnd&, Fluid const& f, bool simulate) override {
        if (at(face.connected()).type != Block::Air) return 0;
        if (!simulate) poured[face] += f.amount;
        return f.amount;
    }
    Fluid openEndDrain(BlockFace const& face, OpenEnd&, Fluid const& req, bool) override {
        if (at(face.connected()).type != Block::Water) return {};
        if (!req.id.empty() && req.id != "minecraft:water") return {};
        return {"minecraft:water", std::min(req.amount, 1000)}; // an infinite source
    }
    void onFlowCollision(GlobalPos const&, Fluid const& a, Fluid const& b) override { collisions.emplace_back(a, b); }

    void pipe(GlobalPos const& p, FaceMask m) { blocks[p] = {Block::Pipe, m}; }
    void tank(GlobalPos const& p, Fluid f, int cap) { blocks[p] = {Block::Tank, 0, Face::East, 0, f, cap}; }
    void pump(GlobalPos const& p, Face front, float speed) { blocks[p] = {Block::Pump, 0, front, speed}; }
};

GlobalPos at(int x, int y = 0, int z = 0) { return {0, x, y, z}; }

void run(Model& m, int ticks, std::int64_t& t) {
    for (int i = 0; i < ticks; ++i) m.tick(++t);
}

/// tank(0) - pipes 1..3 - pump(4, facing east) - pipes 5..7 - tank(8)
struct PipeLine : ::testing::Test {
    TestWorld    w;
    Model        m{w};
    std::int64_t t = 0;
    void SetUp() override {
        w.tank(at(0), {"minecraft:water", 8000}, 8000);
        for (int x : {1, 2, 3, 5, 6, 7}) w.pipe(at(x), kX);
        w.tank(at(8), {}, 8000);
        w.pump(at(4), Face::East, 64);
        m.pumpSpeedChanged(at(4), 0, 64);
    }
};

} // namespace

// ---- rules -------------------------------------------------------------------------------------------

TEST(PipeRules, SingleConnectionExtendsStraight) {
    std::array<bool, 6> can{};
    can[int(Face::North)] = true;
    auto m                = updatePipeConnections(0, can, Face::Up, std::nullopt);
    EXPECT_EQ(m, bit(Face::North) | bit(Face::South));
}

TEST(PipeRules, NoConnectionKeepsStraightOrUsesPreferred) {
    std::array<bool, 6> none{};
    EXPECT_EQ(updatePipeConnections(kX, none, Face::Up, std::nullopt), kX);
    EXPECT_EQ(updatePipeConnections(0, none, Face::South, std::nullopt), axisMask(Axis::Z));
    // any previous two-way pipe (a corner too) is kept; an end opens along the preferred direction
    FaceMask corner = bit(Face::North) | bit(Face::East);
    EXPECT_EQ(updatePipeConnections(corner, none, Face::Down, std::nullopt), corner);
    EXPECT_EQ(updatePipeConnections(kX | bit(Face::Up), none, Face::Down, std::nullopt), axisMask(Axis::Y));
}

TEST(PipeRules, IgnoredFaceKeepsItsValue) {
    std::array<bool, 6> can{};
    can[int(Face::Up)] = true;
    FaceMask prev      = bit(Face::West) | bit(Face::East);
    // the east face is ignored (changed neighbour opposite): stays open
    auto m = updatePipeConnections(prev, can, Face::West, Face::East);
    EXPECT_EQ(m, bit(Face::Up) | bit(Face::East));
}

TEST(PipeRules, StraightCasingAndCorners) {
    EXPECT_EQ(straightAxis(kX), Axis::X);
    EXPECT_FALSE(straightAxis(bit(Face::East) | bit(Face::Up)));
    EXPECT_TRUE(drawsCasing(kX | bit(Face::Up)));
    EXPECT_FALSE(drawsCasing(bit(Face::East) | bit(Face::Up)));
    EXPECT_TRUE(cornerOrEnd(bit(Face::East) | bit(Face::Up)));
    EXPECT_FALSE(cornerOrEnd(kX));
}

TEST(PipeRules, WrenchMakesGlassAlongAxis) {
    // straight along x: clicking a side face makes a glass pipe, clicking an end passes
    EXPECT_EQ(wrenchGlassAxis(kX, Face::Up, 0, .5, 0), Axis::X);
    EXPECT_FALSE(wrenchGlassAxis(kX, Face::East, .5, 0, 0));
    // a corner: the axis of the neighbour nearest to the click, not the clicked face's axis
    FaceMask corner = bit(Face::East) | bit(Face::Up);
    EXPECT_EQ(wrenchGlassAxis(corner, Face::North, .3, .05, -.5), Axis::X);
    EXPECT_EQ(wrenchGlassAxis(corner, Face::North, .05, .4, -.5), Axis::Y);
}

TEST(PipeRules, RegularPipeFromGlass) {
    std::array<bool, 6> can{};
    EXPECT_EQ(regularPipeFromAxis(Axis::Z, can), axisMask(Axis::Z));
    can[int(Face::Up)] = true;
    EXPECT_EQ(regularPipeFromAxis(Axis::Z, can), bit(Face::Up) | bit(Face::Down));
}

TEST(PipeRules, PumpPlacement) {
    std::array<bool, 6> none{};
    // faces the player (away when sneaking)
    EXPECT_EQ(pumpPlacement(Face::East, false, std::nullopt, none), Face::West);
    EXPECT_EQ(pumpPlacement(Face::East, true, std::nullopt, none), Face::East);
    // a shaft facing it on the north side
    EXPECT_EQ(pumpPlacement(Face::East, false, Face::North, none), Face::South);
    // a pipe perpendicular to the look: faces that pipe
    std::array<bool, 6> pipeUp{};
    pipeUp[int(Face::Up)] = true;
    EXPECT_EQ(pumpPlacement(Face::East, false, std::nullopt, pipeUp), Face::Up);
    EXPECT_EQ(pumpPlacement(Face::East, true, std::nullopt, pipeUp), Face::East);
}

TEST(PipeRules, ValveAxes) {
    for (Face f : kFaces)
        for (bool along : {false, true}) {
            auto a = valveAxes(f, along);
            EXPECT_NE(a.pipe, axisOf(f));
            EXPECT_NE(a.shaft, axisOf(f));
            EXPECT_NE(a.pipe, a.shaft);
        }
    std::array<bool, 6> none{};
    std::array<bool, 6> pipes{};
    pipes[int(Face::North)] = pipes[int(Face::South)] = true;
    // looking east at a north-south pipe line: the valve faces west and its pipe runs along z
    auto p = valvePlacement(Face::East, Face::East, false, none, pipes);
    EXPECT_EQ(p.facing, Face::West);
    EXPECT_EQ(valveAxes(p.facing, p.alongFirst).pipe, Axis::Z);
    auto w = valveWrenched(p, Face::West);
    EXPECT_EQ(w.facing, Face::West);
    EXPECT_NE(w.alongFirst, p.alongFirst);
}

TEST(PipeRules, SmartPipePlacement) {
    std::array<bool, 6> none{};
    auto                floor = smartPipePlacement(Face::Down, Face::East, 1, 0, none);
    EXPECT_EQ(floor.face, AttachFace::Floor);
    EXPECT_EQ(smartPipeAxis(floor), Axis::X);
    auto wall = smartPipePlacement(Face::North, Face::North, 0, -1, none);
    EXPECT_EQ(wall.face, AttachFace::Wall);
    EXPECT_EQ(smartPipeAxis(wall), Axis::Y);
    std::array<bool, 6> zPipe{};
    zPipe[int(Face::South)] = true;
    auto aligned            = smartPipePlacement(Face::North, Face::North, 0, -1, zPipe);
    EXPECT_EQ(smartPipeAxis(aligned), Axis::Z);
    std::array<bool, 6> xPipe{};
    xPipe[int(Face::East)] = true;
    // looking mostly north, slightly west: the pipe faces west (the end along the look)
    auto west = smartPipePlacement(Face::North, Face::North, -.2, -.9, xPipe);
    EXPECT_EQ(west.facing, Face::West);
}

// ---- flow model -----------------------------------------------------------------------------------------

TEST(PipeFlow, SpeedFormulas) {
    EXPECT_FLOAT_EQ(flowSpeed(0), 1.f / 32);
    EXPECT_FLOAT_EQ(flowSpeed(128), 1.f);
    EXPECT_FLOAT_EQ(flowSpeed(256), 1.f);
    EXPECT_EQ(transferSpeedFor(64), 32);
    EXPECT_EQ(transferSpeedFor(1), 1);
    EXPECT_EQ(transferSpeedFor(256), 128);
}

TEST_F(PipeLine, PumpPutsPressureOnBothSides) {
    run(m, 2, t);
    // behind the pump: inbound pressure towards the source; in front: outbound
    auto const* back = m.node(at(1));
    ASSERT_TRUE(back);
    ASSERT_TRUE(back->at(Face::West));
    EXPECT_FLOAT_EQ(back->at(Face::West)->pressureIn, 64);
    auto const* front = m.node(at(7));
    ASSERT_TRUE(front);
    EXPECT_FLOAT_EQ(front->at(Face::East)->pressureOut, 64);
}

TEST_F(PipeLine, MovesFluidFromTankToTank) {
    run(m, 200, t);
    int moved = w.blocks[at(8)].contents.amount;
    EXPECT_GT(moved, 0);
    EXPECT_EQ(moved + w.blocks[at(0)].contents.amount, 8000); // nothing lost
    // steady state: max(1, 64 / 2) mB per tick
    int before = moved;
    run(m, 10, t);
    EXPECT_EQ(w.blocks[at(8)].contents.amount - before, 320);
    EXPECT_EQ(m.visibleFluid(at(6)).id, "minecraft:water");
}

TEST_F(PipeLine, StoppingThePumpStopsTheTransfer) {
    run(m, 200, t);
    w.blocks[at(4)].speed = 0;
    m.pumpSpeedChanged(at(4), 64, 0);
    run(m, 20, t);
    int a = w.blocks[at(8)].contents.amount;
    run(m, 20, t);
    EXPECT_EQ(w.blocks[at(8)].contents.amount, a);
}

TEST_F(PipeLine, ReversedPumpMovesBack) {
    run(m, 200, t);
    w.blocks[at(4)].front = Face::West;
    m.pumpUpdatePressure(at(4));
    run(m, 100, t);
    int a = w.blocks[at(0)].contents.amount;
    run(m, 10, t);
    EXPECT_EQ(w.blocks[at(0)].contents.amount - a, 320);
}

TEST_F(PipeLine, SaveAndLoad) {
    run(m, 200, t);
    auto  j = m.save();
    Model copy{w};
    copy.load(j);
    ASSERT_TRUE(copy.node(at(6)));
    EXPECT_FLOAT_EQ(copy.node(at(6))->at(Face::East)->pressureOut, 64);
    EXPECT_TRUE(copy.node(at(6))->at(Face::East)->flow.has_value());
    EXPECT_TRUE(copy.pumps().contains(at(4)));
    // keeps working after the load
    int a = w.blocks[at(8)].contents.amount;
    run(copy, 40, t);
    EXPECT_GT(w.blocks[at(8)].contents.amount, a);
}

TEST(PipeFlow, BranchesSplitPressureAndOutput) {
    TestWorld    w;
    Model        m{w};
    std::int64_t t = 0;
    // water tank - pipe - pump(east) - pipe - T junction - two tanks north and south
    w.tank(at(0), {"minecraft:water", 64000}, 64000);
    w.pipe(at(1), kX);
    w.pump(at(2), Face::East, 128);
    w.pipe(at(3), kX);
    w.pipe(at(4), bit(Face::West) | bit(Face::North) | bit(Face::South));
    w.pipe(at(4, 0, -1), axisMask(Axis::Z));
    w.pipe(at(4, 0, 1), axisMask(Axis::Z));
    w.tank(at(4, 0, -2), {}, 64000);
    w.tank(at(4, 0, 2), {}, 64000);
    m.pumpSpeedChanged(at(2), 0, 128);
    run(m, 300, t);
    // the junction's three faces share the pressure: 128 / (3 - 1)
    EXPECT_FLOAT_EQ(m.node(at(4))->at(Face::North)->pressureOut, 64);
    int a = w.blocks[at(4, 0, -2)].contents.amount, b = w.blocks[at(4, 0, 2)].contents.amount;
    EXPECT_GT(a, 0);
    EXPECT_GT(b, 0);
    EXPECT_LE(std::abs(a - b), 64);
}

TEST(PipeFlow, PumpRangeLimitsPressure) {
    TestWorld    w;
    Model        m{w};
    std::int64_t t = 0;
    w.pump(at(0), Face::East, 32);
    for (int x = 1; x <= 30; ++x) w.pipe(at(x), kX);
    m.pumpSpeedChanged(at(0), 0, 32);
    run(m, 3, t);
    EXPECT_TRUE(m.node(at(15)) && m.node(at(15))->at(Face::East)->hasPressure());
    auto const* far = m.node(at(20));
    EXPECT_FALSE(far && far->at(Face::East) && far->at(Face::East)->hasPressure());
}

TEST(PipeFlow, OpenEndsDrainAndPour) {
    TestWorld    w;
    Model        m{w};
    std::int64_t t = 0;
    // water source block - pipe - pump - pipe - air
    w.blocks[at(0)] = {Block::Water};
    w.pipe(at(1), kX);
    w.pump(at(2), Face::East, 64);
    w.pipe(at(3), kX);
    m.pumpSpeedChanged(at(2), 0, 64);
    run(m, 200, t);
    BlockFace out{at(3), Face::East};
    EXPECT_GT(w.poured[out], 0);
}

TEST(PipeFlow, ClosedValveBlocksTheFlow) {
    TestWorld    w;
    Model        m{w};
    std::int64_t t = 0;
    w.tank(at(0), {"minecraft:water", 8000}, 8000);
    w.pipe(at(1), kX);
    w.blocks[at(1)].closed = true; // like a closed valve: can't pull
    w.pump(at(2), Face::East, 64);
    w.pipe(at(3), kX);
    w.tank(at(4), {}, 8000);
    m.pumpSpeedChanged(at(2), 0, 64);
    run(m, 200, t);
    EXPECT_EQ(w.blocks[at(4)].contents.amount, 0);
}

TEST(PipeFlow, DifferentFluidsCollide) {
    TestWorld    w;
    Model        m{w};
    std::int64_t t = 0;
    // water from the west, lava from the east, both pumped into the middle pipe going down to a tank
    w.tank(at(-3), {"minecraft:water", 8000}, 8000);
    w.pipe(at(-2), kX);
    w.pump(at(-1), Face::East, 64);
    w.pipe(at(0), kX | bit(Face::Down));
    w.pump(at(1), Face::West, 64);
    w.pipe(at(2), kX);
    w.tank(at(3), {"minecraft:lava", 8000}, 8000);
    w.pipe(at(0, -1), axisMask(Axis::Y));
    w.tank(at(0, -2), {}, 8000);
    m.pumpSpeedChanged(at(-1), 0, 64);
    m.pumpSpeedChanged(at(1), 0, 64);
    run(m, 200, t);
    ASSERT_FALSE(w.collisions.empty());
}
