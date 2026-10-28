#pragma once

// Basin unit (U14) internals: basin, mechanical press, mechanical mixer, blaze burners.
// Server thread only. See docs/SPEC.md §2.9 / §3.6.1.

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"

#include "core/GlobalPos.h"
#include "core/basin/BasinLogic.h"
#include "core/common/Recipe.h"

class Block;
class BlockSource;
class Player;
class Recipe;

namespace lv::feature::basin {

namespace cb = core::basin;
using core::GlobalPos;
using Json = nlohmann::json;

// ---- ids -----------------------------------------------------------------------------------------
inline constexpr char const* kBasin       = "levitate:basin";
inline constexpr char const* kPress       = "levitate:mechanical_press";
inline constexpr char const* kMixer       = "levitate:mechanical_mixer";
inline constexpr char const* kBurner      = "levitate:blaze_burner";
inline constexpr char const* kEmptyBurner = "levitate:empty_blaze_burner";
inline constexpr char const* kLitBurner   = "levitate:lit_blaze_burner";
inline constexpr char const* kBlazeCake   = "levitate:blaze_cake";
inline constexpr char const* kCreativeCake = "levitate:creative_blaze_cake";
inline constexpr char const* kGoggles     = "levitate:goggles";
inline constexpr char const* kWrench      = "levitate:wrench";

/// Bedrock face (0 down, 2 north, 3 south, 4 west, 5 east): the basin's spout side (0 = none),
/// the side a burner's blaze looks at.
inline constexpr char const* kFacingState = "levitate:facing";
inline constexpr char const* kAxisState   = "levitate:axis";  ///< press: 0 x, 2 z
inline constexpr char const* kRotorState  = "levitate:rotor"; ///< press / mixer: 0 full model, 1 static parts
inline constexpr char const* kBlazeState  = "levitate:blaze"; ///< blaze burner heat 1..4
inline constexpr char const* kFlameState  = "levitate:flame"; ///< lit burner: 0 regular, 1 soul

enum class Kind : int { Basin = 0, Press = 1, Mixer = 2, Burner = 3, LitBurner = 4, EmptyBurner = 5 };
[[nodiscard]] std::optional<Kind> kindOf(std::string_view blockId);

// ---- faces (Bedrock FacingID) ----------------------------------------------------------------------
inline constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;
/// Create's Iterate.horizontalDirections order: south, west, north, east.
inline constexpr std::array<int, 4> kHorizontals{kSouth, kWest, kNorth, kEast};
[[nodiscard]] int      oppositeFace(int face);
[[nodiscard]] BlockPos offsetOf(BlockPos const& p, int face, int n = 1);

// ---- world helpers ---------------------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] bool         loaded(BlockSource& region, BlockPos const& pos);
[[nodiscard]] std::string  idAt(BlockSource& region, BlockPos const& pos);
[[nodiscard]] int          stateAt(BlockSource& region, BlockPos const& pos, char const* state, int fallback = 0);
/// Sets one integer state of the block at `pos` (no-op when unchanged).
bool setState(BlockSource& region, BlockPos const& pos, char const* state, int value);
void playSound(int dim, BlockPos const& pos, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void particle(int dim, float x, float y, float z, std::string const& effect);
/// Drops an item entity at the centre of `pos` (Create's Block.popResource).
void popItem(BlockSource& region, float x, float y, float z, ItemStack const& stack);
/// Gives a stack to the player, dropping what doesn't fit (Create's placeItemBackInInventory).
void giveOrDrop(Player& player, ItemStack const& stack);
/// A uniform float in [0, 1).
[[nodiscard]] float random01();
/// Whether the block has a collision box.
[[nodiscard]] bool hasCollision(BlockSource& region, BlockPos const& pos);
[[nodiscard]] int  maxStackOf(ItemStack const& stack);

// ---- basin -------------------------------------------------------------------------------------------
struct BasinState {
    std::array<ItemStack, cb::kSlotCount> in;
    std::array<ItemStack, cb::kSlotCount> out;
    std::vector<cb::Fluid>                tankIn  = std::vector<cb::Fluid>(cb::kTankCount);
    std::vector<cb::Fluid>                tankOut = std::vector<cb::Fluid>(cb::kTankCount);
    int                                   preferred = -1; ///< Create's preferredSpoutput (face) or -1
    std::vector<int>                      disabled;       ///< Create's disabledSpoutputs
    std::vector<ItemStack>                spout;          ///< spoutputBuffer
    std::vector<cb::Fluid>                spoutFluid;     ///< spoutputFluidBuffer
    // runtime (not saved)
    bool contentsChanged   = true;
    int  lazy              = 0;
    int  recipeBackupCheck = 20;
    int  intakeCooldown    = 0;
    std::vector<std::pair<ItemStack, int>> visualOut;
};

namespace basinops {
/// The basin at `pos` (loaded from BlockData on first use); nullptr if `pos` is no basin.
[[nodiscard]] BasinState* get(BlockSource& region, GlobalPos const& pos);
/// Writes the basin's contents to BlockData (after every change).
void save(GlobalPos const& pos, BasinState const& b);
void forget(GlobalPos const& pos);
void forgetAll();

[[nodiscard]] bool isEmpty(BasinState const& b);
[[nodiscard]] bool canContinueProcessing(BasinState const& b);
void notifyChangeOfContents(BasinState& b);

/// Create's BasinInventory insertion (one stack per item kind, first free slot): returns the remainder.
ItemStack insertInput(BasinState& b, ItemStack const& stack, bool simulate);
/// Create's acceptOutputs: whether the outputs fit (the spout target or the output buffers).
bool acceptOutputs(BlockSource& region, GlobalPos const& pos, BasinState& b, std::vector<ItemStack> const& items,
                   std::vector<cb::Fluid> const& fluids, bool simulate);
/// Heat of the block under the basin.
[[nodiscard]] cb::HeatLevel heatUnder(BlockSource& region, GlobalPos const& pos);

/// Combined tanks as Create's CombinedTankWrapper(output, input).
[[nodiscard]] std::vector<cb::Fluid*> combinedTanks(BasinState& b);
/// Combined slots (input 0..8, output 9..17).
[[nodiscard]] ItemStack& slot(BasinState& b, int i);

void tick(BlockSource& region, GlobalPos const& pos, BasinState& b);
/// The block above the basin changed etc.: the processing machine above should look again.
void wakeOperator(GlobalPos const& basinPos);
/// Wrench on a side (Create's onWrenched).
void onWrenched(BlockSource& region, GlobalPos const& pos, BasinState& b, int face);
void updateSpoutput(BlockSource& region, GlobalPos const& pos, BasinState& b);
[[nodiscard]] bool canOutputTo(BlockSource& region, BlockPos const& basin, int face);
/// Player use (empty hand, fluid containers, sponge). True when handled.
bool use(Player& player, BlockSource& region, GlobalPos const& pos, ItemStack const& held, int face);
/// Drops the contents (block broken).
void dropContents(BlockSource& region, GlobalPos const& pos, Json const& doc);
/// Contents as JSON for the client renderer.
[[nodiscard]] Json renderJson(BasinState const& b);

void registerCapabilities();
} // namespace basinops

// ---- recipes in a basin (Create's BasinRecipe) ---------------------------------------------------------
struct BasinRecipe {
    std::string                         id;
    bool                                crafting = false;
    core::common::ProcessingRecipe const* processing = nullptr;
    std::shared_ptr<::Recipe const>     vanilla;      ///< a crafting table recipe
    int                                 ingredientCount = 0;
};

namespace recipes {
/// Recipes the press (`press` true) or the mixer may run, Create's static filters applied.
[[nodiscard]] std::vector<BasinRecipe> const& candidates(bool press);
/// Create's BasinRecipe.match: the filter accepts the output and the basin can run it now.
[[nodiscard]] bool match(BlockSource& region, GlobalPos const& basinPos, BasinState& b, BasinRecipe const& r);
/// Create's BasinRecipe.apply: consumes ingredients and stores outputs. False when not possible.
bool apply(BlockSource& region, GlobalPos const& basinPos, BasinState& b, BasinRecipe const& r);
/// Matching recipes, most ingredients first (Create's getMatchingRecipes).
[[nodiscard]] std::vector<BasinRecipe const*> matching(BlockSource& region, GlobalPos const& basinPos, BasinState& b, bool press);
[[nodiscard]] BasinRecipe const* byId(bool press, std::string const& id);
/// First pressing recipe for an item (Create's MechanicalPressBlockEntity.getRecipe).
[[nodiscard]] core::common::ProcessingRecipe const* pressing(ItemStack const& stack);
/// Rolled outputs of a processing recipe for one input.
[[nodiscard]] std::vector<ItemStack> roll(core::common::ProcessingRecipe const& r);
/// Container left after crafting (milk bucket -> bucket, ...).
[[nodiscard]] ItemStack remainderOf(ItemStack const& ingredient);
void reset();
} // namespace recipes

// ---- press / mixer (Create's BasinOperatingBlockEntity) -------------------------------------------------
struct OperatorState {
    bool        basinCheck   = true; ///< DeferralBehaviour pending
    std::string recipeId;            ///< currentRecipe
    // press (PressingBehaviour)
    bool running          = false;
    int  runningTicks     = 0;
    int  prevRunningTicks = 0;
    int  mode             = 0; ///< cb::PressMode
    bool finished         = false;
    int  scanCooldown     = 0;
    // mixer
    int  processingTicks = -1;
    // runtime
    bool basinRemoved = false; ///< Create's basinRemoved (the basin was emptied by hand)
};

namespace operators {
[[nodiscard]] OperatorState* get(GlobalPos const& pos, bool press);
void save(GlobalPos const& pos, OperatorState const& s, bool press);
void forget(GlobalPos const& pos);
void forgetAll();
void tickPress(BlockSource& region, GlobalPos const& pos, OperatorState& s);
void tickMixer(BlockSource& region, GlobalPos const& pos, OperatorState& s);
/// Kinetic speed changed (Create's onSpeedChanged): look at the basin again.
void onSpeedChanged(GlobalPos const& pos);
/// Animation state for the client renderer.
[[nodiscard]] Json renderJson(GlobalPos const& pos, OperatorState const& s, bool press);
} // namespace operators

// ---- blaze burner ------------------------------------------------------------------------------------
struct BurnerData {
    cb::BurnerState state;
    bool            goggles = false;
};

namespace burners {
[[nodiscard]] BurnerData* get(GlobalPos const& pos);
void save(GlobalPos const& pos, BurnerData const& d);
void forget(GlobalPos const& pos);
void forgetAll();
void tick(BlockSource& region, GlobalPos const& pos, BurnerData& d);
/// What an item gives as fuel (Create: blaze cake special, furnace fuels normal).
[[nodiscard]] cb::FuelOffer fuelOf(ItemStack const& stack);
/// Create's BlazeBurnerBlock.tryInsert: returns the accepted flag and the container left over.
std::pair<bool, ItemStack> tryInsert(BlockSource& region, GlobalPos const& pos, ItemStack const& stack, bool doNotConsume,
                                     bool forceOverflow, bool simulate);
/// Player use on any burner block. True when handled.
bool use(Player& player, BlockSource& region, GlobalPos const& pos, ItemStack const& held);
/// The empty burner item on a blaze / blaze spawner.
bool captureFromEntity(Player& player, class Actor& target);
bool captureFromSpawner(Player& player, BlockSource& region, BlockPos const& pos);
[[nodiscard]] cb::HeatLevel heatOfBlock(Block const& block);
void registerHeat();
void particles(BlockSource& region, GlobalPos const& pos, Kind kind);
} // namespace burners

// ---- machine registry (BasinModule.cpp) ----------------------------------------------------------------
/// Remembers a machine position (ticking); called when a block of the unit appears.
void track(GlobalPos const& pos, Kind kind);
[[nodiscard]] std::map<GlobalPos, Kind> const& tracked();
/// Ticks since the module started (server thread).
[[nodiscard]] std::uint64_t now();
/// Runs `fn` on the next server tick.
void defer(std::function<void()> fn);

// ---- client sync (BasinSync.cpp) -----------------------------------------------------------------------
namespace sync {
/// Marks a machine's visible state as changed (sent to companions near it at the end of the tick).
void dirty(GlobalPos const& pos);
void flush();
void reset();
} // namespace sync

/// Config `config/basin.json` (Create's CRecipes keys used by this unit).
struct BasinConfig {
    int  version                  = 1;
    bool bulkPressing             = false;
    bool allowShapelessInMixer    = true;
    bool allowShapedSquareInPress = true;
    bool allowBrewingInMixer      = true;
    /// Item entities above a basin are collected every this many ticks.
    int  basinIntakeInterval      = 1;
    /// Blaze burner flame particles (server side, seen by every client).
    bool burnerParticles          = true;
};

} // namespace lv::feature::basin
