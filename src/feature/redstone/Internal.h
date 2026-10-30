#pragma once

// Internals of the redstone unit (U23). Not a contract: other units use Redstone.h /
// RedstoneLink.h.
//
// Every block of the unit is a data-driven block whose redstone *output* is native: its
// permutations carry `minecraft:redstone_producer` with the power of that state, so vanilla
// wires, repeaters, lamps, ... read it through the game's own circuit system. *Inputs* are
// read from the game's circuit components around the block (signalFrom) once per tick, which
// also drives Create's block entity logic (diode counters, latch ticks, link transmission).

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/common/ValueSettings.h"
#include "core/redstone/Diodes.h"
#include "core/redstone/LinkNetwork.h"

class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;
class Vec3;

namespace lv::feature::redstone {

using Json = nlohmann::json;
using GP   = core::GlobalPos;
namespace cr = core::redstone;

// ---- ids ---------------------------------------------------------------------------------------
inline constexpr char kLink[]            = "levitate:redstone_link";
inline constexpr char kLever[]           = "levitate:analog_lever";
inline constexpr char kPulseRepeater[]   = "levitate:pulse_repeater";
inline constexpr char kPulseExtender[]   = "levitate:pulse_extender";
inline constexpr char kPulseTimer[]      = "levitate:pulse_timer";
inline constexpr char kPoweredLatch[]    = "levitate:powered_latch";
inline constexpr char kToggleLatch[]     = "levitate:powered_toggle_latch";
inline constexpr char kSmartObserver[]   = "levitate:content_observer"; ///< Create registers the smart observer as content_observer
inline constexpr char kThresholdSwitch[] = "levitate:stockpile_switch"; ///< and the threshold switch as stockpile_switch
inline constexpr char kContact[]         = "levitate:redstone_contact";
inline constexpr char kElevatorContact[] = "levitate:elevator_contact"; ///< U20 (pairs with contacts)
inline constexpr char kWrench[]          = "levitate:wrench";
inline constexpr char kLinkedController[] = "levitate:linked_controller";
inline constexpr char kGoggles[]         = "levitate:goggles";
inline constexpr char kDeskBell[]        = "levitate:desk_bell";        ///< U49
inline constexpr char kRoseQuartzLamp[]  = "levitate:rose_quartz_lamp"; ///< U49

enum class Kind : std::uint8_t {
    Link,
    Lever,
    PulseRepeater,
    PulseExtender,
    PulseTimer,
    PoweredLatch,
    ToggleLatch,
    SmartObserver,
    ThresholdSwitch,
    Contact,
    DeskBell,       ///< U49
    RoseQuartzLamp, ///< U49
};
/// The last kind (saved kinds are clamped to it).
inline constexpr Kind kLastKind = Kind::RoseQuartzLamp;
[[nodiscard]] std::optional<Kind> kindOf(std::string_view blockId);
[[nodiscard]] char const*         idOf(Kind kind);
[[nodiscard]] constexpr bool      isBrassDiode(Kind k) { return k == Kind::PulseRepeater || k == Kind::PulseExtender || k == Kind::PulseTimer; }
[[nodiscard]] constexpr bool      isLatch(Kind k) { return k == Kind::PoweredLatch || k == Kind::ToggleLatch; }
[[nodiscard]] constexpr bool      isDiodeShaped(Kind k) { return isBrassDiode(k) || isLatch(k); }
[[nodiscard]] cr::DiodeKind       diodeKind(Kind k);

// ---- states (all integer block states) ---------------------------------------------------------
inline constexpr char sFacing[]      = "levitate:facing";       ///< FacingID 0..5
inline constexpr char sReceiver[]    = "levitate:receiver";     ///< link
inline constexpr char sPowered[]     = "levitate:powered";      ///< input (diodes) / output (others)
inline constexpr char sPower[]       = "levitate:power";        ///< analog output 0..15
inline constexpr char sFace[]        = "levitate:face";         ///< lever: 0 floor, 1 wall, 2 ceiling
inline constexpr char sLeverState[]  = "levitate:state";        ///< lever handle 0..15
inline constexpr char sPowering[]    = "levitate:powering";     ///< diode / latch output
inline constexpr char sInverted[]    = "levitate:inverted";     ///< brass diodes
inline constexpr char sPoweredSide[] = "levitate:powered_side"; ///< powered latch
inline constexpr char sTarget[]      = "levitate:target";       ///< observer / switch: 0 wall, 1 floor, 2 ceiling
inline constexpr char sLevel[]       = "levitate:level";        ///< threshold switch 0..5
inline constexpr char sActivate[]    = "levitate:activate";     ///< rose quartz lamp (ACTIVATE)
inline constexpr char sRotor[]       = "levitate:rotor";        ///< desk bell: 1 = without bell and plunger (client mod)

// ---- directions (Bedrock FacingID) -------------------------------------------------------------
enum Dir : int { kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5 };
[[nodiscard]] int                opposite(int d);
[[nodiscard]] std::array<int, 3> offsetOf(int d);
[[nodiscard]] BlockPos           step(BlockPos const& p, int d);
[[nodiscard]] GP                 step(GP const& p, int d);
/// Horizontal rotation as seen from above (north -> east -> south -> west).
[[nodiscard]] int  clockwise(int d);
[[nodiscard]] int  counterClockwise(int d);
[[nodiscard]] bool isHorizontal(int d);
[[nodiscard]] char const* dirName(int d);
/// The direction closest to a (pitch, yaw) look (Bedrock rotation, degrees).
[[nodiscard]] int nearestLooking(float pitch, float yaw);
/// The horizontal direction a player looks along.
[[nodiscard]] int horizontalLooking(float yaw);

// ---- blocks ------------------------------------------------------------------------------------
[[nodiscard]] int          state(Block const& block, std::string_view name, int fallback = 0);
[[nodiscard]] Block const* withStates(Block const& block, std::initializer_list<std::pair<std::string_view, int>> changes);
/// Changes integer states of the block at `pos` (no-op when unchanged). Returns true if set.
bool setStates(BlockSource& region, BlockPos const& pos, std::initializer_list<std::pair<std::string_view, int>> changes);
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] bool         loadedAt(BlockSource& region, BlockPos const& pos);
/// A vanilla block from its id and states (string, int or bool values), nullptr if unknown.
[[nodiscard]] Block const* vanillaBlock(std::string const& id, Json const& states);
/// A block's string state ("" when absent).
[[nodiscard]] std::string strState(Block const& block, std::string const& name);

// ---- config (config/redstone.json, Create's CLogistics) ----------------------------------------
struct RedstoneConfig {
    int version   = 1;
    int linkRange = 256; ///< Create's logistics.linkRange: maximum range of link connections (blocks)
};

/// Create's RedstoneLinkFrequencySlot scale.
inline constexpr double kFrequencySlotScale = .4975;

/// Game ticks since the mod started (one per server level tick).
std::uint64_t& gameTickCounter();

// ---- signals -----------------------------------------------------------------------------------
/// Like signalFrom, but wires and powered blocks count whatever their direction (Java's
/// `getSignal(neighbour, UP)` used by the redstone link).
[[nodiscard]] int undirectedSignal(BlockSource& region, BlockPos const& pos, int face);
/// The circuit component at `pos` as text (selftests).
[[nodiscard]] std::string circuitDebug(BlockSource& region, BlockPos const& pos);
/// The strength one of our blocks emits into its neighbour on side `d` (nullopt: not ours).
[[nodiscard]] std::optional<int> ourEmission(Block const& block, int d);
/// Which faces the producer of a block powers (for the selftests / docs), bit per FacingID.
[[nodiscard]] int targetDirection(Block const& block); ///< observer / switch target side

// ---- block data --------------------------------------------------------------------------------
/// `data["redstone"]` of the block at `pos` (nullptr when none).
[[nodiscard]] Json const* dataOf(GP const& pos);
/// `data["redstone"]` for writing (created as {}).
[[nodiscard]] Json& dataFor(GP const& pos);

// ---- effects -----------------------------------------------------------------------------------
void playSound(int dim, double x, double y, double z, std::string const& name, float volume, float pitch);
void spawnParticle(int dim, double x, double y, double z, std::string const& effect);
void actionBar(Player& player, std::string const& text);
void giveOrDrop(Player& player, ItemStack const& stack);

// ---- tracked blocks ----------------------------------------------------------------------------

/// Runtime state of a loaded block of the unit (Create's block entity fields that need no
/// saving; the rest is in BlockData).
struct Runtime {
    Kind                     kind = Kind::Link;
    cr::ScheduledTick        sched;          ///< vanilla diode block tick
    std::optional<cr::LinkId> link;          ///< link network node while loaded
    int                      received   = 0; ///< link: last network power
    int                      blockTick  = -1; ///< generic scheduled block tick (observer off, switch power, contact)
    int                      lazy       = 0; ///< threshold switch: ticks to the next lazy tick
    bool                     neighbour  = true; ///< a neighbour changed (checked next tick)
    bool                     loaded     = false;
    bool                     contraptionContact = false; ///< contact powered by a passing contraption
    std::uint64_t            scheduledAt = 0; ///< U49 (bell, lamp): game tick of a pending block tick, 0 none
    /// Create's per-tick counter (diode state, lever lastChange, observer turnOff), kept in
    /// memory and written to BlockData on world save.
    int  counter       = 0;
    bool counterLoaded = false;
    /// Inputs read for this tick before any block changes (so the order blocks tick in doesn't
    /// matter): back / side of diodes and latches, a transmitter's strength.
    bool inBack = false;
    bool inSide = false;
    int  inLink = 0;
};

/// The block's counter (loaded from BlockData on first use).
int& counterOf(GP const& pos, Runtime& rt);
/// Writes the counter to BlockData when it changed.
void persistCounter(GP const& pos, Runtime const& rt);
/// Reads the block's redstone inputs for this tick (before any block of the unit ticks).
void readInputs(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
/// A transmitter's input strength (Create's RedstoneLinkBlock.getPower).
[[nodiscard]] int linkInput(BlockSource& region, BlockPos const& pos, int facing);
/// Forgets cached circuit lookups (call when components may have changed).
void invalidateCircuitCache();

std::map<GP, Runtime>& tracked();
Runtime*               runtimeAt(GP const& pos);
void                   track(GP const& pos, Kind kind);
void                   untrack(GP const& pos);
/// Marks the block (if tracked) to re-check its surroundings on the next tick.
void                   touch(GP const& pos);

// ---- behaviour (Components.cpp / Links.cpp / Detectors.cpp) ------------------------------------

struct Placement {
    int   clickedFace = kUp;
    float pitch       = 0;
    float yaw         = 0;
    bool  sneaking    = false;
};
/// The state a block placed by a player takes (Create's getStateForPlacement).
[[nodiscard]] Block const* placementState(BlockSource& region, BlockPos const& pos, Block const& placed, Kind kind, Placement const& p);
/// Whether the block can stay (support behind / below).
[[nodiscard]] bool canSurvive(BlockSource& region, BlockPos const& pos, Block const& block, Kind kind);

/// A block of the unit appeared (placed, generated, loaded for the first time): default data.
void onAdded(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
/// The block is gone (broken, replaced): leave networks.
void onRemoved(GP const& pos, Runtime& rt);
/// The chunk was unloaded.
void onUnloaded(GP const& pos, Runtime& rt);
/// One game tick of a loaded block.
void tickBlock(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);

/// A player's right-click on one of our blocks. `hit` is block local. Returns true when used
/// (the click does nothing else).
struct Click {
    Player&           player;
    GP                pos;
    int               face = kUp;
    core::common::Vec hit;
    ItemStack const&  held;
    bool              sneaking = false;
};
bool use(Click const& click, Block const& block, Kind kind);

// ---- link block ---------------------------------------------------------------------------------
/// The frequency slot hit at block-local `hit` (true = first), nullopt if none.
[[nodiscard]] std::optional<bool> frequencySlotAt(Block const& block, core::common::Vec const& hit);
/// The slot's centre (block local) for the link's facing.
[[nodiscard]] core::common::Vec frequencySlotCentre(int facing, bool first);
/// Sets a frequency slot (Create's LinkBehaviour.setFrequency): a copy of `stack`, count 1.
void setFrequency(GP const& pos, bool first, ItemStack const& stack);
/// The two frequency stacks of a link.
[[nodiscard]] std::pair<ItemStack, ItemStack> frequencies(GP const& pos);
/// Toggles transmitter / receiver (Create's toggleMode).
void toggleLinkMode(BlockSource& region, GP const& pos);
/// Link node registration while loaded.
void linkLoaded(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
void linkUnloaded(GP const& pos, Runtime& rt);
void tickLink(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
/// Drops every node (world closed).
void resetLinks();
/// The link network (selftests).
cr::LinkNetwork& linkNetwork();

// ---- analog lever ------------------------------------------------------------------------------
void leverUse(BlockSource& region, GP const& pos, Block const& block, bool back);

// ---- diodes ------------------------------------------------------------------------------------
[[nodiscard]] int  diodeDelay(GP const& pos, Kind kind);
void               setDiodeDelay(GP const& pos, int ticks);
void               registerValueBoxes();

// ---- detectors ---------------------------------------------------------------------------------
void registerFilterSlots();
void registerThresholdScreen();
void openThresholdScreen(Player& player, GP const& pos);
/// Threshold switch settings (Create's ConfigureThresholdSwitchPacket), validated.
void configureThreshold(GP const& pos, int offBelow, int onAbove, bool inverted, bool inStacks);
/// Measures the switch's target now (Create's updateCurrentLevel).
void updateThresholdLevel(BlockSource& region, GP const& pos, Block const& block);
void tickSmartObserver(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
void tickThresholdSwitch(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
/// Smart observer pulse (Create's activate(ticks)).
void activateObserver(BlockSource& region, GP const& pos, int ticks);

// ---- contacts ----------------------------------------------------------------------------------
[[nodiscard]] bool hasValidContact(BlockSource& region, BlockPos const& pos, int facing);
void               registerContactActor();

// ---- desk bell and rose quartz lamp (BellLamp.cpp, U49) ------------------------------------------
/// DeskBellBlock.useWithoutItem: rings (sound, the client mod's animation) and powers for 20 ticks.
void ringDeskBell(BlockSource& region, GP const& pos, Player* player);
void tickDeskBell(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
/// RoseQuartzLampBlock: neighbour changes and the scheduled activation.
void tickRoseQuartzLamp(BlockSource& region, GP const& pos, Block const& block, Runtime& rt);
/// What the lamp at t sends into its neighbour on side 	owards (RoseQuartzLampBlock.getSignal).
[[nodiscard]] int lampSignal(BlockSource& region, BlockPos const& at, Block const& block, int towards);
/// The input of a lamp (Level.hasNeighborSignal, without lamps feeding back through powered blocks).
[[nodiscard]] int lampInput(BlockSource& region, BlockPos const& pos);
/// What a comparator reads from the lamp at pos (distance to the lit lamp of its column).
[[nodiscard]] std::optional<int> lampComparatorOutput(BlockSource& region, BlockPos const& pos);
/// Registers the emitters, the comparator output, the wrench handlers and the bell's contraption
/// behaviour.
void registerBellAndLamp();
/// Desk bell rings sent so far (selftests).
[[nodiscard]] int deskBellRings();

// ---- hover text (client mod overlay, vanilla action bar) ---------------------------------------
[[nodiscard]] Json hoverLines(Player& player, GP const& pos, int face, core::common::Vec const& hit);

} // namespace lv::feature::redstone
