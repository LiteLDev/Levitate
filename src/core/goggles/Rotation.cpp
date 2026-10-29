#include "core/goggles/Rotation.h"

namespace lv::core::goggles {

namespace {
constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;
constexpr int kX = 0, kY = 1, kZ = 2;

bool has(States const& s, char const* name) { return s.contains(name); }
} // namespace

int axisOfDirection(int dir) {
    switch (dir) {
    case kDown:
    case kUp:
        return kY;
    case kNorth:
    case kSouth:
        return kZ;
    default:
        return kX;
    }
}

int positiveDirection(int axis) { return axis == kX ? kEast : axis == kY ? kUp : kSouth; }

int clockwise(int dir, int axis) {
    if (axisOfDirection(dir) == axis) return dir;
    switch (axis) {
    case kX: // Direction.getClockWiseX
        switch (dir) {
        case kDown:
            return kSouth;
        case kSouth:
            return kUp;
        case kUp:
            return kNorth;
        default:
            return kDown; // north
        }
    case kY: // Direction.getClockWise
        switch (dir) {
        case kNorth:
            return kEast;
        case kEast:
            return kSouth;
        case kSouth:
            return kWest;
        default:
            return kNorth; // west
        }
    default: // Direction.getClockWiseZ
        switch (dir) {
        case kDown:
            return kWest;
        case kWest:
            return kUp;
        case kUp:
            return kEast;
        default:
            return kDown; // east
        }
    }
}

int directionFromHorizontal(int value) {
    switch (value & 3) {
    case 0:
        return kSouth;
    case 1:
        return kWest;
    case 2:
        return kNorth;
    default:
        return kEast;
    }
}

int horizontalFromDirection(int dir) {
    switch (dir) {
    case kSouth:
        return 0;
    case kWest:
        return 1;
    case kNorth:
        return 2;
    default:
        return 3;
    }
}

States rotatedStates(States const& in, int face) {
    if (face < 0 || face > 5) return in;
    States out      = in;
    int    faceAxis = axisOfDirection(face);

    if (faceAxis == kY) {
        if (has(in, kHorizontalAxis)) {
            int axis             = in.at(kHorizontalAxis) == kZ ? kZ : kX;
            out[kHorizontalAxis] = axisOfDirection(clockwise(positiveDirection(axis), faceAxis));
            return out;
        }
        if (has(in, kHorizontalFacing)) {
            int dir                = directionFromHorizontal(in.at(kHorizontalFacing));
            out[kHorizontalFacing] = horizontalFromDirection(clockwise(dir, faceAxis));
            return out;
        }
    }

    if (has(in, kAxis)) {
        int axis   = in.at(kAxis);
        if (axis < 0 || axis > 2) return out;
        out[kAxis] = axisOfDirection(clockwise(positiveDirection(axis), faceAxis));
        return out;
    }

    char const* facingKey = has(in, kFacing) ? kFacing : has(in, kVanillaFacing) ? kVanillaFacing : nullptr;
    if (!facingKey) return out;
    int facing = in.at(facingKey);
    if (facing < 0 || facing > 5) return out;
    bool alongFirst = has(in, kAxisAlongFirst);

    if (axisOfDirection(facing) == faceAxis) {
        if (alongFirst) out[kAxisAlongFirst] = out[kAxisAlongFirst] ? 0 : 1;
        return out;
    }
    do {
        facing = clockwise(facing, faceAxis);
        if (faceAxis == kY && alongFirst) out[kAxisAlongFirst] = out[kAxisAlongFirst] ? 0 : 1;
    } while (axisOfDirection(facing) == faceAxis);
    out[facingKey] = facing;
    return out;
}

} // namespace lv::core::goggles
