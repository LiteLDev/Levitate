#pragma once

// Pure logic of the item logistics unit (U12, SPEC §3.6.U12): brass tunnel distribution, tunnel
// shapes, chute air flow, item vault structures, extraction counts. No game headers.

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lv::core::funnels {

// ---- faces --------------------------------------------------------------------------------------
// Bedrock FacingID: 0 down, 1 up, 2 north (-z), 3 south (+z), 4 west (-x), 5 east (+x).
inline constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;

[[nodiscard]] constexpr int oppositeFace(int face) { return face < 0 ? face : (face ^ 1); }
[[nodiscard]] constexpr bool isHorizontal(int face) { return face >= 2 && face <= 5; }
/// Axis of a face: 0 x, 1 y, 2 z.
[[nodiscard]] constexpr int axisOf(int face) { return face <= 1 ? 1 : (face <= 3 ? 2 : 0); }
[[nodiscard]] constexpr bool isPositive(int face) { return face == kUp || face == kSouth || face == kEast; }
[[nodiscard]] constexpr std::array<int, 3> faceOffset(int face) {
    switch (face) {
    case kDown: return {0, -1, 0};
    case kUp: return {0, 1, 0};
    case kNorth: return {0, 0, -1};
    case kSouth: return {0, 0, 1};
    case kWest: return {-1, 0, 0};
    case kEast: return {1, 0, 0};
    default: return {0, 0, 0};
    }
}
/// Java's horizontal order (2D data values): south, west, north, east.
inline constexpr std::array<int, 4> kHorizontals{kSouth, kWest, kNorth, kEast};
/// Java's Direction order: down, up, north, south, west, east (= FacingID order).
inline constexpr std::array<int, 6> kAllFaces{kDown, kUp, kNorth, kSouth, kWest, kEast};
/// Java's Direction.getClockWise / getCounterClockWise (seen from above) of a horizontal face.
[[nodiscard]] constexpr int clockWise(int face) {
    switch (face) {
    case kNorth: return kEast;
    case kEast: return kSouth;
    case kSouth: return kWest;
    case kWest: return kNorth;
    default: return face;
    }
}
[[nodiscard]] constexpr int counterClockWise(int face) { return oppositeFace(clockWise(face)); }
/// The positive horizontal face of an axis (0 x -> east, 2 z -> south).
[[nodiscard]] constexpr int positiveFace(int axis) { return axis == 0 ? kEast : (axis == 1 ? kUp : kSouth); }

// ---- extraction counts (Create's FilteringBehaviour amount + ExtractionCountMode) ------------------

/// Stored amount setting of a filter with a count: `upTo` (row 0 "Up to") or exactly (row 1).
struct AmountSetting {
    int  count = 64;
    bool upTo  = true;
    friend bool operator==(AmountSetting const&, AmountSetting const&) = default;
};
/// The value box text of an amount: "Any" for "up to" the maximum, else "≤n" / "=n".
[[nodiscard]] std::string amountLabel(AmountSetting s, int maxStack, std::string_view anyText);

// ---- brass tunnel distribution -------------------------------------------------------------------

/// Create's BrassTunnelBlockEntity.SelectionMode, in the option box order.
enum class SelectionMode { Split, ForcedSplit, RoundRobin, ForcedRoundRobin, PreferNearest, Randomize, Synchronize };
inline constexpr int kSelectionModes = 7;
[[nodiscard]] std::string_view selectionModeKey(SelectionMode mode);     ///< "create.tunnel.selection_mode.split"
[[nodiscard]] std::string_view selectionModeEnglish(SelectionMode mode); ///< "Split"

/// How many of `count` items output `target` would refuse (0 = takes all); nullopt when it is not
/// an output right now (filtered out, gone).
using OutputProbe = std::function<std::optional<int>(int target, int count)>;

struct DistributionPlan {
    std::vector<std::pair<int, int>> give; ///< (target, items), in insertion order
    int                              kept = 0; ///< items that stay in the tunnel
};

/// Index of the output to start with (Create: round robin continues after the previous output,
/// randomize picks `random % targets`, prefer nearest / synchronize start at the first output).
[[nodiscard]] int distributionStart(SelectionMode mode, int previousIndex, int targets, unsigned random);

/// Plans handing `stackCount` items to `targets` outputs (Create's BrassTunnelBlockEntity.distribute):
/// - split: even shares (the first `count % outputs` outputs one more), full outputs skipped and
///   their share spread over the others (a second round takes the leftovers);
/// - forced split: never skips an output: while one takes none of its share nothing moves;
/// - round robin: the whole stack to the output at `start` (nothing when it is full: the next
///   distribution moves on to the next output);
/// - forced round robin: like round robin, but waits while that output can't take the whole stack;
/// - prefer nearest / randomize / synchronize: the whole stack to the first output (from `start`)
///   that takes any of it.
[[nodiscard]] DistributionPlan planDistribution(SelectionMode mode, int stackCount, int targets, int start, OutputProbe const& probe);

// ---- tunnel shapes ------------------------------------------------------------------------------

/// Create's BeltTunnelBlock.Shape (block state `levitate:shape`).
enum class TunnelShape { Straight = 0, Window = 1, Closed = 2, TLeft = 3, TRight = 4, Cross = 5 };

[[nodiscard]] constexpr bool hasWindow(TunnelShape s) { return s == TunnelShape::Window || s == TunnelShape::Closed; }
[[nodiscard]] constexpr bool isStraight(TunnelShape s) { return hasWindow(s) || s == TunnelShape::Straight; }
[[nodiscard]] constexpr bool isJunction(TunnelShape s) {
    return s == TunnelShape::Cross || s == TunnelShape::TLeft || s == TunnelShape::TRight;
}
/// The "left" side of a tunnel along `axis` (0 x / 2 z): the clockwise turn of the positive face.
[[nodiscard]] constexpr int tunnelLeft(int axis) { return clockWise(positiveFace(axis)); }
/// Create's getTunnelState: outputs on the left / right make T / cross junctions; a straight tunnel
/// between two tunnels (or extended belt funnels) gets a window.
[[nodiscard]] TunnelShape tunnelShape(bool outputLeft, bool outputRight, bool canHaveWindow);
/// Create's updateTunnelConnections: the horizontal faces a tunnel is open towards (`sides`).
[[nodiscard]] std::vector<int> tunnelSides(int axis, TunnelShape shape);

// ---- chutes -------------------------------------------------------------------------------------

/// Item speed in chutes per tick (Create's ChuteBlockEntity.getItemMotion): positive = upwards.
/// `push` / `pull` are the fan speeds acting on the chute (RPM).
[[nodiscard]] float chuteItemMotion(float push, float pull);
/// How far below an upward chute the air pulls items in (Create's updateAirFlow), before the
/// flow limit of the blocks below: 3 / 2 / 1 blocks at 128 / 64 / 32 RPM, less below.
[[nodiscard]] float chutePullDistance(float speed);

/// Create's ChuteBlock.Shape (block state `levitate:shape`).
enum class ChuteShape { Normal = 0, Window = 1, Intersection = 2, Encased = 3 };

/// Create's ChuteBlock.updateChuteState. `facing` is the chute's facing (down, or the horizontal
/// face of a diagonal chute); `diagonalInputs[f]` = a chute above-`f` faces `f` (feeds into this
/// one), indexed by FacingID; `targetBelowIsChute` = for a diagonal chute, the block below and
/// behind is a chute; `aboveFacesDown` = the block above is a chute facing down or a funnel facing
/// down. Returns the new (facing, shape).
[[nodiscard]] std::pair<int, ChuteShape> updateChuteState(
    int                        facing,
    ChuteShape                 shape,
    std::array<bool, 6> const& diagonalInputs,
    bool                       targetBelowIsChute,
    bool                       aboveFacesDown
);

// ---- item vaults ----------------------------------------------------------------------------------

inline constexpr int kVaultMaxWidth = 3;
/// A vault can grow to 3x its width along its axis.
[[nodiscard]] constexpr int vaultMaxLength(int width) { return width * 3; }

struct VaultCell {
    int x = 0, y = 0, z = 0;
    friend bool operator==(VaultCell const&, VaultCell const&) = default;
    friend auto operator<=>(VaultCell const&, VaultCell const&) = default;
};

/// One multi-block vault: a width x width x length cuboid along `axis` (0 x / 2 z), controller at
/// its lowest corner (`origin`).
struct VaultStructure {
    VaultCell origin;
    int       axis   = 0;
    int       width  = 1;
    int       length = 1;
    friend bool operator==(VaultStructure const&, VaultStructure const&) = default;

    [[nodiscard]] int  size() const { return width * width * length; }
    /// Block `i` (0..size-1) in Create's inventory order: along the axis, then the two cross axes.
    [[nodiscard]] VaultCell cell(int index) const;
    /// Index of a block, -1 if not part of this structure.
    [[nodiscard]] int indexOf(VaultCell const& c) const;
};

/// Groups the vault blocks of one connected component into structures like Create's
/// ConnectivityHandler: repeatedly the controller (lowest corner) that forms the most blocks
/// (widths 1..3, the larger width on ties) takes its cuboid. `axisAt` returns a vault block's axis
/// (0 x / 2 z) or nullopt.
[[nodiscard]] std::vector<VaultStructure>
formVaults(std::vector<VaultCell> const& component, std::function<std::optional<int>(VaultCell const&)> const& axisAt);

/// Connected-texture position of a vault block (block states `levitate:len`, `levitate:u`,
/// `levitate:v`, `levitate:large`): along the axis 0 single / 1 first / 2 middle / 3 last; across
/// (u = the horizontal cross axis, v = y) 0 single / 1 low / 2 middle / 3 high.
struct VaultLook {
    int  len = 0, u = 0, v = 0;
    bool large = false;
    friend bool operator==(VaultLook const&, VaultLook const&) = default;
};
[[nodiscard]] VaultLook vaultLook(VaultStructure const& s, VaultCell const& c);

/// Create's ItemHelper.calcRedstoneFromInventory over (count, maxStack) of every slot.
[[nodiscard]] int comparatorSignal(std::vector<std::pair<int, int>> const& slots, int totalSlots);

} // namespace lv::core::funnels
