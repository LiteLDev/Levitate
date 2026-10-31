#pragma once

// Train signals, signal sections and track observers (U28), pure C++.
//
// Create's SignalBoundary / SignalEdgeGroup / SignalPropagator / Navigation signal logic over the
// track graph of `core/tracks/Graph.h`:
//
// - A *boundary* is a signal edge point. It splits the graph into *sections* (Create's signal edge
//   groups): every stretch of track reachable from one side of a boundary without passing another
//   boundary is one section. Track networks without boundaries have no section (Create's passive
//   group): trains there ignore signals.
// - Each boundary has two sides. Side 1 is the half of its edge towards the edge point's `node2`
//   (trains travelling node1 -> node2 enter it), side 0 the half towards `node1`. Signal blocks
//   attach to one side: they control the trains that enter that side's section.
// - Sections whose tracks physically cross without a graph node (curves over other tracks) are
//   linked: a train in one of them occupies all of them (Create's intersecting groups).
// - Trains report the track they cover (`setTrainSpans`); a section is occupied while any part of a
//   train is on it. Trains ask `checkAhead` before entering sections: entry signals let a train in
//   when the section is free; cross signals only when every section up to the next entry signal on
//   the train's path is free too (chain signalling). A train that is let in reserves the sections,
//   so no other train gets them until it has entered (or gave up).
// - `updateStates` computes the state of every signalled side (Create's tickState): red when the
//   section is occupied by a train or reserved through another signal (or the side is forced red
//   by redstone), invalid when both sides belong to the same section (a loop); a cross signal
//   whose section is free resolves its chain (Create's resolveSignalChain): green when every
//   signal leading out of it on the way forward is green (or invalid), red when all of them are
//   red, yellow otherwise.
//
// Server thread only. Unit-tested in tests/core/signals/.

#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/tracks/Graph.h"

namespace lv::core::signals {

namespace ct = core::tracks;

/// Create's SignalBlock.SignalType (block state `levitate:signal_type`).
enum class SignalType : int { Entry = 0, Cross = 1 };
/// Create's SignalBlockEntity.SignalState (block state `levitate:signal_state`).
enum class SignalState : int { Red = 0, Yellow = 1, Green = 2, Invalid = 3 };

/// One side of a boundary.
struct BoundarySide {
    bool       hasSignal = false; ///< at least one signal block controls trains entering this side
    bool       forcedRed = false; ///< one of them is powered by redstone
    SignalType type      = SignalType::Entry;
};

/// A signal boundary on the edge between `node1` and `node2`, `position` blocks from `node1`.
struct Boundary {
    std::string                 id;
    ct::NodeLocation            node1, node2;
    double                      position = 0;
    std::array<BoundarySide, 2> sides;
};

/// A track observer on an edge.
struct Observer {
    std::string      id;
    ct::NodeLocation node1, node2;
    double           position = 0;
};

/// A stretch of track covered by a train: on the edge between `node1` and `node2`, from `from` to
/// `to` blocks measured from `node1` (`from <= to`).
struct Span {
    ct::NodeLocation node1, node2;
    double           from = 0, to = 0;
};

using GroupId                   = int;
inline constexpr GroupId kNoGroup = -1;

/// A boundary on a train's way: entering `side` of `boundary`, `distance` blocks ahead.
struct Crossing {
    std::string boundary;
    int         side     = 1;
    double      distance = 0;
};

/// The answer to a train asking for its way ahead (Create's Navigation signal scan).
struct Authority {
    bool        clear        = true; ///< nothing within the look-ahead stops the train
    double      stopDistance = std::numeric_limits<double>::infinity(); ///< to the signal it must wait at
    std::string waitingFor;          ///< that signal's boundary id ("" when clear)
    int         waitingSide = 1;
    std::vector<GroupId> reserved;   ///< sections reserved for the train by this check
};

/// A section reserved for a train, through the boundary side it will enter by.
struct Reservation {
    std::string train;
    std::string boundary;
    int         side = 1;
};

class SignalSystem {
public:
    // ---- configuration ----------------------------------------------------------------------
    /// Recomputes the sections from the graph, the boundaries and the observers (after any
    /// change to one of them). Train spans and reservations are kept and re-mapped.
    void rebuild(ct::Railways const& rail, std::vector<Boundary> boundaries, std::vector<Observer> observers = {});
    /// Updates the signal blocks of one boundary (redstone, type, blocks added/removed) without
    /// a full rebuild. False if the boundary is unknown.
    bool setSides(std::string const& boundaryId, std::array<BoundarySide, 2> const& sides);

    [[nodiscard]] Boundary const*                 boundary(std::string const& id) const;
    [[nodiscard]] std::vector<Boundary> const&    boundaries() const { return mBoundaries; }
    [[nodiscard]] std::vector<Observer> const&    observers() const { return mObservers; }
    [[nodiscard]] size_t                          groupCount() const { return mGroupCount; }

    // ---- sections ---------------------------------------------------------------------------
    /// The section entered through `side` of a boundary (kNoGroup if unknown / not on the graph).
    [[nodiscard]] GroupId groupOf(std::string const& boundaryId, int side) const;
    /// The section at a graph location.
    [[nodiscard]] GroupId groupAt(ct::Railways const& rail, ct::GraphLocation const& loc) const;
    /// Sections touched by a span.
    [[nodiscard]] std::vector<GroupId> groupsOn(Span const& span) const;
    /// The section and every section crossing it (transitively): what a train in it blocks.
    [[nodiscard]] std::vector<GroupId> const& linked(GroupId g) const;
    /// Signalled boundary sides that lead out of the section entered through (boundaryId, side),
    /// as reached by trains going forward from there (Create's chained signals).
    [[nodiscard]] std::vector<std::pair<std::string, int>> exits(std::string const& boundaryId, int side) const;
    /// Boundaries met going forward from `start` along `path` (edges after the start edge), in
    /// order, up to `maxDistance` blocks.
    [[nodiscard]] std::vector<Crossing> boundariesAlong(
        ct::Railways const&              rail,
        ct::GraphLocation const&         start,
        std::vector<ct::PathStep> const& path,
        double                           maxDistance = std::numeric_limits<double>::infinity()
    ) const;

    // ---- trains -----------------------------------------------------------------------------
    /// The track a train covers now (replaces what it reported before). Entering a section
    /// releases the train's reservation of it.
    void setTrainSpans(std::string const& train, std::vector<Span> spans);
    void removeTrain(std::string const& train);
    [[nodiscard]] std::vector<std::string> trains() const;
    [[nodiscard]] std::set<GroupId>        trainGroups(std::string const& train) const;
    [[nodiscard]] std::vector<std::string> trainsIn(GroupId g) const;
    /// Create's isOccupiedUnless(train): another train is in the section (or a crossing one),
    /// or another train reserved it.
    [[nodiscard]] bool occupiedFor(GroupId g, std::string const& train) const;
    /// Create's isOccupiedUnless(boundary): a train is in it, or it is reserved through another
    /// signal.
    [[nodiscard]] bool occupiedUnless(GroupId g, std::string const& boundaryId, int side) const;

    /// Create's Navigation signal scan for a train whose front is at `front`, going along
    /// `path`: signals within `lookahead` blocks are checked (a cross signal chain once started is
    /// followed to its end). The train's earlier reservations are replaced by what this check
    /// grants.
    Authority checkAhead(
        ct::Railways const&              rail,
        std::string const&               train,
        ct::GraphLocation const&         front,
        std::vector<ct::PathStep> const& path,
        double                           lookahead
    );
    void releaseReservations(std::string const& train);
    [[nodiscard]] std::optional<Reservation> reservation(GroupId g) const;

    /// Create's Navigation.search over the signals of the edge from `from` to `to`, in travel order:
    /// a boundary side without a signal for that direction can't be passed (`canNavigateVia`:
    /// `barrier` is its distance from `from`, infinity when every boundary may be passed);
    /// redstone-forced red signals cost `redstonePenalty`; occupied ones cost `redWeight`, which is
    /// halved after each of them (Create keeps one `signalWeight` for the whole search).
    using EdgeCost = ct::EdgeCost;
    [[nodiscard]] EdgeCost edgeCost(
        ct::NodeLocation const& from,
        ct::NodeLocation const& to,
        std::string const&      train,
        double&                 redWeight,
        double                  redstonePenalty
    ) const;
    /// `edgeCost` with a fixed red weight, penalty only.
    [[nodiscard]] double edgePenalty(
        ct::NodeLocation const& from,
        ct::NodeLocation const& to,
        std::string const&      train,
        double                  redPenalty,
        double                  redstonePenalty
    ) const;

    // ---- states -----------------------------------------------------------------------------
    /// Create's SignalBoundary.tickState for every boundary.
    void                      updateStates();
    [[nodiscard]] SignalState state(std::string const& boundaryId, int side) const;

    // ---- observers --------------------------------------------------------------------------
    /// Trains covering the observer's position.
    [[nodiscard]] std::vector<std::string> trainsOver(std::string const& observerId) const;

private:
    struct EdgeKey {
        ct::NodeLocation a, b; ///< a < b
        friend auto operator<=>(EdgeKey const&, EdgeKey const&) = default;
    };
    struct EdgeInfo {
        double                                   length = 0;
        std::vector<std::pair<double, size_t>>   cuts;     ///< (position from a, boundary index), sorted
        int                                      firstSegment = 0;
    };
    struct Placed {
        EdgeKey key;
        double  pos   = 0; ///< from key.a
        bool    alongAB = true; ///< node1 -> node2 runs a -> b
        bool    valid = false;
    };

    [[nodiscard]] static EdgeKey keyOf(ct::NodeLocation const& x, ct::NodeLocation const& y);
    [[nodiscard]] Placed         place(ct::NodeLocation const& n1, ct::NodeLocation const& n2, double pos) const;
    [[nodiscard]] GroupId        segmentGroup(EdgeKey const& key, double pos, bool towardsB) const;
    void                         computeLinks(ct::Railways const& rail);
    void                         remapTrains();

    std::vector<Boundary>                    mBoundaries;
    std::map<std::string, size_t>            mBoundaryIndex;
    std::vector<Observer>                    mObservers;
    std::map<EdgeKey, EdgeInfo>              mEdges;
    std::vector<GroupId>                     mSegmentGroup;
    std::vector<std::array<GroupId, 2>>      mBoundaryGroups;
    std::vector<std::array<SignalState, 2>>  mStates;
    std::vector<std::vector<std::pair<std::string, int>>> mExitCache[2];
    std::vector<std::vector<GroupId>>        mLinked;
    size_t                                   mGroupCount = 0;
    /// for exits: the graph topology by location
    std::map<ct::NodeLocation, std::vector<std::pair<ct::NodeLocation, ct::Vec3d>>> mOut; ///< node -> (next, dir at start)
    std::map<std::pair<ct::NodeLocation, ct::NodeLocation>, std::pair<ct::Vec3d, ct::Vec3d>> mDirs; ///< (from,to) -> (start dir, end dir)

    std::map<std::string, std::vector<Span>> mTrainSpans;
    std::map<std::string, std::set<GroupId>> mTrainGroups;
    std::vector<Reservation>                 mReservations;
};

} // namespace lv::core::signals
