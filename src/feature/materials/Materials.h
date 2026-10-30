#pragma once

// Materials unit (U9): Create's materials, foods, ores, world generation and decorative stone.
// Internal header of the unit (selftests); other units refer to the items by id only.

#include <array>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;
class Actor;
class Vec3;

namespace lv::feature::materials {

// ---- block table (generated: MaterialsBlocks.inc) ----------------------------------------------

enum class Tool { Pickaxe, Axe };
/// Create's needs_*_tool tags (none: any pickaxe).
enum class Tier { None, Iron };
/// Layered / StonePillar: the palette blocks with connected textures (MaterialsConnections.cpp).
enum class Shape { Cube, Pillar, Haxis, Stairs, Slab, Wall, Layered, StonePillar };
/// Drop rules beyond "the block itself" (Create's loot tables).
enum class Drop { Self, ZincOre, BoundCardboard };

struct BlockInfo {
    std::string_view id;
    Tool             tool;
    Tier             tier;
    Shape            shape;
    Drop             drop;
};

/// Every block of the unit (Create's ids under levitate:).
[[nodiscard]] std::vector<BlockInfo> const& allBlocks();
[[nodiscard]] BlockInfo const*              blockInfo(std::string_view id);
/// The unit's block of `block`'s type (nullptr: not one of ours, or no world yet). Fast (type table).
[[nodiscard]] BlockInfo const* blockInfo(Block const& block);
/// A string / bool state of a block as text ("" when the block has no such state).
[[nodiscard]] std::string stringState(Block const& block, std::string const& key);

// ---- neighbour-dependent states (MaterialsConnections.cpp) ---------------------------------------

/// Recomputes the neighbour-dependent states of the block at `pos` now (walls: Java WallBlock
/// side heights and post; layered stone / pillars: connected-texture states). True if it changed.
bool updateConnections(BlockSource& region, BlockPos const& pos);
/// Collision boxes of the unit's wall in block pixels (Java WallBlock collision: 24 px high).
[[nodiscard]] std::vector<std::array<float, 6>> wallCollision(Block const& block);
void installConnections();
void uninstallConnections();

// ---- config: config/materials.json (Create's CWorldGen + the CRecipes keys of these items) ------

struct MaterialsConfig {
    int version = 1;
    /// CWorldGen.disableWorldGen: prevents all world generation added by the mod.
    bool disableWorldGen = false;
    /// CRecipes.lightSourceCountForRefinedRadiance: light sources a chromatic compound absorbs.
    int lightSourceCountForRefinedRadiance = 10;
    /// CRecipes.enableRefinedRadianceRecipe: chromatic compound turns into refined radiance.
    bool enableRefinedRadianceRecipe = true;
    /// CRecipes.enableShadowSteelRecipe: chromatic compound falling into the void turns into shadow steel.
    bool enableShadowSteelRecipe = true;
};

// ---- drops ---------------------------------------------------------------------------------------

/// The tool used decides whether a block drops (Create: requiresCorrectToolForDrops + pickaxeOnly /
/// axeOnly + needs_iron_tool).
[[nodiscard]] bool toolCanHarvest(BlockInfo const& info, ItemStack const& tool);
/// What a player mining `block` gets (Create's loot tables incl. silk touch, fortune and double
/// slabs). `nextInt(n)` returns a uniform number in [0, n).
[[nodiscard]] std::vector<std::pair<std::string, int>>
dropsFor(BlockInfo const& info, Block const& block, bool silkTouch, int fortune, std::function<int(int)> const& nextInt);

// ---- slabs ---------------------------------------------------------------------------------------

/// Whether `block` is one of the unit's single (not double) slabs, and its half (true = top).
[[nodiscard]] std::optional<bool> singleSlabHalf(Block const& block);
/// Turns the single slab at `pos` into a double slab. False when it isn't a single slab.
bool mergeSlab(BlockSource& region, BlockPos const& pos);

// ---- items ---------------------------------------------------------------------------------------

/// Create's ExperienceNuggetItem.use: the whole stack (one when sneaking) becomes experience,
/// 3 per nugget, in up to 5 orbs. Returns the experience spawned.
int useExperienceNuggets(Player& player, ItemStack& stack, bool sneaking);
/// Experience values of the orbs Create spawns for `amount` nuggets.
[[nodiscard]] std::vector<int> experienceOrbValues(int amount);
/// Builder's Tea: Haste I for 3 minutes, no particles, no icon (Create's food effect).
void applyBuildersTea(Player& player);

/// Item entity behaviours of the legacy materials (chromatic compound, shadow steel, refined
/// radiance), run every tick for item actors holding them.
void tickMagicalItem(Actor& itemActor);
/// Light charge of a chromatic compound stack (Create's CHROMATIC_COMPOUND_COLLECTING_LIGHT).
[[nodiscard]] int  chromaticLight(ItemStack const& stack);
/// The chromatic compound item entity takes one light-emitting block item from the belt / depot
/// at `target` (Create: a TransportedItemStackHandlerBehaviour there). True when it did.
bool absorbFromTransporter(Actor& itemActor, BlockPos const& target);
void              setChromaticLight(ItemStack& stack, int light);

// ---- furnace xp and the block of experience (MaterialsFx.cpp) -----------------------------------

/// Create's xp of a smelting / blasting recipe of the unit by its input (nullopt: not Create's).
[[nodiscard]] std::optional<float> createSmeltingXp(std::string const& inputId);
using ParticleSink = std::function<void(Vec3 const& pos, Vec3 const& velocity)>;
/// Java ClientLevel.animateTick around `center` for the block of experience (Create's
/// ExperienceBlock.animateTick): `emit` gets each end rod particle.
void animateExperienceBlocks(BlockSource& region, BlockPos const& center, std::mt19937& random, ParticleSink const& emit);

// ---- world generation -------------------------------------------------------------------------

/// Decorates a freshly generated chunk (zinc veins, striated layers). Thread safe; returns the
/// number of blocks placed.
int decorateChunk(BlockSource& region, int dim, int chunkX, int chunkZ);
/// Number of chunks decorated since the world loaded (all threads).
[[nodiscard]] long long decoratedChunks();

// ---- misc ----------------------------------------------------------------------------------------

[[nodiscard]] std::string itemIdOf(ItemStack const& stack);
/// A stack of `id` (empty stack if unknown).
[[nodiscard]] ItemStack makeStack(std::string const& id, int count);
/// The loaded block of `id` (nullptr if the pack didn't load it).
[[nodiscard]] Block const* defaultBlock(std::string const& id);

} // namespace lv::feature::materials
