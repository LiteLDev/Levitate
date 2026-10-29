#include "core/pistons/Linear.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace lv::core::pistons {

namespace cc = contraption;

float convertToLinear(float rpm) { return rpm / 512.0f; }

float actuatorSpeed(float rpm) { return std::clamp(convertToLinear(rpm), -0.49f, 0.49f); }

float pistonMovementSpeed(float rpm, Dir facing, float offset, int range) {
    float speed = actuatorSpeed(rpm);
    // Create: movementModifier = axisDirection * (axis == Z ? -1 : 1); speed *= -movementModifier
    int step     = cc::positive(facing) ? 1 : -1;
    int modifier = step * (cc::axisOf(facing) == Axis::Z ? -1 : 1);
    speed        = speed * static_cast<float>(-modifier);
    return std::clamp(speed, 0.0f - offset, static_cast<float>(range) - offset);
}

int gridOffset(float offset, int range) { return std::clamp(static_cast<int>(offset + 0.5f), 0, std::max(0, range)); }

StopAction stopAction(MovementMode mode, float offset, int initialOffset, bool removed) {
    if (removed) return StopAction::Disassemble;
    if (mode == MovementMode::MoveNeverPlace) return StopAction::Wait;
    if (mode == MovementMode::MovePlaceReturned && static_cast<int>(offset + 0.5f) != initialOffset) return StopAction::Wait;
    return StopAction::Disassemble;
}

ElevatorStep elevatorStep(float rpm, float offset, int targetOffset, float prevSpeed) {
    float diff          = static_cast<float>(targetOffset) - offset;
    float movementSpeed = std::clamp(convertToLinear(rpm * 2), -1.99f, 1.99f);
    float rpmLimit      = std::abs(movementSpeed);
    // Mth.lerp(delta, start, end) = start + delta * (end - start)
    float acc   = 0.0075f + std::abs(movementSpeed) * (0.0175f - 0.0075f);
    float decel = std::sqrt(2 * std::abs(diff) * acc);

    float speed = diff;
    speed       = std::clamp(speed, -rpmLimit, rpmLimit);
    speed       = std::clamp(speed, prevSpeed - acc, prevSpeed + acc);
    speed       = std::clamp(speed, -decel, decel);
    return {speed, std::abs(diff) < 0.5f};
}

double elevatorSettle(double y, double target) {
    double diff = target - y;
    if (std::abs(diff) > 1.0 / 128) diff *= 0.25;
    return y + diff;
}

Axis directionalAxis(Dir facing, bool alongFirst) {
    switch (cc::axisOf(facing)) {
    case Axis::X:
        return alongFirst ? Axis::Y : Axis::Z;
    case Axis::Y:
        return alongFirst ? Axis::X : Axis::Z;
    default:
        return alongFirst ? Axis::X : Axis::Y;
    }
}

std::optional<bool> alongFirstFor(Dir facing, Axis axis) {
    for (bool b : {false, true})
        if (directionalAxis(facing, b) == axis) return b;
    return std::nullopt;
}

bool rotateAlongFirst(Dir facing, bool alongFirst, Axis about, int quarterTurns) {
    Axis shaft    = directionalAxis(facing, alongFirst);
    Axis newShaft = cc::rotate(shaft, about, quarterTurns);
    Dir  newFace  = cc::rotate(facing, about, quarterTurns);
    return alongFirstFor(newFace, newShaft).value_or(alongFirst);
}

float gantryPinionModifier(Dir shaft, Dir pinion) {
    Axis  axis = cc::axisOf(shaft);
    float dm   = cc::positive(shaft) ? 1.0f : -1.0f;
    if (axis == Axis::Y && (pinion == Dir::North || pinion == Dir::East)) return -dm;
    if (axis == Axis::X && (pinion == Dir::Down || pinion == Dir::South)) return -dm;
    if (axis == Axis::Z && (pinion == Dir::Up || pinion == Dir::West)) return -dm;
    return dm;
}

float pinionMovementSpeed(float rpm) { return std::clamp(convertToLinear(-rpm), -0.49f, 0.49f); }

ShaftPart shaftPart(bool front, bool back) {
    if (front && back) return ShaftPart::Middle;
    if (front) return ShaftPart::Start;
    if (back) return ShaftPart::End;
    return ShaftPart::Single;
}

bool canAssembleOn(ShaftPart part, bool powered, float speed) {
    if (powered) return false;
    switch (part) {
    case ShaftPart::End:
        return speed < 0;
    case ShaftPart::Middle:
        return speed != 0;
    case ShaftPart::Start:
        return speed > 0;
    default:
        return false;
    }
}

bool crossesShaftCentre(double coord, double next, float pinionSpeed, Dir shaftFacing) {
    float step = cc::positive(shaftFacing) ? 1.0f : -1.0f;
    return (std::floor(coord) + 0.5 < next) != (pinionSpeed * step < 0);
}

std::vector<Pos> colliders(std::vector<ColliderBlock> const& blocks, Dir dir) {
    std::unordered_set<Pos, cc::PosHash> solid;
    for (auto const& b : blocks)
        if (b.solid) solid.insert(b.local);
    std::vector<Pos> out;
    for (auto const& b : blocks) {
        if (!b.solid) continue;
        if (solid.contains(b.local + cc::offset(dir))) continue;
        out.push_back(b.local);
    }
    return out;
}

Pos collisionGrid(cc::Vec3d const& origin, Dir dir) {
    Pos grid{
        static_cast<int>(std::floor(origin.x)),
        static_cast<int>(std::floor(origin.y)),
        static_cast<int>(std::floor(origin.z)),
    };
    if (cc::positive(dir)) grid = grid + cc::offset(dir);
    return grid;
}

namespace {
std::optional<int> parseInt(std::string const& s) {
    if (s.empty()) return std::nullopt;
    char const* b = s.data();
    char const* e = s.data() + s.size();
    if (*b == '+') ++b;
    int  v = 0;
    auto r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e) return std::nullopt;
    return v;
}
bool blank(std::string const& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}
} // namespace

void initFloorNames(std::vector<Floor>& floors) {
    std::optional<int> prevLevel;
    for (std::size_t i = 0; i < floors.size(); ++i) {
        auto&              floor = floors[i];
        std::optional<int> currentLevel;
        if (!blank(floor.shortName)) {
            currentLevel = parseInt(floor.shortName);
            if (!currentLevel) continue; // a custom name stays
        }
        if (prevLevel) currentLevel = *prevLevel + 1;

        std::optional<int> nextLevel;
        for (std::size_t peek = i + 1; peek < floors.size(); ++peek) {
            auto v = parseInt(floors[peek].shortName);
            if (!v) continue;
            if (currentLevel && *currentLevel >= *v) {
                floors[peek].shortName.clear();
                break;
            }
            nextLevel = v;
            break;
        }
        if (!currentLevel) currentLevel = nextLevel ? *nextLevel - 1 : 0;
        floor.shortName = std::to_string(*currentLevel);
        prevLevel       = currentLevel;
    }
}

} // namespace lv::core::pistons
