#pragma once

// Internals of the bulk processing unit (U13) shared by its .cpp files (not a contract).
//
// Blocks: levitate:millstone, levitate:crushing_wheel (+ the invisible
// levitate:crushing_wheel_controller), levitate:encased_fan, levitate:nozzle. Behaviour of
// Create 6.0.10 (docs/SPEC.md §2.8, §3.6.13). Server thread only.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/bulk/Bulk.h"
#include "core/common/Recipe.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"

class Actor;
class Block;
class BlockSource;
class Player;

namespace lv::feature::bulk {

namespace cb = core::bulk;
using core::GlobalPos;
using Json = nlohmann::json;

inline constexpr std::string_view kMillstone  = "levitate:millstone";
inline constexpr std::string_view kWheel      = "levitate:crushing_wheel";
inline constexpr std::string_view kController = "levitate:crushing_wheel_controller";
inline constexpr std::string_view kFan        = "levitate:encased_fan";
inline constexpr std::string_view kNozzle     = "levitate:nozzle";

/// `config/bulk.json`: Create's CKinetics values of these machines (same keys and defaults).
struct BulkConfig {
    int   version           = 1;
    int   crushingDamage    = 4;   ///< damage dealt by active crushing wheels
    float fanPushDistance   = 20;  ///< blocks a fan pushes entities
    float fanPullDistance   = 20;  ///< blocks from where a fan pulls entities
    int   fanBlockCheckRate = 30;  ///< ticks between fans checking for anything blocking their air flow
    float fanRotationArgmax = 256; ///< RPM at which fans reach their maximum stats
    int   fanProcessingTime = 150; ///< ticks a fan processing recipe takes (per 16 items)
    /// Levitate: airflow particles (Create's client option fanParticleDensity).
    float fanParticleDensity = 0.5f;
};
[[nodiscard]] BulkConfig const& config();
[[nodiscard]] cb::FanConfig fanConfig();

enum class Kind : std::uint8_t { Millstone, Wheel, Controller, Fan, Nozzle };
[[nodiscard]] std::optional<Kind> kindOf(std::string_view blockId);

// ---- world helpers (World.cpp) ------------------------------------------------------------------------------

[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] BlockPos     toBlockPos(GlobalPos const& p);
[[nodiscard]] GlobalPos    toGlobal(int dim, BlockPos const& p);
[[nodiscard]] bool         loaded(BlockSource& region, BlockPos const& p);
/// Block id at `p` ("" when not loaded).
[[nodiscard]] std::string idAt(BlockSource& region, BlockPos const& p);
[[nodiscard]] int         stateOf(Block const& block, std::string_view state, int fallback = 0);
/// Sets integer states of the block at `p` (no-op when unchanged).
bool setStates(BlockSource& region, BlockPos const& p, std::vector<std::pair<std::string, int>> const& states);
/// Breaks a block and drops it as an item (Create's `destroyBlock(pos, true)`).
void breakBlock(BlockSource& region, BlockPos const& p);
[[nodiscard]] cb::Face faceOf(int v);
[[nodiscard]] BlockPos offset(BlockPos p, cb::Face f, int n = 1);

/// Entities whose bounding box intersects `box`.
[[nodiscard]] std::vector<Actor*> entitiesIn(BlockSource& region, cb::Box const& box);
[[nodiscard]] Actor*              entityById(std::int64_t uniqueId);
[[nodiscard]] std::int64_t        idOf(Actor const& a);
[[nodiscard]] cb::Vec3d           feetOf(Actor const& a);
[[nodiscard]] cb::Box             boxOf(Actor const& a);
[[nodiscard]] cb::Vec3d           motionOf(Actor const& a);
/// Sets an entity's motion (players get a motion packet: their client moves them).
void                              setMotion(Actor& a, cb::Vec3d const& m);
/// Moves a non-player entity (ignores collision).
void                              moveTo(Actor& a, cb::Vec3d const& feet);
[[nodiscard]] bool                isItem(Actor const& a);
[[nodiscard]] bool                isLiving(Actor const& a);
[[nodiscard]] bool                isCreativeFlying(Actor const& a);
[[nodiscard]] bool                isSneaking(Actor const& a);
/// The item of an item entity (nullptr for other entities).
[[nodiscard]] ItemStack*          itemOf(Actor& a);
/// Spawns an item entity with a motion; returns its id (0 on failure).
std::int64_t spawnItem(BlockSource& region, ItemStack const& stack, cb::Vec3d const& pos, cb::Vec3d const& motion, int pickupDelay = -1);
/// Drops stacks at the centre of a block.
void dropStacks(BlockSource& region, BlockPos const& p, std::vector<ItemStack> const& stacks);
/// Gives an item to a player (inventory first, the rest dropped at their feet).
void giveOrDrop(Player& player, ItemStack const& stack);

/// A vanilla particle effect seen by players near it.
void particle(int dim, cb::Vec3d const& pos, std::string const& effect);
/// A legacy particle (Bedrock ParticleType) with data, e.g. item crack particles.
void legacyParticle(int dim, cb::Vec3d const& pos, int particleType, int data);
/// A custom particle effect with float molang variables ("variable.<name>").
void particleWith(int dim, cb::Vec3d const& pos, std::string const& effect, std::vector<std::pair<std::string, float>> const& vars);
void sound(int dim, cb::Vec3d const& pos, std::string const& name, float volume, float pitch);
[[nodiscard]] float random01();

// ---- item helpers ---------------------------------------------------------------------------------------------

[[nodiscard]] int  slotLimit(ItemStack const& s); ///< min(64, max stack size)
[[nodiscard]] bool sameItem(ItemStack const& a, ItemStack const& b);
/// Create's `ItemHelper.addToList`: merges into equal stacks (up to their max), appends the rest.
void addToList(ItemStack stack, std::vector<ItemStack>& list);
/// Create's `ItemHandlerHelper.insertItemStacked` into `slots`: merge first, then empty slots;
/// returns what did not fit.
[[nodiscard]] ItemStack insertStacked(std::vector<ItemStack>& slots, ItemStack stack, bool simulate = false);
/// Rolls a processing recipe once per item of `count` and merges the outputs (Create's
/// `RecipeApplier.applyRecipeOn` for processing recipes).
[[nodiscard]] std::vector<ItemStack> rollRecipe(core::common::ProcessingRecipe const& recipe, int count);
/// Create's `ItemHelper.multipliedOutput`: `out` times the input count, split into stacks.
[[nodiscard]] std::vector<ItemStack> multipliedOutput(ItemStack const& in, ItemStack const& out);
/// The vanilla furnace recipe result of `tag` ("furnace", "smoker", "blast_furnace").
[[nodiscard]] std::optional<ItemStack> furnaceResult(ItemStack const& in, char const* tag);
/// The first recipe of `type` for `stack` (nullptr if none).
[[nodiscard]] core::common::ProcessingRecipe const* findRecipe(std::string const& type, ItemStack const& stack);
[[nodiscard]] Json      stackJson(ItemStack const& s);
[[nodiscard]] ItemStack stackOf(Json const& j);

// ---- fan processing (Fan.cpp) -------------------------------------------------------------------------------------

/// Create's FanProcessingType for the block at `p` (catalysts), None if none.
[[nodiscard]] cb::FanType catalystAt(BlockSource& region, BlockPos const& p);
[[nodiscard]] bool        canFanProcess(ItemStack const& stack, cb::FanType type);
/// The outputs of processing `stack` (nullopt: nothing happens; empty: the stack is destroyed).
[[nodiscard]] std::optional<std::vector<ItemStack>> fanProcess(ItemStack const& stack, cb::FanType type);
/// Applies a type's effect to a non-item entity (fire, damage, effects, conversion).
void affectEntity(Actor& entity, cb::FanType type, int dim);

// ---- per-machine ticks ------------------------------------------------------------------------------------------

void tickMillstone(BlockSource& region, GlobalPos const& pos);
void tickController(BlockSource& region, GlobalPos const& pos);
void tickFan(BlockSource& region, GlobalPos const& pos);
void tickNozzle(BlockSource& region, GlobalPos const& pos);
/// Create's `CrushingWheelBlock.updateControllers` for every side of the wheel at `pos`.
void fixControllers(BlockSource& region, GlobalPos const& pos);
/// Removes the controllers next to a wheel that is gone (Create's `onRemove`).
void removeControllersAround(BlockSource& region, GlobalPos const& pos, int axis);
/// A fan's current must be rebuilt (speed or blocks in front changed).
void markFanDirty(GlobalPos const& pos);
void forgetFan(GlobalPos const& pos);
void clearFans();
void clearCrushers();
/// Whether `wheel`'s placement is allowed (Create's `CrushingWheelBlock.canSurvive`).
[[nodiscard]] bool wheelCanSurvive(BlockSource& region, BlockPos const& p);
/// Whether a nozzle's fan is behind it (Create's `NozzleBlock.canSurvive`).
[[nodiscard]] bool nozzleCanSurvive(BlockSource& region, BlockPos const& p);

// ---- millstone (Millstone.cpp) ----------------------------------------------------------------------------------

/// Create's MillstoneBlock.useItemOn with an empty hand: outputs (or else inputs) to the player.
bool millstoneTakeOut(Player& player, GlobalPos const& pos);
/// Item entities landing on top (Create's `updateEntityAfterFallOn`).
void millstoneCatchItems(BlockSource& region, GlobalPos const& pos);
struct MillstoneView {
    ItemStack              input;
    std::vector<ItemStack> outputs;
    int                    timer = 0;
};
[[nodiscard]] MillstoneView millstoneAt(GlobalPos const& pos);

// ---- crushers (Crusher.cpp) ---------------------------------------------------------------------------------------

struct ControllerView {
    std::vector<ItemStack> inventory; ///< 32 slots: 0 input, 1.. outputs
    float                  remaining = 0;
    bool                   applied   = false;
    std::int64_t           entity    = 0;
};
[[nodiscard]] ControllerView controllerAt(GlobalPos const& pos);
/// Crushing speed of a controller (from its wheels; 0 when not valid).
[[nodiscard]] float crushingSpeedAt(BlockSource& region, GlobalPos const& pos);

// ---- fans (Fan.cpp) ---------------------------------------------------------------------------------------------------

struct FanView {
    cb::AirCurrent current;
    cb::Box        bounds;
    float          speed = 0;
};
[[nodiscard]] std::optional<FanView> fanAt(GlobalPos const& pos);

// ---- index (BulkModule.cpp) ------------------------------------------------------------------------------------------

/// Positions of this unit's blocks the module ticks.
void track(GlobalPos const& pos, Kind kind);
void untrack(GlobalPos const& pos);
/// Runs `fn` at the end of this tick (world edits outside events).
void defer(std::function<void()> fn);
[[nodiscard]] std::uint64_t currentTick();

/// Capability handlers, the airflow provider (Capabilities.cpp).
void registerCapabilities();
/// Drops state of entities that are gone (every few seconds).
void pruneBypass();
void pruneFanState();

namespace millstone_cap {
[[nodiscard]] int       slots();
[[nodiscard]] ItemStack getSlot(GlobalPos const& pos, int slot);
[[nodiscard]] ItemStack insert(GlobalPos const& pos, ItemStack const& stack, bool simulate);
[[nodiscard]] ItemStack extract(GlobalPos const& pos, std::function<bool(ItemStack const&)> const& filter, int amount, bool simulate);
} // namespace millstone_cap

namespace controller_cap {
[[nodiscard]] int       slots();
[[nodiscard]] ItemStack getSlot(GlobalPos const& pos, int slot);
[[nodiscard]] ItemStack insert(GlobalPos const& pos, ItemStack const& stack, bool simulate);
} // namespace controller_cap

} // namespace lv::feature::bulk
