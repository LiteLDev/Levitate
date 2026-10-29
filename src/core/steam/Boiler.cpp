#include "core/steam/Boiler.h"

#include <algorithm>
#include <cmath>

namespace lv::core::steam {

int heatScore(common::HeatLevel level) {
    switch (level) {
    case common::HeatLevel::None:
        return -1;
    case common::HeatLevel::Seething:
        return 2;
    case common::HeatLevel::Fading:
    case common::HeatLevel::Kindled:
        return 1;
    default:
        return 0; // smouldering: passive
    }
}

int Boiler::maxHeatForSize(int boilerSize) const { return std::min(kMaxBoilerLevel, boilerSize / 4); }

int Boiler::maxHeatForWater() const {
    return std::min(kMaxBoilerLevel, static_cast<int>(std::ceil(waterSupply)) / kWaterSupplyPerLevel);
}

bool Boiler::isPassive(int boilerSize) const {
    return passiveHeat && maxHeatForSize(boilerSize) > 0 && maxHeatForWater() > 0;
}

float Boiler::engineEfficiency(int boilerSize) const {
    if (isPassive(boilerSize)) return attachedEngines > 0 ? kPassiveEngineEfficiency / attachedEngines : 0.f;
    if (activeHeat == 0) return 0;
    int level = actualHeat(boilerSize);
    return attachedEngines <= level ? 1.f : static_cast<float>(level) / static_cast<float>(attachedEngines);
}

int Boiler::actualHeat(int boilerSize) const {
    return std::min(activeHeat, std::min(maxHeatForWater(), maxHeatForSize(boilerSize)));
}

BoilerDisplay Boiler::display(int boilerSize) const {
    BoilerDisplay d;
    d.maxHeatForSize  = maxHeatForSize(boilerSize);
    d.maxHeatForWater = maxHeatForWater();
    d.heat            = passiveHeat ? 1 : activeHeat;
    d.minValue        = std::min(d.heat, std::min(d.maxHeatForWater, d.maxHeatForSize));
    d.maxValue        = std::max(d.heat, std::max(d.maxHeatForWater, d.maxHeatForSize));
    d.level           = std::min(activeHeat, std::min(d.maxHeatForWater, d.maxHeatForSize));
    d.passive         = passiveHeat && d.maxHeatForSize > 0 && d.maxHeatForWater > 0;
    return d;
}

bool Boiler::updateTemperature(std::vector<int> const& scores) {
    needsHeatLevelUpdate = false;
    bool prevPassive     = passiveHeat;
    int  prevActive      = activeHeat;
    passiveHeat          = false;
    activeHeat           = 0;
    for (int heat : scores) {
        if (heat == 0) passiveHeat = true;
        else if (heat > 0) activeHeat += heat;
    }
    passiveHeat = passiveHeat && activeHeat == 0;
    return prevActive != activeHeat || prevPassive != passiveHeat;
}

bool Boiler::setAttachments(int engines, int whistles) {
    bool changed         = engines != attachedEngines || whistles != attachedWhistles;
    attachedEngines      = engines;
    attachedWhistles     = whistles;
    needsHeatLevelUpdate = true;
    return changed;
}

bool Boiler::tick(int capacity, bool creative) {
    if (!isActive()) return false;
    if (--ticksUntilNextSample > 0) return false;
    if (capacity == 0) return false;
    ticksUntilNextSample         = kSampleRate;
    supplyOverTime[currentIndex] = static_cast<float>(gatheredSupply) / static_cast<float>(kSampleRate);
    waterSupply                  = std::max(waterSupply, supplyOverTime[currentIndex]);
    currentIndex                 = (currentIndex + 1) % kSupplySamples;
    gatheredSupply               = 0;
    if (currentIndex == 0) {
        waterSupply = 0;
        for (float s : supplyOverTime) waterSupply = std::max(s, waterSupply);
    }
    if (creative) waterSupply = static_cast<float>(kWaterSupplyPerLevel * 20);
    return true;
}

void Boiler::clear() {
    waterSupply      = 0;
    activeHeat       = 0;
    passiveHeat      = false;
    attachedEngines  = 0;
    attachedWhistles = 0;
    supplyOverTime.fill(0);
}

double boilerCapacityShown(Boiler const& boiler, int boilerSize, double engineCapacity) {
    auto d = boiler.display(boilerSize);
    return boiler.engineEfficiency(boilerSize) * 16.0 * std::max(d.level, boiler.attachedEngines) * engineCapacity;
}

std::vector<std::pair<Colour, int>> boilerBars(BoilerDisplay const& d, int level) {
    std::vector<std::pair<Colour, int>> out;
    auto add = [&](Colour c, int n) {
        if (n > 0) out.emplace_back(c, n);
    };
    add(Colour::DarkGreen, std::max(0, d.minValue - 1));
    add(Colour::Green, d.minValue > 0 ? 1 : 0);
    add(Colour::DarkGreen, std::max(0, level - d.minValue));
    add(Colour::DarkRed, std::max(0, d.maxValue - level));
    add(Colour::DarkGray, std::max(0, std::min(kMaxBoilerLevel - d.maxValue, ((d.maxValue / 5 + 1) * 5) - d.maxValue)));
    return out;
}

int speedModifier(float efficiency) {
    return 1 + (efficiency >= 1 ? 3 : std::min(2, static_cast<int>(std::floor(efficiency * 4))));
}

float poweredShaftSpeed(int direction, float efficiency, double engineCapacity) {
    double combined = efficiency * engineCapacity;
    return combined > 0 ? static_cast<float>(direction * 16 * speedModifier(efficiency)) : 0.f;
}

double poweredShaftCapacity(float efficiency, double engineCapacity) {
    return efficiency * engineCapacity / speedModifier(efficiency);
}

Axis axisOf(Dir d) {
    switch (d) {
    case Dir::Down:
    case Dir::Up:
        return Axis::Y;
    case Dir::North:
    case Dir::South:
        return Axis::Z;
    default:
        return Axis::X;
    }
}

bool positive(Dir d) { return d == Dir::Up || d == Dir::South || d == Dir::East; }

Dir opposite(Dir d) { return static_cast<Dir>(static_cast<int>(d) ^ 1); }

int conveyedDirection(float efficiency, Axis shaftAxis, Dir facing, bool counterClockwise) {
    int level = efficiency == 0 ? 1 : shaftAxis == Axis::Y ? 1 : (positive(facing) ? 1 : -1);
    if (shaftAxis == Axis::Z) level = -level;
    if (counterClockwise) level = -level;
    return level;
}

} // namespace lv::core::steam
