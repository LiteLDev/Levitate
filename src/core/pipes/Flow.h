#pragma once

// Create's pipe network flow model (pure C++), reproduced from Create 6.0.10's behaviour:
//
// - Pumps (PumpBlockEntity) search the pipes on each side (up to `pumpRange` blocks), find the
//   paths that end at an endpoint (a fluid handler, an open end, an opposing pump, or the range
//   limit) and put pressure on every pipe face along them: |speed| per face, divided by
//   (faces at that distance - 1) at branches. The back side pulls (inbound pressure), the front
//   pushes (outbound pressure).
// - Every pipe face (PipeConnection) holds a pressure pair, a source (what is beyond the face) and
//   at most one flow. Flows start at faces with pressure (inbound from the source, outbound with the
//   fluid that entered the pipe elsewhere), advance by 1/32 + min(pressure / 128, 1) * 31/32 of the
//   face per tick and pass on once complete. Two different fluids entering one pipe collide.
// - An inbound flow from an endpoint owns a FluidNetwork: it searches the complete flows from its
//   face to the endpoints they reach (up to 16 steps per tick) and then moves
//   max(1, inbound pressure / 2) mB per tick from the source to those targets, split evenly.
// - Changing a pipe wipes the pressure of every pipe it reaches (FluidPropagator) and makes the pumps
//   found there distribute again.
//
// The world is behind `Host`: which blocks are pipes / pumps, their open faces, the fluid handlers
// and open ends. Everything runs on one thread (the server thread).

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/pipes/Types.h"

namespace lv::core::pipes {

/// What a block is to the flow model (Create: which FluidTransportBehaviour it has).
enum class Kind : std::uint8_t { None, Pipe, Pump };

/// The 1000 mB buffer of an open pipe end (Create's OpenEndedPipe).
struct OpenEnd {
    Fluid tank;
    bool  pulling = false;
};

/// What is beyond a pipe face (Create's FlowSource).
enum class SourceKind : std::uint8_t { OpenEnd, Handler, OtherPipe, Blocked };
struct Source {
    SourceKind kind = SourceKind::Blocked;
    OpenEnd    openEnd; ///< only for OpenEnd
    [[nodiscard]] bool endpoint() const { return kind == SourceKind::OpenEnd || kind == SourceKind::Handler; }
};

struct Flow {
    bool  inbound = true;
    Fluid fluid;
    float progress = 0;
    bool  complete = false;
};

class Network;

/// One face of a pipe (Create's PipeConnection).
struct Connection {
    Face                     side = Face::Down;
    float                    pressureIn = 0, pressureOut = 0;
    std::optional<Source>    source, previousSource;
    std::optional<Flow>      flow;
    std::shared_ptr<Network> network;

    [[nodiscard]] float pressure(bool inbound) const { return inbound ? pressureIn : pressureOut; }
    [[nodiscard]] float comparePressure() const { return pressureOut - pressureIn; }
    [[nodiscard]] bool  hasPressure() const { return pressureIn != 0 || pressureOut != 0; }
    [[nodiscard]] bool  hasFlow() const { return flow.has_value(); }
    /// The fluid entering the pipe here (complete inbound flow).
    [[nodiscard]] Fluid providedFluid() const;
    /// The fluid leaving the pipe here (complete outbound flow).
    [[nodiscard]] Fluid providedOutboundFluid() const;
};

enum class Phase : std::uint8_t { WaitForPumps, FlipFlows, Idle };

/// A pipe or pump block (Create's FluidTransportBehaviour).
struct Node {
    std::array<std::optional<Connection>, 6> connections;
    Phase                                    phase = Phase::WaitForPumps;

    [[nodiscard]] Connection*       at(Face f) { return connections[static_cast<int>(f)] ? &*connections[static_cast<int>(f)] : nullptr; }
    [[nodiscard]] Connection const* at(Face f) const {
        return connections[static_cast<int>(f)] ? &*connections[static_cast<int>(f)] : nullptr;
    }
    [[nodiscard]] bool active() const;
};

/// The world as the flow model sees it.
class Host {
public:
    virtual ~Host() = default;

    [[nodiscard]] virtual bool loaded(GlobalPos const& pos) = 0;
    [[nodiscard]] virtual Kind kind(GlobalPos const& pos)   = 0;
    /// Create's `canHaveFlowToward`: the pipe / pump at `pos` is open towards `face`.
    [[nodiscard]] virtual bool canHaveFlowToward(GlobalPos const& pos, Face face) = 0;
    /// Create's `canPullFluidFrom` (closed valves, smart pipe filters).
    [[nodiscard]] virtual bool canPullFluidFrom(GlobalPos const& pos, Face side, Fluid const& fluid) = 0;
    /// Front (arrow direction) and speed (RPM) of the pump at `pos`.
    [[nodiscard]] virtual Face  pumpFront(GlobalPos const& pos) = 0;
    [[nodiscard]] virtual float pumpSpeed(GlobalPos const& pos) = 0;
    [[nodiscard]] virtual int   pumpRange()                     = 0;

    /// Create's `FluidPropagator.isOpenEnd` for the face `side` of the pipe at `pipe`.
    [[nodiscard]] virtual bool isOpenEnd(GlobalPos const& pipe, Face side) = 0;
    /// The block at `target` has a fluid handler accessed from its side `face`.
    [[nodiscard]] virtual bool hasHandler(GlobalPos const& target, Face face) = 0;
    [[nodiscard]] virtual std::vector<Fluid> handlerTanks(GlobalPos const& target, Face face) = 0;
    virtual int   handlerFill(GlobalPos const& target, Face face, Fluid const& fluid, bool simulate)       = 0;
    /// Drains up to `request.amount` of `request.id` ("" = any fluid).
    virtual Fluid handlerDrain(GlobalPos const& target, Face face, Fluid const& request, bool simulate)     = 0;

    /// The open end in front of `at` (Create's OpenEndedPipe fluid handler) with its buffer.
    virtual int   openEndFill(BlockFace const& at, OpenEnd& state, Fluid const& fluid, bool simulate)      = 0;
    virtual Fluid openEndDrain(BlockFace const& at, OpenEnd& state, Fluid const& request, bool simulate)   = 0;

    /// Two different fluids entered the pipe at `pos` (Create's FluidReactions.handlePipeFlowCollision).
    virtual void onFlowCollision(GlobalPos const& pos, Fluid const& a, Fluid const& b) = 0;
    /// The flows of the pipe at `pos` changed (rendering).
    virtual void onFlowsChanged(GlobalPos const& /*pos*/) {}
};

class Model {
public:
    explicit Model(Host& host) : mHost(host) {}
    Model(Model const&)            = delete;
    Model& operator=(Model const&) = delete;

    [[nodiscard]] Host& host() { return mHost; }

    // ---- nodes -------------------------------------------------------------------------------------------------
    [[nodiscard]] Node*       node(GlobalPos const& pos);
    [[nodiscard]] Node const* node(GlobalPos const& pos) const;
    /// The node of a pipe / pump block (created with its connections; nullptr for other blocks).
    Node*                     ensure(GlobalPos const& pos);
    [[nodiscard]] Connection* connection(BlockFace const& face);
    /// The block at `pos` is gone: forget it.
    void remove(GlobalPos const& pos);
    void clear();
    [[nodiscard]] std::map<GlobalPos, Node> const& nodes() const { return mNodes; }

    // ---- changes (Create's FluidPropagator / FluidTransportBehaviour / PumpBlockEntity) -------------------------
    /// Create's `propagateChangedPipe`: wipes the pressure of the connected pipes (up to the pump
    /// range, further through pipes with pressure) and makes the pumps found redistribute.
    /// `startFaces` overrides the open faces of `start` (a block that was just removed).
    void propagateChangedPipe(GlobalPos const& start, std::optional<FaceMask> startFaces = std::nullopt);
    void wipePressure(GlobalPos const& pos);
    void addPressure(GlobalPos const& pos, Face side, bool inbound, float amount);
    void resetAffectedFluidNetworks(GlobalPos const& start, Face side);

    // ---- pumps ---------------------------------------------------------------------------------------------------
    void addPump(GlobalPos const& pos);
    [[nodiscard]] std::set<GlobalPos> const& pumps() const { return mPumps; }
    /// Create's `PumpBlockEntity.onSpeedChanged`.
    void pumpSpeedChanged(GlobalPos const& pos, float previous, float speed);
    /// Create's `updatePressureChange` (also after the pump was flipped).
    void pumpUpdatePressure(GlobalPos const& pos);

    // ---- simulation --------------------------------------------------------------------------------------------
    /// One server tick: pumps first, then every active pipe (sorted by position).
    void tick(std::int64_t gameTime);
    [[nodiscard]] std::int64_t gameTime() const { return mGameTime; }

    /// The fluid moving through a pipe (the first face with a flow), for rendering.
    [[nodiscard]] Fluid visibleFluid(GlobalPos const& pos) const;

    // ---- persistence ---------------------------------------------------------------------------------------------
    [[nodiscard]] nlohmann::json save(std::optional<int> dim = std::nullopt) const;
    void                         load(nlohmann::json const& j);

private:
    friend class Network;

    void createConnections(GlobalPos const& pos, Node& node);
    void tickNode(GlobalPos const& pos, Node& node);
    void tickPump(GlobalPos const& pos);
    void distributePressureTo(GlobalPos const& pump, Face side);
    bool hasReachedValidEndpoint(BlockFace const& face, bool pull);
    bool isPullingOnSide(GlobalPos const& pump, Face side);

    // PipeConnection
    bool  determineSource(GlobalPos const& pos, Connection& c);
    Fluid provideFluid(GlobalPos const& pos, Connection& c, std::function<bool(Fluid const&)> const& pred);
    bool  manageFlows(GlobalPos const& pos, Connection& c, Fluid const& internal, std::function<bool(Fluid const&)> const& pred);
    bool  flipFlowsIfPressureReversed(Connection& c);
    void  wipeConnection(Connection& c);
    void  tickFlowProgress(Connection& c);

    struct PumpFlags {
        bool front = false, back = false;
    };

    Host&                          mHost;
    std::map<GlobalPos, Node>      mNodes;
    std::set<GlobalPos>            mPumps;
    std::map<GlobalPos, PumpFlags> mPumpUpdates;
    std::int64_t                   mGameTime = 0;
};

/// Create's FluidNetwork: transfers from the endpoint behind `start` to the endpoints its flows reach.
class Network {
public:
    Network(Model& model, BlockFace start);
    void reset();
    void tick();

    [[nodiscard]] Fluid const&                  fluid() const { return mFluid; }
    [[nodiscard]] std::vector<BlockFace> const& targets() const { return mTargets; }
    [[nodiscard]] int                           transferSpeed() const { return mTransferSpeed; }

private:
    struct HandlerRef {
        BlockFace  face; ///< the pipe face whose source is the endpoint
        bool       openEnd = false;
        [[nodiscard]] std::string key() const;
    };
    std::optional<HandlerRef> handlerOf(BlockFace const& face);
    std::vector<Fluid>        tanks(HandlerRef const& h);
    Fluid                     drain(HandlerRef const& h, Fluid const& request, bool simulate);
    int                       fill(HandlerRef const& h, Fluid const& fluid, bool simulate);

    Model&                 mModel;
    BlockFace              mStart;
    Fluid                  mFluid;
    std::vector<BlockFace> mFrontier;
    std::set<GlobalPos>    mVisited;
    std::vector<BlockFace> mTargets;
    std::vector<BlockFace> mQueued;
    int                    mPause         = 2;
    int                    mTransferSpeed = 1;
};

/// Speed a flow advances per tick at `pressure` (Create: 1/32 + clamp(p / 128) * 31/32).
[[nodiscard]] float flowSpeed(float pressure);
/// mB per tick a network moves for its start pressure (Create: max(1, (int)(p / 2))).
[[nodiscard]] int transferSpeedFor(float pressure);

} // namespace lv::core::pipes
