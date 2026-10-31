#pragma once

// Create's contraption assembly rules (pure C++): which blocks a moving structure takes along.
//
// The search starts from a frontier (e.g. the block in front of a bearing) and adds blocks
// breadth-first. From each collected block, a neighbour is added when
//   - both are inside one super glue region, or
//   - the neighbour is attached towards this block (a torch on it, a ladder, a door half...), or
//   - both can stick and this block is sticky (slime / honey), or
//   - the neighbour lies in the forced movement direction (pistons push the line in front),
// plus the chassis rules (linear chassis lines and sticky ends, radial chassis discs, connected
// chassis), stickers (an extended sticker takes the block it faces) and linked parts (both halves
// of a door, a bed, a double chest). Unmovable blocks stop the assembly with an error, as do
// unloaded chunks and more than `maxBlocks` blocks.
//
// The game adapter describes each position with `BlockInfo`; everything else is decided here.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/contraption/Geometry.h"

namespace lv::core::contraption {

using DirMask = std::uint8_t;
[[nodiscard]] inline constexpr DirMask bit(Dir d) { return DirMask(1u << static_cast<int>(d)); }
[[nodiscard]] inline constexpr bool    has(DirMask m, Dir d) { return (m & bit(d)) != 0; }

enum class ChassisKind : std::uint8_t { None, Linear, SecondaryLinear, Radial };
enum class StickyKind : std::uint8_t { None, Slime, Honey };

struct BlockInfo {
    bool        loaded    = true;  ///< false: chunk not loaded (assembly error)
    bool        necessary = false; ///< false for air, replaceable or collision-less blocks (not moved)
    bool        movable   = true;  ///< false: unmovable block (assembly error)
    bool        brittle   = false; ///< torches, ladders, rails, ... (placed last, never stuck to)
    bool        pushOnly  = false; ///< glazed terracotta (never sticks)
    bool        anchor    = false; ///< the anchor itself (never moved)
    StickyKind  sticky    = StickyKind::None;
    DirMask     attachedTowards = 0; ///< faces this block hangs on (torch on a wall: the wall's side)
    DirMask     notSupportive   = 0; ///< faces that don't carry neighbours (Create's isNotSupportive)
    std::vector<Pos> linked;          ///< positions that always move with this one (other door half)
    DirMask     pulls = 0;            ///< neighbours that always join when this block moves (bearing fronts)
    std::string id;                   ///< block id (error messages)

    // chassis
    ChassisKind chassis      = ChassisKind::None;
    Axis        chassisAxis  = Axis::Y;
    DirMask     stickyFaces  = 0; ///< glueable faces that are sticky (world directions)
    int         chassisRange = 0;

    // sticker
    std::optional<Dir> stickerFacing; ///< set when the block is an *extended* sticker
};

using WorldQuery = std::function<BlockInfo(Pos const&)>;
/// True if `a` and `a + offset(d)` are inside one super glue region.
using GlueQuery = std::function<bool(Pos const& a, Dir d)>;

struct AssemblyLimits {
    int                maxBlocks = 2048;
    std::optional<Dir> forced; ///< movement direction of a linear anchor (pistons, pulleys)
    int                minY = -64, maxY = 320; ///< outside the build height nothing is collected
};

enum class AssemblyError : std::uint8_t { None, Unmovable, UnloadedChunk, TooLarge };

struct AssemblyResult {
    AssemblyError    error = AssemblyError::None;
    Pos              errorPos;
    std::string      errorBlock;
    std::vector<Pos> blocks; ///< collected positions in visit order (solid first is up to the caller)
    [[nodiscard]] bool ok() const { return error == AssemblyError::None; }
};

/// Collects the structure reachable from `start`.
[[nodiscard]] AssemblyResult
assemble(std::vector<Pos> const& start, WorldQuery const& world, GlueQuery const& glue, AssemblyLimits const& limits);

/// The positions a chassis adds (Create's getIncludedBlockPositions). `forced` is the movement
/// direction (a linear chassis end that faces it collects even when not sticky).
[[nodiscard]] std::vector<Pos>
chassisIncluded(Pos const& chassisPos, BlockInfo const& chassis, WorldQuery const& world, std::optional<Dir> forced);

/// Chassis connected to `chassisPos` that join the frontier (Create's addAttachedChasses).
[[nodiscard]] std::vector<Pos> attachedChassis(Pos const& chassisPos, BlockInfo const& chassis, WorldQuery const& world);

/// The glueable faces of a chassis: the two axis ends (linear) or the four sides (radial).
[[nodiscard]] DirMask glueableFaces(ChassisKind kind, Axis axis);

/// Whether `a` and `b` (a block and its neighbour) may stick (Create: both non-brittle, not push
/// only, supportive towards each other, and slime doesn't stick to honey).
[[nodiscard]] bool canStick(BlockInfo const& a, BlockInfo const& b, Dir fromAToB);

} // namespace lv::core::contraption
