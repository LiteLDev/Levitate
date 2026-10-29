#pragma once

// Fluids unit (U19) internals shared by its .cpp files. Server thread only.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/fluids/Fluids.h"
#include "feature/common/Capabilities.h"
#include "feature/fluids/FluidRegistry.h"

class BlockSource;
class BlockPos;
class Block;
class ItemStack;
class Player;

namespace lv::feature::fluids {

namespace cf = core::fluids;
using core::GlobalPos;
using Json = nlohmann::json;

inline constexpr char kSpout[]           = "levitate:spout";
inline constexpr char kItemDrain[]       = "levitate:item_drain";
inline constexpr char kHosePulley[]      = "levitate:hose_pulley";
inline constexpr char kPfi[]             = "levitate:portable_fluid_interface";
inline constexpr char kHoneyBlock[]      = "levitate:honey";
inline constexpr char kChocolateBlock[]  = "levitate:chocolate";
inline constexpr char kHoneyBucket[]     = "levitate:honey_bucket";
inline constexpr char kChocolateBucket[] = "levitate:chocolate_bucket";
inline constexpr char kTestDepot[]       = "levitate:fluids_test_depot";
inline constexpr char kFacing[]          = "levitate:facing"; ///< FacingID 0..5 (hose pulley: 2..5)
inline constexpr char kRotor[]           = "levitate:rotor";  ///< kinetic rotor state (client mod)
inline constexpr char kDataKey[]         = "fluids";          ///< our key in BlockData documents

/// `config/fluids.json`: Create's CFluids (and the PSI timeout of CLogistics) defaults.
struct FluidsConfig {
    int version = 1;
    /// The minimum amount of fluid blocks the hose pulley needs to find before deeming it an
    /// infinite source (-1 disables infinite sources).
    int  hosePulleyBlockThreshold = 10000;
    /// Whether hose pulleys should continue filling up above-threshold sources.
    bool fillInfinite = false;
    /// The maximum distance a hose pulley can draw fluid blocks from.
    int  hosePulleyRange = 128;
    /// Which fluids can be drained infinitely: ALLOW_ALL, DENY_ALL, ALLOW_BY_TAG, DENY_BY_TAG.
    std::string              bottomlessFluidMode = "ALLOW_BY_TAG";
    /// Create's fluid tag `create:bottomless/allow` / `create:bottomless/deny`.
    std::vector<std::string> bottomlessAllow = {"minecraft:water", "minecraft:lava"};
    std::vector<std::string> bottomlessDeny  = {};
    /// Whether hose pulleys may place fluid source blocks.
    bool fluidFillPlaceFluidSourceBlocks = true;
    /// Ticks a portable interface stays connected without a transfer (CLogistics psiTimeout).
    int  psiTimeout = 60;
};
[[nodiscard]] FluidsConfig const& config();

// ---- world access --------------------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] BlockPos     toBlockPos(GlobalPos const& p);
[[nodiscard]] GlobalPos    offsetPos(GlobalPos const& p, int face, int n = 1);
/// Our tracked block id at `p` ("" when unloaded / not ours).
[[nodiscard]] std::string blockIdAt(GlobalPos const& p);
[[nodiscard]] int         intStateAt(GlobalPos const& p, char const* state, int fallback = 0);
/// The block at `p` has a full solid face towards `face` (Create's hasBlockSolidSide approximation).
[[nodiscard]] bool solidAt(GlobalPos const& p);
/// The block has no collision (air, plants, fluids, our fluid blocks): spouts work through it.
[[nodiscard]] bool passableAt(GlobalPos const& p);
/// Redstone power from a neighbour (polled: custom blocks get no neighbour updates).
[[nodiscard]] bool poweredAt(GlobalPos const& p);
void playSound(GlobalPos const& p, std::string const& sound, float volume = 1.f, float pitch = 1.f);
/// Drops an item entity at a world position with an initial motion.
void spawnItem(int dim, double x, double y, double z, ItemStack const& stack, double mx = 0, double my = 0, double mz = 0);
/// Gives the stack to the player (or drops it at the player).
void giveOrDrop(Player& player, ItemStack const& stack);

// ---- tanks in BlockData --------------------------------------------------------------------------
[[nodiscard]] FluidStack tankOf(Json const& doc, char const* key = "tank");
void                     setTank(Json& doc, FluidStack const& f, char const* key = "tank");
/// Our part of the BlockData document at `p` (nullptr when absent).
[[nodiscard]] Json const* dataAt(GlobalPos const& p);
[[nodiscard]] Json&       editData(GlobalPos const& p);
/// A Create SmartFluidTank fill into a single tank: accepts up to the free space of the same fluid.
int tankFill(FluidStack& tank, int capacity, FluidStack const& in, bool simulate);
FluidStack tankDrain(FluidStack& tank, FluidStack const& request, bool simulate);

// ---- machines tracked for ticking ----------------------------------------------------------------
enum class Kind { Spout, Drain, Hose, Pfi, Source };
[[nodiscard]] std::optional<Kind> kindOf(std::string_view blockId);
/// Positions of our ticking blocks (per kind), kept in the world DB.
[[nodiscard]] std::vector<GlobalPos> tracked(Kind kind);
void untrack(GlobalPos const& p);
/// Runs `fn` at the end of this tick (world edits outside of block / interaction events).
void defer(std::function<void()> fn);
/// Server ticks since the world was loaded (fluids unit clock).
[[nodiscard]] std::uint64_t currentTick();

// ---- per machine ---------------------------------------------------------------------------------
void tickSpout(GlobalPos const& p);
void tickDrain(GlobalPos const& p);
void tickHose(GlobalPos const& p);
void tickPfi(GlobalPos const& p);
void tickSources(std::uint64_t tick);
void registerSpout();
void registerDrain();
void registerHose();
void registerPfi();
void registerSources();

/// Right-click on our blocks; true when handled (the interaction is consumed).
bool useOnDrain(Player& player, GlobalPos const& p, ItemStack const& held);
/// Bucket use on / for our fluid blocks; true when handled.
bool useBucket(Player& player, GlobalPos const& clicked, int face, ItemStack const& held);
/// Player exchange with a tank (Create's FluidHelper.tryEmptyItemIntoBE / tryFillItemFromBE).
bool exchangeWithTank(Player& player, FluidStack& tank, int capacity, ItemStack const& held, bool allowInsert);

/// Entities in honey / chocolate (applied every few ticks).
void applyFluidEffects();

// ---- client sync (companions) --------------------------------------------------------------------
/// Sends a hose pulley's state ({offset, speed per tick}; removed when `gone`).
void syncHose(GlobalPos const& p, float offset, float speedPerTick, bool gone = false);
/// A spout started filling (`fluid` colour for the stream).
void syncSpout(GlobalPos const& p, std::string const& fluid);
/// Sends every hose of the player's dimension (after the handshake).
void syncAllTo(Player& player);
/// The hose state for the selftests.
[[nodiscard]] float hoseOffset(GlobalPos const& p);

} // namespace lv::feature::fluids
