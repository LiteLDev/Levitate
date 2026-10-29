#pragma once

// Internals of the transmission unit (U6), shared by its own .cpp files only.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/GlobalPos.h"
#include "core/kinetics/Spec.h"
#include "core/kinetics/Types.h"
#include "core/transmission/Transmission.h"

class Block;
class BlockSource;
class BlockPos;
class Player;
class ItemStack;

namespace lv::feature::transmission {

namespace ct = core::transmission;
namespace ck = core::kinetics;
using core::GlobalPos;
using ck::Axis;
using ck::Face;

// ---- ids --------------------------------------------------------------------------------------------
inline constexpr char kShaft[]               = "levitate:shaft";
inline constexpr char kCogwheel[]            = "levitate:cogwheel";
inline constexpr char kLargeCogwheel[]       = "levitate:large_cogwheel";
inline constexpr char kGearbox[]             = "levitate:gearbox";
inline constexpr char kVerticalGearbox[]     = "levitate:vertical_gearbox"; ///< item only
inline constexpr char kClutch[]              = "levitate:clutch";
inline constexpr char kGearshift[]           = "levitate:gearshift";
inline constexpr char kChainDrive[]          = "levitate:encased_chain_drive";
inline constexpr char kChainGearshift[]      = "levitate:adjustable_chain_gearshift";
inline constexpr char kAndesiteShaft[]       = "levitate:andesite_encased_shaft";
inline constexpr char kBrassShaft[]          = "levitate:brass_encased_shaft";
inline constexpr char kAndesiteCog[]         = "levitate:andesite_encased_cogwheel";
inline constexpr char kBrassCog[]            = "levitate:brass_encased_cogwheel";
inline constexpr char kAndesiteLargeCog[]    = "levitate:andesite_encased_large_cogwheel";
inline constexpr char kBrassLargeCog[]       = "levitate:brass_encased_large_cogwheel";
inline constexpr char kSequencedGearshift[]  = "levitate:sequenced_gearshift";
inline constexpr char kSpeedController[]     = "levitate:rotation_speed_controller";
inline constexpr char kSpeedometer[]         = "levitate:speedometer";
inline constexpr char kStressometer[]        = "levitate:stressometer";
// other units' ids (referenced by id only)
inline constexpr char kAndesiteCasing[] = "levitate:andesite_casing";
inline constexpr char kBrassCasing[]    = "levitate:brass_casing";
inline constexpr char kWrench[]         = "levitate:wrench";
inline constexpr char kGoggles[]        = "levitate:goggles";

// ---- state names ----------------------------------------------------------------------------------
inline constexpr char kAxis[]       = "levitate:axis";
inline constexpr char kRotor[]      = "levitate:rotor";
inline constexpr char kPowered[]    = "levitate:powered";
inline constexpr char kTopShaft[]   = "levitate:top_shaft";
inline constexpr char kBottomShaft[] = "levitate:bottom_shaft";
inline constexpr char kPart[]       = "levitate:part";
inline constexpr char kAlongFirst[] = "levitate:along_first";
inline constexpr char kSignal[]     = "levitate:signal";
inline constexpr char kSeqState[]   = "levitate:state";
inline constexpr char kSeqMod[]     = "levitate:mod";
inline constexpr char kFacing[]     = "levitate:facing";
inline constexpr char kDial[]       = "levitate:dial";
inline constexpr char kRevision[]   = "levitate:revision";
inline constexpr char kBracket[]    = "levitate:bracket";

/// Every block id of the unit (for listeners and selftests).
[[nodiscard]] std::vector<std::string> const& allBlocks();

[[nodiscard]] bool isSmallCogBlock(std::string_view id);  ///< cogwheel + encased small cogs
[[nodiscard]] bool isLargeCogBlock(std::string_view id);  ///< large cogwheel + encased large cogs
[[nodiscard]] bool isEncasedCog(std::string_view id);
[[nodiscard]] bool isEncasedShaft(std::string_view id);
[[nodiscard]] bool isChainDrive(std::string_view id);
[[nodiscard]] bool isGauge(std::string_view id);
/// Blocks that read redstone (clutch, gearshift, adjustable chain gearshift, sequenced gearshift).
[[nodiscard]] bool readsRedstone(std::string_view id);
/// Blocks with a comparator output (gauges, sequenced gearshift).
[[nodiscard]] bool hasComparatorOutput(std::string_view id);

// ---- world helpers (World.cpp) ---------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] std::string  idAt(BlockSource& region, GlobalPos const& pos);
/// Integer state of the block at `pos` (fallback when missing / unloaded).
[[nodiscard]] int stateAt(BlockSource& region, GlobalPos const& pos, std::string_view state, int fallback = 0);
/// Sets the block at `pos` to `id` with `states` (unknown states ignored). False on failure.
bool place(BlockSource& region, GlobalPos const& pos, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states);
/// Changes some states of the block at `pos` (no-op when unchanged). False on failure.
bool setStates(BlockSource& region, GlobalPos const& pos, std::vector<std::pair<std::string_view, int>> const& states);
/// The kinetic spec of the block at `pos` says it has a shaft end towards `face`.
[[nodiscard]] bool shaftTowards(BlockSource& region, GlobalPos const& pos, Face face);
/// The rotation axis of the kinetic block at `pos` (nullopt if not kinetic / no axis).
[[nodiscard]] std::optional<Axis> rotationAxisAt(BlockSource& region, GlobalPos const& pos);
/// Kinetic cog type at `pos` (none for non-kinetic blocks).
[[nodiscard]] ck::CogType cogAt(BlockSource& region, GlobalPos const& pos);
/// Air, water, grass, ... (a placement may replace it).
[[nodiscard]] bool replaceableAt(BlockSource& region, GlobalPos const& pos);
/// Redstone power reaching `pos` from its neighbours (Create's getBestNeighborSignal), 0..15.
[[nodiscard]] int neighbourSignal(BlockSource& region, GlobalPos const& pos);
void playSound(int dim, GlobalPos const& pos, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void actionBar(Player& player, std::string const& text);
[[nodiscard]] Player* playerById(std::int64_t uid);
[[nodiscard]] GlobalPos stepPos(GlobalPos p, Face f, int n = 1);
/// Create's Direction.getNearest of the player's look vector.
[[nodiscard]] Face nearestLooking(Player const& player);
/// Create's getHorizontalDirection (the horizontal face the player looks towards).
[[nodiscard]] Face horizontalLooking(Player const& player);

// ---- placement (Placement.cpp) ----------------------------------------------------------------------
struct PlaceContext {
    GlobalPos clickedPos; ///< the block that was clicked
    Face      face;       ///< the clicked face
    GlobalPos pos;        ///< where the block goes
    double    hitX = .5, hitY = .5, hitZ = .5; ///< hit point, block-local (0..1) on the clicked block
    bool      sneaking = false;
};
/// Create's getStateForPlacement of `itemId` (states to apply) or nullopt when it is not ours.
[[nodiscard]] std::optional<std::vector<std::pair<std::string_view, int>>>
stateForPlacement(BlockSource& region, std::string_view itemId, PlaceContext const& ctx, Player const& player);
/// Handles a right-click of `player` holding `item` on a block of the unit (placement helpers,
/// encasing, the sequenced gearshift screen). Returns true when the click was used.
bool useOn(Player& player, ItemStack& item, GlobalPos const& clicked, Face face, double hx, double hy, double hz);
void installPlacementHooks();
void uninstallPlacementHooks();

// ---- block behaviour (Behaviour.cpp) ----------------------------------------------------------------
/// Recomputes the chain drive shape at `pos` from its neighbours.
void updateChainShape(BlockSource& region, GlobalPos const& pos);
/// Tracks / untracks a block that needs ticking (redstone, comparator output, gauges).
void track(GlobalPos const& pos);
void untrack(GlobalPos const& pos);
/// One server tick of every tracked block.
void tickBlocks();
/// The comparator output of a block of the unit (-1: none).
[[nodiscard]] int comparatorSignal(GlobalPos const& pos);
/// The gauge's current dial target (0 when it is not a gauge).
[[nodiscard]] float dialTarget(GlobalPos const& pos);
void loadTracked();
void saveTracked();
void clearTracked();
/// A block changed from `before` to `after` at `pos`: tracking, chain shapes (work for the tick
/// goes to `later`).
void onBlockChanged(BlockSource& region, GlobalPos const& pos, std::string const& before, std::string const& after,
                    std::vector<std::function<void()>>& later);
void installComparatorHook();
void uninstallComparatorHook();

// ---- sequenced gearshift (Sequencer.cpp) -------------------------------------------------------------
void sequencerRedstone(BlockSource& region, GlobalPos const& pos, bool powered);
void sequencerTick(BlockSource& region, GlobalPos const& pos);
void sequencerSpeedChanged(GlobalPos const& pos, float previous, float now);
bool openSequencerScreen(Player& player, GlobalPos const& pos);
[[nodiscard]] std::vector<ct::Instruction> sequencerInstructions(GlobalPos const& pos);
void setSequencerInstructions(GlobalPos const& pos, std::vector<ct::Instruction> list);
[[nodiscard]] int sequencerCurrent(GlobalPos const& pos);
void registerSequencerScreen();
void clearSequencers();

// ---- speed controller (SpeedController.cpp) -------------------------------------------------------
[[nodiscard]] int  targetSpeed(GlobalPos const& pos);
void               setTargetSpeed(GlobalPos const& pos, int rpm);
void               registerSpeedControllerBox();
void               clearSpeedControllers();
inline constexpr int kDefaultTargetSpeed = 16;

// ---- gauges (Gauges.cpp) ------------------------------------------------------------------------------
void gaugeTick(BlockSource& region, GlobalPos const& pos);
void gaugeForget(GlobalPos const& pos);
void gaugeSyncTo(Player& player);
void clearGauges();

} // namespace lv::feature::transmission
