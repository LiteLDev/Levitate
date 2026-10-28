#pragma once

// Generators unit (U7): hand crank, copper / dyed valve handles, water wheel, large water wheel
// (+ its structure blocks) and the creative motor. Internal header of the unit (not a contract):
// the rules live in core/generators/Generators.h, the kinetic network in feature/kinetics.
//
// Per-position data: value box settings in BlockData (`data["generators"]`), the turning state
// of cranks / valves and the flow score of wheels in memory (cranks that were turning when the
// world was saved are stopped when it loads again), the positions of the wheels in the world DB
// (`lv:generators:wheels`) so they are re-checked every 60 ticks as Create's lazy tick does.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/GlobalPos.h"
#include "core/generators/Generators.h"

class Block;
class BlockSource;
class ItemStack;
class Player;

namespace lv::feature::generators {

namespace cg = core::generators;
using core::GlobalPos;
using cg::Axis;
using cg::Face;

inline constexpr char kHandCrank[]   = "levitate:hand_crank";
inline constexpr char kCopperValve[] = "levitate:copper_valve_handle";
inline constexpr char kWaterWheel[]  = "levitate:water_wheel";
inline constexpr char kLargeWheel[]  = "levitate:large_water_wheel";
inline constexpr char kStructure[]   = "levitate:water_wheel_structure";
inline constexpr char kMotor[]       = "levitate:creative_motor";

inline constexpr char kFacing[]    = "levitate:facing";    ///< 0..5 (down, up, north, south, west, east)
inline constexpr char kAxisState[] = "levitate:axis";      ///< 0..2 (x, y, z)
inline constexpr char kWood[]      = "levitate:wood";      ///< 0..11 (core::generators::kWoods)
inline constexpr char kRotor[]     = "levitate:rotor";     ///< kinetics renderer: 0 full, 1 static parts
inline constexpr char kSegment[]   = "levitate:segment";   ///< structure cell 0..7
inline constexpr char kExtension[] = "levitate:extension"; ///< large wheel behind another one

inline constexpr char kWrench[]      = "levitate:wrench";
inline constexpr char kExtendoGrip[] = "levitate:extendo_grip";

/// `config/generators.json` (Create's CKinetics keys of this unit).
struct Config {
    int   version               = 1;
    /// Hunger used per hand crank use, multiplied by the crank's speed (Create's crankHungerMultiplier).
    float crankHungerMultiplier = cg::kCrankHunger;
};

enum class Kind : std::uint8_t { None, Crank, Valve, WaterWheel, LargeWheel, Structure, Motor };
[[nodiscard]] Kind kindOf(std::string_view id);
[[nodiscard]] Kind kindOf(Block const& block);
/// "levitate:copper_valve_handle" for -1, "levitate:<colour>_valve_handle" for 0..15.
[[nodiscard]] std::string valveId(int colour);
/// -1 copper, 0..15 dyed, -2 not a valve handle.
[[nodiscard]] int valveColour(std::string_view id);

// ---- world access (server thread) -------------------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
/// The block at `pos` when its chunk is loaded.
[[nodiscard]] Block const* blockAt(GlobalPos const& pos);
[[nodiscard]] int          stateOf(Block const& block, char const* state, int fallback = 0);
[[nodiscard]] Face         facingOf(Block const& block);
[[nodiscard]] Axis         axisStateOf(Block const& block);
/// Sets integer states on the block at `pos` (keeps the others). Returns false if not possible.
bool setStates(GlobalPos const& pos, std::initializer_list<std::pair<char const*, int>> states);

// ---- actions --------------------------------------------------------------------------------------------------------
/// Create's HandCrankBlock.use: turns the crank (sneaking: backwards) and costs hunger.
bool useCrank(Player& player, GlobalPos const& pos, bool backwards, bool costHunger);
/// Create's ValveHandleBlock.clicked: a dye of another colour dyes the handle, anything else
/// turns it by its angle (sneaking: the other way). Returns true when the click was used.
bool useValve(Player& player, GlobalPos const& pos, std::string const& heldId, bool sneaking);
/// Create's WaterWheelBlockEntity.applyMaterialIfValid (wheel, large wheel or a structure
/// block of one). Returns true when the wood changed.
bool applyMaterial(GlobalPos const& pos, std::string const& heldId);
/// Turns a freshly placed crank / valve / wheel / motor as Create's getStateForPlacement.
void orientPlaced(Player& player, GlobalPos const& pos, Face clickedFace);
/// Places a large water wheel as Create's LargeWaterWheelBlockItem (the clicked position or one
/// further). Returns false (and tells the player) when there is no room.
bool placeLargeWheel(Player& player, ItemStack& held, GlobalPos const& clickedPlacement, Face clickedFace);
/// Notes that a creative player broke the block at `pos` (structure blocks: no drop).
void noteCreativeBreak(GlobalPos const& pos);

// ---- value settings ---------------------------------------------------------------------------------------------------
[[nodiscard]] int motorSpeed(GlobalPos const& pos);
void              setMotorSpeed(GlobalPos const& pos, int rpm);
[[nodiscard]] int valveAngle(GlobalPos const& pos);
void              setValveAngle(GlobalPos const& pos, int degrees);

// ---- inspection (selftests) ---------------------------------------------------------------------------------------------
[[nodiscard]] int  crankInUse(GlobalPos const& pos);
[[nodiscard]] int  wheelFlowScore(GlobalPos const& pos);
[[nodiscard]] bool tracksWheel(GlobalPos const& pos);
/// Re-reads the flow around a wheel now (Create's determineAndApplyFlowScore).
void evaluateWheel(GlobalPos const& pos);
/// The fluid flow at a position (fluid flow vector, bubble columns up / down), as Create reads it.
[[nodiscard]] cg::Vec3d flowAt(BlockSource& region, GlobalPos const& pos);

namespace detail {
/// Set while a player's click on a valve handle must turn it rather than open its value box.
inline thread_local bool suppressValveBox = false;
void installHooks();
void uninstallHooks();
void registerValueBoxes();
} // namespace detail

} // namespace lv::feature::generators
