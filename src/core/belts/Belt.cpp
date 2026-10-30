#include "core/belts/Belt.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace lv::core::belts {

// ---- geometry ---------------------------------------------------------------------------------------

Vec3i facingNormal(int facing) {
    switch (facing & 3) {
    case 0:
        return {0, 0, 1};
    case 1:
        return {-1, 0, 0};
    case 2:
        return {0, 0, -1};
    default:
        return {1, 0, 0};
    }
}

int  facingStep(int facing) { return (facing & 3) == 0 || (facing & 3) == 3 ? 1 : -1; }
Axis facingAxis(int facing) { return (facing & 1) ? Axis::X : Axis::Z; }
int  oppositeFacing(int facing) { return (facing + 2) & 3; }
int  clockwise(int facing) { return (facing + 1) & 3; }

std::optional<int> facingOf(int dx, int dz) {
    if (dx == 0 && dz == 1) return 0;
    if (dx == -1 && dz == 0) return 1;
    if (dx == 0 && dz == -1) return 2;
    if (dx == 1 && dz == 0) return 3;
    return std::nullopt;
}

int faceOfFacing(int facing) {
    static constexpr int kFaces[] = {3, 4, 2, 5};
    return kFaces[facing & 3];
}

std::optional<int> facingOfFace(int face) {
    switch (face) {
    case 2:
        return 2;
    case 3:
        return 0;
    case 4:
        return 1;
    case 5:
        return 3;
    default:
        return std::nullopt;
    }
}

int oppositeFace(int face) { return face < 0 ? face : face ^ 1; }

Vec3i faceNormal(int face) {
    switch (face) {
    case 0:
        return {0, -1, 0};
    case 1:
        return {0, 1, 0};
    case 2:
        return {0, 0, -1};
    case 3:
        return {0, 0, 1};
    case 4:
        return {-1, 0, 0};
    case 5:
        return {1, 0, 0};
    default:
        return {};
    }
}

namespace {
int facingFromAxis(Axis axis, bool positive) {
    if (axis == Axis::X) return positive ? 3 : 1;
    return positive ? 0 : 2;
}
int choose(Axis axis, Vec3i const& v) { return axis == Axis::X ? v.x : axis == Axis::Y ? v.y : v.z; }
} // namespace

Vec3i beltVector(SegmentState const& s) {
    if (s.slope == Slope::Vertical) return {0, facingStep(s.facing), 0};
    int verticality = s.slope == Slope::Downward ? -1 : s.slope == Slope::Upward ? 1 : 0;
    return facingNormal(s.facing) + Vec3i{0, verticality, 0};
}

std::optional<Vec3i> nextSegmentOffset(SegmentState const& s, bool forward) {
    if ((s.part == Part::End && forward) || (s.part == Part::Start && !forward)) return std::nullopt;
    int offset = forward ? 1 : -1;
    if (s.slope == Slope::Vertical) return Vec3i{0, facingStep(s.facing) * offset, 0};
    Vec3i p = facingNormal(s.facing) * offset;
    if (s.slope != Slope::Horizontal && s.slope != Slope::Sideways) p.y += s.slope == Slope::Upward ? offset : -offset;
    return p;
}

Axis rotationAxis(SegmentState const& s) {
    if (s.slope == Slope::Sideways) return Axis::Y;
    return facingAxis(clockwise(s.facing));
}

bool statesCompatible(SegmentState const& a, SegmentState const& b) {
    switch (a.slope) {
    case Slope::Upward:
        if (b.slope == Slope::Downward) return a.facing == oppositeFacing(b.facing);
        return b.slope == a.slope && a.facing == b.facing;
    case Slope::Downward:
        if (b.slope == Slope::Upward) return a.facing == oppositeFacing(b.facing);
        return b.slope == a.slope && a.facing == b.facing;
    default:
        return b.slope == a.slope && facingAxis(a.facing) == facingAxis(b.facing);
    }
}

SegmentState flipped(SegmentState s) {
    s.facing = oppositeFacing(s.facing);
    if (s.slope == Slope::Upward) s.slope = Slope::Downward;
    else if (s.slope == Slope::Downward) s.slope = Slope::Upward;
    if (s.part == Part::Start) s.part = Part::End;
    else if (s.part == Part::End) s.part = Part::Start;
    return s;
}

// ---- placing ---------------------------------------------------------------------------------------------

char const* connectErrorName(ConnectError e) {
    switch (e) {
    case ConnectError::None:
        return "ok";
    case ConnectError::Unloaded:
        return "unloaded";
    case ConnectError::SamePosition:
        return "same position";
    case ConnectError::TooFar:
        return "too far";
    case ConnectError::NotAligned:
        return "not aligned";
    case ConnectError::AxisMismatch:
        return "axis mismatch";
    case ConnectError::VerticalShaftDiagonal:
        return "vertical shafts can't connect diagonally";
    case ConnectError::SpeedConflict:
        return "speed conflict";
    case ConnectError::Obstructed:
        return "obstructed";
    case ConnectError::NotShaft:
        return "not a shaft";
    }
    return "?";
}

ConnectError canConnect(ConnectWorld const& w, Vec3i const& first, Vec3i const& second, int maxLength) {
    if (!w.loaded(first) || !w.loaded(second)) return ConnectError::Unloaded;
    if (first == second) return ConnectError::SamePosition;
    Vec3i diff = second - first;
    if (!(diff.distSqr() < 1LL * maxLength * maxLength)) return ConnectError::TooFar;
    auto axis = w.shaftAxis(first);
    if (!axis) return ConnectError::NotShaft;
    int x = diff.x, y = diff.y, z = diff.z;
    int sames = (std::abs(x) == std::abs(y) ? 1 : 0) + (std::abs(y) == std::abs(z) ? 1 : 0) + (std::abs(z) == std::abs(x) ? 1 : 0);
    if (choose(*axis, diff) != 0) return ConnectError::NotAligned;
    if (sames != 1) return ConnectError::NotAligned;
    auto axis2 = w.shaftAxis(second);
    if (!axis2) return ConnectError::NotShaft;
    if (*axis2 != *axis) return ConnectError::AxisMismatch;
    if (*axis == Axis::Y && x != 0 && z != 0) return ConnectError::VerticalShaftDiagonal;
    float s1 = w.speed(first), s2 = w.speed(second);
    auto  sign = [](float v) { return v > 0 ? 1 : v < 0 ? -1 : 0; };
    if (sign(s1) != sign(s2) && s1 != 0 && s2 != 0) return ConnectError::SpeedConflict;
    Vec3i step{sign(float(x)), sign(float(y)), sign(float(z))};
    int   limit = 1000;
    for (Vec3i p = first + step; !(p == second) && limit-- > 0; p = p + step) {
        auto a = w.shaftAxis(p);
        if (a && *a == *axis) continue;
        if (!w.replaceable(p)) return ConnectError::Obstructed;
    }
    return ConnectError::None;
}

std::optional<PreviewLine> previewLine(Vec3i const& first, Vec3i const& selected, Axis axis) {
    Vec3i actual = selected - first;
    Vec3i diff   = actual;
    if (axis == Axis::X) diff.x = 0;
    if (axis == Axis::Z) diff.z = 0;
    double x = std::abs(diff.x), y = std::abs(diff.y), z = std::abs(diff.z);
    double length = std::max(x, std::max(y, z));
    if (length <= 0) return std::nullopt;
    double norm = std::sqrt(double(diff.x) * diff.x + double(diff.y) * diff.y + double(diff.z) * diff.z);
    double sx = diff.x / norm, sy = diff.y / norm, sz = diff.z / norm;
    int    sames = (x == y ? 1 : 0) + (y == z ? 1 : 0) + (z == x ? 1 : 0);
    if (sames == 0) {
        // the closest of the straight / diagonal steps in the plane of the belt
        double best = 1e18, ox = sx, oy = sy, oz = sz;
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j)
                for (int k = -1; k <= 1; ++k) {
                    int along = axis == Axis::X ? i : axis == Axis::Y ? j : k;
                    if (along != 0) continue;
                    if (axis == Axis::Y && i != 0 && k != 0) continue;
                    if (i == 0 && j == 0 && k == 0) continue;
                    double d = std::sqrt((ox - i) * (ox - i) + (oy - j) * (oy - j) + (oz - k) * (oz - k));
                    if (d < best) {
                        best = d;
                        sx = i, sy = j, sz = k;
                    }
                }
    }
    auto sgn = [](double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; };
    Vec3i step{sgn(sx), sgn(sy), sgn(sz)};
    if (axis == Axis::Y && step.x != 0 && step.z != 0) return std::nullopt;
    return PreviewLine{step, float(length)};
}

Slope slopeBetween(Vec3i const& start, Vec3i const& end) {
    Vec3i diff = end - start;
    if (diff.y != 0) {
        if (diff.z != 0 || diff.x != 0) return diff.y > 0 ? Slope::Upward : Slope::Downward;
        return Slope::Vertical;
    }
    return Slope::Horizontal;
}

int facingBetween(Vec3i const& start, Vec3i const& end, Axis shaftAxis) {
    Axis  beltAxis = start.x == end.x ? Axis::Z : Axis::X;
    Vec3i diff     = end - start;
    bool  positive;
    if (diff.x == 0 && diff.z == 0) positive = diff.y > 0;
    else positive = (beltAxis == Axis::X ? diff.x : diff.z) > 0;
    int facing = facingFromAxis(beltAxis, positive);
    if (diff.x == diff.z) facing = facingFromAxis(shaftAxis == Axis::X ? Axis::Z : Axis::X, positive);
    return facing;
}

std::vector<Vec3i> chainBetween(Vec3i const& start, Vec3i const& end, Slope slope, int facing) {
    Vec3i              diff = end - start;
    std::vector<Vec3i> out;
    int                limit = 1000;
    Vec3i              cur   = start;
    do {
        out.push_back(cur);
        if (slope == Slope::Vertical) {
            cur.y += diff.y > 0 ? 1 : -1;
            continue;
        }
        cur = cur + facingNormal(facing);
        if (slope != Slope::Horizontal) cur.y += slope == Slope::Upward ? 1 : -1;
    } while (!(cur == end) && limit-- > 0);
    out.push_back(end);
    return out;
}

std::vector<PlannedSegment> planBelt(ConnectWorld const& w, Vec3i const& start, Vec3i const& end) {
    auto  shaftAxis = w.shaftAxis(start).value_or(Axis::X);
    Slope slope     = slopeBetween(start, end);
    int   facing    = facingBetween(start, end, shaftAxis);
    auto  chain     = chainBetween(start, end, slope, facing);
    if (shaftAxis == Axis::Y) slope = Slope::Sideways;
    std::vector<PlannedSegment> out;
    for (auto const& p : chain) {
        Part part   = p == start ? Part::Start : p == end ? Part::End : Part::Middle;
        bool pulley = w.shaftAxis(p).has_value();
        if (part == Part::Middle && pulley) part = Part::Pulley;
        out.push_back({p, SegmentState{facing, slope, part, Casing::None, -1}});
    }
    return out;
}

// ---- speeds -------------------------------------------------------------------------------------------

float directionAwareSpeed(float rpm, int facing) {
    int offset = facingStep(facing);
    if (facingAxis(facing) == Axis::X) offset *= -1;
    return beltMovementSpeed(rpm) * float(offset);
}

int movementFacing(float rpm, int facing) {
    Axis axis     = facingAxis(facing);
    bool negative = (beltMovementSpeed(rpm) < 0) != (axis == Axis::X);
    return facingFromAxis(axis, !negative);
}

Vec3d vectorForOffset(SegmentState const& controller, int length, float offset) {
    int   verticality      = controller.slope == Slope::Downward ? -1 : controller.slope == Slope::Upward ? 1 : 0;
    float verticalMovement = float(verticality);
    if (offset < .5f) verticalMovement = 0;
    verticalMovement = verticalMovement * (std::min(offset, float(length) - .5f) - .5f);
    Vec3i n          = facingNormal(controller.facing);
    Vec3d horizontal = Vec3d{double(n.x), 0, double(n.z)} * (offset - .5);
    if (controller.slope == Slope::Vertical) horizontal = {};
    return horizontal + Vec3d{0, verticalMovement, 0};
}

// ---- inventory ---------------------------------------------------------------------------------------

float endingMargin(Ending e) {
    switch (e) {
    case Ending::Insert:
        return .25f;
    case Ending::Funnel:
        return .5f;
    case Ending::Blocked:
        return .45f;
    default:
        return 0;
    }
}

void Inventory::insertSorted(Item item) {
    auto it = items.begin();
    for (; it != items.end(); ++it) {
        // Create's insert: stop at the first stack that is behind the new one
        bool behind = it->beltPosition < item.beltPosition;
        if (behind == movementPositive) break;
    }
    items.insert(it, item);
}

void Inventory::flushQueues() {
    for (auto& i : toInsert) insertSorted(i);
    toInsert.clear();
    if (!toRemove.empty()) {
        std::erase_if(items, [&](Item const& i) {
            return std::find(toRemove.begin(), toRemove.end(), i.id) != toRemove.end();
        });
        toRemove.clear();
    }
}

bool Inventory::tick(InventoryEnv& env) {
    bool changed = false;
    if (!toInsert.empty() || !toRemove.empty()) {
        flushQueues();
        changed = true;
    }
    float speed = env.speed();
    if (speed == 0) return changed;
    if (movementPositive != (speed > 0)) {
        movementPositive = !movementPositive;
        std::reverse(items.begin(), items.end());
        changed = true;
    }
    constexpr float spacing = 1;
    Ending          ending  = Ending::Unresolved;
    int const       length  = env.length();
    bool const      horiz   = env.horizontal();
    long            prev    = -1; // the item in front (index), -1 none

    for (std::size_t i = 0; i < items.size();) {
        Item& cur           = items[i];
        cur.prevBeltPosition = cur.beltPosition;
        cur.prevSideOffset   = cur.sideOffset;
        if (env.emptyStack(cur)) {
            items.erase(items.begin() + long(i));
            changed = true;
            prev    = -1;
            continue;
        }
        float movement = speed;
        if (cur.lockedExternally) {
            cur.lockedExternally = false;
            prev                 = long(i++);
            continue;
        }
        bool  noMovement = false;
        float currentPos = cur.beltPosition;
        if (prev >= 0) {
            float diff = items[std::size_t(prev)].beltPosition - currentPos;
            if (std::abs(diff) <= spacing) noMovement = true;
            movement = movementPositive ? std::min(movement, diff - spacing) : std::max(movement, diff + spacing);
        }
        float diffToEnd = movementPositive ? float(length) - currentPos : -currentPos;
        if (std::abs(diffToEnd) < std::abs(movement) + 1) {
            if (ending == Ending::Unresolved) ending = env.resolveEnding();
            float margin = endingMargin(ending);
            diffToEnd += movementPositive ? -margin : margin;
        }
        float limited    = movementPositive ? std::min(movement, diffToEnd) : std::max(movement, diffToEnd);
        float nextOffset = cur.beltPosition + limited;

        if (horiz) {
            if (env.processing(cur, nextOffset, noMovement)) {
                items.erase(items.begin() + long(i));
                changed = true;
                continue;
            }
            if (cur.locked) {
                prev = long(i++);
                continue;
            }
        }
        if (env.funnels(cur, nextOffset)) {
            prev = long(i++);
            continue;
        }
        if (noMovement) {
            prev = long(i++);
            continue;
        }
        if (env.obstacles(cur, nextOffset)) {
            prev = long(i++);
            continue;
        }

        cur.beltPosition += limited;
        float diffToMiddle = cur.targetSideOffset - cur.sideOffset;
        cur.sideOffset += std::clamp(diffToMiddle * std::abs(limited) * 6.f, -std::abs(diffToMiddle), std::abs(diffToMiddle));

        if (limited == movement) {
            prev = long(i++);
            continue;
        }
        // the end is reached
        if (ending == Ending::Insert) {
            auto r = env.insertAtEnd(cur);
            if (r == EndInsert::Consumed) {
                items.erase(items.begin() + long(i));
                changed = true;
                continue;
            }
            if (r == EndInsert::Partial) changed = true;
        } else if (ending == Ending::Eject) {
            env.eject(cur);
            items.erase(items.begin() + long(i));
            changed = true;
            continue;
        }
        prev = long(i++);
    }
    return changed;
}

bool Inventory::canInsertAtFromSide(int segment, int side, int movementFace) const {
    float segmentPos = float(segment);
    if (movementFace == oppositeFace(side)) return false;
    if (movementFace != side) segmentPos += .5f;
    else if (!movementPositive) segmentPos += 1.f;
    auto blocking = [&](Item const& it) {
        float p = it.beltPosition;
        return it.insertedAt == segment && it.insertedFrom == side
            && (movementPositive ? p <= segmentPos + 1 : p >= segmentPos - 1);
    };
    for (auto const& it : items)
        if (blocking(it)) return false;
    for (auto const& it : toInsert)
        if (blocking(it)) return false;
    return true;
}

Item* Inventory::stackAtOffset(int segment) {
    float min = float(segment), max = float(segment + 1);
    for (auto& it : items) {
        if (std::find(toRemove.begin(), toRemove.end(), it.id) != toRemove.end()) continue;
        if (it.beltPosition > max) continue;
        if (it.beltPosition > min) return &it;
    }
    return nullptr;
}

Item const* Inventory::find(std::uint64_t id) const {
    for (auto const& it : items)
        if (it.id == id) return &it;
    for (auto const& it : toInsert)
        if (it.id == id) return &it;
    return nullptr;
}

Item* Inventory::find(std::uint64_t id) {
    return const_cast<Item*>(static_cast<Inventory const*>(this)->find(id));
}

std::vector<Item> Inventory::all() const {
    std::vector<Item> out = items;
    out.insert(out.end(), toInsert.begin(), toInsert.end());
    return out;
}

bool hoveringEnd(SegmentState const& s, Vec3d const& hitFromCentre) {
    if (s.part != Part::Start && s.part != Part::End) return false;
    Vec3i  v   = beltVector(s);
    double dot = hitFromCentre.x * v.x + hitFromCentre.y * v.y + hitFromCentre.z * v.z;
    return s.part == Part::End ? dot > 0 : dot < 0;
}

// ---- depots ------------------------------------------------------------------------------------------

bool depotSlide(Item& item) {
    item.prevBeltPosition = item.beltPosition;
    item.prevSideOffset   = item.sideOffset;
    float diff            = .5f - item.beltPosition;
    if (diff > 1 / 512.f) item.beltPosition += diff / 4.f;
    return diff < 1 / 16.f;
}

// ---- ejector -----------------------------------------------------------------------------------------

namespace {
/// Height after `t` whole ticks (Minecraft order: move, then gravity, then drag).
double heightAfter(double vy, int ticks, LaunchPhysics const& p) {
    double y = 0;
    for (int i = 0; i < ticks; ++i) {
        y += vy;
        vy = (vy - p.gravity) * p.verticalDrag;
    }
    return y;
}
double apexOf(double vy, LaunchPhysics const& p) {
    double y = 0, best = 0;
    for (int i = 0; i < 400 && vy > 0; ++i) {
        y += vy;
        vy   = (vy - p.gravity) * p.verticalDrag;
        best = std::max(best, y);
    }
    return best;
}
} // namespace

std::pair<double, double> Launch::at(double t) const {
    if (t <= 0) return {0, 0};
    double along = vx * (1 - std::pow(physics.horizontalDrag, t)) / (1 - physics.horizontalDrag);
    int    whole = int(std::floor(t));
    double y0    = heightAfter(vy, whole, physics);
    double y1    = heightAfter(vy, whole + 1, physics);
    double frac  = t - whole;
    return {along, y0 + (y1 - y0) * frac};
}

Launch planLaunch(int horizontal, int vertical, LaunchPhysics physics) {
    Launch l;
    l.horizontal = horizontal;
    l.vertical   = vertical;
    l.physics    = physics;
    // A high arc: the apex clears the target height by at least a block, more for longer shots.
    double apex = std::max(vertical, 0) + 1.0 + 0.3 * std::abs(horizontal);
    double lo = 0, hi = 8;
    for (int i = 0; i < 60; ++i) {
        double mid = (lo + hi) / 2;
        if (apexOf(mid, physics) < apex) lo = mid;
        else hi = mid;
    }
    l.vy = hi;
    // first tick on the way down at or below the target height (interpolated)
    double y = 0, vy = l.vy, prevY = 0;
    double t = 1;
    for (int i = 1; i < 600; ++i) {
        prevY = y;
        y += vy;
        vy = (vy - physics.gravity) * physics.verticalDrag;
        if (vy < 0 && y <= vertical && prevY > y) {
            double frac = prevY == y ? 1 : (prevY - vertical) / (prevY - y);
            t           = (i - 1) + std::clamp(frac, 0.0, 1.0);
            break;
        }
        t = i;
    }
    l.ticks = std::max(1.0, t);
    double reach = (1 - std::pow(physics.horizontalDrag, l.ticks)) / (1 - physics.horizontalDrag);
    l.vx         = horizontal / reach;
    return l;
}

float ejectorWindUp(float rpm, int horizontal, int vertical) {
    float speedFactor = std::abs(rpm) / 256.f;
    float distance    = (horizontal == 0 && vertical == 0) ? 1.f : std::sqrt(float(horizontal * horizontal + vertical * vertical));
    return speedFactor / distance;
}

std::optional<int> ejectorTargetFacing(Vec3i const& ejector, Vec3i const& target, int maxDistance) {
    if (ejector.x == target.x && ejector.z == target.z) return std::nullopt;
    int dx = target.x - ejector.x, dz = target.z - ejector.z;
    if (std::abs(dx) > maxDistance || std::abs(dz) > maxDistance) return std::nullopt;
    if (dx == 0) return dz < 0 ? 2 : 0;
    if (dz == 0) return dx < 0 ? 1 : 3;
    return std::nullopt;
}

} // namespace lv::core::belts
