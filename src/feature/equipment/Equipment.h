#pragma once

// Internals of the equipment unit (U31): toolboxes, backtanks, diving gear, extendo grip, potato
// cannon, wand of symmetry, handheld worldshaper, linked controller, sand paper and tree
// fertilizer. Not a contract for other units; see docs/SPEC.md §2.25 / §3.6.31.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/equipment/Shaper.h"
#include "core/equipment/Symmetry.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class ItemStackBase;
class Player;
class Vec3;

namespace lv::feature::equipment {

using core::GlobalPos;
using Json = nlohmann::json;

// ---- ids --------------------------------------------------------------------------------------
inline constexpr std::array<std::string_view, 16> kColours = {
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
};
inline constexpr std::string_view kCopperBacktank       = "levitate:copper_backtank";
inline constexpr std::string_view kNetheriteBacktank    = "levitate:netherite_backtank";
inline constexpr std::string_view kCopperHelmet         = "levitate:copper_diving_helmet";
inline constexpr std::string_view kCopperBoots          = "levitate:copper_diving_boots";
inline constexpr std::string_view kNetheriteHelmet      = "levitate:netherite_diving_helmet";
inline constexpr std::string_view kNetheriteBoots       = "levitate:netherite_diving_boots";
inline constexpr std::string_view kExtendoGrip          = "levitate:extendo_grip";
inline constexpr std::string_view kPotatoCannon         = "levitate:potato_cannon";
inline constexpr std::string_view kWand                 = "levitate:wand_of_symmetry";
inline constexpr std::string_view kWorldshaper          = "levitate:handheld_worldshaper";
inline constexpr std::string_view kLinkedController     = "levitate:linked_controller";
inline constexpr std::string_view kLecternController    = "levitate:lectern_controller";
inline constexpr std::string_view kSandPaper            = "levitate:sand_paper";
inline constexpr std::string_view kRedSandPaper         = "levitate:red_sand_paper";
inline constexpr std::string_view kTreeFertilizer       = "levitate:tree_fertilizer";
/// Owned by U23 (redstone): referenced by id only.
inline constexpr std::string_view kRedstoneLink = "levitate:redstone_link";

// ---- states -----------------------------------------------------------------------------------
/// Horizontal facing of toolboxes, backtanks and the lectern controller: Java 2D order
/// 0 south, 1 west, 2 north, 3 east (game::playerFacing); the block faces the player.
inline constexpr char kFacing[] = "levitate:facing";

// ---- item data keys (user data, items::setData) -------------------------------------------------
inline constexpr char kToolboxData[]    = "lv:toolbox";    ///< {inv, uuid}
inline constexpr char kAirData[]        = "lv:air";        ///< backtank air (number)
inline constexpr char kSymmetryData[]   = "lv:symmetry";   ///< Mirror json
inline constexpr char kShaperData[]     = "lv:shaper";     ///< ShaperSettings json
inline constexpr char kControllerData[] = "lv:controller"; ///< {"freq": [[a, b] x 6]}

/// `config/equipment.json` (Create's CEquipment keys and defaults).
struct Config {
    int version               = 1;
    int toolboxRange          = 10;   ///< Create: toolboxRange
    int airInBacktank         = 900;  ///< Create: airInBacktank (seconds of breathing)
    int maxExtendoGripActions = 1000; ///< Create: maxExtendoGripActions (0: unbreakable)
    int maxPotatoCannonShots  = 200;  ///< Create: maxPotatoCannonShots (0: unbreakable)
    int maxSymmetryWandRange  = 50;   ///< Create: maxSymmetryWandRange
    int enchantedBacktankCapacity = 300; ///< Create: enchantedBacktankCapacity (air per Capacity level)
};
[[nodiscard]] Config const& config();

// ---- world helpers (World.cpp) ------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] Block const* blockAt(GlobalPos const& p);
[[nodiscard]] std::string  idAt(GlobalPos const& p);
[[nodiscard]] std::optional<int> stateAt(GlobalPos const& p, char const* name);
bool place(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {});
bool placeBlock(GlobalPos const& p, Block const& block);
bool clearAt(GlobalPos const& p);
[[nodiscard]] bool replaceableAt(GlobalPos const& p);
void sound(int dim, double x, double y, double z, std::string const& name, float volume = 1.f, float pitch = 1.f);
void particle(int dim, double x, double y, double z, std::string const& effect);
void actionBar(Player& player, std::string const& text);
/// Create's "subtitle with an empty title" (fade 10/40/10).
void subtitle(Player& player, std::string const& text);
[[nodiscard]] Player* playerById(std::int64_t uid);
[[nodiscard]] std::int64_t uidOf(Actor const& a);
/// Gives an item to the player (inventory, else dropped at the player).
void giveOrDrop(Player& player, ItemStack const& stack);
void dropAt(int dim, double x, double y, double z, ItemStack const& stack);
/// Sets a motion (players through a motion packet, as their client moves them).
void setMotion(Actor& a, double x, double y, double z);
[[nodiscard]] std::string itemId(ItemStack const& s);
[[nodiscard]] bool isPaper(std::string_view id);
[[nodiscard]] double random01();
[[nodiscard]] std::uint64_t currentTick();
/// Runs `fn` at the start of the next tick (world edits out of events).
void defer(std::function<void()> fn);
/// True when a screen was opened for the player in the last 10 ticks (repeated use packets).
[[nodiscard]] bool screenDebounced(Player& player);
void runDeferred();
void clearDeferred();

// ---- toolboxes (Toolbox.cpp) --------------------------------------------------------------------
[[nodiscard]] bool isToolbox(std::string_view id);
[[nodiscard]] std::string toolboxId(std::string_view colour);
/// The toolbox's inventory json at `pos` ({} when empty).
[[nodiscard]] Json toolboxInventory(GlobalPos const& pos);
void               setToolboxInventory(GlobalPos const& pos, Json const& inv);
/// A player's right-click on a toolbox block (open; dye).
bool               useToolbox(Player& player, ItemStack& held, GlobalPos const& pos);
/// Left-click (Create: pick up with contents).
bool               attackToolbox(Player& player, GlobalPos const& pos);
/// After the player placed a toolbox: move the item's contents into the block.
void               toolboxPlaced(Player& player, GlobalPos const& pos, Json const& itemData);
/// Opens the toolbox screen (native or form).
void               openToolbox(Player& player, GlobalPos const& pos);
/// Radial menu actions (client mod and form): equip a compartment / unequip / deposit.
bool equipFromToolbox(Player& player, GlobalPos const& pos, int compartment, int hotbarSlot);
bool unequipSlot(Player& player, int hotbarSlot, bool keepItems);
int  depositAll(Player& player, std::vector<GlobalPos> const& boxes);
/// The 8 nearest toolboxes in range of the player.
[[nodiscard]] std::vector<GlobalPos> nearbyToolboxes(Player& player);
/// Opens the vanilla "nearby toolboxes" form.
void openToolboxForm(Player& player);
/// Before a player breaks a toolbox (creative breaks of empty toolboxes drop nothing).
void noteToolboxBreak(GlobalPos const& p);
/// The player closed the toolbox screen (native or form): the lid closes when nobody has it open.
void toolboxScreenClosed(Player& player, GlobalPos const& pos);
/// Players that have the toolbox's screen open (Create's openTracker.openCount).
[[nodiscard]] int toolboxOpenCount(GlobalPos const& pos);
/// Holds the lid open for `ticks` (0: releases it) as if a player had the screen open (debug command).
bool holdToolboxOpen(GlobalPos const& pos, int ticks);
/// Create's ToolboxDyeingRecipe: when the player's crafting grid holds exactly a toolbox and a dye,
/// the crafted toolbox `result` gets the toolbox's user data (contents, name). True when copied.
bool keepDyedToolboxData(Player& player, ItemStackBase& result);
void tickToolboxes();
void resetToolboxes();
void installToolboxes();

// ---- backtanks and diving gear (Backtank.cpp) ---------------------------------------------------
[[nodiscard]] bool isBacktank(std::string_view id);
/// Air stored in a backtank item (its user data).
[[nodiscard]] int  airOf(ItemStack const& stack);
void               setAir(ItemStack& stack, int air);
/// Create's air without enchantments (airInBacktank).
[[nodiscard]] int  maxAir();
/// The air a backtank stack holds at most (Create's BacktankUtil#maxAir with its Capacity level).
[[nodiscard]] int  maxAir(ItemStack const& stack);
/// The air of a placed backtank holds at most (Create's BacktankBlockEntity keeps the Capacity level).
[[nodiscard]] int  blockMaxAir(GlobalPos const& pos);
/// Air tools (extendo grip, potato cannon): uses air from the worn backtank, else durability.
/// Returns true when the use was paid for by air.
bool useAirOrDurability(Player& player, bool mainHand, int usesPerTank);
bool useBacktankBlock(Player& player, ItemStack& held, GlobalPos const& pos);
void backtankPlaced(Player& player, GlobalPos const& pos, int air, std::string const& name);
[[nodiscard]] int blockAir(GlobalPos const& pos);
void              setBlockAir(GlobalPos const& pos, int air);
void tickBacktanks();
void tickDivingGear();
/// Before a player breaks a backtank block (creative breaks drop nothing).
void noteBacktankBreak(GlobalPos const& p, bool creative);
/// The break did not happen: forget the note.
void clearBacktankBreak(GlobalPos const& p);
void uninstallBacktanks();
void resetBacktanks();
void installBacktanks();
/// Armour toughness of the netherite diving pieces an actor wears (Create: 3 per piece).
[[nodiscard]] int netheriteToughness(Actor const& a);
/// The Mob::getToughnessValue hook adding it.
void hookToughness(bool on);

// ---- tools (Tools.cpp) ----------------------------------------------------------------------------
/// Extra reach of the extendo grip(s) the player holds (0, 3 or 5).
[[nodiscard]] float extendoReach(Player const& player);
void extendoAction(Player& player);
bool firePotatoCannon(Player& player);
void tickProjectiles();
void resetProjectiles();
bool startSanding(Player& player, bool mainHand);
bool sandBlock(Player& player, ItemStack& held, GlobalPos const& pos);
void cancelSanding(Player& player);
void tickSanding();
bool useTreeFertilizer(Player& player, ItemStack& held, GlobalPos const& pos);
/// Number of projectiles in flight (selftests).
[[nodiscard]] int projectileCount();

// ---- potato projectiles (Projectile.cpp) ------------------------------------------------------------
/// The projectile entity (Create's PotatoProjectileEntity; behaviour pack `entities/potato_projectile.json`).
inline constexpr std::string_view kPotatoProjectile = "levitate:potato_projectile";
/// Tag of every projectile entity (orphans without a simulated projectile are removed).
inline constexpr char kPotatoProjectileTag[] = "lv_potato_projectile";
struct ProjectileInfo {
    std::int64_t entity = 0;     ///< the projectile entity
    std::string  type;           ///< Create's type id
    double       x = 0, y = 0, z = 0;
    std::int64_t stuckTo = 0;    ///< the entity a sticky projectile sticks to (0: flying)
    double       recovery = 0;   ///< recovery chance
};
/// The projectiles in flight (selftests).
[[nodiscard]] std::vector<ProjectileInfo> projectiles();
/// Launches one projectile as the cannon of `owner` does (selftests: collisions, stuck projectiles).
std::int64_t launchProjectile(int dim, double x, double y, double z, double vx, double vy, double vz, std::string const& ammoId,
                              std::int64_t owner, double recoveryChance);

// ---- enchantments (Enchants.cpp) ----------------------------------------------------------------------
/// Create's enchantments, kept in the item's user data (`lv:enchants`: {"capacity": L, "potato_recovery": L})
/// and shown as lore lines named like enchantments.
inline constexpr char kEnchantsData[]   = "lv:enchants";
inline constexpr char kCapacity[]       = "capacity";
inline constexpr char kPotatoRecovery[] = "potato_recovery";
/// The level of one of Create's enchantments on a stack (0: none).
[[nodiscard]] int  enchantLevel(ItemStack const& stack, std::string_view key);
/// Sets (0: removes) the level and rewrites the lore lines (in the language of `viewer`, else the default).
void               setEnchantLevel(ItemStack& stack, std::string_view key, int level, Player const* viewer = nullptr);
/// Whether the enchantment applies to the item (Capacity: backtanks; Potato Recovery: the potato cannon;
/// both: books, Java's enchanting table offers every enchantment for a book).
[[nodiscard]] bool enchantApplies(std::string_view itemId, std::string_view key);
/// Rolls Create's enchantment for one enchanting-table option as Java does (selftests): the level it adds
/// (0: none) for an item, the option's cost and slot, and the player's enchantment seed.
[[nodiscard]] std::pair<std::string, int> rollTableEnchant(std::string const& itemId, int enchantValue, int cost, int seed, int slot);
/// What the table rolled for the player's current item, per option (selftests).
[[nodiscard]] std::vector<std::pair<std::string, int>> tableRolls(Player const& player);
/// Runs `fn` as the player applying an enchanting-table option (the scope of the table's craft request; selftests).
void asTableEnchant(Player& player, std::function<void()> const& fn);

// ---- symmetry wand (Symmetry.cpp) ---------------------------------------------------------------
bool wandUseOn(Player& player, ItemStack& wand, GlobalPos const& clicked, int face);
bool wandUse(Player& player, ItemStack& wand);
/// A block was placed by `player` at `pos` (mirror it for every enabled wand in the hotbar).
void mirrorPlacement(Player& player, GlobalPos const& pos);
/// A block of `blockId` was broken by `player` at `pos`.
void mirrorBreak(Player& player, GlobalPos const& pos, std::string const& blockId);
/// True while mirrored copies are being placed / broken.
[[nodiscard]] bool mirroringNow();
/// A block as a mirror copy turns it (Mat2 of core/equipment/Symmetry.h; selftests).
[[nodiscard]] Block const& mirroredBlock(Block const& block, core::equipment::Mat2 const& t);
void tickWands();
void installWand();

// ---- worldshaper (Worldshaper.cpp) -----------------------------------------------------------
bool shaperUse(Player& player, ItemStack& shaper);
bool shaperSelect(Player& player, ItemStack& shaper, GlobalPos const& pos);
/// Applies the worldshaper settings at a block face (Create's WorldshaperItem.activate); returns the changed blocks.
int shapeAt(Player& player, core::equipment::ShaperSettings const& st, GlobalPos const& target, int face);
/// The material block of the settings (nullptr: none).
[[nodiscard]] Block const* shaperMaterial(core::equipment::ShaperSettings const& st);
/// Serialises a block for the settings' material.
[[nodiscard]] std::string shaperMaterialOf(Block const& block);
void tickShaper();
void installShaper();

// ---- linked controller (Controller.cpp) ----------------------------------------------------------
bool controllerUse(Player& player, ItemStack& held);
bool controllerUseOn(Player& player, ItemStack& held, GlobalPos const& pos);
bool lecternUse(Player& player, ItemStack& held, GlobalPos const& pos);
/// Button presses from a controller in hand or on a lectern (client mod messages / polled input).
void controllerButtons(Player& player, std::array<bool, 6> const& pressed, std::optional<GlobalPos> lectern);
[[nodiscard]] bool controllerActive(Player const& player);
void tickControllers();
void resetControllers();
void installControllers();
void uninstallControllers();

} // namespace lv::feature::equipment
