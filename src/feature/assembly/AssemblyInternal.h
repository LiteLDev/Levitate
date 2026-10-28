#pragma once

// Internals of the assembly unit (U15): deployer, mechanical arm, item application. Not a
// contract: other units use SequencedAssembly.h, capabilities, value boxes and actors.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/assembly/Arm.h"
#include "core/assembly/Deployer.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;
class SimulatedPlayer;
class Vec3;

namespace lv::feature::assembly {

using core::GlobalPos;
using Json = nlohmann::json;
namespace ca = core::assembly;
using ca::Face;

inline constexpr char kDeployerId[] = "levitate:deployer";
inline constexpr char kArmId[]      = "levitate:mechanical_arm";
inline constexpr char kWrenchId[]   = "levitate:wrench";

/// Config section `assembly` (Create's CLogistics key of the mechanical arm).
struct AssemblyConfig {
    int version = 1;
    /// Maximum distance in blocks at which a Mechanical Arm can interact with targets.
    int mechanicalArmRange = 5;
};
AssemblyConfig const& config();

namespace detail {

BlockSource* regionOf(int dim);
Player*      playerById(std::int64_t uniqueId);
[[nodiscard]] std::uint64_t currentTick();
/// Runs `fn` at the start of the next server tick (world edits after an interaction).
void defer(std::function<void()> fn);

void actionBar(Player& player, std::string const& text);
void playSound(int dim, double x, double y, double z, std::string const& name, float volume, float pitch);
void particle(int dim, double x, double y, double z, std::string const& effect);
/// Spawns an item entity (Create's Block.popResource / dropItem).
void dropItem(BlockSource& region, double x, double y, double z, ItemStack const& stack);
/// Redstone power reaching `pos` from a neighbour (polled: custom blocks get no neighbour updates).
[[nodiscard]] bool powered(BlockSource& region, BlockPos const& pos);

[[nodiscard]] std::optional<int> intState(Block const& block, std::string_view state);
/// `block` with integer states changed (nullptr if a state is unknown).
[[nodiscard]] Block const* withStates(Block const& block, std::vector<std::pair<std::string, int>> const& states);
/// Whether the kinetic block at `pos` has a shaft end on `face` (Create's prefersConnectionTo).
[[nodiscard]] bool shaftTowards(BlockSource& region, BlockPos const& pos, Face face);

[[nodiscard]] BlockPos offsetPos(BlockPos const& p, Face f, int n = 1);
[[nodiscard]] Face     nearestFace(double x, double y, double z);
/// The direction a player looks along (Create's getNearestLookingDirection).
[[nodiscard]] Face lookFace(Actor const& actor);

[[nodiscard]] Json      posJson(GlobalPos const& p);
[[nodiscard]] std::optional<GlobalPos> posFrom(Json const& j);

} // namespace detail

// ---- deployer (Deployer.cpp) ------------------------------------------------------------------
namespace deployer {
void install();
void uninstall();
void tick();
void onWorldReady();
void onWorldSave();
void onWorldClosed();
/// A deployer was placed (orientation from the placer).
void onPlaced(BlockSource& region, BlockPos const& pos, Player* placer, Face clickedFace);
/// A player's right-click on a deployer; true when handled (no vanilla use).
bool onUse(Player& player, BlockPos const& pos, Face face, ItemStack& held);
/// Mode / state accessors for selftests and the arm.
[[nodiscard]] ca::DeployerMode mode(GlobalPos const& pos);
void                           setMode(GlobalPos const& pos, ca::DeployerMode mode);
[[nodiscard]] ItemStack        held(GlobalPos const& pos);
void                           setHeld(GlobalPos const& pos, ItemStack const& stack);
[[nodiscard]] std::vector<ItemStack> overflow(GlobalPos const& pos);
[[nodiscard]] ca::DeployerState      state(GlobalPos const& pos);
/// Activates a deployer once now, as when its hand reaches out (selftests).
void activateNow(GlobalPos const& pos);
/// Recipe lookup on an item (depots, belts, contraption deployers): outputs for one item.
[[nodiscard]] std::optional<std::vector<ItemStack>> deployOnItem(ItemStack const& item, ItemStack& held, bool simulate);
} // namespace deployer

// ---- fake player (FakePlayer.cpp) -------------------------------------------------------------
namespace fakeplayer {
/// The deployer player of a dimension (Create's DeployerFakePlayer), created on first use.
/// nullptr while it can't exist (no level / still joining).
[[nodiscard]] SimulatedPlayer* get(int dim);
[[nodiscard]] bool isFake(Player const& player);
void forget();
void removeAll();
} // namespace fakeplayer

// ---- mechanical arm (Arm.cpp) ---------------------------------------------------------------
namespace arm {
void install();
void uninstall();
void tick();
void onWorldReady();
void onWorldSave();
void onWorldClosed();
/// Whether the block at `pos` can be an interaction point (Create's ArmInteractionPoint.create != null).
[[nodiscard]] bool isPoint(BlockSource& region, BlockPos const& pos);
/// Right-click with the arm item on a block: selects / cycles an interaction point. True if handled.
bool onSelect(Player& player, BlockPos const& pos);
/// Left-click with the arm item on a selected point: removes it. True if handled.
bool onDeselect(Player& player, BlockPos const& pos);
/// An arm was placed by `placer` (null: no player): applies the player's selection.
void onPlaced(BlockSource& region, BlockPos const& pos, Player* placer, Face clickedFace);
/// Number of selected points of a player (selftests).
[[nodiscard]] std::pair<int, int> selectionCounts(Player const& player);
[[nodiscard]] std::pair<int, int> pointCounts(GlobalPos const& pos);
[[nodiscard]] ItemStack           held(GlobalPos const& pos);
[[nodiscard]] ca::ArmPhase        phase(GlobalPos const& pos);
void                              setSelectionMode(GlobalPos const& pos, ca::ArmSelectionMode mode);
/// Adds an interaction point directly (selftests / commands).
bool addPoint(GlobalPos const& arm, BlockPos const& point, bool take);
} // namespace arm

// ---- item application (ItemApplication.cpp) ----------------------------------------------------
namespace application {
/// Create's ManualApplicationRecipe: right-click a block with an item. True when applied.
bool tryApply(Player& player, BlockSource& region, BlockPos const& pos, ItemStack& held, bool isFakePlayer);
} // namespace application

// ---- client sync (Sync.cpp) -------------------------------------------------------------------
namespace sync {
/// Sends a deployer's animation state to nearby companions.
void deployer(GlobalPos const& pos, Json const& state);
/// Sends an arm's animation state to nearby companions.
void arm(GlobalPos const& pos, Json const& state);
void install();
void uninstall();
} // namespace sync

// ---- hooks (Hooks.cpp) ------------------------------------------------------------------------
void installHooks();
void uninstallHooks();

} // namespace lv::feature::assembly
