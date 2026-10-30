#pragma once

// Internals of the display unit (U24): shared helpers of the link, board, nixie tube and clipboard
// files. Not a contract: other units use Display.h.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "feature/display/Clipboard.h"
#include "feature/display/Display.h"

class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;
class Vec3;

namespace lv::feature::display {

inline constexpr char kLinkId[]       = "levitate:display_link";
inline constexpr char kBoardId[]      = "levitate:display_board";
inline constexpr char kNixieId[]      = "levitate:nixie_tube"; ///< the orange one (Create's ORANGE_NIXIE_TUBE)
inline constexpr char kTextEntityId[] = "levitate:display_text";
inline constexpr char kWrenchId[]     = "levitate:wrench";

/// Dye colours in Java's DyeColor order.
inline constexpr std::array<char const*, 16> kDyeNames{
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
inline constexpr int kOrange = 1;

/// The characters (UTF-8 code points) of a string.
[[nodiscard]] inline std::vector<std::string> utf8Glyphs(std::string const& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        auto        c   = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}
/// `s` cut to `max` characters (never inside a code point).
[[nodiscard]] inline std::string truncateGlyphs(std::string const& s, int max) {
    auto g = utf8Glyphs(s);
    if (static_cast<int>(g.size()) <= max) return s;
    std::string out;
    for (int i = 0; i < max; ++i) out += g[static_cast<std::size_t>(i)];
    return out;
}

/// Dye index of a dye item ("minecraft:red_dye" -> 14), nullopt for other items.
[[nodiscard]] std::optional<int> dyeOf(std::string_view itemId);
/// Text colour (RGB) of a dye as used on display boards and nixie tubes.
[[nodiscard]] unsigned dyeRgb(int dye);
/// The nametag colour code (§x) closest to a dye (vanilla fallback text).
[[nodiscard]] std::string dyeFormatCode(int dye);

// ---- block helpers ----------------------------------------------------------------------------
using StateValue = std::variant<int, std::string>;
[[nodiscard]] std::optional<int>         intState(Block const& block, std::string_view name);
[[nodiscard]] std::optional<std::string> strState(Block const& block, std::string_view name);
/// `block` with the given states changed (unknown states are ignored); nullptr on failure.
[[nodiscard]] Block const* withStates(Block const& block, std::vector<std::pair<std::string, StateValue>> const& states);
/// The block at `pos` if its chunk is loaded.
[[nodiscard]] Block const* loadedBlock(BlockSource& region, BlockPos const& pos);
[[nodiscard]] std::string  idAt(BlockSource& region, GlobalPos const& pos);
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] GlobalPos    offsetPos(GlobalPos p, int face, int n = 1);
/// Face offsets (Bedrock FacingID order: down, up, north, south, west, east).
inline constexpr int kDx[6]{0, 0, 0, 0, -1, 1}, kDy[6]{-1, 1, 0, 0, 0, 0}, kDz[6]{0, 0, -1, 1, 0, 0};
/// Java 2D facing (0 S, 1 W, 2 N, 3 E) -> FacingID.
[[nodiscard]] int faceOfHorizontal(int facing);
/// Java Direction.getClockWise / getCounterClockWise on 2D facings.
[[nodiscard]] inline int clockwise(int f) { return (f + 1) & 3; }
[[nodiscard]] inline int counterClockwise(int f) { return (f + 3) & 3; }

/// Analog redstone power reaching `pos` from its neighbours (Level.getBestNeighborSignal for
/// vanilla emitters, read from their states; custom blocks get no neighbour updates, so callers
/// poll). `exceptFace`: a neighbour face to ignore (-1 = none).
[[nodiscard]] int signalAt(BlockSource& region, BlockPos const& pos, int exceptFace = -1);

// ---- players / feedback -----------------------------------------------------------------------
void actionBar(Player& player, std::string const& text);
void playSound(BlockSource& region, GlobalPos const& pos, std::string const& sound, float volume = 1, float pitch = 1);
/// Gives an item to the player (dropped at their feet when full).
void giveOrDrop(Player& player, ItemStack const& stack);
/// Drops an item at a block centre.
void dropAt(BlockSource& region, GlobalPos const& pos, ItemStack const& stack);
/// The level's tick counter.
[[nodiscard]] std::uint64_t gameTime();
/// Runs `fn` at the start of the next server tick (world edits after an interaction).
void defer(std::function<void()> fn);
/// The player by unique id (nullptr if gone).
[[nodiscard]] Player* playerById(std::int64_t id);

// ---- floating text (vanilla fallback of boards and nixie tubes) -------------------------------
/// Shows `text` as a name tag at `pos` (x, y, z in world coordinates); empty text removes it.
void setFloatingText(std::string const& key, int dim, std::array<double, 3> pos, std::string const& text);
void removeFloatingText(std::string const& key);
[[nodiscard]] int floatingTextCount();

// ---- display links ----------------------------------------------------------------------------
namespace link {
/// Data of a link in its BlockData (`data["display_link"]`).
struct LinkData {
    std::array<int, 3> targetOffset{0, 0, 0};
    int                line = 0;
    std::string        sourceId;
    Json               config = Json::object();
};
[[nodiscard]] std::optional<LinkData> read(GlobalPos const& pos);
void                                  write(GlobalPos const& pos, LinkData const& data);
/// The block the link reads from (the block its back faces).
[[nodiscard]] std::optional<GlobalPos> sourcePos(BlockSource& region, GlobalPos const& link);
[[nodiscard]] GlobalPos                targetPos(GlobalPos const& link, LinkData const& data);
/// Create's updateGatheredData: reads the source and writes the target now. Returns false when
/// the source or target is missing.
bool update(GlobalPos const& link);
/// Selects a source by id (screen action); "" deselects.
void selectSource(GlobalPos const& link, std::string const& sourceId);
/// Registers a link position (placement, load).
void track(GlobalPos const& link);
[[nodiscard]] std::set<GlobalPos> const& tracked();
/// Opens the configuration screen.
void openScreen(Player& player, GlobalPos const& link);
/// The link item's use on a block (selection / placement); returns true when handled (no
/// vanilla placement), false to let the game place the block.
bool useItem(Player& player, BlockSource& region, BlockPos const& clicked, int face);
/// Called after the game placed a link from the item.
void placed(Player& player, GlobalPos const& link);
/// Items passed / gatherer notifications.
void notify(GlobalPos const& source);
void itemsPassed(GlobalPos const& source, int count);
} // namespace link

// ---- display boards ---------------------------------------------------------------------------
namespace board {
struct Structure {
    GlobalPos controller;
    int       facing = 0; ///< Java 2D facing of the front
    int       width = 1, height = 1;
};
[[nodiscard]] bool                     isBoard(std::string_view id);
[[nodiscard]] std::optional<Structure> structureOf(BlockSource& region, GlobalPos const& pos);
/// Re-computes the up/down connections of the rows at and around `pos` (placement / removal).
void updateColumns(BlockSource& region, GlobalPos const& pos);
[[nodiscard]] int  maxChars(int width);
[[nodiscard]] bool running(GlobalPos const& controller);
/// Text of a line ("" when none).
[[nodiscard]] std::string lineText(GlobalPos const& controller, int line);
void setLine(BlockSource& region, GlobalPos const& controller, int line, std::string const& text, bool manual);
void setLineSections(BlockSource& region, GlobalPos const& controller, int line, Lines const& sections);
/// Right-click on a board (dye, glow ink, name tag, clipboard, empty hand). Returns true if used.
bool use(Player& player, BlockSource& region, GlobalPos const& pos, int face, Vec3 const& hit, ItemStack const& held);
void placed(Player& player, BlockSource& region, GlobalPos const& pos, std::optional<int> clickedFacing);
void removed(BlockSource& region, GlobalPos const& pos);
/// Board placement helper: a board item used on a board's front / back extends it.
std::optional<BlockPos> placementHelper(Player& player, BlockSource& region, GlobalPos const& pos, int face, Vec3 const& hit);
/// Line colours / glow for tests.
[[nodiscard]] Json state(GlobalPos const& controller);
void               syncAll(Player& player);
} // namespace board

// ---- nixie tubes ------------------------------------------------------------------------------
namespace nixie {
[[nodiscard]] bool        isNixie(std::string_view id);
[[nodiscard]] int         colourOf(std::string_view id);
[[nodiscard]] std::string idFor(int dye);
/// The tubes of the row through `pos`, left to right.
[[nodiscard]] std::vector<GlobalPos> row(BlockSource& region, GlobalPos const& pos);
/// The two characters a tube shows.
[[nodiscard]] std::string displayed(BlockSource& region, GlobalPos const& pos);
/// The full text of the row (custom text, or the redstone strength).
[[nodiscard]] std::string fullText(BlockSource& region, GlobalPos const& pos);
/// Writes custom text to the whole row (display link, clipboard, name tag).
void displayCustomText(BlockSource& region, GlobalPos const& pos, std::string const& text);
void clearCustomText(BlockSource& region, GlobalPos const& pos);
/// Re-reads the redstone power of a tube (forced: also when it shows custom text).
void updateRedstone(BlockSource& region, GlobalPos const& pos, bool force = false);
bool use(Player& player, BlockSource& region, GlobalPos const& pos, ItemStack const& held);
void placed(Player& player, BlockSource& region, GlobalPos const& pos, int face);
void removed(GlobalPos const& pos);
void track(GlobalPos const& pos);
void syncAll(Player& player);
} // namespace nixie

// ---- clipboards -------------------------------------------------------------------------------
namespace clipboard {
/// Create's ClipboardEntry.getLastViewedEntries: the entries of the page last opened.
[[nodiscard]] std::vector<Entry> lastViewedEntries(Contents const& c);
/// Clipboard item used on a block (copy settings, copy entries, open the screen). Returns true when
/// handled (no placement).
bool useOnBlock(Player& player, BlockSource& region, GlobalPos const& pos, int face, Vec3 const& hit);
/// Right-click in the air.
void useInAir(Player& player);
/// Left-click on a block with the clipboard: paste settings. Returns true when handled.
bool paste(Player& player, BlockSource& region, GlobalPos const& pos);
/// Right-click on a placed clipboard (open / sneak: pick up).
bool useBlock(Player& player, BlockSource& region, GlobalPos const& pos);
void placed(Player& player, BlockSource& region, GlobalPos const& pos, int face, ItemStack const& item);
/// Copy / paste of machine settings (value boxes, filter slots) without a player check.
[[nodiscard]] Json copySettings(BlockSource& region, GlobalPos const& pos);
bool               pasteSettings(Player& player, BlockSource& region, GlobalPos const& pos, Json const& copied, bool simulate);
/// Records the breaking player (creative breaks of empty clipboards drop nothing).
void noteBreak(Player& player, GlobalPos const& pos);
} // namespace clipboard

// ---- client sync ------------------------------------------------------------------------------
/// Sends a message to every companion in `dim`.
void broadcast(int dim, std::string const& channel, Json const& payload);

// ---- hooks ------------------------------------------------------------------------------------
void installHooks();
void uninstallHooks();

} // namespace lv::feature::display
