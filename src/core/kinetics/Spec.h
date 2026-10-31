#pragma once

// What a kinetic block is: its shaft / cog connections, stress, generation and the hooks that
// reproduce Create's special propagation rules (gearboxes, split shafts, chain drives, speed
// controllers, belts). One `KineticSpec` per block id, registered by the unit that owns the block.
//
// Every callback receives the block's state snapshot (`BlockState`, the states listed in
// `KineticSpec::states`). Callbacks must be pure (no world access): the solver runs them on the
// server thread, the client renderer runs `rotationAxis` / `parts` / `angleOffset` on the
// client thread.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/kinetics/Types.h"

namespace lv::core::kinetics {

enum class CogType : std::uint8_t { None, Small, Large };

/// A kinetic block as the propagation hooks see it.
struct NodeView {
    GlobalPos                pos;
    BlockState const*        state = nullptr;
    float                    speed = 0; ///< theoretical speed (ignores overstress), RPM, signed
    std::optional<GlobalPos> source;    ///< the neighbour this block receives its speed from
    bool                     generator = false;

    [[nodiscard]] bool hasSource() const { return source.has_value(); }
    /// Direction from this block towards its source (Create's `getSourceFacing`).
    [[nodiscard]] std::optional<Face> sourceFacing() const {
        if (!source) return std::nullopt;
        auto d = diff(pos, *source);
        return nearestFace(d.x, d.y, d.z);
    }
};

/// One rotating part drawn by the client renderer: a bone of the spec's `rotorModel`, modelled
/// around the Y axis through the block centre; the renderer turns it onto `axis` and spins it.
struct RotorPart {
    std::string bone        = "rotor";
    Axis  axis        = Axis::Y;
    float speedFactor = 1;    ///< multiplies the block's speed (e.g. -1 for a reversed half shaft)
    float angleOffset = 0;    ///< degrees, added to Create's per-position offset
    bool  largeCog    = false; ///< use the large cog's position offset (11.25 degrees)
    /// Drawn at a visual speed that chases the network speed by 1/64 a tick (Create's
    /// FlywheelBlockEntity.visualSpeed) instead of turning at it at once.
    bool  eased       = false;
};

/// What the renderer knows about a block: its state, synced speed and source direction.
struct RenderView {
    BlockState const*   state = nullptr;
    float               speed = 0;
    std::optional<Face> sourceFacing;
};

struct KineticSpec {
    // ---- state snapshot ------------------------------------------------------------------------
    /// Block states the hooks read (e.g. "levitate:axis", "levitate:powered"). Only these are
    /// snapshot; a change of any of them is a kinetic state change (detach + re-attach, as
    /// Create's `switchToBlockState` for states that are not kinetically equivalent).
    std::vector<std::string> states;

    // ---- shape (Create's IRotate) ----------------------------------------------------------------
    /// Rotation axis of the block (nullopt: no single axis, e.g. a gearbox's casing).
    std::function<std::optional<Axis>(BlockState const&)> rotationAxis;
    /// The block has a shaft end on `face` (Create's `hasShaftTowards`).
    std::function<bool(BlockState const&, Face)> hasShaftTowards;
    /// Small / large cog teeth around `rotationAxis` (Create's ICogWheel).
    std::function<CogType(BlockState const&)> cog;
    /// The block reaches its 12 diagonal neighbours in the plane of its axis (default: cogs).
    std::function<bool(BlockState const&)> propagatesDiagonally;

    // ---- stress (config `kinetics.json` overrides these defaults by `stressKey`) ----------------
    /// Key into the stress table (default: the id's path, "levitate:mechanical_press" -> "mechanical_press").
    std::string stressKey;
    double      impact   = 0; ///< SU per RPM used when the config has no entry
    double      capacity = 0; ///< SU per RPM provided at the generated speed (generators)

    // ---- generation --------------------------------------------------------------------------------
    /// The block is a rotation source (Create's GeneratingKineticBlockEntity).
    bool generator = false;
    /// Generated speed right after placement (units may instead call `setGeneratedSpeed`).
    std::function<float(BlockState const&)> initialGeneratedSpeed;

    // ---- propagation hooks (all optional) --------------------------------------------------------
    /// Speed factor of the shaft end on `face` for blocks whose halves turn differently
    /// (Create's DirectionalShaftHalvesBlockEntity: gearbox, gearshift, clutch). 0 disconnects.
    std::function<float(NodeView const& self, Face face)> axisModifier;
    /// Create's `propagateRotationTo`: a custom speed ratio from `from` (this block) to `to`; 0 = use
    /// the standard rules. Gets the offset and whether shafts / small cogs connect them.
    std::function<float(NodeView const& from, NodeView const& to, Offset diff, bool byAxis, bool byGears)> propagate;
    /// Create's `isCustomConnection`: connects this block to `other` even without a ratio.
    /// Asked on both blocks.
    std::function<bool(NodeView const& self, NodeView const& other)> customConnection;
    /// Overrides the speed conveyed between this block and `other` (speed controller). Asked
    /// first on the sender (`selfIsFrom` true), then on the receiver.
    std::function<std::optional<float>(NodeView const& self, NodeView const& other, bool selfIsFrom)> conveyedSpeed;
    /// Extra positions this block may connect to (Create's `addPropagationLocations`, e.g. belts).
    std::function<void(NodeView const& self, std::vector<GlobalPos>& out)> extraNeighbours;

    // ---- client rendering --------------------------------------------------------------------------
    /// Integer block state that selects the chunk model: 0 = the full static model (what the world
    /// holds and vanilla clients see), 1 = the parts that do not rotate (what the client mod puts
    /// in the chunk mesh; may be empty). Empty: the client mod draws nothing special.
    std::string rotorState;
    /// Geometry identifier ("geometry.levitate.shaft", RP `models/blocks/*.geo.json`) whose bones
    /// are the rotating parts, and the terrain texture name they use.
    std::string rotorModel;
    std::string rotorTexture;
    /// Rotating parts (default: bone "rotor" around `rotationAxis`, large-cog offset for large cogs).
    std::function<void(RenderView const&, std::vector<RotorPart>&)> parts;
    /// Create's `getRotationAngleOffset(axis)` (degrees).
    std::function<float(BlockState const&, Axis)> angleOffset;

    // ---- helpers -----------------------------------------------------------------------------------
    [[nodiscard]] std::optional<Axis> axisOf(BlockState const& s) const {
        return rotationAxis ? rotationAxis(s) : std::nullopt;
    }
    [[nodiscard]] bool shaftTowards(BlockState const& s, Face f) const {
        return hasShaftTowards && hasShaftTowards(s, f);
    }
    [[nodiscard]] CogType cogOf(BlockState const& s) const { return cog ? cog(s) : CogType::None; }
    [[nodiscard]] bool    diagonal(BlockState const& s) const {
        return propagatesDiagonally ? propagatesDiagonally(s) : cogOf(s) != CogType::None;
    }
    /// The parts to draw for `view` (applies the default when `parts` is unset).
    void rotorParts(RenderView const& view, std::vector<RotorPart>& out) const;
};

/// The stress key of `id` when the spec leaves `stressKey` empty: the id's path.
[[nodiscard]] std::string defaultStressKey(std::string_view id);

// ---- builders for common shapes ------------------------------------------------------------------
namespace specs {

/// Reads an axis from an integer state (0 = x, 1 = y, 2 = z).
[[nodiscard]] std::function<std::optional<Axis>(BlockState const&)> axisFromState(std::string state);
/// Reads an axis from a `minecraft:block_face`-style facing state (face index -> its axis).
[[nodiscard]] std::function<std::optional<Axis>(BlockState const&)> axisFromFacing(std::string state);

/// Sets the client model of the rotating parts (`rotorModel`, `rotorTexture`).
[[nodiscard]] KineticSpec withModel(KineticSpec spec, std::string model, std::string texture);
/// A shaft along `axisState` (shaft ends on both faces of the axis). Rotor state "levitate:rotor".
[[nodiscard]] KineticSpec shaft(std::string axisState = "levitate:axis");
/// A cogwheel (small or large) along `axisState`, with shaft ends on both faces of its axis.
[[nodiscard]] KineticSpec cogwheel(bool large, std::string axisState = "levitate:axis");
/// A generator with a shaft along `axisState`, providing `capacity` SU per RPM.
[[nodiscard]] KineticSpec generatorShaft(double capacity, std::string axisState = "levitate:axis");

/// Create's gearbox halves: the face opposite the source turns the other way; a perpendicular face
/// turns the same way when its direction sign (Up/South/East positive) differs from the source
/// face's, the other way otherwise.
[[nodiscard]] float gearboxModifier(NodeView const& self, Face face);
/// Create's split shafts (gearshift reverses, clutch disconnects the far half when `active`).
[[nodiscard]] float splitShaftModifier(NodeView const& self, Face face, bool active, bool reverse);

} // namespace specs

} // namespace lv::core::kinetics
