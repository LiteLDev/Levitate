#pragma once

// The kinetic network solver (pure C++): Create's RotationPropagator + KineticNetwork semantics
// over an abstract set of kinetic blocks.
//
// The game adapter tells the solver about blocks (`add` / `remove` / `update`) and generator
// speeds; the solver propagates speeds, tracks networks, sums stress, detects overstress and speed
// conflicts. Effects are collected, not called back, so the adapter applies them after each call:
//   - `takeDestroyed()`: blocks to break (and drop) because of a speed conflict, a speed above the
//     limit, a gear-ratio loop or flickering; the solver has already removed them;
//   - `takeChanges()`: blocks whose speed / source / overstress changed (for listeners and sync);
//   - `takeNetworkEvents()`: networks that became overstressed or recovered.
//
// Behaviour reference: Create 6.0.10 (RotationPropagator.propagateNewSource,
// propagateMissingSource, handleAdded/handleRemoved, GeneratingKineticBlockEntity
// .updateGeneratedRotation/applyNewSpeed, KineticNetwork, KineticBlockEntity.validateKinetics,
// flicker score). See docs/SPEC.md §3.1.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json_fwd.hpp"

#include "core/kinetics/Spec.h"

namespace lv::core::kinetics {

/// A network is named after the generator that created it (Create: the source's position).
using NetworkId = GlobalPos;

struct SolverConfig {
    float maxRotationSpeed       = 256; ///< CKinetics.maxRotationSpeed
    bool  disableStress          = false; ///< CKinetics.disableStress
    int   validationFrequency    = 60;  ///< CKinetics.kineticValidationFrequency (ticks)
    /// SU per RPM of a block (default: `spec.impact`). The adapter reads the stress table here.
    std::function<double(KineticSpec const&, BlockState const&)> impact;
    /// SU per RPM a generator provides (default: `spec.capacity`).
    std::function<double(KineticSpec const&, BlockState const&)> capacity;
};

/// Create's `MAX_FLICKER_SCORE`.
inline constexpr int kMaxFlickerScore = 128;

struct NodeInfo {
    GlobalPos                pos;
    BlockState               state;
    float                    speed            = 0; ///< effective: 0 while overstressed
    float                    theoreticalSpeed = 0;
    float                    generatedSpeed   = 0;
    std::optional<GlobalPos> source;
    std::optional<NetworkId> network;
    bool                     generator    = false;
    bool                     overstressed = false;
    double                   impact       = 0; ///< SU per RPM in effect
    double                   capacity     = 0; ///< SU per RPM in effect (generators)
    int                      flicker      = 0;
};

struct NetworkInfo {
    NetworkId id;
    double    stress       = 0; ///< SU used (impact * |speed| summed)
    double    capacity     = 0; ///< SU provided (capacity * |generated speed| summed)
    bool      overstressed = false;
    int       members      = 0;
    int       sources      = 0;
};

struct Change {
    GlobalPos                pos;
    std::string              id; ///< block id
    float                    prevSpeed = 0, speed = 0; ///< effective speeds
    float                    prevTheoretical = 0, theoretical = 0;
    std::optional<GlobalPos> prevSource, source;
    bool                     added   = false;
    bool                     removed = false;
};

enum class DestroyReason : std::uint8_t {
    Conflict,   ///< two sources force opposite directions
    TooFast,    ///< a ratio pushes the speed above `maxRotationSpeed`
    RatioLoop,  ///< a loop of ratios would speed itself up
    Flicker,    ///< the speed changed too often
    Generator,  ///< a generator is driven against its own direction
};
[[nodiscard]] char const* reasonName(DestroyReason r);

struct Destroyed {
    GlobalPos     pos;
    BlockState    state;
    DestroyReason reason;
};

struct NetworkEvent {
    NetworkId id;
    bool      overstressed = false;
    double    stress = 0, capacity = 0;
};

/// Everything the world DB keeps for a block (a reload resumes the network as it was).
struct NodeRecord {
    GlobalPos                pos;
    BlockState               state;
    float                    speed     = 0;
    float                    generated = 0;
    std::optional<GlobalPos> source;
    std::optional<NetworkId> network;
    std::optional<double>    impactOverride, capacityOverride;
};
void to_json(nlohmann::json& j, NodeRecord const& r);
void from_json(nlohmann::json const& j, NodeRecord& r);

class Solver {
public:
    using SpecLookup = std::function<KineticSpec const*(std::string_view id)>;

    explicit Solver(SpecLookup specs, SolverConfig config = {});
    ~Solver();
    Solver(Solver const&)            = delete;
    Solver& operator=(Solver const&) = delete;

    void                              setConfig(SolverConfig config);
    [[nodiscard]] SolverConfig const& config() const { return mConfig; }

    // ---- world changes ----------------------------------------------------------------------------
    /// A kinetic block appeared (placed, pushed into place, ...). Replaces an existing node.
    /// Returns false when `state.id` has no spec.
    bool add(GlobalPos const& pos, BlockState state);
    /// The block is gone (broken, replaced by a non-kinetic block).
    void remove(GlobalPos const& pos);
    /// The block's kinetic states changed (axis, powered, ...): detach and re-attach as Create.
    void update(GlobalPos const& pos, BlockState state);
    /// A generator's speed (RPM, signed; not clamped: a speed above `maxRotationSpeed` breaks the generator,
    /// as in Create, so generators keep their own range). No-op for other blocks.
    void setGeneratedSpeed(GlobalPos const& pos, float rpm);
    /// Per-block stress overrides (nullopt: back to the spec / config value). Mark the network dirty.
    void setImpactOverride(GlobalPos const& pos, std::optional<double> impact);
    void setCapacityOverride(GlobalPos const& pos, std::optional<double> capacity);
    /// Re-sums every network (after a config reload).
    void refreshAllNetworks();
    /// One game tick: flicker decay, generator re-activation, validation.
    void tick();
    /// Drops everything (world closed).
    void clear();

    // ---- persistence ---------------------------------------------------------------------------------
    /// Restores a saved block silently (no propagation), as Create's BEs re-join their network on
    /// load. Call `finishRestore()` after the batch.
    void restore(NodeRecord const& record);
    void finishRestore();
    [[nodiscard]] NodeRecord record(GlobalPos const& pos) const;

    // ---- queries ----------------------------------------------------------------------------------------
    [[nodiscard]] bool                       contains(GlobalPos const& pos) const;
    [[nodiscard]] std::optional<NodeInfo>    node(GlobalPos const& pos) const;
    [[nodiscard]] std::optional<NetworkInfo> network(NetworkId const& id) const;
    [[nodiscard]] std::optional<NetworkInfo> networkAt(GlobalPos const& pos) const;
    /// Members of a network (sorted).
    [[nodiscard]] std::vector<GlobalPos> members(NetworkId const& id) const;
    [[nodiscard]] std::size_t            size() const;
    void forEachNode(std::function<void(NodeInfo const&)> const& fn) const;
    /// Blocks connected to `pos` (Create's `getConnectedNeighbours`).
    [[nodiscard]] std::vector<GlobalPos> connectedNeighbours(GlobalPos const& pos) const;
    /// Speed `from` conveys to its neighbour `to` (0 if not connected).
    [[nodiscard]] float conveyedSpeed(GlobalPos const& from, GlobalPos const& to) const;

    // ---- effects ------------------------------------------------------------------------------------------
    [[nodiscard]] std::vector<Destroyed>    takeDestroyed();
    [[nodiscard]] std::vector<Change>       takeChanges();
    [[nodiscard]] std::vector<NetworkEvent> takeNetworkEvents();

private:
    struct Node;
    struct Network;
    struct Snapshot;

    Node*       find(GlobalPos const& pos);
    Node const* find(GlobalPos const& pos) const;
    bool        alive(Node const* n) const;
    NodeView    view(Node const& n) const;
    NodeInfo    info(Node const& n) const;

    double impactOf(Node const& n) const;
    double capacityOf(Node const& n) const;
    float  effective(Node const& n) const;

    // Create's RotationPropagator
    std::vector<GlobalPos> potentialNeighbourLocations(Node const& n) const;
    std::vector<GlobalPos> connected(Node const& n) const;
    bool                   isConnected(Node const& from, Node const& to) const;
    float                  rotationSpeedModifier(Node const& from, Node const& to) const;
    float                  axisModifier(Node const& n, Face face) const;
    float                  conveyed(Node const& from, Node const& to) const;
    void                   propagateNewSource(GlobalPos const& start);
    void                   propagateMissingSource(GlobalPos const& start);
    void                   handleRemoved(GlobalPos const& pos, std::vector<GlobalPos> const& locations, float speed);

    // Create's KineticBlockEntity / GeneratingKineticBlockEntity
    void touch(Node const& n);
    void setNetwork(Node& n, std::optional<NetworkId> id);
    void setSource(Node& n, Node const& source);
    void removeSource(Node& n);
    void onSpeedChanged(Node& n, float prevSpeed);
    void detach(Node& n);
    void updateGeneratedRotation(Node& n);
    void validate(Node& n);
    void destroy(Node& n, DestroyReason reason);
    void markDirty(std::optional<NetworkId> const& id);

    void removeNow(GlobalPos const& pos);
    /// Runs pending removals and re-sums dirty networks (end of every public operation).
    void settle();
    void recomputeNetwork(NetworkId const& id);

    SpecLookup   mSpecs;
    SolverConfig mConfig;

    std::unordered_map<GlobalPos, std::unique_ptr<Node>, GlobalPosHash> mNodes;
    std::map<NetworkId, Network>                                        mNetworks;
    std::unordered_set<GlobalPos, GlobalPosHash>                        mDirtyNetworks;
    std::unordered_map<GlobalPos, Snapshot, GlobalPosHash>              mTouched;
    std::vector<GlobalPos>                                              mPendingRemoval;
    std::vector<Destroyed>                                              mDestroyed;
    std::vector<NetworkEvent>                                           mNetworkEvents;
    std::unordered_set<GlobalPos, GlobalPosHash>                        mFlickering;
    std::unordered_set<GlobalPos, GlobalPosHash>                        mReactivate;
    std::vector<GlobalPos>                                              mValidationOrder;
    std::size_t                                                         mValidationCursor = 0;
    int                                                                 mDepth            = 0;
};

// ---- speed / stress levels (Create's SpeedLevel / StressImpact) -----------------------------------
enum class SpeedLevel : std::uint8_t { None, Slow, Medium, Fast };
/// CKinetics.mediumSpeed (30) / fastSpeed (100).
[[nodiscard]] SpeedLevel speedLevel(float speed, float mediumSpeed = 30, float fastSpeed = 100);

enum class StressLevel : std::uint8_t { Low, Medium, High, Overstressed };
/// Network stress level by usage (stress / capacity): > 1 overstressed, > .75 high, > .5 medium.
[[nodiscard]] StressLevel stressLevel(double stress, double capacity);

// ---- rendering maths (Create's KineticBlockEntityRenderer) ------------------------------------------
/// Initial angle (degrees) of a rotating part at `pos` so that meshing cogs line up: 22.5 for an
/// even position sum (ignoring the axis component), otherwise 0 (11.25 for large cogs), plus `extra`.
[[nodiscard]] float rotationOffset(GlobalPos const& pos, Axis axis, bool largeCog, float extra = 0);
/// Angle in degrees at render time `ticks` (game ticks + partial tick) for `speed` RPM.
[[nodiscard]] float rotationAngle(double ticks, float speed, float offset);

} // namespace lv::core::kinetics
