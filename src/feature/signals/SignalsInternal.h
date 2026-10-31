#pragma once

// Internal state of the signals unit (U28, server thread). Not a contract: other units use
// feature/tracks/SignalsExt.h.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/GlobalPos.h"
#include "core/signals/Signals.h"
#include "core/tracks/Graph.h"
#include "feature/tracks/SignalsExt.h"

class Block;
class BlockPos;
class BlockSource;
class Player;
class ItemStack;

namespace lv::feature::signals {

namespace cs  = core::signals;
namespace ct  = core::tracks;
namespace api = tracks::signals;
using core::GlobalPos;

enum class Kind { Signal, Observer };

/// What a signal / observer block targets (Create's TrackTargetingBehaviour: the track block
/// and the direction along it; or a point on a curve).
struct Target {
    int           dim = 0;
    ct::BlockPos3 track;      ///< the clicked track (or fake track of a curve)
    ct::Vec3d     dir;        ///< travel direction the block acts for
    bool          curve = false;
    ct::Vec3d     at;         ///< curves: the world point clicked
};

/// One signal or observer block.
struct BlockRec {
    Kind        kind = Kind::Signal;
    GlobalPos   pos;
    Target      target;
    std::string point;  ///< bound edge point ("" = unbound: its track is gone)
    int         side = 1; ///< signals: boundary side the block controls
    int         type = 0; ///< signals: SignalType (cached block state)
    bool        powered = false; ///< signals: redstone input (cached block state)
    // Create's SignalBlockEntity (not saved)
    cs::SignalState shown     = cs::SignalState::Invalid; ///< the state the block shows (enterState)
    int             redHold   = 0;  ///< switchToRedAfterTrainEntered: ticks before it may turn red
    std::uint64_t   unpowerAt = 0;  ///< SignalBlock.neighborChanged: loss of power applies 4 ticks later
};

/// A selected track. As Create's TrackTargetingBlockItem it lives on the item stack (item data
/// `lv:track_target`, see selectionOf / setHeldSelection) and travels with the item.
struct Selection {
    Target      target;
    std::string item;
};

struct ObserverRun {
    int         activated = 0; ///< Create's TrackObserver.activated (8 ticks after a train)
    std::string train;
};

/// Result of targeting a track (Create's OverlapResult).
enum class Overlap { Valid, Occupied, Junction, NoTrack };

struct State {
    std::map<GlobalPos, BlockRec>      blocks;
    std::map<GlobalPos, int>           watches; ///< signal -> common::redstone watch id
    cs::SignalSystem                   system;
    std::map<std::string, ObserverRun> observers;  ///< by point id
    std::map<GlobalPos, std::pair<std::string, Selection>> intents; ///< placement in progress: uuid, selection
    std::vector<std::function<void()>> tasks;      ///< run at the start of the next tick
    api::CargoProvider                 cargo;
    api::NameProvider                  trainName;
    uint64_t                           tick         = 0;
    uint64_t                           seenVersion  = ~0ull;
    bool                               indexDirty   = false;
    bool                               sidesDirty   = false;
    bool                               loaded       = false;
    std::string                        lastOverlay;  ///< last overlay sent to companions
    bool                               loggedSync = false;
};

State& state();

// ---- helpers (SignalsModule.cpp) ---------------------------------------------------------------
BlockSource* regionOf(int dim);
std::optional<Kind> kindOf(Block const& block);
void actionbar(Player& player, std::string const& key, std::string const& fallback, char colour);
void playSound(Player& player, std::string const& name, float volume, float pitch);

/// Resolves a target to a graph location travelling along `target.dir`.
std::optional<ct::GraphLocation> resolve(Target const& target);
/// Checks a target as Create's TrackTargetingBlockItem.withGraphLocation does.
Overlap check(Target const& target, Kind kind);
/// The target of a click on a track block or a curve (nullopt: not a track).
std::optional<Target> targetOfClick(Player& player, BlockPos const& clicked);

/// Registers a block and binds it to its target (joining an existing signal boundary).
/// Returns false when the target can't be bound (the block stays registered, unbound).
bool addBlock(Kind kind, GlobalPos const& pos, Target const& target);
/// The block is gone: unbinds it.
void removeBlock(GlobalPos const& pos);
/// Wrench: cycles the signal type of the block's side. False when the signal isn't bound.
bool cycleType(GlobalPos const& pos);
/// The state of the signal's boundary side (what the block is going to show).
cs::SignalState lampOf(BlockRec const& rec);
/// Create's SignalBlockEntity.enterState: the state the block shows (green / yellow hold 15 ticks
/// before turning red).
cs::SignalState shownOf(BlockRec const& rec);
/// Whether the block at `pos` gets redstone power.
bool poweredAt(BlockSource& region, BlockPos const& pos);

/// Runs one tick of the unit (selftests may call parts of it through the API).
void runTick();
/// Rebuilds sections now if the graph or the points changed.
void refresh();

// ---- item use (SignalsHooks.cpp) ---------------------------------------------------------------
/// Item data key (common/Items.h) of the selected track on a signal / observer stack.
inline constexpr char kTargetData[] = "track_target";
/// The selection carried by a stack (nullopt: none, or not a signal / observer item).
[[nodiscard]] std::optional<Selection> selectionOf(ItemStack const& stack);
/// Writes (nullopt: clears) the selection on a stack (in a click: the click's own stack, which
/// the game writes back to the hand after the use).
void setSelection(ItemStack& stack, std::optional<Target> const& target);
/// Writes (nullopt: clears) the selection on the player's held stack and sends the slot
/// (outside a click).
void setHeldSelection(Player& player, std::optional<Target> const& target);
/// Right-click with the signal / observer item. Returns true when vanilla must not place.
bool onTargetingItemUse(Player& player, ItemStack& held, std::string const& item, BlockPos const& clicked, int face);
void installHooks();
void uninstallHooks();

} // namespace lv::feature::signals
