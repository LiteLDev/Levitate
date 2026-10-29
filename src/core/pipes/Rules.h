#pragma once

// Block rules of Create's fluid pipes, pumps, valves and smart pipes (pure C++): how a pipe picks its
// connections, which axis a wrench turns into a glass pipe, how pumps / valves / smart pipes orient
// themselves when placed. The game adapter feeds in what it sees around the block.

#include <array>
#include <optional>

#include "core/pipes/Types.h"

namespace lv::core::pipes {

// ---- fluid pipe (Create's FluidPipeBlock) --------------------------------------------------------

/// Create's `FluidPipeBlock.updateBlockState`: every face but `ignored` connects where
/// `canConnect[face]`; one open face extends straight through; no open face keeps a previous
/// straight pipe or opens along `preferred`.
[[nodiscard]] FaceMask updatePipeConnections(
    FaceMask                   previous,
    std::array<bool, 6> const& canConnect,
    std::optional<Face>        preferred,
    std::optional<Face>        ignored
);

/// The axis of a straight pipe (exactly the two faces of one axis open), else nullopt.
[[nodiscard]] std::optional<Axis> straightAxis(FaceMask open);
/// Create's `shouldDrawCasing`: three or more open faces around one axis (T and cross junctions).
[[nodiscard]] bool drawsCasing(FaceMask open);
/// Corners / ends (Create's `isCornerOrEndPipe`).
[[nodiscard]] bool cornerOrEnd(FaceMask open);

/// Create's `FluidPipeBlock.onWrenched`: the axis of the glass pipe a wrench makes, or nullopt when
/// the click passes (the clicked face lies on that axis). `hit` is the click relative to the block
/// centre (-0.5 .. 0.5).
[[nodiscard]] std::optional<Axis>
wrenchGlassAxis(FaceMask open, Face clicked, double hitX, double hitY, double hitZ);

/// Faces of a regular pipe made from a glass / encased pipe along `axis` (Create's `toRegularPipe`):
/// both axis ends open, then `updatePipeConnections` with the positive end preferred.
[[nodiscard]] FaceMask regularPipeFromAxis(Axis axis, std::array<bool, 6> const& canConnect);

// ---- placement helpers ---------------------------------------------------------------------------

/// Unit vector components of a face.
[[nodiscard]] std::array<int, 3> normal(Face f);
/// The face nearest to a look vector (Create's `getNearestLookingDirection`).
[[nodiscard]] Face nearestFace(double x, double y, double z);
/// The horizontal face nearest to a look vector (Create's `getHorizontalDirection`).
[[nodiscard]] Face horizontalFace(double x, double z);
/// Rotates a horizontal face clockwise seen from above (north -> east -> south -> west).
[[nodiscard]] Face clockwise(Face f);

// ---- mechanical pump (Create's PumpBlock) -----------------------------------------------------------

/// Create's `PumpBlock.getStateForPlacement` on top of `DirectionalKineticBlock`: `kineticPreferred`
/// is the side with a shaft facing the pump (nullopt when none or ambiguous); `pipeAt[face]` tells
/// whether a pipe there would connect to the pump.
[[nodiscard]] Face pumpPlacement(
    Face                       nearestLook,
    bool                       sneaking,
    std::optional<Face>        kineticPreferred,
    std::array<bool, 6> const& pipeAt
);

// ---- fluid valve (Create's FluidValveBlock / DirectionalAxisKineticBlock) ---------------------------

struct ValveAxes {
    Axis pipe;
    Axis shaft;
};
/// Pipe and shaft axes of a valve state (the facing axis is neither).
[[nodiscard]] ValveAxes valveAxes(Face facing, bool alongFirst);

struct ValvePlacement {
    Face facing;
    bool alongFirst;
};
/// Create's `DirectionalAxisKineticBlock.getStateForPlacement` with the valve's pipe preference:
/// `shaftAt[face]` = a shaft end faces the valve there, `pipeAt[face]` = a pipe there would connect.
[[nodiscard]] ValvePlacement valvePlacement(
    Face                       nearestLook,
    Face                       horizontalLook,
    bool                       sneaking,
    std::array<bool, 6> const& shaftAt,
    std::array<bool, 6> const& pipeAt
);
/// The wrench on a valve: on the facing axis it cycles `alongFirst`, otherwise it turns the facing
/// a quarter around the clicked axis.
[[nodiscard]] ValvePlacement valveWrenched(ValvePlacement state, Face clicked);
/// A face rotated a quarter turn clockwise around `axis` (seen from the positive end).
[[nodiscard]] Face rotateAround(Face f, Axis axis);

// ---- smart fluid pipe (Create's SmartFluidPipeBlock) ------------------------------------------------

enum class AttachFace : std::uint8_t { Floor = 0, Ceiling = 1, Wall = 2 };
struct SmartPipeState {
    AttachFace face   = AttachFace::Floor;
    Face       facing = Face::North; ///< horizontal
};
/// Pipe axis of a smart pipe: vertical on a wall, else the facing's axis.
[[nodiscard]] Axis smartPipeAxis(SmartPipeState s);
/// Create's placement: the face-attached default from the look, then aligned to neighbours that
/// would connect (`pipeAt`).
[[nodiscard]] SmartPipeState
smartPipePlacement(Face nearestLook, Face horizontalLook, double lookX, double lookZ, std::array<bool, 6> const& pipeAt);

} // namespace lv::core::pipes
