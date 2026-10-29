#pragma once

// Fluid transport (U17, SPEC §2.12 / §3.6.1): fluid pipes, glass and encased pipes, mechanical
// pumps, fluid valves and smart fluid pipes, on top of the pure flow model (core/pipes).
//
// Internal header of the unit (other units integrate through the fluid capability and the fluid
// registry in feature/common, never through this file).

#include <array>
#include <optional>
#include <string>

#include "core/GlobalPos.h"
#include "core/pipes/Flow.h"
#include "core/pipes/Rules.h"

class Block;
class BlockPos;
class BlockSource;
class Player;
class ItemStack;

namespace lv::feature::pipes {

using core::GlobalPos;
namespace cp = core::pipes;
using cp::Axis;
using cp::Face;
using cp::FaceMask;

inline constexpr char kFluidPipe[]    = "levitate:fluid_pipe";
inline constexpr char kGlassPipe[]    = "levitate:glass_fluid_pipe";
inline constexpr char kEncasedPipe[]  = "levitate:encased_fluid_pipe";
inline constexpr char kPump[]         = "levitate:mechanical_pump";
inline constexpr char kValve[]        = "levitate:fluid_valve";
inline constexpr char kSmartPipe[]    = "levitate:smart_fluid_pipe";
inline constexpr char kWrench[]       = "levitate:wrench";
inline constexpr char kCopperCasing[] = "levitate:copper_casing";
inline constexpr char kMetalBracket[] = "levitate:metal_bracket";
inline constexpr char kWoodBracket[]  = "levitate:wooden_bracket";

/// `config/pipes.json` (Create's CFluids keys used by pipes).
struct PipesConfig {
    int version = 1;
    /// How far a mechanical pump can push or pull fluid on either side (blocks).
    int mechanicalPumpRange = 16;
    /// Whether open-ended pipes can place fluid source blocks.
    bool pipesPlaceFluidSourceBlocks = true;
    /// Java's `waterSourceConversion` rule (on by default): a drained water source with two
    /// neighbouring sources refills, so pipes can pull from a 2x2 pool forever.
    bool waterSourceConversion = true;
    /// Java's `lavaSourceConversion` rule (off by default).
    bool lavaSourceConversion = false;
};

// ---- block kinds ----------------------------------------------------------------------------------------

enum class BlockKind : std::uint8_t { None, Pipe, Glass, Encased, Pump, Valve, Smart };
[[nodiscard]] BlockKind kindOf(Block const& block);
[[nodiscard]] BlockKind kindOf(std::string_view id);
[[nodiscard]] bool      isPipeLike(BlockKind k); ///< anything with a fluid transport behaviour

/// The faces through which fluid can flow (Create's `canHaveFlowToward`).
[[nodiscard]] FaceMask flowFaces(Block const& block);
/// Six-way connection states of fluid / encased pipes.
[[nodiscard]] FaceMask   connections(Block const& block);
[[nodiscard]] Block const* withConnections(Block const& block, FaceMask open);
[[nodiscard]] Face        pumpFront(Block const& block);
[[nodiscard]] cp::ValvePlacement valveState(Block const& block);
[[nodiscard]] cp::SmartPipeState smartState(Block const& block);
/// What can make the flow model redo its search (axis, faces, facing, valve open...).
[[nodiscard]] std::string flowSignature(Block const& block);

/// Create's `FluidPipeBlock.canConnectTo`: would a pipe at `pos` connect to the block on `face`?
[[nodiscard]] bool canConnectTo(BlockSource& region, BlockPos const& pos, Face face);

// ---- world actions -----------------------------------------------------------------------------------

/// What a player looked like when placing a block (Create's BlockPlaceContext).
struct PlaceContext {
    double lookX = 0, lookY = 0, lookZ = 1;
    bool   sneaking = false;
};
/// Orients a freshly placed pipe / pump / valve / smart pipe as Create's getStateForPlacement.
void orientPlaced(BlockSource& region, BlockPos const& pos, PlaceContext const& ctx);
/// Create's `FluidPipeBlock.updateShape` for a neighbour change on `face`.
void updatePipeShape(BlockSource& region, BlockPos const& pos, std::optional<Face> changed);
/// Recomputes the rim states of a fluid pipe.
void updateRims(BlockSource& region, BlockPos const& pos);

/// Wrench right-click on one of the unit's blocks; `hit` relative to the block's corner (0..1).
/// Returns true when it acted.
bool wrench(Player* player, BlockSource& region, BlockPos const& pos, Face clicked, double hx, double hy, double hz);
/// Copper casing on a pipe (Create's EncasableBlock); returns true when encased.
bool encase(BlockSource& region, BlockPos const& pos);
/// A bracket item on a straight pipe; returns true when attached.
bool attachBracket(BlockSource& region, BlockPos const& pos, std::string const& bracketId);
/// The bracket on a pipe ("" when none).
[[nodiscard]] std::string bracketOf(GlobalPos const& pos);

// ---- flow -------------------------------------------------------------------------------------------------

/// The flow model of the running world.
[[nodiscard]] cp::Model& model();
/// Runs the queued block changes and one flow tick now (selftests).
void tickNow();
/// The fluid shown in a pipe (rendering / goggles).
[[nodiscard]] cp::Fluid fluidIn(GlobalPos const& pos);
/// Marks a pipe-like block as changed (Create's scheduled `propagateChangedPipe`).
void scheduleChanged(GlobalPos const& pos, std::optional<FaceMask> oldFaces = std::nullopt);
/// Valve pointer (0 closed .. 1 open).
[[nodiscard]] float valvePointer(GlobalPos const& pos);

/// Effects of open pipe ends (water, lava, milk, potions, tea), registered in the fluid registry.
void registerOpenPipeEffects();

/// BlockSource of a dimension (nullptr when it isn't loaded).
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] BlockPos     toBlockPos(GlobalPos const& p);

} // namespace lv::feature::pipes
