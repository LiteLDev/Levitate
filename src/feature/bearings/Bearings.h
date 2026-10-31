#pragma once

// Rotating contraptions (U21): mechanical / windmill / clockwork bearings, sails, turntables,
// cart assemblers, controller rails, minecart couplings and minecart contraptions.
// Internal header of the unit (not a contract for other units). See docs/SPEC.md §2.16, §3.6.21.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/bearings/BearingMath.h"
#include "core/contraption/Geometry.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;

namespace lv::feature::bearings {

namespace cc = ::lv::core::contraption;
namespace cb = ::lv::core::bearings;
using GP     = core::GlobalPos;
using Json   = nlohmann::json;

// ---- ids --------------------------------------------------------------------------------------
inline constexpr std::string_view kMechanicalBearing = "levitate:mechanical_bearing";
inline constexpr std::string_view kWindmillBearing   = "levitate:windmill_bearing";
inline constexpr std::string_view kClockworkBearing  = "levitate:clockwork_bearing";
inline constexpr std::string_view kTurntable         = "levitate:turntable";
inline constexpr std::string_view kCartAssembler     = "levitate:cart_assembler";
inline constexpr std::string_view kControllerRail    = "levitate:controller_rail";
inline constexpr std::string_view kSailFrame         = "levitate:sail_frame";
inline constexpr std::string_view kWhiteSail         = "levitate:white_sail";
inline constexpr std::string_view kMinecartCoupling  = "levitate:minecart_coupling";
inline constexpr std::string_view kMinecartContraption      = "levitate:minecart_contraption";
inline constexpr std::string_view kChestMinecartContraption = "levitate:chest_minecart_contraption";
inline constexpr std::string_view kWrench            = "levitate:wrench"; // U8's item, by id

/// `config/bearings.json` (Create's CKinetics keys).
struct Config {
    int  version                   = 1;
    int  windmillSailsPerRPM       = 8;    ///< sail-like blocks per RPM of a windmill
    int  minimumWindmillSails      = 8;    ///< sail-like blocks a windmill needs to assemble
    int  maxCartCouplingLength     = 32;   ///< longest minecart coupling (blocks)
    bool survivalContraptionPickup = true; ///< survival players may pick up minecart contraptions with a wrench
    /// Whether minecart contraptions can be placed into container items (Create's
    /// canFitInsideContainerItems: toolboxes and packages here).
    bool minecartContraptionInContainers = false;
};
/// Create's MinecartContraptionItem.canFitInsideContainerItems: false for a minecart contraption
/// item unless `minecartContraptionInContainers`; true for every other item. Other units' container
/// items (toolbox, package) ask this.
[[nodiscard]] bool fitsInsideContainerItems(std::string_view itemId);

/// The blocks this unit ticks.
enum class Kind : int { Mechanical, Windmill, Clockwork, Turntable, CartAssembler, ControllerRail };
[[nodiscard]] std::optional<Kind> kindOf(std::string_view id);

// ---- sails --------------------------------------------------------------------------------------
/// The 16 dye colours in Create's (vanilla) order; white first.
[[nodiscard]] std::vector<std::string> const& dyeColours();
/// "levitate:<colour>_sail".
[[nodiscard]] std::string sailIdOf(std::string_view colour);
[[nodiscard]] bool isSail(std::string_view id);      ///< sails and the sail frame
[[nodiscard]] bool isSailFrame(std::string_view id);
/// Create's #create:windmill_sails: sails, the sail frame and wool.
[[nodiscard]] bool isWindmillSail(std::string_view id);
/// The dye colour of a dye item ("minecraft:red_dye", "minecraft:ink_sac" ...), nullopt otherwise.
[[nodiscard]] std::optional<std::string> dyeColourOf(std::string_view itemId);

// ---- shared helpers (Common.cpp) -------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] BlockPos     bp(GP const& p);
[[nodiscard]] BlockPos     bp(cc::Pos const& p);
[[nodiscard]] GP           gp(int dim, cc::Pos const& p);
[[nodiscard]] cc::Pos      ccp(GP const& p);
[[nodiscard]] bool         loaded(BlockSource& region, GP const& p);
[[nodiscard]] std::string  idAt(BlockSource& region, GP const& p);
[[nodiscard]] int          stateAt(BlockSource& region, GP const& p, std::string_view state, int fallback = 0);
/// Sets integer states of the block at `p` (keeps the block type). False if unchanged / unknown.
bool setStates(BlockSource& region, GP const& p, std::vector<std::pair<std::string_view, int>> const& states);
/// Places `id` with states at `p` (neighbour + client updates).
bool placeBlock(BlockSource& region, GP const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {});
/// `levitate:facing` (0 down .. 5 east) of a block.
[[nodiscard]] cc::Dir facingOf(Block const& block);
/// Strongest redstone signal (0..15) reaching `p` from its neighbours (state-based, see SPEC §8).
[[nodiscard]] int redstoneSignal(BlockSource& region, cc::Pos const& p);

[[nodiscard]] Player* playerById(std::int64_t uniqueId);
[[nodiscard]] Actor*  actorById(std::int64_t uniqueId);
[[nodiscard]] std::int64_t uidOf(Actor const& actor);
void actionBar(Player& player, std::string const& text);
void playSound(int dim, cc::Vec3d const& at, std::string const& sound, float volume = 1.f, float pitch = 1.f);
/// Create's CONTRAPTION_ASSEMBLE / CONTRAPTION_DISASSEMBLE (vanilla sounds they are made of).
void assembleSound(int dim, cc::Vec3d const& at);
void disassembleSound(int dim, cc::Vec3d const& at);
/// Gives a stack to a player (drops what doesn't fit).
void give(Player& player, ItemStack const& stack);
/// Uses one item of the selected stack (not in creative).
void consumeSelected(Player& player);
/// Runs `fn` at the start of the next unit tick (world edits out of hooks and events).
void defer(std::function<void()> fn);
[[nodiscard]] std::uint64_t currentTick();

// ---- index of the unit's blocks (Module.cpp) --------------------------------------------------
void track(GP const& pos);
[[nodiscard]] std::vector<GP> tracked();

// ---- bearings (Bearing.cpp) -------------------------------------------------------------------
/// Right-click with an empty hand on a bearing (Create's BearingBlock#useItemOn).
void bearingUse(Player& player, GP const& pos);
void tickBearing(BlockSource& region, GP const& pos, Kind kind);
/// The bearing block at `pos` is gone: disassemble what it held.
void bearingRemoved(GP const& pos);
/// Places a bearing facing like Create's DirectionalKineticBlock#getStateForPlacement.
void orientBearing(BlockSource& region, GP const& pos, Player& player);
/// Bearing state (BlockData "bearings") for selftests and goggles.
[[nodiscard]] Json bearingState(GP const& pos);
void forgetBearings();
void registerBearingBoxes();

// ---- sails (Sails.cpp) --------------------------------------------------------------------------
/// Item use on a sail / sail frame (placement helper, dye, shears). True if handled.
bool sailUse(Player& player, GP const& pos, int face, cc::Vec3d const& hit, ItemStack const& held);
/// Applies a dye (nullopt: shears -> sail frame) as Create's SailBlock#applyDye.
void applyDye(BlockSource& region, GP const& pos, cc::Vec3d const& hit, std::optional<std::string> const& colour);
/// Sets the facing of a freshly placed sail (Create: the face clicked; sneaking: looking direction).
void orientSail(BlockSource& region, GP const& pos, Player& player, int clickedFace);

// ---- turntable (Turntable.cpp) -------------------------------------------------------------------
void tickTurntable(BlockSource& region, GP const& pos);

// ---- carts (Carts.cpp) ----------------------------------------------------------------------------
void tickRail(BlockSource& region, GP const& pos, Kind kind);
void tickCarts();
/// The assembler / controller rail at `pos` is gone.
void railRemoved(GP const& pos, Kind kind);
/// A creative player broke the block at `pos` (no rail drop).
void markCreativeBreak(GP const& pos);
/// Cart assembler item used on a block (Create's CartAssemblerBlockItem#useOn). True if handled.
bool placeAssembler(Player& player, GP const& clicked, bool consume = true);
/// Orients a freshly placed controller rail (shape and direction from the player's facing).
void orientControllerRail(BlockSource& region, GP const& pos, Player& player);
/// Player interacts with an actor (minecart coupling, wrench pickup). True if handled.
bool cartInteract(Player& player, Actor& target);
/// Minecart contraption item used on a rail. True if handled.
bool placeMinecartContraption(Player& player, GP const& rail);
/// Minecart item used on one of our rails (vanilla refuses: they aren't vanilla rails).
bool placeMinecartOn(Player& player, GP const& pos, std::string const& itemId);
void forgetCarts();
void loadCarts();
void saveCarts();
void registerCartBoxes();
/// Contraption riding a minecart (selftests).
[[nodiscard]] std::optional<std::uint64_t> contraptionOfCart(std::int64_t cartUid);
[[nodiscard]] std::optional<double> couplingLength(std::int64_t a, std::int64_t b);
/// Couples two carts (Create's CouplingHandler#tryToCoupleCarts); returns the error key or "".
std::string coupleCarts(Player* player, std::int64_t a, std::int64_t b);
/// Picks up the contraption on `cart` as an item for `player` (Create's wrench pickup).
bool pickupContraption(Player& player, Actor& cart);
/// Spawns a minecart entity (selftests, minecart contraption items).
Actor* spawnCart(BlockSource& region, cc::Vec3d const& at, std::string const& entityId = "minecraft:minecart");
/// How high a cart rides above its rail block (calibrated from carts on vanilla rails).
[[nodiscard]] double cartRailY();

} // namespace lv::feature::bearings
