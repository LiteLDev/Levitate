#pragma once

// Bulk processing maths (U13, pure C++): Create 6.0.10's millstone, crushing wheel and encased fan
// numbers, independent of the game so they are unit-tested (tests/core/BulkTest.cpp).
//
// - Fans: `AirCurrent` (Create's AirCurrent: flow limit through partial blocks, processing
//   segments, bounding box, entity push) and `IAirCurrentSource.getMaxDistance`.
// - Millstone: `MillstoneBlockEntity.getProcessingSpeed`.
// - Crushing wheels: `CrushingWheelBlock.updateControllers` (controller facing) and
//   `CrushingWheelControllerBlockEntity.tick` (processing speed, entity motion).
// - Nozzle: `NozzleBlockEntity.tick` (push vector).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "core/kinetics/Types.h"

namespace lv::core::bulk {

using kinetics::Axis;
using kinetics::Face;

struct Vec3d {
    double x = 0, y = 0, z = 0;
    friend Vec3d operator+(Vec3d a, Vec3d const& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec3d operator-(Vec3d a, Vec3d const& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec3d operator*(Vec3d a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    [[nodiscard]] double length() const;
    [[nodiscard]] Vec3d  normalized() const;
    [[nodiscard]] double get(Axis a) const { return a == Axis::X ? x : a == Axis::Y ? y : z; }
};

/// Unit vector of a face.
[[nodiscard]] Vec3d normalOf(Face f);

// ---- fan processing types --------------------------------------------------------------------------------

/// Create's FanProcessingType (None = no processing; Create's null).
enum class FanType : std::uint8_t { None = 0, Splashing, Haunting, Smoking, Blasting };

/// Create's priorities: when several types are valid at a block the highest wins.
[[nodiscard]] int              priorityOf(FanType t);
[[nodiscard]] std::string_view fanTypeName(FanType t); ///< "splashing", "haunting", "smoking", "blasting", ""
[[nodiscard]] FanType          fanTypeByName(std::string_view name);
/// The highest priority type among `valid` (None if empty).
[[nodiscard]] FanType bestType(std::vector<FanType> const& valid);

// ---- fan source ----------------------------------------------------------------------------------------------

struct FanConfig {
    float pushDistance   = 20;  ///< fanPushDistance
    float pullDistance   = 20;  ///< fanPullDistance
    float rotationArgmax = 256; ///< fanRotationArgmax
};

/// `IAirCurrentSource.getMaxDistance`: lerp(min(|speed| / argmax, 1), 3, push / pull distance).
[[nodiscard]] float fanMaxDistance(float speed, FanConfig const& config);
/// `EncasedFanBlockEntity.getAirFlowDirection` == origin side: the speed converted to the facing's
/// direction (negated for negative faces) is positive. `speed` must not be 0.
[[nodiscard]] bool fanPushing(Face facing, float speed);
/// Ticks an item needs in a fan's current: fanProcessingTime * ((count - 1) / 16 + 1) + 1.
[[nodiscard]] int fanProcessingTime(int count, int baseTime);

// ---- air current -----------------------------------------------------------------------------------------------

/// What a block in the current's path does to it (`AirCurrent.getFlowLimit`).
struct FlowBlock {
    enum class Kind : std::uint8_t { Pass, Full, Partial, Unloaded };
    Kind kind = Kind::Pass;
    /// Collision box of a partial block, in block units (0..1).
    std::array<double, 3> min{0, 0, 0}, max{1, 1, 1};
    /// More boxes of a partial block whose shape has several (stairs, walls, ...): the shape is
    /// the union of `min`/`max` and these.
    struct Box {
        std::array<double, 3> min{0, 0, 0}, max{1, 1, 1};
    };
    std::vector<Box> more;
};

/// Create's `findMaxDepth`: how deep the current reaches into a partial block when travelling
/// along `face` (checked at 5 points of the face); +infinity when one of them is a hole.
[[nodiscard]] double findMaxDepth(FlowBlock const& box, Face face);
/// Create's `getFlowLimit`: `blockAt(i)` describes the block `i` (1-based) blocks in front.
[[nodiscard]] float flowLimit(float max, Face facing, std::function<FlowBlock(int i)> const& blockAt);
/// `AirCurrent.getLimit`: the number of blocks the current touches.
[[nodiscard]] int limitOf(float maxDistance);

struct Segment {
    FanType type        = FanType::None;
    int     startOffset = 0;
    int     endOffset   = 0;
};

/// The air current of one source (Create's AirCurrent after `rebuild`).
struct AirCurrent {
    Face                 direction   = Face::Up; ///< the source's origin side (fan facing)
    bool                 pushing     = true;
    float                maxDistance = 0;
    std::vector<Segment> segments;

    /// Builds the segments: `catalystAt(i)` is the processing type of the block `i` blocks in front
    /// (None = no catalyst); a type applies downstream of its catalyst (Create's `rebuild`).
    void buildSegments(std::function<FanType(int i)> const& catalystAt);
    /// `AirCurrent.getTypeAt`: the type at `offset` blocks from the fan's face.
    [[nodiscard]] FanType typeAt(float offset) const;
};

/// Axis-aligned box in world coordinates.
struct Box {
    Vec3d min, max;
    [[nodiscard]] bool empty() const { return max.x <= min.x || max.y <= min.y || max.z <= min.z; }
    [[nodiscard]] bool intersects(Box const& o) const {
        return min.x < o.max.x && max.x > o.min.x && min.y < o.max.y && max.y > o.min.y && min.z < o.max.z && max.z > o.min.z;
    }
};

/// The current's bounding box (Create's `rebuild`): from the block in front of the fan at
/// (x, y, z) along `direction`, `maxDistance` long; empty below 0.25.
[[nodiscard]] Box currentBounds(int x, int y, int z, Face direction, float maxDistance);
/// `VecHelper.alignedDistanceToFace`: distance along `face`'s axis from `p` to the block's face.
[[nodiscard]] double alignedDistanceToFace(Vec3d const& p, int bx, int by, int bz, Face face);

/// `AirCurrent.tickAffectedEntities`: the entity's new motion. `distanceToCenter` is the
/// distance from the entity to the fan block's centre.
[[nodiscard]] Vec3d
pushedMotion(Vec3d const& motion, AirCurrent const& current, float speed, bool sneaking, double distanceToCenter);

// ---- nozzle ----------------------------------------------------------------------------------------------------

/// `NozzleBlockEntity.tick`: added motion for an entity at `diff` from the nozzle's centre
/// (nullopt: not affected: out of range, or a pulling nozzle closer than 1.5 blocks).
[[nodiscard]] std::optional<Vec3d> nozzlePush(Vec3d const& diff, float range, bool pushing, bool isItem);

// ---- millstone -------------------------------------------------------------------------------------------------

/// `MillstoneBlockEntity.getProcessingSpeed`: clamp((int) |speed / 16|, 1, 512) per tick.
[[nodiscard]] int millstoneProcessingSpeed(float speed);
/// Default duration of a recipe without `processing_time` (Create's ProcessingRecipe default 100).
inline constexpr int kDefaultProcessingTime = 100;

// ---- crushing wheels -------------------------------------------------------------------------------------------

/// `CrushingWheelControllerBlock.updateSpeed`: |wheel speed| / 50.
[[nodiscard]] float crushingSpeedOf(float wheelSpeed);
/// Processing per tick: clamp(crushingSpeed * 4 / log2(count) (1 once the recipe is applied),
/// .25, 20) (a single item: 20).
[[nodiscard]] float crusherProcessingSpeed(float crushingSpeed, int count, bool appliedRecipe);
/// `updateControllers`: the controller's facing for a wheel with `wheelAxis` turning at `speed`,
/// whose partner is on `side` (the direction items move through the pair).
[[nodiscard]] Face controllerFacing(Axis wheelAxis, Face side, float speed);
/// The motion Create gives the entity being crushed (`movement` along the facing axis, centring on
/// the other horizontal axes unless sneaking).
[[nodiscard]] Vec3d crusherEntityMotion(Face facing, float crushingSpeed, Vec3d const& entityPos, int bx, int bz, bool sneaking);
/// Where ejected items appear (`outPos`) and their motion (`outSpeed`).
[[nodiscard]] Vec3d crusherOutPos(int bx, int by, int bz, Face facing);
[[nodiscard]] Vec3d crusherOutSpeed(Face facing);
/// Whether an item entity being pulled through has gone far enough to be taken in.
[[nodiscard]] bool crusherIntakeReached(Face facing, Vec3d const& entityPos, int bx, int by, int bz);

} // namespace lv::core::bulk
