#pragma once

// Internals of the crafter unit (U16): mechanical crafter, mechanical saw, mechanical drill.
// Not a contract for other units.

#include <cstdint>
#include <functional>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mc/world/item/ItemStack.h"

#include "core/GlobalPos.h"
#include "core/crafter/Crafter.h"
#include "core/kinetics/Types.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class Player;
class Vec3;

namespace lv::feature::crafter {

using core::GlobalPos;
using core::kinetics::Face;
namespace cr = core::crafter;

inline constexpr std::string_view kCrafter   = "levitate:mechanical_crafter";
inline constexpr std::string_view kSaw       = "levitate:mechanical_saw";
inline constexpr std::string_view kDrill     = "levitate:mechanical_drill";
inline constexpr std::string_view kWrench    = "levitate:wrench";
inline constexpr std::string_view kSlotCover = "levitate:crafter_slot_cover";
inline constexpr std::string_view kArm       = "levitate:mechanical_arm";

inline constexpr char kFacing[]     = "levitate:facing";
inline constexpr char kPointing[]   = "levitate:pointing";
inline constexpr char kCovered[]    = "levitate:covered";
inline constexpr char kAlongFirst[] = "levitate:axis_along_first";
inline constexpr char kRotor[]      = "levitate:rotor";

/// `config/crafter.json` (Create's CRecipes / CKinetics keys used by this unit, same defaults).
struct Config {
    int  version                         = 1;
    bool allowRegularCraftingInCrafter   = true; ///< CRecipes
    int  maxFireworkIngredientsInCrafter = 9;    ///< CRecipes
    bool allowStonecuttingOnSaw          = true; ///< CRecipes
    bool bulkCutting                     = false; ///< CRecipes: saws process whole stacks
    bool moveItemsToStorage              = true; ///< CKinetics: moving drills/saws fill contraption storage
};
[[nodiscard]] Config const& config();

// ---- world helpers (World.cpp) ---------------------------------------------------------------------
namespace world {
[[nodiscard]] BlockSource*       regionOf(int dim);
[[nodiscard]] BlockSource*       loadedRegion(GlobalPos const& pos);
[[nodiscard]] std::string        idAt(GlobalPos const& pos);
[[nodiscard]] Block const*       blockAt(GlobalPos const& pos);
[[nodiscard]] int                intState(Block const& block, char const* state, int fallback = 0);
/// Sets integer states of the block at `pos` (keeps its type). Returns false if unchanged.
bool                             setStates(GlobalPos const& pos, std::vector<std::pair<std::string, int>> const& states);
[[nodiscard]] GlobalPos          offset(GlobalPos p, Face f);
/// Redstone power reaching `pos` from its neighbours (Bedrock gives custom blocks no neighbour
/// updates: polled; see SPEC §8).
[[nodiscard]] bool               powered(GlobalPos const& pos);
void playSound(GlobalPos const& at, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void playSoundAt(int dim, double x, double y, double z, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void particle(int dim, double x, double y, double z, std::string const& effect);
/// Spawns an item entity (Create's ItemEntity with the default pickup delay when `pickupDelay`).
Actor* dropItem(int dim, double x, double y, double z, ItemStack const& stack, std::optional<std::array<double, 3>> motion = {});
/// Breaks a block like a player without tool would (vanilla drops, particles with `effectChance`)
/// and returns its drops instead of spawning them when `collect` is set.
std::vector<ItemStack> destroyBlock(GlobalPos const& pos, bool collect, float effectChance = 1.f);
/// Hardness of the block (Create's getDestroySpeed; -1 unbreakable).
[[nodiscard]] float hardness(Block const& block);
/// Create's isBreakable: not air, not a liquid, breakable, not in #create:non_breakable.
[[nodiscard]] bool  breakable(Block const& block);
[[nodiscard]] bool  hasCollision(GlobalPos const& pos);
/// Block cracking overlay for every player near `pos` (progress 0..9; -1 clears).
void crack(GlobalPos const& pos, int progress);
/// Item entities and other entities in a box (world coordinates).
std::vector<Actor*> entitiesIn(int dim, double x0, double y0, double z0, double x1, double y1, double z1);
[[nodiscard]] bool  isItemEntity(Actor const& a);
[[nodiscard]] ItemStack itemOf(Actor const& itemEntity);
void                    setItem(Actor& itemEntity, ItemStack const& stack);
void                    discard(Actor& a);
void                    hurt(Actor& a, float damage);
void                    push(Actor& a, double vx, double vy, double vz);
[[nodiscard]] Player*   playerById(std::int64_t uniqueId);
/// Calls `fn` for every companion player (client mod) in `pos`'s dimension within `radius` blocks.
void                    forEachCompanionNear(GlobalPos const& pos, double radius, std::function<void(Player&)> const& fn);
/// Gives the item to the player (inventory, else dropped at their feet).
void                    give(Player& player, ItemStack const& stack);
/// Whether `id` (a Levitate block) accepts items from a belt/saw/crafter output like Create's
/// DirectBeltInputBehaviour (Levitate machines with an item capability, except vaults).
[[nodiscard]] bool      acceptsDirectInput(std::string const& id);
} // namespace world

// ---- crafter (Crafter.cpp) ---------------------------------------------------------------------------
namespace crafterlogic {
struct State {
    cr::Phase                       phase      = cr::Phase::Idle;
    int                             countDown  = 0;
    bool                            covered    = false;
    bool                            wasPowered = false;
    ItemStack                       slot;
    std::map<cr::Cell, ItemStack>   grid;
    bool                            isController = true;
    std::vector<core::kinetics::Offset> input{{0, 0, 0}}; ///< controller: members; else [offset to controller]
    std::uint64_t                   lastLazy = 0;
};
/// The crafter at `pos` (nullptr if the block isn't a crafter or its chunk isn't loaded).
State* at(GlobalPos const& pos);
void   save(GlobalPos const& pos);
void   tickAll(std::uint64_t tick);
void   track(GlobalPos const& pos);
/// Create's useItemOn on the front face; returns true if handled.
bool   use(Player& player, GlobalPos const& pos, int face, double hx, double hy, double hz);
/// Same with the used stack (the hand's stack in GameMode::useItemOn; changed in place).
bool   use(Player& player, GlobalPos const& pos, int face, double hx, double hy, double hz, ItemStack& held);
/// Orientation of a freshly placed crafter (Create's getStateForPlacement).
void   orientPlaced(GlobalPos const& pos, Face clickedFace, GlobalPos const& clickedPos, Face lookFacing, bool sneaking);
/// Toggles the connected input with the neighbour (Create's ConnectedInputHandler.toggleConnection).
void   toggleConnection(GlobalPos const& a, GlobalPos const& b);
[[nodiscard]] bool shouldConnect(GlobalPos const& pos, Face face, Face direction);
/// Inserts into the crafter's connected input (Create's item capability).
ItemStack insert(GlobalPos const& pos, ItemStack const& stack, bool simulate);
void      checkCompletedRecipe(GlobalPos const& pos, bool poweredStart);
void      ejectWholeGrid(GlobalPos const& pos);
/// The result of a crafting grid (vanilla crafting, then mechanical crafting), nullopt if none.
std::optional<ItemStack> tryToApplyRecipe(std::map<cr::Cell, ItemStack> const& grid);
void      install();
void      uninstall();
void      reset();
[[nodiscard]] std::vector<GlobalPos> tracked();
/// Sends the crafter's state to companion clients (Create's sendData).
void      sendData(GlobalPos const& pos);
} // namespace crafterlogic

// ---- saw and drill (Machines.cpp) ----------------------------------------------------------------------
namespace machines {
struct SawState {
    std::vector<ItemStack> inv; ///< slot 0 input, 1.. outputs (Create's ProcessingInventory, 16 slots)
    float remainingTime  = -1;
    float recipeDuration = 0;
    bool  appliedRecipe  = false;
    int   recipeIndex    = 0;
};
struct BreakerState {
    int  ticksUntilNextProgress = 0;
    int  destroyProgress        = 0;
    std::optional<GlobalPos> breakingPos;
};
SawState*     sawAt(GlobalPos const& pos);
BreakerState* breakerAt(GlobalPos const& pos);
void          saveSaw(GlobalPos const& pos);
void          tickAll(std::uint64_t tick);
void          track(GlobalPos const& pos);
void          orientPlaced(GlobalPos const& pos, std::string_view id, Face lookFacing, Face horizontalLook, bool sneaking);
/// Puts an item entity's stack into an up-facing saw (Create's SawBlockEntity.insertItem).
void          insertItemEntity(GlobalPos const& pos, Actor& item);
ItemStack     sawInsert(GlobalPos const& pos, ItemStack const& stack, bool simulate);
[[nodiscard]] std::optional<Face> facingOf(Block const& block);
void          install();
void          uninstall();
void          reset();
} // namespace machines

// ---- recipes (Recipes.cpp) ------------------------------------------------------------------------------
namespace sawrecipes {
struct Option {
    std::string            id;      ///< recipe id
    int                    time = 50;
    std::function<std::vector<ItemStack>()> roll; ///< one roll of the results
    ItemStack              display; ///< first result (filter test)
};
/// Create's getRecipes for a saw holding `input`, filtered by the saw's filter slot at `pos`.
std::vector<Option> forInput(GlobalPos const& pos, ItemStack const& input);
} // namespace sawrecipes

// ---- tree cutting (Trees.cpp) -------------------------------------------------------------------------
namespace trees {
[[nodiscard]] bool sawable(Block const& block);
/// Fells what stands on `cut` after the saw broke it (`brokenId` the cut block's id); each drop
/// goes to `drop(pos, stack)`.
void fell(GlobalPos const& cut, std::string const& brokenId, std::function<void(GlobalPos const&, ItemStack const&)> const& drop);
} // namespace trees

// ---- shared ----------------------------------------------------------------------------------------------
/// Positions that a placement intent will orient (Hooks.cpp).
void          installHooks();
void          uninstallHooks();
[[nodiscard]] std::uint64_t currentTick();

} // namespace lv::feature::crafter
