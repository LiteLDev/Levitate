#pragma once

// Internals of the pistons unit (U20): mechanical pistons, rope pulleys, elevator pulleys and
// gantries (Create's linear actuators). Not a contract for other units; see docs/SPEC.md §3.6.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/contraption/Geometry.h"
#include "core/pistons/Linear.h"

class Block;
class BlockPos;
class BlockSource;
class Player;

namespace lv::feature::pistons {

namespace cc = core::contraption;
namespace cp = core::pistons;
using core::GlobalPos;
using Json = nlohmann::json;

// ---- ids --------------------------------------------------------------------------------------
inline constexpr std::string_view kPiston          = "levitate:mechanical_piston";
inline constexpr std::string_view kStickyPiston    = "levitate:sticky_mechanical_piston";
inline constexpr std::string_view kPole            = "levitate:piston_extension_pole";
inline constexpr std::string_view kHead            = "levitate:mechanical_piston_head";
inline constexpr std::string_view kRopePulley      = "levitate:rope_pulley";
inline constexpr std::string_view kRope            = "levitate:rope";
inline constexpr std::string_view kMagnet          = "levitate:pulley_magnet";
inline constexpr std::string_view kElevatorPulley  = "levitate:elevator_pulley";
inline constexpr std::string_view kElevatorContact = "levitate:elevator_contact";
inline constexpr std::string_view kGantryCarriage  = "levitate:gantry_carriage";
inline constexpr std::string_view kGantryShaft     = "levitate:gantry_shaft";
/// Owned by U23 (redstone): referenced by id only.
inline constexpr std::string_view kRedstoneContact = "levitate:redstone_contact";
/// Owned by U8 (goggles / wrench): referenced by id only.
inline constexpr std::string_view kWrench = "levitate:wrench";

// ---- states -----------------------------------------------------------------------------------
inline constexpr char kFacing[]    = "levitate:facing";      ///< 0..5 (down, up, north, south, west, east)
inline constexpr char kAxisFirst[] = "levitate:axis_first";  ///< Create's AXIS_ALONG_FIRST_COORDINATE
inline constexpr char kState[]     = "levitate:state";       ///< piston: 0 retracted, 1 moving, 2 extended
inline constexpr char kAxis[]      = "levitate:axis";        ///< rope pulley: 0 x, 2 z
inline constexpr char kSticky[]    = "levitate:sticky";      ///< piston head
inline constexpr char kRunning[]   = "levitate:running";     ///< pulleys: running (unmovable)
inline constexpr char kPart[]      = "levitate:part";        ///< gantry shaft: start, middle, end, single
inline constexpr char kPowered[]   = "levitate:powered";
inline constexpr char kCalling[]   = "levitate:calling";     ///< elevator contact
inline constexpr char kPowering[]  = "levitate:powering";    ///< elevator contact: emits redstone
inline constexpr char kRotor[]     = "levitate:rotor";       ///< client mod chunk model (kinetics)

enum class PistonState : int { Retracted = 0, Moving = 1, Extended = 2 };

/// `config/pistons.json` (Create's CKinetics keys).
struct Config {
    int version        = 1;
    int maxPistonPoles = 64;  ///< Create: maxPistonPoles
    int maxRopeLength  = 384; ///< Create: maxRopeLength
};

[[nodiscard]] bool isPiston(std::string_view id);
[[nodiscard]] bool isPulley(std::string_view id); ///< rope or elevator pulley
[[nodiscard]] bool isRopeLike(std::string_view id); ///< rope or magnet

// ---- world helpers (World.cpp) ------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] BlockPos     bp(GlobalPos const& p);
[[nodiscard]] cc::Pos      cpos(GlobalPos const& p);
[[nodiscard]] GlobalPos    gpos(int dim, cc::Pos const& p);
[[nodiscard]] GlobalPos    rel(GlobalPos p, cc::Dir d, int n = 1);
[[nodiscard]] bool         loaded(GlobalPos const& p);
/// Block id at `p` ("" when not loaded).
[[nodiscard]] std::string idAt(GlobalPos const& p);
[[nodiscard]] Block const* blockAt(GlobalPos const& p);
[[nodiscard]] std::optional<int> stateAt(GlobalPos const& p, char const* name);
[[nodiscard]] int stateOr(GlobalPos const& p, char const* name, int fallback);
[[nodiscard]] cc::Dir facingAt(GlobalPos const& p);
/// The block with integer states changed (nullptr if a state is missing).
[[nodiscard]] Block const* withStates(Block const& block, std::vector<std::pair<std::string_view, int>> const& states);
/// Changes states of the block at `p` (flags: 3 = neighbours + clients, 2 = clients only).
bool setStates(GlobalPos const& p, std::vector<std::pair<std::string_view, int>> const& states, int flags = 3);
/// Places `id` with states.
bool place(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {}, int flags = 3);
bool placeBlock(GlobalPos const& p, Block const& block, int flags = 3);
bool clearAt(GlobalPos const& p, int flags = 3);
/// Breaks the block like the game (particles; drops when `drop`).
void destroyAt(GlobalPos const& p, bool drop);
/// Destroy time (-1: unbreakable).
[[nodiscard]] float destroySpeedAt(GlobalPos const& p);
/// Air or replaceable.
[[nodiscard]] bool replaceableAt(GlobalPos const& p);
void sound(GlobalPos const& p, std::string const& name, float volume = 1.f, float pitch = 1.f);
[[nodiscard]] Player* playerById(std::int64_t uid);

/// Our own block edits (assembly swaps, ropes, states): removal detection ignores them.
struct EditGuard {
    EditGuard();
    ~EditGuard();
    EditGuard(EditGuard const&)            = delete;
    EditGuard& operator=(EditGuard const&) = delete;
};
[[nodiscard]] bool editing();

/// The unit's part of a block's BlockData (`data["pistons"]`).
[[nodiscard]] Json&       data(GlobalPos const& p);
[[nodiscard]] Json const* dataIf(GlobalPos const& p);
void                      eraseData(GlobalPos const& p);

/// Runs `fn` at the start of the next tick (world edits out of events).
void defer(std::function<void()> fn);
[[nodiscard]] std::uint64_t currentTick();

/// True while a piston collects its structure: poles and heads then carry nothing on any face
/// (Create: they are anchoring blocks of the piston contraption).
[[nodiscard]] bool& assemblingPiston();

// ---- actuators (Actuators.cpp) ------------------------------------------------------------------
enum class Kind { Piston, RopePulley, ElevatorPulley };
[[nodiscard]] std::optional<Kind> actuatorKind(std::string_view id);
/// Empty-hand right-click on a piston / rope pulley (Create: assembleNextTick).
void requestAssemble(GlobalPos const& p);
/// Elevator pulley right-click (Create's ElevatorPulleyBlockEntity.clicked).
void elevatorClicked(GlobalPos const& p);
/// The actuator at `p` is gone (`doc` its last data): Create's remove() -> disassemble().
void actuatorRemoved(GlobalPos const& p, Kind kind, Json const& doc, std::string const& previousId);
/// Movement mode value box.
[[nodiscard]] cp::MovementMode movementMode(GlobalPos const& p);
void                           setMovementMode(GlobalPos const& p, cp::MovementMode mode);
/// The actuator's current offset / running flag / contraption (selftests, goggles).
[[nodiscard]] float              offsetOf(GlobalPos const& p);
[[nodiscard]] bool               runningAt(GlobalPos const& p);
[[nodiscard]] std::uint64_t      contraptionAt(GlobalPos const& p);
[[nodiscard]] std::string        lastErrorAt(GlobalPos const& p);
void                             trackActuator(GlobalPos const& p);
void                             tickActuators();
void                             resetActuators();
void                             loadActuators();
void                             onActuatorSpeed(GlobalPos const& p, float prevSpeed, float speed);

/// Player breaks (Create's playerWillDestroy) of the piston chain: run before the block goes.
void pistonChainBroken(GlobalPos const& p, std::string const& id, cc::Dir facing, bool drops);
/// Destroys the poles / head in front of and behind a piston (Create's
/// MechanicalPistonBlock.playerWillDestroy).
void destroyPistonChain(GlobalPos const& piston, cc::Dir facing, bool drops);
/// A rope / magnet / rope pulley went: the rope column breaks (Create's RopeBlockBase.onRemove).
void ropeBroken(GlobalPos const& p, bool pulleyRemoved);

// ---- gantries (Gantry.cpp) --------------------------------------------------------------------
void trackShaft(GlobalPos const& p);
void checkCarriage(GlobalPos const& carriage);
void tickGantries();
void resetGantries();
void loadGantries();
void onShaftSpeed(GlobalPos const& p);
/// Placement state of a new gantry shaft (Create's GantryShaftBlock.getStateForPlacement).
void orientShaft(GlobalPos const& p, Player& player, cc::Dir clickedFace);
void orientCarriage(GlobalPos const& p, cc::Dir clickedFace);
[[nodiscard]] std::vector<std::uint64_t> gantryContraptions();

// ---- elevators (Elevator.cpp) -----------------------------------------------------------------
struct ColumnCoords {
    int     dim = 0, x = 0, z = 0;
    cc::Dir side = cc::Dir::North;
    friend auto operator<=>(ColumnCoords const&, ColumnCoords const&) = default;
};
/// The column a contact belongs to (its x/z and facing), nullopt for other blocks.
[[nodiscard]] std::optional<ColumnCoords> columnOf(GlobalPos const& contact);
/// Elevator contacts (y levels, sorted) of a column, read from the world.
[[nodiscard]] std::vector<int> columnContacts(ColumnCoords const& c);
/// Converts the redstone contacts of the column to elevator contacts (Create's gatherAll).
void gatherColumn(ColumnCoords const& c);
void columnTarget(ColumnCoords const& c, int y);
[[nodiscard]] std::optional<int> columnTargetOf(ColumnCoords const& c);
void columnSetActive(ColumnCoords const& c, bool active);
[[nodiscard]] bool columnActive(ColumnCoords const& c);
/// Short names of every floor (Create's compileNamesList): {y, short, long}.
[[nodiscard]] std::vector<cp::Floor> columnFloors(ColumnCoords const& c);
[[nodiscard]] int columnNamesVersion(ColumnCoords const& c);
/// The elevator reached the floor of contact y (Create's floorReached).
void floorReached(ColumnCoords const& c, int contactY);
/// The elevator stands at contact y: Create's triggerContact (the contact emits a pulse).
void triggerContact(ColumnCoords const& c, int contactY);
/// Calls the elevator to the contact at `p` (redstone or controls): Create's callToContactAndUpdate.
void callToContact(GlobalPos const& p, bool powered);
/// Opens the contact's settings (floor identifier, description, door control).
void openContactScreen(Player& player, GlobalPos const& p);
/// Applies new names (selftests and the form).
void setContactNames(GlobalPos const& p, std::string shortName, std::string longName, int doorMode);
void trackContact(GlobalPos const& p);
void tickElevators();
void resetElevators();
void loadElevators();

// ---- rope sync (Sync.cpp) ---------------------------------------------------------------------
/// Tells companions about a pulley's rope (offset, speed, magnet) for the client renderer.
void syncRope(GlobalPos const& pulley, float offset, float speed, bool running, bool elevator);
void syncRopeRemoved(GlobalPos const& pulley);
void tickRopeSync();
void resetRopeSync();

// ---- placement helpers (Blocks.cpp) -------------------------------------------------------------
/// The direction the player looks along (Create's getNearestLookingDirection).
[[nodiscard]] cc::Dir nearestLooking(Player const& player);
/// The player's horizontal facing (Create's getHorizontalDirection).
[[nodiscard]] cc::Dir horizontalLooking(Player const& player);
/// The kinetic block at `q` has a shaft end on `face` (Create's IRotate.hasShaftTowards).
[[nodiscard]] bool shaftTowards(GlobalPos const& q, cc::Dir face);
/// Create's getPreferredFacing: the one side (axis) with a shaft facing `p` (nullopt: none or
/// several axes).
[[nodiscard]] std::optional<cc::Dir> preferredSide(GlobalPos const& p, bool horizontalOnly);

// ---- misc -------------------------------------------------------------------------------------
/// Movement mode labels (value box options).
[[nodiscard]] std::vector<std::string> movementModeNames(Player const* viewer);

} // namespace lv::feature::pistons
