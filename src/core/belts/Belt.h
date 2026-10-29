#pragma once

// Mechanical belts, depots and weighted ejectors: the game-independent rules (pure C++, gtest).
//
// Checked against Create 6.0.10: BeltBlock, BeltHelper, BeltConnectorItem (canConnect,
// createBelts), BeltSlicer, BeltInventory (tick, insertion, endings), BeltMovementHandler
// (entities), DepotBehaviour (held item sliding to the centre), WeightedEjectorBlockEntity.
//
// Conventions:
// - A horizontal facing is the Java 2D data value (0 south, 1 west, 2 north, 3 east), the order of
//   `game::playerFacing` and of the `levitate:facing` block state.
// - A face / side is a Bedrock FacingID (0 down, 1 up, 2 north, 3 south, 4 west, 5 east), as the
//   capabilities' `face` (Create's `Direction side`).
// - Belt positions (`beltPosition`) run from 0 at the outer edge of the START segment to `length`
//   at the outer edge of the END segment; segment i covers [i, i + 1).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lv::core::belts {

// ---- geometry ---------------------------------------------------------------------------------------

enum class Slope : std::uint8_t { Horizontal = 0, Upward = 1, Downward = 2, Vertical = 3, Sideways = 4 };
enum class Part : std::uint8_t { Start = 0, Middle = 1, End = 2, Pulley = 3 };
enum class Casing : std::uint8_t { None = 0, Andesite = 1, Brass = 2 };
enum class Axis : std::uint8_t { X = 0, Y = 1, Z = 2 };

struct Vec3i {
    int x = 0, y = 0, z = 0;
    friend bool  operator==(Vec3i const&, Vec3i const&) = default;
    friend auto  operator<=>(Vec3i const&, Vec3i const&) = default;
    Vec3i        operator+(Vec3i const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3i        operator-(Vec3i const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3i        operator*(int k) const { return {x * k, y * k, z * k}; }
    [[nodiscard]] long long distSqr() const { return 1LL * x * x + 1LL * y * y + 1LL * z * z; }
};

struct Vec3d {
    double x = 0, y = 0, z = 0;
    Vec3d operator+(Vec3d const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3d operator-(Vec3d const& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3d operator*(double k) const { return {x * k, y * k, z * k}; }
};

/// The block states of one belt segment.
struct SegmentState {
    int    facing = 0; ///< horizontal facing (start -> end for straight belts)
    Slope  slope  = Slope::Horizontal;
    Part   part   = Part::Middle;
    Casing casing = Casing::None;
    int    color  = -1; ///< dye colour 0..15 (Bedrock / Java dye order), -1 undyed
    friend bool operator==(SegmentState const&, SegmentState const&) = default;
};

[[nodiscard]] Vec3i facingNormal(int facing);
/// +1 for south / east, -1 for north / west (Create's `getAxisDirection().getStep()`).
[[nodiscard]] int  facingStep(int facing);
[[nodiscard]] Axis facingAxis(int facing);
[[nodiscard]] int  oppositeFacing(int facing);
/// Java `Direction.getClockWise()` (north -> east -> south -> west).
[[nodiscard]] int clockwise(int facing);
/// The facing of a horizontal unit offset (nullopt for anything else).
[[nodiscard]] std::optional<int> facingOf(int dx, int dz);
/// Bedrock FacingID of a horizontal facing.
[[nodiscard]] int faceOfFacing(int facing);
/// Horizontal facing of a FacingID (nullopt for up / down).
[[nodiscard]] std::optional<int> facingOfFace(int face);
[[nodiscard]] int  oppositeFace(int face);
[[nodiscard]] Vec3i faceNormal(int face);

/// Create's BeltHelper.getBeltVector: one step from START towards END.
[[nodiscard]] Vec3i beltVector(SegmentState const& s);
/// Create's BeltBlock.nextSegmentPosition (offset to the next / previous segment; nullopt past
/// the ends).
[[nodiscard]] std::optional<Vec3i> nextSegmentOffset(SegmentState const& s, bool forward);
/// Rotation axis of the pulleys (Create's BeltBlock.getRotationAxis).
[[nodiscard]] Axis rotationAxis(SegmentState const& s);
/// START, END and PULLEY segments have a shaft (Create's `hasPulley`).
[[nodiscard]] inline bool hasPulley(Part p) { return p != Part::Middle; }
/// Items and entities ride horizontal and sloped belts only (Create's `canTransportObjects`).
[[nodiscard]] inline bool canTransportObjects(Slope s) { return s != Slope::Vertical && s != Slope::Sideways; }
/// Create's BeltSlicer.beltStatesCompatible (two belts may be merged end to end).
[[nodiscard]] bool statesCompatible(SegmentState const& a, SegmentState const& b);
/// The segment seen from the other end (Create's BeltSlicer.flipBelt).
[[nodiscard]] SegmentState flipped(SegmentState s);

// ---- placing with the belt connector -----------------------------------------------------------------

/// What the connector needs to know about the world (Create's BeltConnectorItem.canConnect).
struct ConnectWorld {
    std::function<bool(Vec3i const&)>                loaded;
    /// The axis of a shaft at the position (nullopt: not a plain shaft).
    std::function<std::optional<Axis>(Vec3i const&)> shaftAxis;
    /// The theoretical speed of a kinetic block (RPM, signed; 0 when none).
    std::function<float(Vec3i const&)>               speed;
    /// The block can be replaced by a placement (air, grass, water, ...).
    std::function<bool(Vec3i const&)>                replaceable;
};

enum class ConnectError : std::uint8_t {
    None,
    Unloaded,
    SamePosition,
    TooFar,        ///< not closer than maxBeltLength
    NotAligned,    ///< not in a line / 45 degree diagonal perpendicular to the shaft axis
    AxisMismatch,  ///< the shafts have different axes
    VerticalShaftDiagonal,
    SpeedConflict, ///< the shafts turn in opposite directions
    Obstructed,    ///< a block in between can't be replaced
    NotShaft,
};
[[nodiscard]] char const* connectErrorName(ConnectError e);

/// Create's canConnect: `maxLength` is the config `maxBeltLength` (20).
[[nodiscard]] ConnectError canConnect(ConnectWorld const& w, Vec3i const& first, Vec3i const& second, int maxLength);

/// Create's getSlopeBetween.
[[nodiscard]] Slope slopeBetween(Vec3i const& start, Vec3i const& end);
/// Create's getFacingFromTo plus the vertical-belt special case of createBelts (`shaftAxis`).
[[nodiscard]] int facingBetween(Vec3i const& start, Vec3i const& end, Axis shaftAxis);
/// Create's getBeltChainBetween.
[[nodiscard]] std::vector<Vec3i> chainBetween(Vec3i const& start, Vec3i const& end, Slope slope, int facing);

struct PlannedSegment {
    Vec3i        pos;
    SegmentState state;
};
/// Create's createBelts: the segments to place (START, MIDDLE / PULLEY where a shaft stands,
/// END; SIDEWAYS when a pulley shaft is vertical).
[[nodiscard]] std::vector<PlannedSegment> planBelt(ConnectWorld const& w, Vec3i const& start, Vec3i const& end);

// ---- speeds ------------------------------------------------------------------------------------------

/// Create's getBeltMovementSpeed: blocks per tick (speed / 480).
[[nodiscard]] inline float beltMovementSpeed(float rpm) { return rpm / 480.f; }
/// Create's getDirectionAwareBeltMovementSpeed: > 0 moves items from START towards END.
[[nodiscard]] float directionAwareSpeed(float rpm, int facing);
/// Create's getMovementFacing: the horizontal facing items move to.
[[nodiscard]] int movementFacing(float rpm, int facing);

/// Create's BeltHelper.getVectorForOffset: centre line point at `offset` along the belt,
/// relative to the controller (START) block's centre.
[[nodiscard]] Vec3d vectorForOffset(SegmentState const& controller, int length, float offset);
/// The top surface of a belt / depot (Create: 13 px).
inline constexpr double kBeltTop = 13.0 / 16.0;

// ---- items on a belt ---------------------------------------------------------------------------------

/// Create's TransportedItemStack (the stack itself lives in the game layer, keyed by `id`).
struct Item {
    std::uint64_t id               = 0;
    float         beltPosition     = 0;
    float         prevBeltPosition = 0;
    float         sideOffset       = 0;
    float         prevSideOffset   = 0;
    int           insertedAt       = 0;
    int           insertedFrom     = 1; ///< FacingID (up by default)
    bool          locked           = false;
    bool          lockedExternally = false;
    /// Create's `getTargetSideOffset`: 0 unless the item was put on the belt sideways.
    float         targetSideOffset = 0;
};

/// Create's BeltInventory.Ending (margins in blocks before the end where items stop).
enum class Ending : std::uint8_t { Unresolved, Eject, Insert, Funnel, Blocked };
[[nodiscard]] float endingMargin(Ending e);

enum class EndInsert : std::uint8_t { Unchanged, Partial, Consumed };

/// The world as the belt inventory sees it during one tick.
class InventoryEnv {
public:
    virtual ~InventoryEnv() = default;
    /// Direction-aware speed (blocks per tick).
    [[nodiscard]] virtual float speed() const = 0;
    [[nodiscard]] virtual int   length() const = 0;
    /// Items are processed only on horizontal belts.
    [[nodiscard]] virtual bool horizontal() const { return true; }
    virtual Ending resolveEnding() { return Ending::Eject; }
    /// Belt processing (Create's handleBeltProcessingAndCheckIfRemoved); true = the item is gone.
    virtual bool processing(Item&, float /*nextOffset*/, bool /*noMovement*/) { return false; }
    /// Funnels on the belt (checkForFunnels); true = the item stops this tick.
    virtual bool funnels(Item&, float /*nextOffset*/) { return false; }
    /// Tunnels / crushers on the belt; true = the item is stuck this tick.
    virtual bool obstacles(Item&, float /*nextOffset*/) { return false; }
    /// Insert into the block after the end (Ending::Insert).
    virtual EndInsert insertAtEnd(Item&) { return EndInsert::Unchanged; }
    /// The item falls off the end (Ending::Eject); the inventory removes it afterwards.
    virtual void eject(Item&) {}
    /// The stack of an item is empty (removed by some other means).
    [[nodiscard]] virtual bool emptyStack(Item const&) const { return false; }
};

/// Create's BeltInventory: items sorted with the one furthest along the movement first.
class Inventory {
public:
    std::vector<Item>          items;
    std::vector<Item>          toInsert;
    std::vector<std::uint64_t> toRemove;
    bool                       movementPositive = true;

    /// Queues an item (inserted at the start of the next tick, as Create's addItem).
    void addItem(Item item) { toInsert.push_back(item); }
    /// Inserts now, keeping the order.
    void insertSorted(Item item);
    /// One tick (Create's BeltInventory.tick). Returns true when the contents changed.
    bool tick(InventoryEnv& env);
    /// Create's canInsertAtFromSide (`movementFace` = FacingID of the movement direction).
    [[nodiscard]] bool canInsertAtFromSide(int segment, int side, int movementFace) const;
    [[nodiscard]] bool canInsertAt(int segment, int movementFace) const { return canInsertAtFromSide(segment, 1, movementFace); }
    /// Create's getStackAtOffset (the first item within [segment, segment + 1]).
    [[nodiscard]] Item*       stackAtOffset(int segment);
    [[nodiscard]] Item const* find(std::uint64_t id) const;
    [[nodiscard]] Item*       find(std::uint64_t id);
    /// Every item, queued ones included.
    [[nodiscard]] std::vector<Item> all() const;
    [[nodiscard]] bool              empty() const { return items.empty() && toInsert.empty(); }
    /// Applies the queues now (removals, insertions).
    void flushQueues();
};

/// Create's BeltSlicer.hoveringEnd: the click is on the outer half of an end segment.
/// `hitFromCentre` = hit point minus the segment's centre.
[[nodiscard]] bool hoveringEnd(SegmentState const& s, Vec3d const& hitFromCentre);

// ---- depots ------------------------------------------------------------------------------------------

/// Create's DepotBehaviour.tick(TransportedItemStack): slides the held item to the centre; true
/// when it has arrived (within 1/16).
bool depotSlide(Item& item);

// ---- weighted ejector --------------------------------------------------------------------------------

/// Gravity and drag of a launched object (Bedrock entity physics: what players and mobs follow).
struct LaunchPhysics {
    double gravity         = 0.08;
    double horizontalDrag  = 0.91;
    double verticalDrag    = 0.98;
};

/// A launch towards a target `horizontal` blocks ahead and `vertical` blocks up (Create's
/// EntityLauncher): initial motion, flight time and positions along the arc.
struct Launch {
    int    horizontal = 1;
    int    vertical   = 0;
    double vx = 0, vy = 0; ///< initial motion (blocks per tick) along the facing / up
    double ticks = 1;      ///< flight time until the target height is reached on the way down
    LaunchPhysics physics;

    /// Offset (along, up) from the launch point after `t` ticks.
    [[nodiscard]] std::pair<double, double> at(double t) const;
};
[[nodiscard]] Launch planLaunch(int horizontal, int vertical, LaunchPhysics physics = {});

/// Create's WeightedEjectorBlockEntity.getWindUpSpeed: charge per tick (1 = fully charged).
[[nodiscard]] float ejectorWindUp(float rpm, int horizontal, int vertical);
/// Create's EjectorTargetHandler.getValidTargetDirection: the facing that reaches `target` from
/// `ejector` (nullopt: off to a side, straight above / below, or further than `maxDistance`).
[[nodiscard]] std::optional<int> ejectorTargetFacing(Vec3i const& ejector, Vec3i const& target, int maxDistance);

} // namespace lv::core::belts
