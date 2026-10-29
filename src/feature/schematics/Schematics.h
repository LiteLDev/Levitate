#pragma once

// Internals of the schematics unit (U29): schematic and quill, schematic table, schematic item
// placement, schematicannon and crafting blueprint (SPEC §2.24, §3.6.29). Not a contract for
// other units.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/schematics/Logic.h"
#include "core/schematics/Schematic.h"

class Block;
class BlockPos;
class CompoundTag;
class BlockSource;
class ItemStack;
class Player;

namespace lv::feature::schematics {

namespace cs = core::schematics;
using core::GlobalPos;
using Json = nlohmann::json;

// ---- ids ---------------------------------------------------------------------------------------
inline constexpr std::string_view kTable          = "levitate:schematic_table";
inline constexpr std::string_view kCannon         = "levitate:schematicannon";
inline constexpr std::string_view kEmptySchematic = "levitate:empty_schematic";
inline constexpr std::string_view kSchematic      = "levitate:schematic";
inline constexpr std::string_view kQuill          = "levitate:schematic_and_quill";
inline constexpr std::string_view kBlueprintItem  = "levitate:crafting_blueprint";
inline constexpr std::string_view kBlueprintBlock = "levitate:placed_crafting_blueprint";
/// Owned by U12 (creative crate): referenced by id only.
inline constexpr std::string_view kCreativeCrate = "levitate:creative_crate";
/// Owned by U8 (wrench): referenced by id only.
inline constexpr std::string_view kWrench = "levitate:wrench";
/// Owned by U24 (clipboard): accepted as checklist paper by id only.
inline constexpr std::string_view kClipboard = "levitate:clipboard";

inline constexpr char kFacing[] = "levitate:facing"; ///< table: 0..3 (S W N E), blueprint: 0..5

/// `config/schematics.json` (Create's CSchematics keys and defaults).
struct Config {
    int  version                         = 1;
    bool creativePrintIncludesAir        = false;
    int  maxSchematics                   = 10;   ///< per player, oldest overwritten
    int  maxTotalSchematicSize           = 256;  ///< KB
    int  maxSchematicPacketSize          = 1024; ///< bytes per upload chunk
    int  schematicIdleTimeout            = 600;  ///< ticks
    int  schematicannonDelay             = 10;   ///< ticks between shots
    int  schematicannonShotsPerGunpowder = 400;
};
[[nodiscard]] Config const& config();

// ---- world helpers (World.cpp) ------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] BlockPos     bp(GlobalPos const& p);
[[nodiscard]] GlobalPos    gp(int dim, cs::Pos const& p);
[[nodiscard]] cs::Pos      cp(GlobalPos const& p);
[[nodiscard]] bool         loaded(GlobalPos const& p);
[[nodiscard]] Block const* blockAt(GlobalPos const& p);
[[nodiscard]] std::string  idAt(GlobalPos const& p);
[[nodiscard]] int          stateOr(GlobalPos const& p, char const* name, int fallback);
bool                       place(GlobalPos const& p, Block const& block, int flags = 3);
bool                       placeId(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states = {});
bool                       clearAt(GlobalPos const& p);
void                       destroyAt(GlobalPos const& p, bool drop);
/// The block is a full solid cube (Create's isRedstoneConductor for replace modes).
[[nodiscard]] bool solid(Block const& block);
/// The block holds data (vanilla block entity or Levitate BlockData).
[[nodiscard]] bool holdsData(GlobalPos const& p, Block const& block);
[[nodiscard]] bool replaceable(GlobalPos const& p);
/// Bedrock, barriers, ... (destroy time -1; air and liquids never count).
[[nodiscard]] bool unbreakable(Block const& block);

/// Block <-> schematic state.
[[nodiscard]] cs::BlockState stateOf(Block const& block);
/// The block for a schematic state (alternatives tried, unknown states dropped). nullptr when the
/// block id doesn't exist.
[[nodiscard]] Block const* resolve(cs::BlockState const& state);
/// A game CompoundTag from a core NBT compound (nullptr on failure).
[[nodiscard]] std::unique_ptr<CompoundTag> gameTag(cs::nbt::Tag const& tag);
/// SNBT of a block (for saving launched blocks) and back.
[[nodiscard]] std::string  blockSnbt(Block const& block);
[[nodiscard]] Block const* blockFromSnbt(std::string const& snbt);

void message(Player& player, std::string const& text);
void actionBar(Player& player, std::string const& text);
void sound(GlobalPos const& p, std::string const& name, float volume = 1.f, float pitch = 1.f);
void particle(int dim, double x, double y, double z, std::string const& effect);
[[nodiscard]] Player* playerById(std::int64_t uid);
[[nodiscard]] std::int64_t uidOf(Player const& player);
[[nodiscard]] std::uint64_t currentTick();
void                        defer(std::function<void()> fn);

/// The unit's part of a block's BlockData (`data["schematics"]`).
[[nodiscard]] Json&       data(GlobalPos const& p);
[[nodiscard]] Json const* dataIf(GlobalPos const& p);

/// Within interaction reach (8 blocks) of a block position, same dimension.
[[nodiscard]] bool inReach(Player const& player, GlobalPos const& p, double reach = 8.0);
[[nodiscard]] Json      posJson(GlobalPos const& p);
[[nodiscard]] std::optional<GlobalPos> posFrom(Json const& j);

// ---- items ----------------------------------------------------------------------------------------
[[nodiscard]] std::optional<ItemStack> makeItem(std::string_view id, int count = 1);
[[nodiscard]] bool                     isItem(ItemStack const& s, std::string_view id);
/// Gives an item to the player (dropped at their feet when the inventory is full).
void give(Player& player, ItemStack const& stack);
/// Drops an item entity at the centre of a block position.
void dropAt(GlobalPos const& p, ItemStack const& stack);
/// Moves a stack out of a player inventory slot (count = -1: all) and returns it.
[[nodiscard]] ItemStack takeFromInventory(Player& player, int slot, int count = -1);

/// The data of a written schematic (`lv:schematic`).
struct SchematicData {
    std::string file;
    std::string owner;
    bool        deployed = false;
    cs::Pos     anchor;
    cs::Transform transform;
    cs::Pos     size;
};
[[nodiscard]] std::optional<SchematicData> schematicData(ItemStack const& stack);
void                                       setSchematicData(ItemStack& stack, SchematicData const& d);
/// A written schematic for `owner/file` (Create's SchematicItem.create), with its size and lore.
[[nodiscard]] std::optional<ItemStack> makeSchematic(std::string const& file, std::string const& owner);
void                                   updateSchematicLore(ItemStack& stack);

// ---- files (Files.cpp) -----------------------------------------------------------------------------
/// A path from UTF-8 text.
[[nodiscard]] inline std::filesystem::path upath(std::string const& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
/// `<mod>/schematics/`
[[nodiscard]] std::filesystem::path schematicsDir();
/// `<mod>/schematics/<player>/` (the server's store of a player's schematics).
[[nodiscard]] std::filesystem::path playerDir(std::string const& player);
/// Player names as directory names (unsafe characters replaced).
[[nodiscard]] std::string safeName(std::string const& name);
[[nodiscard]] std::vector<std::string> listFiles(std::filesystem::path const& dir);
[[nodiscard]] std::optional<std::string> readFile(std::filesystem::path const& path, std::size_t maxBytes = 64u << 20);
bool                                     writeFile(std::filesystem::path const& path, std::string const& bytes);
/// Create's maxSchematics rule: deletes the oldest file of `dir` when it holds `max` or more.
bool deleteOldestIfFull(std::filesystem::path const& dir, int max);
/// A free file name "<base>.mcstructure", "<base>_2.mcstructure", ... (Create's findFirstValidFilename).
[[nodiscard]] std::string freeName(std::filesystem::path const& dir, std::string base, std::string const& ext);

/// A loaded schematic with its blocks resolved for this world.
struct Loaded {
    cs::Schematic             schematic;
    std::vector<Block const*> blocks;   ///< per palette index (nullptr: unmapped)
    std::vector<std::string>  unmapped; ///< palette names without a Bedrock block
    std::string               error;
};
/// Loads `<owner>/<file>` (cached until the file changes). nullptr when missing or broken (`error`).
[[nodiscard]] std::shared_ptr<Loaded const> loadSchematic(std::string const& owner, std::string const& file, std::string* error = nullptr);
void                                        clearSchematicCache();

/// Reads an area of the world into a schematic (Create's SchematicExport.saveSchematic).
[[nodiscard]] cs::Schematic capture(int dim, cs::Pos lo, cs::Pos hi);
/// Saves an area as `<dir>/<name>.mcstructure` (name made unique unless `overwrite`). Returns the
/// file name, nullopt on failure.
[[nodiscard]] std::optional<std::string> saveArea(std::filesystem::path const& dir, std::string name, bool overwrite, int dim, cs::Pos lo, cs::Pos hi);

// ---- quill (Quill.cpp) ---------------------------------------------------------------------------
/// Right-click with the schematic and quill (`hit` = clicked block + face, nullopt in the air).
void quillUse(Player& player, std::optional<std::pair<cs::Pos, int>> hit);
[[nodiscard]] cs::Selection& selectionOf(Player const& player);
/// Saves the selection (the name prompt's result). Returns the file name.
std::optional<std::string> quillSave(Player& player, std::string name, bool convert);
void                       quillDiscard(Player& player);
void                       resetQuills();
void                       tickQuillOutlines();

// ---- table (Table.cpp) -----------------------------------------------------------------------------
void openTable(Player& player, GlobalPos const& pos);
/// Writes `file` (from the player's server-side folder) onto the table's empty schematic
/// (vanilla players, and the end of an upload). Returns false with a reason.
bool writeOnTable(Player& player, GlobalPos const& table, std::string const& file, std::string* reason = nullptr);
void tickUploads();
void resetUploads();
void tableRemoved(GlobalPos const& p, Json const& doc);

// ---- schematic item (Placement.cpp) --------------------------------------------------------------
/// Right-click with a written schematic (vanilla deploy / sneak: coordinates screen).
void schematicUse(Player& player, std::optional<std::pair<cs::Pos, int>> hit);
/// Places a schematic instantly (Create's creative Print tool). Returns the blocks placed.
int printInstantly(Player* player, int dim, SchematicData const& d, bool includeAir);
void tickPlacementOutlines();
/// Tells the player which block types of a schematic have no Bedrock mapping (once per file).
void reportUnmapped(Player& player, std::string const& owner, std::string const& file);

// ---- cannon (Cannon.cpp) ---------------------------------------------------------------------------
void openCannon(Player& player, GlobalPos const& pos);
void trackCannon(GlobalPos const& p);
void tickCannons();
void resetCannons();
void loadCannons();
void cannonRemoved(GlobalPos const& p, Json const& doc);
/// Cannon state for selftests.
struct CannonView {
    std::string state, status;
    int         placed = 0, toPlace = 0, fuel = 0;
    std::string missing;
};
[[nodiscard]] CannonView cannonView(GlobalPos const& p);
/// Selftests / form: runs an action like the screen ("play", "pause", "stop", "replace", ...).
void cannonAction(Player* player, GlobalPos const& p, Json const& action);
/// Puts a stack into a cannon slot (0 schematic, 1 schematic out, 2 book, 3 book out, 4 gunpowder).
void cannonSetSlot(GlobalPos const& p, int slot, ItemStack const& stack);
[[nodiscard]] ItemStack cannonSlot(GlobalPos const& p, int slot);

// ---- blueprint (Blueprint.cpp) ---------------------------------------------------------------------
/// Places a blueprint against `face` of `support` (Create: the largest of 3x3, 2x2, 1x1 that fits).
bool placeBlueprint(Player& player, GlobalPos const& support, int face);
/// Right-click on a blueprint section: crafts (configured) or opens the recipe screen.
void blueprintUse(Player& player, GlobalPos const& p, bool holdingWrench);
/// Left-click: clears a configured section (true) instead of breaking it.
bool blueprintAttack(Player& player, GlobalPos const& p);
void blueprintRemoved(GlobalPos const& p, Json const& doc, bool drop);
/// True while a blueprint removes the rest of its group (those removals are not new breaks).
[[nodiscard]] bool blueprintSilent();
/// Crafts with a section's recipe from the player's inventory (selftests: `sneaking` = up to a stack).
int blueprintCraft(Player& player, GlobalPos const& section, bool sneaking);
void setBlueprintSlot(GlobalPos const& section, int slot, ItemStack const& stack);
/// The crafting recipes giving `result` (selftest diagnostics).
[[nodiscard]] std::string debugRecipes(std::string const& result);

} // namespace lv::feature::schematics
