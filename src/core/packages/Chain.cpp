#include "core/packages/Chain.h"

#include <algorithm>
#include <numbers>

#include "core/packages/Address.h"

namespace lv::core::packages {

namespace {
constexpr double kDeg       = std::numbers::pi / 180.0;
constexpr float  kOffBranch = 35.f; // degrees between a chain's direction and where it leaves the loop
constexpr double kPx        = 1 / 16.0;

int signOf(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }
} // namespace

// ---- geometry ----------------------------------------------------------------------------------

V3 rotateY(V3 v, double degrees) {
    if (degrees == 0) return v;
    double a = degrees * kDeg;
    double s = std::sin(a), c = std::cos(a);
    return {v.x * c + v.z * s, v.y, v.z * c - v.x * s};
}

float wrapAngle(float angle) {
    angle = std::fmod(angle, 360.f);
    if (angle < 0) angle += 360.f;
    return angle;
}

float shortestAngleDiff(double current, double target) {
    current = std::fmod(current, 360.0);
    target  = std::fmod(target, 360.0);
    return float(std::fmod(std::fmod(target - current, 360.0) + 540.0, 360.0) - 180.0);
}

ConnectionStats connectionStats(V3 origin, Offset connection, bool reversed) {
    float dirSign   = reversed ? -1.f : 1.f;
    float direction = float(std::atan2(double(connection.x), double(connection.z)) / kDeg);
    float angle     = wrapAngle(direction - kOffBranch * dirSign);
    float opposite  = wrapAngle(angle + 180 + 2 * kOffBranch * dirSign);
    V3    bottom    = origin + V3{0.5, 0, 0.5};
    V3    other     = bottom + V3{double(connection.x), double(connection.y), double(connection.z)};
    ConnectionStats s;
    s.tangentAngle = angle;
    s.start        = bottom + rotateY({0, 0, 1.25}, angle) + V3{0, 6 * kPx, 0};
    s.end          = other + rotateY({0, 0, 1.25}, opposite) + V3{0, 6 * kPx, 0};
    s.chainLength  = float(s.start.distanceTo(s.end));
    return s;
}

V3 loopPosition(V3 origin, float chainPosition) {
    return origin + V3{0.5, 0, 0.5} + rotateY({0, 6 * kPx, 0.875}, chainPosition);
}

V3 travelPosition(ConnectionStats const& stats, float chainPosition) {
    V3 dir = (stats.end - stats.start).normalized();
    return stats.start + dir * std::min<double>(stats.chainLength, chainPosition);
}

bool loopThresholdCrossed(float now, float prev, float offBranchAngle, bool reversed) {
    int  sign1      = signOf(shortestAngleDiff(offBranchAngle, prev));
    int  sign2      = signOf(shortestAngleDiff(offBranchAngle, now));
    bool notCrossed = (sign1 >= sign2 && !reversed) || (sign1 <= sign2 && reversed);
    return !notCrossed;
}

float distancePerTick(float rpm) { return std::abs(rpm / 360.f); }

float degreesPerTick(float rpm) {
    float speed = rpm / 360.f;
    return float(speed / (std::numbers::pi * 1.5) * 360.0);
}

int chainCost(Offset connection) {
    return int(std::max<long long>(std::llround(std::floor(connection.length() / 2.5 + 0.5)), 1));
}

std::optional<std::string> validateConnection(Offset diff, int maxLength) {
    double distSqr = double(diff.x) * diff.x + double(diff.y) * diff.y + double(diff.z) * diff.z;
    if (!(distSqr < double(maxLength) * maxLength)) return "too_far";
    if (distSqr < 2.5 * 2.5) return "too_close";
    double horizontal = std::sqrt(double(diff.x) * diff.x + double(diff.z) * diff.z) - 1.5;
    if (horizontal <= 0) return "cannot_connect_vertically";
    if (std::abs(double(diff.y)) / horizontal > 1) return "too_steep";
    return std::nullopt;
}

V3 mirroredStart(ConnectionStats const& stats, V3 centre) {
    V3 direction = stats.end - stats.start;
    V3 normal    = direction.cross({0, 1, 0}).normalized();
    V3 offset    = stats.start - centre;
    return centre + (offset + normal * (-2 * normal.dot(offset)));
}

std::vector<V3> pointsAlongChains(ConnectionStats const& stats, V3 centre, int count) {
    std::vector<V3> out;
    V3              direction = stats.end - stats.start;
    V3              start2    = mirroredStart(stats, centre);
    for (bool first : {true, false}) {
        int steps = count / 2 + (first ? count % 2 : 0);
        for (int i = 0; i < steps; ++i) out.push_back((first ? stats.start : start2) + direction * ((0.5 + i) / steps));
    }
    return out;
}

std::optional<SegmentHit> raySegment(V3 from, V3 dir, double range, V3 a, V3 b, double radius) {
    V3     u = dir, v = b - a, w = from - a;
    double aa = u.dot(u), bb = u.dot(v), cc = v.dot(v), dd = u.dot(w), ee = v.dot(w);
    double den = aa * cc - bb * bb;
    double t = 0, s = 0;
    if (den < 1e-9) {
        t = 0;
        s = cc > 1e-9 ? ee / cc : 0;
    } else {
        t = (bb * ee - cc * dd) / den;
        s = (aa * ee - bb * dd) / den;
    }
    s = std::clamp(s, 0.0, 1.0);
    // re-project the ray parameter onto the clamped segment point
    t = std::clamp((a + v * s - from).dot(u) / aa, 0.0, range);
    // and the segment point onto the clamped ray point
    if (cc > 1e-9) s = std::clamp((from + u * t - a).dot(v) / cc, 0.0, 1.0);
    double dist = ((from + u * t) - (a + v * s)).length();
    if (dist > radius) return std::nullopt;
    return SegmentHit{t, s, dist};
}

// ---- routing -----------------------------------------------------------------------------------

void RoutingTable::tick() {
    for (auto& e : entries) --e.timeout;
    auto before = entries.size();
    std::erase_if(entries, [](Entry const& e) { return e.timeout <= 0; });
    changed |= entries.size() != before;
    ++lastUpdate;
}

bool RoutingTable::shouldAdvertise() const { return changed || lastUpdate > kEntryTimeout - 20; }

void RoutingTable::receivePortInfo(std::string const& filter, Offset connection) {
    insert({filter, filter == "*" ? 1000 : 0, connection, kPortEntryTimeout, true});
}

Offset RoutingTable::exitFor(std::string const& address) const {
    for (auto const& e : entries)
        if (matchAddress(address, e.port)) return e.nextConnection;
    return {};
}

void RoutingTable::advertiseTo(Offset connection, RoutingTable& other) const {
    Offset back = connection.negated();
    for (auto const& e : entries)
        if (e.endOfRoute || connection != e.nextConnection)
            other.insert({e.port, e.distance + 1, connection.negated(), kEntryTimeout, false});
    std::erase_if(other.entries, [&](Entry const& e) {
        return e.timeout < kEntryTimeout && !e.endOfRoute && back == e.nextConnection;
    });
}

void RoutingTable::removePort(std::string const& filter) {
    std::erase_if(entries, [&](Entry const& e) { return e.endOfRoute && e.port == filter; });
    changed = true;
}

void RoutingTable::insert(Entry entry) {
    std::size_t target = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto& other = entries[i];
        if (other.distance > entry.distance) break;
        if (other.port == entry.port) {
            if (other.distance == entry.distance && other.nextConnection == entry.nextConnection) other.timeout = kEntryTimeout;
            return;
        }
        target = i + 1;
    }
    entries.insert(entries.begin() + std::ptrdiff_t(target), std::move(entry));
    changed = true;
}

// ---- packing -------------------------------------------------------------------------------------

int insertStacked(std::vector<Stack>& slots, Stack stack) {
    int limit = std::min(64, std::max(1, stack.maxStack));
    for (int pass = 0; pass < 2 && stack.count > 0; ++pass) {
        for (auto& slot : slots) {
            bool empty = slot.count <= 0;
            if (pass == 0 ? (empty || slot.key != stack.key) : !empty) continue;
            if (empty) slot = Stack{stack.key, 0, stack.maxStack};
            int moved = std::min(limit - slot.count, stack.count);
            if (moved <= 0) continue;
            slot.count += moved;
            stack.count -= moved;
            if (stack.count <= 0) break;
        }
    }
    return stack.count;
}

bool isOrderComplete(std::vector<Fragment> const& fragments) {
    bool finalLinkReached = false;
    for (int link = 0; link < 1000; ++link) {
        if (finalLinkReached) break;
        bool linkDone = false;
        for (int counter = 0; counter < 1000 && !linkDone; ++counter) {
            Fragment const* found = nullptr;
            for (auto const& f : fragments)
                if (f.linkIndex == link && f.fragmentIndex == counter) {
                    found = &f;
                    break;
                }
            if (!found) return false;
            finalLinkReached = found->isFinalLink;
            if (found->isFinal) linkDone = true;
        }
        if (!linkDone) return false;
    }
    return true;
}

std::vector<std::vector<Stack>> repackSlots(std::vector<Stack> allItems, std::vector<Stack> ordered) {
    std::vector<Stack> outputSlots;
    while (true) {
        std::erase_if(allItems, [](Stack const& e) { return e.count == 0; });
        if (allItems.empty()) break;
        std::optional<Stack> targeted;
        if (!ordered.empty()) {
            targeted = ordered.front();
            ordered.erase(ordered.begin());
        }
        for (auto& entry : allItems) {
            int targetAmount = entry.count;
            if (targetAmount == 0) continue;
            if (targeted) {
                targetAmount = targeted->count;
                if (entry.key != targeted->key) continue;
            }
            bool exhausted = false;
            while (targetAmount > 0) {
                int removed = std::min(std::min(targetAmount, std::max(1, entry.maxStack)), entry.count);
                if (removed == 0) {
                    exhausted = true;
                    break;
                }
                outputSlots.push_back({entry.key, removed, entry.maxStack});
                targetAmount -= removed;
                if (targeted) targeted->count = targetAmount;
                entry.count -= removed;
            }
            if (exhausted) continue; // Create: `continue ItemSearch`
            break;                   // Create: `continue Repack`
        }
    }
    std::vector<std::vector<Stack>> packages;
    std::vector<Stack>              current;
    for (auto const& s : outputSlots) {
        current.push_back(s);
        if (int(current.size()) < kPackageSlots) continue;
        packages.push_back(std::move(current));
        current.clear();
    }
    if (!current.empty()) packages.push_back(std::move(current));
    return packages;
}

} // namespace lv::core::packages
