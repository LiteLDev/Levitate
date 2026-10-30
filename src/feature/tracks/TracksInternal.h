#pragma once

// Internal state of the tracks unit (server thread). Not part of the contract: use Tracks.h.

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "core/GlobalPos.h"
#include "core/tracks/CurveStore.h"
#include "core/tracks/Graph.h"
#include "core/tracks/Placement.h"

class Block;
class BlockType;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;

namespace lv::feature::tracks {

namespace ct = core::tracks;

/// Config section `tracks` (Create's CTrains keys that belong to tracks).
struct TracksConfig {
    int version = 1;
    /// Maximum length of track that can be placed as one batch or turn.
    int maxTrackPlacementLength = 32;
};

/// A player's first click with the track item (Create keeps it on the item stack).
struct Selection {
    int          dim = 0;
    ct::TrackEnd end;
};

/// How a player looked when right-clicking with the track item (for the placed track's shape).
struct PlaceIntent {
    ct::Vec3d   look;
    float       yaw = 0;
    std::string uuid;
};

struct PortalLink {
    int           dim = 0;
    ct::BlockPos3 pos;
};

struct State {
    ct::Railways   rail;
    ct::CurveStore curves;
    std::map<std::pair<int, ct::BlockPos3>, PortalLink> portals; ///< bound portal tracks (both sides)
    std::map<std::string, Selection>                    selections; ///< by player UUID
    std::set<std::string>                               ctrlHeld;   ///< companions holding Ctrl (by UUID)

    std::set<std::tuple<int, int, int>> dirtyCurveChunks; ///< (dim, cx, cz)
    std::set<int>                       savedGraphDims;
    uint64_t                            savedGraphVersion = 0;
    bool                                portalsDirty      = false;

    // per tick
    std::vector<std::function<void()>>        shapeFixes;  ///< run first
    std::vector<std::function<void()>>        tasks;       ///< deferred world edits
    std::set<std::pair<int, ct::BlockPos3>>   railAdded;   ///< graph updates after placement
    std::set<std::pair<int, ct::BlockPos3>>   noDrops;     ///< creative breaks this tick
    std::set<std::pair<int, ct::BlockPos3>>   playerBroke; ///< blocks a player broke this tick
    std::map<std::string, uint64_t>           lastUse;     ///< debounce per player
    std::map<std::pair<int, ct::BlockPos3>, PlaceIntent> placeIntents; ///< plain placements this tick
    uint64_t                                  tick         = 0;
    uint64_t                                  notifiedVersion = 0;
    int                                       editing      = 0; ///< >0 while we change blocks ourselves

    BlockType const* trackType  = nullptr;
    BlockType const* fakeType   = nullptr;
    BlockType const* portalType = nullptr;
};

State& state();
TracksConfig const& config();

// ---- conversions --------------------------------------------------------------------------------
ct::BlockPos3 toCore(BlockPos const& p);
BlockPos      toGame(ct::BlockPos3 const& p);
BlockSource*  regionOf(int dim);

// ---- the world as the graph sees it ------------------------------------------------------------
class GameWorld : public ct::TrackWorld {
public:
    [[nodiscard]] std::optional<ct::TrackBlockView> trackAt(int dim, ct::BlockPos3 const& pos) const override;
};
GameWorld const& world();

/// The view of a track given its (old) block, for removal updates.
ct::TrackBlockView viewOf(int dim, ct::BlockPos3 const& pos, ct::Shape shape);

// ---- block helpers ------------------------------------------------------------------------------
std::optional<ct::Shape> shapeOf(Block const& block);
Block const*             trackBlock(ct::Shape shape, bool turn);
/// Sets a block without our own change handlers reacting to it as a player action.
bool setBlockQuiet(BlockSource& region, ct::BlockPos3 const& pos, Block const& block);

// ---- hooks (TrackHooks.cpp) ---------------------------------------------------------------------
void installHooks();
void uninstallHooks();

// ---- actions (TrackActions.cpp) -----------------------------------------------------------------
void actionbar(Player& player, std::string const& key, std::string const& fallback);
void playSound(Player& player, std::string const& name, float volume = 1, float pitch = 1);
/// Right-click with the track item. Returns true when the vanilla action must be cancelled.
bool onTrackItemUse(Player& player, BlockPos const& clicked, int face, Block const* clickedBlock);
void clearSelection(Player& player);
/// Adds a curve: stores it, places the fake tracks, sets the turn state at both ends.
void addCurve(int dim, ct::Curve const& curve);
/// Removes a curve (both ends) with drops at its segments unless `noDrops`, refunding to
/// `refundTo` instead when given.
void destroyCurve(int dim, ct::Curve const& curve, bool drops, Player* refundTo);
/// The track at `pos` stopped being a track (`oldShape`): curves, portal binding and graph.
void onTrackRemoved(int dim, ct::BlockPos3 const& pos, ct::Shape oldShape, bool drops);
/// A placed / changed track: graph update and portal connection.
void onTrackPlaced(int dim, ct::BlockPos3 const& pos);
/// Places / removes the fake track blocks along a curve (removal keeps blocks another curve uses).
void placeFakeTracks(int dim, ct::Curve const& curve);
void removeFakeTracks(int dim, ct::Curve const& curve);

// ---- tilted tracks (TrackTilt.cpp, Create's TrackBlockEntityTilt) -------------------------------
/// BlockData key of the unit at a track: {"tilt": angle} while the track is tilted.
inline constexpr char const* kBlockDataKey = "tracks";
/// The smoothing angle of the tilted track at `pos` (nullopt: not tilted).
[[nodiscard]] std::optional<double> tiltAt(int dim, ct::BlockPos3 const& pos);
/// Tilts the track at `pos` when two sloped curves meet there (tryApplySmoothing) or puts its
/// curves back when it no longer has exactly two (undoSmoothing). `before`: the track as the graph
/// last saw it, when a curve was just taken away.
void updateTilt(int dim, ct::BlockPos3 const& pos, std::optional<ct::TrackBlockView> const& before = std::nullopt);
/// The track at `pos` is going: its tilt is dropped without touching its curves (they go too).
void forgetTilt(int dim, ct::BlockPos3 const& pos);
/// Registers the BlockData erase listener that tells the companions a tilted track went.
void installTilt();
/// Sends the tilt of `pos` (nullopt: none) to the companions.
void syncTilt(int dim, ct::BlockPos3 const& pos, std::optional<double> angle);
/// Every tilted track (for the full sync).
[[nodiscard]] std::vector<std::tuple<int, ct::BlockPos3, double>> allTilts();

/// Re-places missing fake tracks of curves through `pos`.
void restoreFakeTracks(int dim, ct::BlockPos3 const& pos);
/// A portal block disappeared next to `pos`: portal tracks facing it fall off.
void onPortalRemoved(int dim, ct::BlockPos3 const& portalPos);

// ---- persistence & sync -------------------------------------------------------------------------
void markCurveChunkDirty(int dim, ct::BlockPos3 const& primaryPos);
void loadWorld();
void saveWorld();
void syncCurveAdded(int dim, ct::Curve const& curve);
void syncCurveRemoved(int dim, ct::Curve const& curve);
/// Sends every curve to a companion (no-op for other players).
void syncAllTo(Player& player);
/// Companion protocol channel of the curve sync (server -> client mod).
inline constexpr char const* kSyncChannel = "tracks.curves";
/// The player's track selection for the client mod's placement preview (server -> client mod):
/// {"dim", "end": TrackEnd json, "maxLength"}, or {} when the selection is gone.
inline constexpr char const* kSelectionChannel = "tracks.selection";
/// Ctrl (Create's sprint key) pressed / released (client mod -> server): {"down": bool}.
inline constexpr char const* kCtrlChannel = "tracks.ctrl";
/// Sends the player's current selection to its client mod (no-op for other players).
void syncSelection(Player& player);
/// Whether the player asks for a maximised turn: Ctrl held (client mod) or sprinting (vanilla).
[[nodiscard]] bool maximiseTurn(Player const& player);
void fireGraphChanged();

} // namespace lv::feature::tracks
