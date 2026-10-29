#include <gtest/gtest.h>

#include <map>

#include "core/steam/Boiler.h"
#include "core/steam/Tank.h"
#include "core/steam/Whistle.h"

using namespace lv::core::steam;
using lv::core::common::HeatLevel;

namespace {

class MapWorld : public TankAccess {
public:
    std::map<Pos3, TankPart> parts;

    void place(Pos3 p, std::string fluid = {}, std::int64_t amount = 0) {
        TankPart t;
        t.controller = p;
        t.fluid      = std::move(fluid);
        t.amount     = amount;
        parts[p]     = t;
    }
    std::optional<TankPart> get(Pos3 const& pos) override {
        auto it = parts.find(pos);
        if (it == parts.end()) return std::nullopt;
        return it->second;
    }
    void set(Pos3 const& pos, TankPart const& part) override { parts[pos] = part; }

    TankPart& at(int x, int y, int z) { return parts[Pos3{x, y, z}]; }

    std::int64_t total() const {
        std::int64_t n = 0;
        for (auto const& [p, t] : parts) n += t.amount;
        return n;
    }
};

TankParams params() { return TankParams{}; }

} // namespace

TEST(SteamTank, ColumnFormsUpwardsFromTheBottom) {
    MapWorld w;
    for (int y = 0; y < 3; ++y) {
        w.place({0, y, 0});
        formMulti(w, {{0, y, 0}}, params());
    }
    for (int y = 0; y < 3; ++y) {
        auto const& p = w.at(0, y, 0);
        EXPECT_EQ(p.controller, (Pos3{0, 0, 0}));
        EXPECT_EQ(p.width, 1);
        EXPECT_EQ(p.height, 3);
    }
    EXPECT_EQ(capacityOf(w.at(0, 0, 0), params()), 24000);
}

TEST(SteamTank, SquareBaseUpToThreeWide) {
    MapWorld w;
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) w.place({x, 0, z});
    std::vector<Pos3> all;
    for (auto const& [p, t] : w.parts) all.push_back(p);
    formMulti(w, all, params());
    // the largest structure wins: a 3x3 at the min corner, the rest 1x1 / 1-high columns
    EXPECT_EQ(w.at(0, 0, 0).width, 3);
    EXPECT_EQ(w.at(2, 0, 2).controller, (Pos3{0, 0, 0}));
    EXPECT_EQ(w.at(3, 0, 3).width, 1);
    EXPECT_EQ(w.at(3, 0, 3).controller, (Pos3{3, 0, 3}));
}

TEST(SteamTank, IncompleteLayerIsNotIncluded) {
    MapWorld w;
    for (int x = 0; x < 2; ++x)
        for (int z = 0; z < 2; ++z) w.place({x, 0, z});
    w.place({0, 1, 0});
    std::vector<Pos3> all;
    for (auto const& [p, t] : w.parts) all.push_back(p);
    formMulti(w, all, params());
    EXPECT_EQ(w.at(0, 0, 0).width, 2);
    EXPECT_EQ(w.at(0, 0, 0).height, 1);
    EXPECT_EQ(w.at(0, 1, 0).controller, (Pos3{0, 1, 0}));
}

TEST(SteamTank, MaxHeightLimitsTheColumn) {
    MapWorld   w;
    TankParams p = params();
    p.maxHeight  = 4;
    for (int y = 0; y < 6; ++y) w.place({0, y, 0});
    std::vector<Pos3> all;
    for (auto const& [pos, t] : w.parts) all.push_back(pos);
    formMulti(w, all, p);
    EXPECT_EQ(w.at(0, 0, 0).height, 4);
    EXPECT_EQ(w.at(0, 4, 0).controller, (Pos3{0, 4, 0}));
    EXPECT_EQ(w.at(0, 4, 0).height, 2);
}

TEST(SteamTank, FluidsMergeIntoTheController) {
    MapWorld w;
    w.place({0, 0, 0}, "minecraft:water", 5000);
    w.place({0, 1, 0}, "minecraft:water", 7000);
    formMulti(w, {{0, 0, 0}, {0, 1, 0}}, params());
    EXPECT_EQ(w.at(0, 0, 0).amount, 12000);
    EXPECT_EQ(w.at(0, 1, 0).amount, 0);
    EXPECT_EQ(w.at(0, 0, 0).height, 2);
}

TEST(SteamTank, DifferentFluidsDoNotConnect) {
    MapWorld w;
    w.place({0, 0, 0}, "minecraft:water", 1000);
    w.place({0, 1, 0}, "minecraft:lava", 1000);
    formMulti(w, {{0, 0, 0}, {0, 1, 0}}, params());
    EXPECT_EQ(w.at(0, 0, 0).height, 1);
    EXPECT_EQ(w.at(0, 1, 0).controller, (Pos3{0, 1, 0}));
}

TEST(SteamTank, BreakingSharesTheFluidBottomUp) {
    MapWorld w;
    for (int y = 0; y < 3; ++y) w.place({0, y, 0});
    formMulti(w, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}}, params());
    w.at(0, 0, 0).fluid  = "minecraft:water";
    w.at(0, 0, 0).amount = 20000;
    // break the middle part: controller keeps 8000, the top gets 8000, 4000 are lost
    TankPart gone = w.at(0, 1, 0);
    w.parts.erase({0, 1, 0});
    removePart(w, {0, 1, 0}, gone, params());
    EXPECT_EQ(w.at(0, 0, 0).amount, 8000);
    EXPECT_EQ(w.at(0, 0, 0).height, 1);
    EXPECT_EQ(w.at(0, 2, 0).amount, 8000);
    EXPECT_EQ(w.at(0, 2, 0).controller, (Pos3{0, 2, 0}));
}

TEST(SteamTank, BreakingTheControllerMovesFluidUp) {
    MapWorld w;
    for (int y = 0; y < 3; ++y) w.place({0, y, 0});
    formMulti(w, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}}, params());
    w.at(0, 0, 0).fluid  = "minecraft:lava";
    w.at(0, 0, 0).amount = 10000;
    TankPart gone             = w.at(0, 0, 0);
    w.parts.erase({0, 0, 0});
    removePart(w, {0, 0, 0}, gone, params());
    // the two remaining parts re-form a 1x2 tank holding everything
    EXPECT_EQ(w.at(0, 1, 0).controller, (Pos3{0, 1, 0}));
    EXPECT_EQ(w.at(0, 1, 0).height, 2);
    EXPECT_EQ(w.at(0, 1, 0).amount, 10000);
    EXPECT_EQ(w.at(0, 1, 0).fluid, "minecraft:lava");
}

TEST(SteamTank, CreativeTanksStayFull) {
    MapWorld   w;
    TankParams p = params();
    p.creative   = true;
    w.place({0, 0, 0}, "minecraft:water", 8000);
    w.place({0, 1, 0});
    formMulti(w, {{0, 0, 0}, {0, 1, 0}}, p);
    EXPECT_EQ(w.at(0, 0, 0).amount, 16000);
}

TEST(SteamTank, WindowShapesAndLight) {
    EXPECT_EQ(windowShape(true, 1, 0, 0), 1);
    EXPECT_EQ(windowShape(false, 1, 0, 0), 0);
    EXPECT_EQ(windowShape(true, 2, 0, 0), 2);
    EXPECT_EQ(windowShape(true, 2, 0, 1), 3);
    EXPECT_EQ(windowShape(true, 2, 1, 0), 4);
    EXPECT_EQ(windowShape(true, 2, 1, 1), 5);
    EXPECT_EQ(windowShape(true, 3, 1, 1), 0);
    EXPECT_EQ(windowShape(true, 3, 0, 1), 1);
    EXPECT_EQ(windowShape(true, 3, 0, 0), 0);
    // lava (15) glows 12 up to the fill level, 1 above
    EXPECT_EQ(partLight(15, 8000, 32000, 4, 0, true), 12);
    EXPECT_EQ(partLight(15, 8000, 32000, 4, 1, true), 12);
    EXPECT_EQ(partLight(15, 8000, 32000, 4, 2, true), 1);
    EXPECT_EQ(partLight(15, 8000, 32000, 4, 0, false), 0);
    EXPECT_EQ(comparatorLevel(0, 8000), 0);
    EXPECT_EQ(comparatorLevel(1, 8000), 1);
    EXPECT_EQ(comparatorLevel(4000, 8000), 8);
    EXPECT_EQ(comparatorLevel(8000, 8000), 15);
}

TEST(SteamBoiler, HeatScores) {
    EXPECT_EQ(heatScore(HeatLevel::None), -1);
    EXPECT_EQ(heatScore(HeatLevel::Smouldering), 0);
    EXPECT_EQ(heatScore(HeatLevel::Fading), 1);
    EXPECT_EQ(heatScore(HeatLevel::Kindled), 1);
    EXPECT_EQ(heatScore(HeatLevel::Seething), 2);
    Boiler b;
    EXPECT_TRUE(b.updateTemperature({1, 2, -1, 0}));
    EXPECT_EQ(b.activeHeat, 3);
    EXPECT_FALSE(b.passiveHeat); // passive only without active heat
    EXPECT_TRUE(b.updateTemperature({0, -1}));
    EXPECT_TRUE(b.passiveHeat);
    EXPECT_EQ(b.activeHeat, 0);
}

TEST(SteamBoiler, LevelIsTheSmallestLimit) {
    Boiler b;
    b.attachedEngines = 1;
    b.activeHeat      = 9;
    b.waterSupply     = 45; // 4 levels
    EXPECT_EQ(b.maxHeatForSize(8), 2);
    EXPECT_EQ(b.maxHeatForSize(100), 18);
    EXPECT_EQ(b.maxHeatForWater(), 4);
    EXPECT_EQ(b.actualHeat(32), 4);
    EXPECT_EQ(b.actualHeat(8), 2);
    EXPECT_FLOAT_EQ(b.engineEfficiency(32), 1.f);
    b.attachedEngines = 8;
    EXPECT_FLOAT_EQ(b.engineEfficiency(32), .5f);
    b.activeHeat = 0;
    EXPECT_FLOAT_EQ(b.engineEfficiency(32), 0.f);
    b.passiveHeat = true;
    EXPECT_FLOAT_EQ(b.engineEfficiency(32), 1.f / 64.f);
}

TEST(SteamBoiler, WaterSupplySamples) {
    Boiler b;
    b.attachedEngines = 1;
    EXPECT_TRUE(b.tick(8000, false)); // first sample right away (nothing gathered)
    EXPECT_FLOAT_EQ(b.waterSupply, 0.f);
    // 50 mB per tick for one sample period -> 50 mB/t
    for (int t = 0; t < 5; ++t) {
        b.addSupply(50);
        b.tick(8000, false);
    }
    EXPECT_FLOAT_EQ(b.waterSupply, 50.f);
    EXPECT_EQ(b.maxHeatForWater(), 5);
    // the best sample of the last 10 (50 ticks) counts
    for (int t = 0; t < 40; ++t) b.tick(8000, false);
    EXPECT_FLOAT_EQ(b.waterSupply, 50.f);
    for (int t = 0; t < 60; ++t) b.tick(8000, false);
    EXPECT_FLOAT_EQ(b.waterSupply, 0.f);
    Boiler c;
    c.attachedWhistles = 1;
    c.tick(8000, true);
    EXPECT_FLOAT_EQ(c.waterSupply, 200.f);
    Boiler idle;
    EXPECT_FALSE(idle.tick(8000, false)); // not a boiler
}

TEST(SteamBoiler, EngineNumbers) {
    EXPECT_EQ(speedModifier(1.f), 4);
    EXPECT_EQ(speedModifier(.5f), 3);
    EXPECT_EQ(speedModifier(.3f), 2);
    EXPECT_EQ(speedModifier(.125f), 1);
    EXPECT_FLOAT_EQ(poweredShaftSpeed(1, 1.f, 1024), 64.f);
    EXPECT_FLOAT_EQ(poweredShaftSpeed(-1, .125f, 1024), -16.f);
    EXPECT_FLOAT_EQ(poweredShaftSpeed(1, 0.f, 1024), 0.f);
    // a full engine provides 16384 su at its 64 rpm, a passive one 2048 su at 16 rpm
    EXPECT_DOUBLE_EQ(poweredShaftCapacity(1.f, 1024) * 64, 16384);
    EXPECT_DOUBLE_EQ(poweredShaftCapacity(.125f, 1024) * 16, 2048);
    Boiler b;
    b.attachedEngines = 2;
    b.activeHeat      = 18;
    b.waterSupply     = 200;
    EXPECT_DOUBLE_EQ(boilerCapacityShown(b, 72, 1024), 16.0 * 18 * 1024);
}

TEST(SteamBoiler, ConveyedDirection) {
    EXPECT_EQ(conveyedDirection(1.f, Axis::X, Dir::North, false), -1);
    EXPECT_EQ(conveyedDirection(1.f, Axis::X, Dir::South, false), 1);
    EXPECT_EQ(conveyedDirection(1.f, Axis::Z, Dir::East, false), -1);
    EXPECT_EQ(conveyedDirection(1.f, Axis::Y, Dir::East, false), 1);
    EXPECT_EQ(conveyedDirection(1.f, Axis::X, Dir::South, true), -1);
    EXPECT_EQ(conveyedDirection(0.f, Axis::X, Dir::North, false), 1);
}

TEST(SteamBoiler, GoggleBars) {
    Boiler b;
    b.attachedEngines = 1;
    b.activeHeat      = 4;
    b.waterSupply     = 60; // 6
    auto d            = b.display(40); // size 10
    EXPECT_EQ(d.level, 4);
    EXPECT_EQ(d.minValue, 4);
    EXPECT_EQ(d.maxValue, 10);
    auto bars = boilerBars(d, d.maxHeatForSize);
    int  total = 0;
    for (auto const& [c, n] : bars) total += n;
    EXPECT_EQ(total, 15); // 3 dark green, 1 green, 6 dark green, 0 red, 5 gray
    EXPECT_EQ(bars.front().first, Colour::DarkGreen);
    EXPECT_EQ(bars.front().second, 3);
}

TEST(SteamWhistle, PitchFromExtensions) {
    using S = ExtensionShape;
    std::vector<std::optional<S>> col{S::DoubleConnected, S::DoubleConnected, S::Single};
    auto above = [&](int i) -> std::optional<S> { return i - 1 < static_cast<int>(col.size()) ? col[i - 1] : std::nullopt; };
    EXPECT_EQ(whistlePitch(above), 5);
    col = {};
    EXPECT_EQ(whistlePitch(above), 0);
    col = {S::Double};
    EXPECT_EQ(whistlePitch(above), 2);
    EXPECT_FLOAT_EQ(whistleSoundPitch(12), .5f);
    EXPECT_FLOAT_EQ(octaveFactor(WhistleSize::Small), 2.f);
    EXPECT_EQ(pitchId(3, WhistleSize::Large), 203);
}

TEST(SteamWhistle, IncrementSize) {
    using S = ExtensionShape;
    std::vector<std::optional<S>> col;
    auto above = [&](int i) -> std::optional<S> { return i - 1 < static_cast<int>(col.size()) ? col[i - 1] : std::nullopt; };
    auto free  = [](int) { return true; };
    auto a     = incrementSize(above, free);
    EXPECT_EQ(a.kind, IncrementAction::Place);
    EXPECT_EQ(a.offset, 1);
    col = {S::Single};
    a   = incrementSize(above, free);
    EXPECT_EQ(a.kind, IncrementAction::Widen);
    col = {S::Double, S::Double, S::Double, S::Double, S::Double, S::Double};
    EXPECT_EQ(incrementSize(above, free).kind, IncrementAction::None);
    col = {S::Double};
    EXPECT_EQ(incrementSize(above, [](int) { return false; }).kind, IncrementAction::None);
    EXPECT_EQ(connectedShape(S::Double, true), S::DoubleConnected);
    EXPECT_EQ(connectedShape(S::DoubleConnected, false), S::Double);
}
