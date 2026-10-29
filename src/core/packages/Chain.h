#pragma once

// Chain conveyor maths and routing (U25, SPEC §3.6.25): Create 6.0.10's ChainConveyorBlockEntity
// geometry, its loop / branch rules and ChainConveyorRoutingTable. Pure logic, unit-tested.

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lv::core::packages {

struct V3 {
    double x = 0, y = 0, z = 0;
    friend V3   operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend V3   operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend V3   operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
    [[nodiscard]] double length() const { return std::sqrt(x * x + y * y + z * z); }
    [[nodiscard]] V3     normalized() const {
        double l = length();
        return l < 1e-9 ? V3{} : V3{x / l, y / l, z / l};
    }
    [[nodiscard]] double dot(V3 o) const { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] V3     cross(V3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    [[nodiscard]] double distanceTo(V3 o) const { return (*this - o).length(); }
};

/// A block offset (connection from one conveyor to another, or a port's offset).
struct Offset {
    int  x = 0, y = 0, z = 0;
    friend bool operator==(Offset const&, Offset const&) = default;
    friend auto operator<=>(Offset const&, Offset const&) = default;
    [[nodiscard]] Offset negated() const { return {-x, -y, -z}; }
    [[nodiscard]] bool   zero() const { return x == 0 && y == 0 && z == 0; }
    [[nodiscard]] double length() const { return std::sqrt(double(x) * x + double(y) * y + double(z) * z); }
};

// ---- geometry ----------------------------------------------------------------------------------

/// Create's `VecHelper.rotate(v, degrees, Axis.Y)`.
[[nodiscard]] V3 rotateY(V3 v, double degrees);
/// Angle wrapped into [0, 360).
[[nodiscard]] float wrapAngle(float angle);
/// Create's `AngleHelper.getShortestAngleDiff(current, target)` in (-180, 180].
[[nodiscard]] float shortestAngleDiff(double current, double target);

/// Create's ConnectionStats of the chain from a conveyor at `origin` (block corner) to the one
/// at `origin + connection` (coordinates in blocks; `reversed` = the conveyor turns backwards).
struct ConnectionStats {
    float tangentAngle = 0;
    float chainLength  = 0;
    V3    start, end;
};
[[nodiscard]] ConnectionStats connectionStats(V3 origin, Offset connection, bool reversed);

/// Where a package sits: on the loop around the conveyor at `chainPosition` degrees, or
/// `chainPosition` blocks along the chain of `stats`.
[[nodiscard]] V3 loopPosition(V3 origin, float chainPosition);
[[nodiscard]] V3 travelPosition(ConnectionStats const& stats, float chainPosition);

/// Create's `loopThresholdCrossed`: a package moving from `prev` to `now` degrees passed
/// `offBranchAngle` in the conveyor's direction of motion.
[[nodiscard]] bool loopThresholdCrossed(float now, float prev, float offBranchAngle, bool reversed);

/// Distance a package moves per tick along a chain (blocks) and around a loop (degrees) at
/// `rpm` (Create: speed / 360 blocks, a loop radius of 1.5).
[[nodiscard]] float distancePerTick(float rpm);
[[nodiscard]] float degreesPerTick(float rpm);

/// Chains used by a connection (Create's `getChainCost`).
[[nodiscard]] int chainCost(Offset connection);

/// Create's ChainConveyorConnectionHandler.validateAndConnect geometry checks: nullopt when the
/// second conveyor at `diff` (second - first) can be connected, otherwise the message key suffix
/// ("too_far", "too_close", "cannot_connect_vertically", "too_steep").
[[nodiscard]] std::optional<std::string> validateConnection(Offset diff, int maxLength);

/// The two chain strands of a connection (Create's `forPointsAlongChains`): `count` points in
/// total, alternating between the strands; `centre` is the centre of the first conveyor block.
[[nodiscard]] std::vector<V3> pointsAlongChains(ConnectionStats const& stats, V3 centre, int count);
/// The second strand's start point (the first strand runs `stats.start` -> `stats.end`).
[[nodiscard]] V3 mirroredStart(ConnectionStats const& stats, V3 centre);

/// Closest approach of the segment `a`..`b` to the ray `from` + t * `dir` (|dir| = 1, 0 <= t <=
/// `range`): returns {t along the ray, fraction along the segment, distance} when within `radius`.
struct SegmentHit {
    double t = 0, along = 0, distance = 0;
};
[[nodiscard]] std::optional<SegmentHit> raySegment(V3 from, V3 dir, double range, V3 a, V3 b, double radius);

// ---- routing -----------------------------------------------------------------------------------

/// Create's ChainConveyorRoutingTable: ports advertise their address filter, conveyors pass the
/// entries to their neighbours (distance + 1) and packages leave a loop towards the first entry
/// whose port matches their address.
class RoutingTable {
public:
    static constexpr int kEntryTimeout     = 100;
    static constexpr int kPortEntryTimeout = 20;

    struct Entry {
        std::string port;
        int         distance = 0;
        Offset      nextConnection; ///< zero = this conveyor's loop
        int         timeout  = 0;
        bool        endOfRoute = false;
    };

    std::vector<Entry> entries; ///< sorted by distance
    int                lastUpdate = 0;
    bool               changed    = false;

    void tick();
    [[nodiscard]] bool shouldAdvertise() const;
    /// A port on this conveyor (`connection` zero for loop ports) catches `filter`.
    void receivePortInfo(std::string const& filter, Offset connection);
    /// Where a box with `address` leaves this conveyor (zero offset: stay on the loop).
    [[nodiscard]] Offset exitFor(std::string const& address) const;
    /// Passes this table's entries to the neighbour reached through `connection`.
    void advertiseTo(Offset connection, RoutingTable& other) const;
    /// Drops the end-of-route entries of a port that stopped catching `filter`.
    void removePort(std::string const& filter);

private:
    void insert(Entry entry);
};

// ---- packing -------------------------------------------------------------------------------------

/// A stack as the packing helpers see it: `key` identifies item + data, `maxStack` its stack size.
struct Stack {
    std::string key;
    int         count    = 0;
    int         maxStack = 64;
    friend bool operator==(Stack const&, Stack const&) = default;
};

/// Number of slots of a package (Create's PackageItem.SLOTS).
inline constexpr int kPackageSlots = 9;

/// NeoForge's `ItemHandlerHelper.insertItemStacked` into `slots` (slot limit 64): merges into
/// stacks of the same key first, then fills empty slots in order. Returns the count left over.
int insertStacked(std::vector<Stack>& slots, Stack stack);

/// Order fragment info of a package (Create's PackageOrderData without the context).
struct Fragment {
    int  orderId       = -1;
    int  linkIndex     = 0;
    bool isFinalLink   = false;
    int  fragmentIndex = 0;
    bool isFinal       = false;
};
/// Create's `PackageRepackageHelper.isOrderComplete`: every link up to the final link has
/// fragments 0..final.
[[nodiscard]] bool isOrderComplete(std::vector<Fragment> const& fragments);

/// Create's `PackageRepackageHelper.repack` item distribution: `summary` (merged contents of all
/// fragments, in first-seen order) is split into output slots, the ordered stacks first; the slots
/// are then grouped into packages of 9. Returns the packages' slots.
[[nodiscard]] std::vector<std::vector<Stack>> repackSlots(std::vector<Stack> summary, std::vector<Stack> ordered);

} // namespace lv::core::packages
