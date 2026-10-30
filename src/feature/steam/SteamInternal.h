#pragma once

// Internals of the steam unit (U18): fluid tanks, boilers, steam engines, powered shafts and
// steam whistles. Server thread only.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/steam/Boiler.h"
#include "core/steam/Tank.h"
#include "core/steam/Whistle.h"

class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;

namespace lv::feature::steam {

namespace cs = core::steam;
using core::GlobalPos;
using Json = nlohmann::json;

inline constexpr char kTank[]         = "levitate:fluid_tank";
inline constexpr char kCreativeTank[] = "levitate:creative_fluid_tank";
inline constexpr char kEngine[]       = "levitate:steam_engine";
inline constexpr char kWhistle[]      = "levitate:steam_whistle";
inline constexpr char kExtension[]    = "levitate:steam_whistle_extension";
inline constexpr char kPoweredShaft[] = "levitate:powered_shaft";
inline constexpr char kShaft[]        = "levitate:shaft"; // U6
inline constexpr char kWrench[]       = "levitate:wrench"; // U8
inline constexpr char kDataKey[]      = "steam";

// ---- config ----------------------------------------------------------------------------------------
/// `config/steam.json`: Create's CFluids keys of the unit.
struct SteamConfig {
    int version = 1;
    /// Capacity of one fluid tank block, in buckets (Create's fluidTankCapacity).
    int fluidTankCapacity = 8;
    /// Maximum height of a fluid tank multiblock (Create's fluidTankMaxHeight).
    int fluidTankMaxHeight = 32;
};
[[nodiscard]] cs::TankParams tankParams(bool creative);

// ---- blocks ----------------------------------------------------------------------------------------
[[nodiscard]] bool isTankId(std::string_view id);
[[nodiscard]] bool isCreativeTankId(std::string_view id);
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] std::string  idAt(BlockSource& region, GlobalPos const& pos); ///< "" when not loaded
[[nodiscard]] bool         loaded(BlockSource& region, GlobalPos const& pos);
/// Integer custom state (nullopt when the block has none).
[[nodiscard]] std::optional<int> intState(Block const& block, std::string_view name);
/// String state such as `minecraft:block_face` ("" when absent).
[[nodiscard]] std::string strState(Block const& block, std::string_view name);
/// The block with integer / string states replaced (the same block when nothing applies).
[[nodiscard]] Block const* withStates(Block const& block, std::vector<std::pair<std::string, int>> const& ints,
                                      std::vector<std::pair<std::string, std::string>> const& strings = {});
/// Sets states of the block at `pos` (no-op when unchanged). Returns true when it changed.
bool setStates(BlockSource& region, GlobalPos const& pos, std::vector<std::pair<std::string, int>> const& ints,
               std::vector<std::pair<std::string, std::string>> const& strings = {});
/// Breaks the block as if mined (drops its loot).
void popBlock(BlockSource& region, GlobalPos const& pos);
/// Removes the block without drops.
void removeBlock(BlockSource& region, GlobalPos const& pos);
/// Whether a neighbour powers the block with redstone (polled; SPEC §8).
[[nodiscard]] bool redstonePowered(BlockSource& region, GlobalPos const& pos);

/// The six directions in Create's order.
inline constexpr cs::Dir kDirs[]{cs::Dir::Down, cs::Dir::Up, cs::Dir::North, cs::Dir::South, cs::Dir::West, cs::Dir::East};
[[nodiscard]] cs::Dir    dirFromName(std::string_view name, cs::Dir fallback);
[[nodiscard]] char const* dirName(cs::Dir d);
[[nodiscard]] GlobalPos   offset(GlobalPos p, cs::Dir d, int n = 1);

void playSound(int dim, double x, double y, double z, std::string const& sound, float volume, float pitch);
void particle(int dim, double x, double y, double z, std::string const& effect);
/// Create's SteamJetParticle (`levitate:steam_jet`) at (x, y, z) moving by (mx, my, mz) blocks per
/// tick, sent to the players within 64 blocks.
void steamJet(int dim, double x, double y, double z, double mx, double my, double mz);
void giveOrDrop(Player& player, ItemStack const& stack);

// ---- tanks -----------------------------------------------------------------------------------------
[[nodiscard]] cs::TankPart parsePart(Json const* doc, cs::Pos3 const& self);
[[nodiscard]] Json         partJson(cs::TankPart const& part);
[[nodiscard]] cs::Pos3     pos3(GlobalPos const& p);
[[nodiscard]] GlobalPos    globalOf(int dim, cs::Pos3 const& p);

/// A tank structure as seen from any of its parts.
struct TankInfo {
    GlobalPos    controller;
    std::string  blockId;
    int          width = 1, height = 1;
    std::string  fluid;
    std::int64_t amount   = 0;
    std::int64_t capacity = 0;
    bool         window   = true;
    bool         creative = false;
};
[[nodiscard]] std::optional<TankInfo> tankAt(GlobalPos const& pos);
/// Writes the controller's fluid (amount clamped to the capacity; creative tanks stay full).
void setTankFluid(GlobalPos const& controller, std::string const& fluid, std::int64_t amount);
/// Toggles the windows of the structure at `pos` (Create's FluidTankBlockEntity.toggleWindows).
bool toggleWindows(GlobalPos const& pos);
/// Sets the windows of a structure (and updates its blocks).
void setWindows(GlobalPos const& controller, bool window);
/// Re-applies the block states (top / bottom / window shape / light) of every part of the
/// structure controlled at `controller`.
void refreshStructure(GlobalPos const& controller);

/// Queued world changes of the unit's blocks (BlockChangedEvent), applied once per tick.
void queueChange(GlobalPos const& pos, std::string prevId, std::string newId);
void processChanges();
/// Drops queued changes (the world closed).
void resetPendingChanges();
/// A tank document erased together with its block (BlockData erase listener).
void stashErasedDoc(GlobalPos const& pos, Json const& doc);

/// Fluid exchange with a held item (Create's FluidTankBlock.useItemOn). Returns true when handled.
bool useItemOnTank(Player& player, GlobalPos const& pos, ItemStack const& held);
/// Create's FluidTankItem multi-place: a tank item used on the top / bottom face of a wider tank
/// fills the whole layer. Returns true when it placed the layer.
bool tryMultiPlace(Player& player, GlobalPos const& clicked, int face, ItemStack const& held);

void registerTankCapabilities();

// ---- boilers, engines, whistles ---------------------------------------------------------------------
struct BoilerRuntime {
    cs::Boiler boiler;
    /// Gauge target of the boiler dials (Create's BoilerData.gauge: passive 1/8, else level /
    /// size limit), synced to clients.
    float      gauge = 0;
    /// Sides whose gauge is hidden by a block in front (Create's occludedDirections), bit i =
    /// horizontal direction i in Create's 2D order (south, west, north, east).
    int        occluded = 0;
    bool      evaluated   = false;
    int        sinceEval   = 0;
};
[[nodiscard]] std::map<GlobalPos, BoilerRuntime>& boilers();
/// The boiler of the structure controlled at `controller` (evaluated when needed).
BoilerRuntime* boilerOf(GlobalPos const& controller);
void           markBoilerDirty(GlobalPos const& controller);
/// Water fed into a boiler (fluid capability while it is a boiler).
void           feedBoiler(GlobalPos const& controller, int mb);
[[nodiscard]] bool boilerActive(GlobalPos const& controller);

/// The engine's facing (away from its tank) and, for floor / ceiling engines, its horizontal facing.
struct EngineShape {
    cs::Dir facing     = cs::Dir::North;
    cs::Dir horizontal = cs::Dir::North;
};
[[nodiscard]] EngineShape engineShape(Block const& block);
/// The direction from a whistle to the tank it hangs on (Create's getAttachedDirection).
[[nodiscard]] cs::Dir whistleAttached(Block const& block);

void tickSteam();
void onEngineAdded(GlobalPos const& pos);
void onEngineRemoved(GlobalPos const& pos);
void onWhistleAdded(GlobalPos const& pos);
void onWhistleRemoved(GlobalPos const& pos);
void onExtensionChanged(GlobalPos const& pos, bool added);
void onPoweredShaftRemoved(GlobalPos const& pos);
void onPoweredShaftAdded(GlobalPos const& pos);
/// Whether the whistle at `pos` is sounding (powered on a boiler with heat).
[[nodiscard]] bool whistleSounding(GlobalPos const& pos);
/// Test block of the unit: a heater whose `levitate:heat` state is a Create heat level (blaze burners are U14).
inline constexpr char kTestHeater[] = "levitate:steam_test_heater";
/// Attached engines / whistles of the tank at `removed` lose their support.
void dropAttachmentsOf(GlobalPos const& removedTank);

/// The looping tone of a sounding whistle (Create's WhistleSoundInstance, WhistleSound.cpp): one
/// sound instance per whistle, played on the clients of the players within 64 blocks; called every
/// tick while it sounds (a new `sound` restarts it, a new `pitch` is sent to the playing clients).
void whistleTone(GlobalPos const& whistle, std::string const& sound, float pitch);
/// Create's WHISTLE_CHIFF when a whistle starts: per player at .175 · (64 − distance) / 64.
void whistleChiff(GlobalPos const& whistle, float pitch);
/// The whistle stopped: the tone fades out (4 ticks) and stops.
void whistleQuiet(GlobalPos const& whistle);
/// Range changes (players entering / leaving the 64 blocks) and pending stops, once per tick.
void tickWhistleTones();
void resetWhistleTones();
/// The sound handle of a whistle's tone (0: none) and its listeners (selftests).
[[nodiscard]] std::uint64_t whistleToneHandle(GlobalPos const& whistle);
[[nodiscard]] int           whistleToneListeners(GlobalPos const& whistle);

/// Whistle interactions (Create's WhistleBlock.use / onWrenched / WhistleExtenderBlock).
bool useOnWhistle(Player& player, GlobalPos const& pos, ItemStack const& held, bool sneaking);
[[nodiscard]] int whistlePitchAt(GlobalPos const& whistle);

/// Engine value box (Create's ScrollOptionBehaviour<RotationDirection>).
void registerEngineValueBox();
[[nodiscard]] bool engineCounterClockwise(GlobalPos const& engine);
void               setEngineCounterClockwise(GlobalPos const& engine, bool ccw);

/// Positions of the unit's ticking blocks (persisted in the world DB).
struct Index {
    std::set<GlobalPos> engines, whistles, shafts;
};
[[nodiscard]] Index& index();
void                 saveIndex();
void                 loadIndex();
void                 resetRuntime();

/// Goggle / command lines for the block at `pos` (Create's addToGoggleTooltip).
[[nodiscard]] std::vector<std::string> describe(GlobalPos const& pos, Player const* viewer);

// ---- client sync (companion players) ----------------------------------------------------------------
void markTankForSync(GlobalPos const& controller);
void flushSync();
void sendFullSync(Player& player);

[[nodiscard]] std::uint64_t currentTick();

} // namespace lv::feature::steam
