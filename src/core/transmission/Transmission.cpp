#include "core/transmission/Transmission.h"

#include <algorithm>
#include <cmath>

#include "nlohmann/json.hpp"

namespace lv::core::transmission {

using kinetics::axisOf;
using kinetics::isPositive;

// ---- chain drives ------------------------------------------------------------------------------

Axis chainConnectionAxis(ChainState const& s) {
    if (s.alongFirst) return s.axis == Axis::X ? Axis::Y : Axis::X;
    return s.axis == Axis::Z ? Axis::Y : Axis::Z;
}

ChainState chainUpdateShape(ChainState self, Face face, std::optional<ChainState> const& neighbour) {
    Axis faceAxis         = axisOf(face);
    bool facingAlongFirst = self.axis == Axis::X ? faceAxis == Axis::Y : faceAxis == Axis::X;
    bool positive         = isPositive(face);
    if (self.axis == faceAxis) return self;

    if (!neighbour) {
        if (facingAlongFirst != self.alongFirst || self.part == ChainPart::None) return self;
        if (self.part == ChainPart::Middle) {
            self.part = positive ? ChainPart::End : ChainPart::Start;
            return self;
        }
        if ((self.part == ChainPart::Start) == positive) self.part = ChainPart::None;
        return self;
    }

    auto const& other = *neighbour;
    if (other.axis == faceAxis) return self;
    if (other.part != ChainPart::None && chainConnectionAxis(other) != faceAxis) return self;

    ChainPart part       = self.part;
    bool      alongFirst = self.alongFirst;
    if (part == ChainPart::None) {
        part       = positive ? ChainPart::Start : ChainPart::End;
        alongFirst = self.axis == Axis::X ? faceAxis == Axis::Y : faceAxis == Axis::X;
    } else if (chainConnectionAxis(self) != faceAxis) {
        return self;
    }
    if ((part == ChainPart::Start) != positive) part = ChainPart::Middle;
    self.part       = part;
    self.alongFirst = alongFirst;
    return self;
}

bool chainConnected(ChainState const& s, ChainState const& other, Face facing) {
    Axis connection = chainConnectionAxis(s);
    if (chainConnectionAxis(other) != connection) return false;
    if (axisOf(facing) != connection) return false;
    if (isPositive(facing) && (s.part == ChainPart::Middle || s.part == ChainPart::Start)) return true;
    if (!isPositive(facing) && (s.part == ChainPart::Middle || s.part == ChainPart::End)) return true;
    return false;
}

float chainGearshiftModifier(int signal) {
    if (signal == 0) return 1;
    return 1 + (static_cast<float>(signal + 1) / 16.f);
}

// ---- gauges --------------------------------------------------------------------------------------

Axis gaugeRotationAxis(Face facing, bool alongFirst) {
    switch (axisOf(facing)) {
    case Axis::X:
        return alongFirst ? Axis::Y : Axis::Z;
    case Axis::Y:
        return alongFirst ? Axis::X : Axis::Z;
    default:
        return alongFirst ? Axis::X : Axis::Y;
    }
}

bool gaugeHeadOnFace(Face facing, bool alongFirst, Face face) {
    Axis rot = gaugeRotationAxis(facing, alongFirst);
    if (axisOf(face) == Axis::Y) return false;
    if (face == kinetics::opposite(facing)) return false;
    if (axisOf(face) == rot) return false;
    if (rot == Axis::Y && face != facing) return false;
    return true;
}

float speedDialTarget(float speed, float medium, float fast, float max) {
    speed = std::abs(speed);
    auto lerp = [](float t, float a, float b) { return a + t * (b - a); };
    if (speed == 0) return 0;
    if (speed < medium) return lerp(speed / medium, 0, .45f);
    if (speed < fast) return lerp((speed - medium) / (fast - medium), .45f, .75f);
    return lerp((speed - fast) / (max - fast), .75f, 1.125f);
}

float stressDialTarget(bool stressEnabled, float speed, bool overstressed, double stress, double capacity) {
    if (speed == 0) return 0;
    if (!stressEnabled) return 0;
    if (overstressed) return 1.125f;
    if (capacity == 0) return 0;
    return static_cast<float>(stress / capacity);
}

int gaugeComparatorSignal(float dialTarget) {
    return static_cast<int>(std::ceil(std::clamp(dialTarget * 14.f, 0.f, 15.f)));
}

int dialStateOf(float dialTarget) {
    float steps = std::clamp(dialTarget, 0.f, 1.125f) * 90.f / 22.5f;
    return std::clamp(static_cast<int>(std::lround(steps)), 0, kDialStates - 1);
}

float dialOfState(int state) {
    return static_cast<float>(std::clamp(state, 0, kDialStates - 1)) * 22.5f / 90.f;
}

// ---- sequenced gearshift -------------------------------------------------------------------------

InstrDef const& instrDef(Instr i) {
    static InstrDef const defs[kInstrTypes] = {
        {"turn_angle",    "angle",    true,  true,  360, 45, 90},
        {"turn_distance", "distance", true,  true,  128, 5,  5 },
        {"delay",         "duration", true,  false, 600, 20, 10},
        {"await",         "",         false, false, -1,  -1, -1},
        {"end",           "",         false, false, -1,  -1, -1},
    };
    return defs[std::clamp(static_cast<int>(i), 0, kInstrTypes - 1)];
}

char const* speedModifierId(int modifier) {
    switch (modifier) {
    case 2:
        return "forward_fast";
    case -1:
        return "back";
    case -2:
        return "back_fast";
    default:
        return "forward";
    }
}

std::vector<Instruction> defaultInstructions() { return {Instruction{Instr::TurnAngle, 1, 90}, Instruction{Instr::End, 1, 1}}; }

namespace {
constexpr double toAngular(double speed) { return speed * 3 / 10; } // KineticBlockEntity.convertToAngular
constexpr double toLinear(double speed) { return speed / 512; }     // KineticBlockEntity.convertToLinear
} // namespace

int instructionDuration(Instruction const& i, float progress, float speed) {
    double s      = std::abs(static_cast<double>(speed) * i.speedModifier);
    double target = i.value - progress;
    switch (i.type) {
    // always overshoot (Create: the target stops early)
    case Instr::TurnAngle:
        return static_cast<int>(std::ceil(target / toAngular(s))) + 2;
    case Instr::TurnDistance:
        return static_cast<int>(std::ceil(target / toLinear(s))) + 2;
    case Instr::Delay:
        return static_cast<int>(target);
    case Instr::Await:
        return -1;
    default:
        return 0;
    }
}

float instructionTickProgress(Instruction const& i, float speed) {
    switch (i.type) {
    case Instr::TurnAngle:
        return static_cast<float>(toAngular(speed));
    case Instr::TurnDistance:
        return static_cast<float>(toLinear(speed));
    case Instr::Delay:
        return 1;
    default:
        return 0;
    }
}

int instructionSpeedModifier(Instruction const& i) {
    return i.type == Instr::TurnAngle || i.type == Instr::TurnDistance ? i.speedModifier : 0;
}

std::string formatInstructionValue(Instr type, int value, std::string const& degreesUnit) {
    if (type == Instr::TurnAngle) return std::to_string(value) + degreesUnit;
    if (type == Instr::TurnDistance) return std::to_string(value) + "m";
    if (type == Instr::Delay) return value >= 20 ? std::to_string(value / 20) + "s" : std::to_string(value) + "t";
    return std::to_string(value);
}

int modifierToState(int modifier) {
    switch (modifier) {
    case 1:
        return 1;
    case 2:
        return 2;
    case -1:
        return 3;
    case -2:
        return 4;
    default:
        return 0;
    }
}

int stateToModifier(int state) {
    static constexpr int kMods[] = {0, 1, 2, -1, -2};
    return state >= 0 && state < 5 ? kMods[state] : 0;
}

nlohmann::json instructionsToJson(std::vector<Instruction> const& list) {
    auto out = nlohmann::json::array();
    for (auto const& i : list) out.push_back({{"type", static_cast<int>(i.type)}, {"speed", i.speedModifier}, {"value", i.value}});
    return out;
}

std::vector<Instruction> instructionsFromJson(nlohmann::json const& j) {
    std::vector<Instruction> out;
    if (j.is_array()) {
        for (auto const& e : j) {
            if (!e.is_object() || static_cast<int>(out.size()) >= kMaxInstructions) continue;
            Instruction i;
            i.type = static_cast<Instr>(std::clamp(e.value("type", 0), 0, kInstrTypes - 1));
            int sp = e.value("speed", 1);
            i.speedModifier = std::find(kSpeedModifiers.begin(), kSpeedModifiers.end(), sp) != kSpeedModifiers.end() ? sp : 1;
            auto const& def = instrDef(i.type);
            i.value         = def.hasValue ? std::clamp(e.value("value", def.defaultValue), 1, def.maxValue) : 1;
            out.push_back(i);
        }
    }
    if (out.empty()) return defaultInstructions();
    return out;
}

void setInstructionType(std::vector<Instruction>& list, int index, Instr type) {
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    auto& i = list[static_cast<std::size_t>(index)];
    i.type  = type;
    i.value = instrDef(type).hasValue ? instrDef(type).defaultValue : 1;
    if (type == Instr::End) {
        list.resize(static_cast<std::size_t>(index) + 1);
    } else if (index + 1 < kMaxInstructions && index + 1 == static_cast<int>(list.size())) {
        list.push_back(Instruction{Instr::End, 1, 1});
    }
}

// ---- rotation speed controller -------------------------------------------------------------------

float controllerConveyedSpeed(
    float controllerSpeed,
    float wheelSpeed,
    int   targetSpeed,
    bool  controllerHasSource,
    bool  wheelIsControllerSource,
    bool  targetingController
) {
    // getDesiredOutputSpeed
    float desired = 0;
    if (targetSpeed == 0) desired = 0;
    else if (targetingController && wheelSpeed == 0) desired = 0;
    else if (!controllerHasSource) desired = targetingController ? static_cast<float>(targetSpeed) : 0.f;
    else if (wheelIsControllerSource) desired = targetingController ? static_cast<float>(targetSpeed) : wheelSpeed;
    else desired = targetingController ? controllerSpeed : static_cast<float>(targetSpeed);

    float compare = targetingController ? controllerSpeed : wheelSpeed;
    if (desired >= 0 && compare >= 0) return std::max(desired, compare);
    if (desired < 0 && compare < 0) return std::min(desired, compare);
    return desired;
}

} // namespace lv::core::transmission
