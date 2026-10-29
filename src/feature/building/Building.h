#pragma once

// Building blocks (U10, docs/SPEC.md §2.5 / §3.6.1): casings, connected glass, windows, panes,
// bars, copper shingles / tiles, scaffolding, ladders, sliding doors, trapdoors, girders,
// brackets, table cloths, placards and copycats.
//
// Internal header of the unit (server thread unless noted). Block state conventions are shared
// with tools/gen/building.ps1:
//   levitate:facing  0..5 down, up, north, south, west, east (Java Direction order);
//                    horizontal blocks use 2..5
//   levitate:c_<dir> connected-texture neighbours, levitate:<dir> pane arms

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/GlobalPos.h"

class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;
class Vec3;

namespace lv::feature::building {

using core::GlobalPos;

// ---- directions (Java Direction order, also Bedrock's face index) --------------------------------
enum Dir : int { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };
inline constexpr std::array<int, 6> kDx{0, 0, 0, 0, -1, 1};
inline constexpr std::array<int, 6> kDy{-1, 1, 0, 0, 0, 0};
inline constexpr std::array<int, 6> kDz{0, 0, -1, 1, 0, 0};
inline constexpr std::array<char const*, 6> kDirNames{"down", "up", "north", "south", "west", "east"};
[[nodiscard]] constexpr int opposite(int d) { return d ^ 1; }
[[nodiscard]] constexpr int axisOf(int d) { return d < 2 ? 1 : (d < 4 ? 2 : 0); } ///< 0 x, 1 y, 2 z
[[nodiscard]] constexpr bool horizontal(int d) { return d >= 2; }
/// Clockwise (seen from above): north -> east -> south -> west.
[[nodiscard]] constexpr int clockwise(int d) {
    switch (d) {
    case North: return East;
    case East: return South;
    case South: return West;
    case West: return North;
    default: return d;
    }
}
[[nodiscard]] constexpr int counterClockwise(int d) { return clockwise(clockwise(clockwise(d))); }
/// Quarter turns clockwise from north (north 0, east 1, south 2, west 3).
[[nodiscard]] constexpr int quarterOf(int d) { return d == North ? 0 : d == East ? 1 : d == South ? 2 : 3; }
[[nodiscard]] GlobalPos offset(GlobalPos const& p, int dir, int n = 1);

// ---- catalog ---------------------------------------------------------------------------------------
enum class Kind : std::uint8_t {
    None,
    Casing,       ///< 6-way connected texture
    CtGlass,      ///< framed glass (omni / horizontal / vertical connected textures)
    Window,       ///< vertical connected texture
    TiledGlass,
    Pane,         ///< glass and window panes
    Bars,
    CopperBlock,
    CopperStairs,
    CopperSlab,
    Scaffold,
    Ladder,
    Door,
    Trapdoor,
    Girder,
    GirderShaft,
    Bracket,
    TableCloth,
    Placard,
    CopycatPanel,
    CopycatStep,
};
enum class Ct : std::uint8_t { None, Omni, Horizontal, Vertical };

struct Info {
    std::string id; ///< "levitate:<path>"
    Kind        kind = Kind::None;
    Ct          ct   = Ct::None;
    bool        folds = false; ///< folding sliding door (Create: andesite and copper doors)
    bool        glass = false; ///< glass sound set (framed glass door / trapdoor)
    // copper roofing
    int         age   = 0;     ///< 0 fresh, 1 exposed, 2 weathered, 3 oxidized
    bool        waxed = false;
    std::string copperShape;   ///< "copper_shingles", "copper_shingle_stairs", ...
};

/// The building block `id` is, nullptr if it isn't one.
[[nodiscard]] Info const* infoOf(std::string_view id);
[[nodiscard]] Info const* infoOf(Block const& block);
[[nodiscard]] std::vector<Info const*> const& allBlocks();
/// The copper id of `shape` at `age`, waxed or not ("levitate:waxed_exposed_copper_shingles").
[[nodiscard]] std::string copperId(std::string const& shape, int age, bool waxed);

inline constexpr char kShaftId[]          = "levitate:shaft";
inline constexpr char kWrenchId[]         = "levitate:wrench";
inline constexpr char kGirderId[]         = "levitate:metal_girder";
inline constexpr char kGirderShaftId[]    = "levitate:metal_girder_encased_shaft";
inline constexpr char kPlacardId[]        = "levitate:placard";
inline constexpr char kDataKey[]          = "building"; ///< BlockData top-level key

// ---- block states -----------------------------------------------------------------------------------
[[nodiscard]] int stateOf(Block const& block, std::string_view name, int fallback = 0);
/// The horizontal facing (2..5) of a horizontal block; the default 0 / 1 reads as north.
[[nodiscard]] int hfacingOf(Block const& block);
/// `block` with integer states changed (unknown states are ignored).
[[nodiscard]] Block const* withStates(Block const& block, std::initializer_list<std::pair<std::string_view, int>> states);
[[nodiscard]] Block const* withStates(Block const& block, std::vector<std::pair<std::string, int>> const& states);
/// The default block of `id` carrying over the states `from` has (copper conversions).
[[nodiscard]] Block const* convertKeepingStates(Block const& from, std::string const& id);

// ---- world helpers ---------------------------------------------------------------------------------------
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] Block const* blockAt(GlobalPos const& p); ///< nullptr when not loaded
[[nodiscard]] std::string idAt(GlobalPos const& p);
bool set(GlobalPos const& p, Block const& block);
bool clear(GlobalPos const& p);
[[nodiscard]] bool replaceableAt(GlobalPos const& p);
/// The face `face` of the block at `p` can hold a ladder / torch (Create's isFaceSturdy).
[[nodiscard]] bool sturdy(GlobalPos const& p, int face);
[[nodiscard]] bool solidAt(GlobalPos const& p);
/// Create's PlacardBlock.canAttachLenient: the block at `p` has a collision shape.
[[nodiscard]] bool canHang(GlobalPos const& p);
/// The player may change the world (Java mayBuild).
[[nodiscard]] bool mayBuild(Player const& player);
/// The block at `p` receives redstone power from a neighbour (Java hasNeighborSignal).
[[nodiscard]] bool hasNeighborSignal(GlobalPos const& p);
void playSound(GlobalPos const& p, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void particle(GlobalPos const& p, std::string const& effect);
/// Drops `count` items of `id` at the centre of `p`.
void drop(GlobalPos const& p, ItemStack const& item);
[[nodiscard]] ItemStack makeItem(std::string_view id, int count = 1);
/// Gives the item to the player (what doesn't fit drops at its feet).
void give(Player& player, ItemStack const& item);
/// Uses one of the held item (nothing in creative).
void consumeHeld(Player& player);
[[nodiscard]] Player* playerByUid(std::int64_t uid);
/// Java's nearest looking directions, nearest first.
[[nodiscard]] std::array<int, 6> nearestLooking(Player const& player);
/// The horizontal direction the player looks along (Java getHorizontalDirection).
[[nodiscard]] int horizontalFacing(Player const& player);

// ---- unit logic -----------------------------------------------------------------------------------------
/// Runs `fn` at the start of the next server tick (world edits outside of game callbacks).
void defer(std::function<void()> fn);
/// Re-reads the connection states of the block at `p` and its neighbours (next tick).
void markDirty(GlobalPos const& p);
/// Recomputes the connection-dependent states of the block at `p` now. Returns true if changed.
bool updateConnections(GlobalPos const& p);

/// A use of `item` on the block at `pos` (face, hit inside the block 0..1). Returns true when the
/// unit handled it (block interaction or special item placement): the game must not go on.
bool onUse(Player& player, ItemStack const& item, GlobalPos const& pos, int face, Vec3 const& hit, bool firstEvent);
/// After the game placed one of our blocks at `pos` from a click on `face`: orientation, second
/// door half, survival checks.
void onPlaced(Player& player, GlobalPos const& pos, int face, Vec3 const& hit);
/// Left click on a block (Java attack): placard item removal. True to cancel the destruction.
bool onAttack(Player& player, GlobalPos const& pos);
/// A block of ours changed or vanished (any cause): doors, tracked sets, drops of attached data.
void onBlockChanged(GlobalPos const& pos, Block const& previous, Block const& now);
/// Remembers that a player in creative mode broke the block at `pos` (no drops for door halves).
void notePlayerBreak(GlobalPos const& pos, bool creative);

void tick();
void loadWorld();
void saveWorld();
void closeWorld();

// copper
bool tryWaxOrScrape(Player& player, ItemStack const& item, GlobalPos const& pos);
void copperTick();
void trackCopper(GlobalPos const& pos);
/// One random tick of the copper block at `pos` (Java WeatheringCopper.changeOverTime with a
/// fixed random source for tests). Returns true if it aged.
bool copperRandomTick(GlobalPos const& pos, std::function<float()> const& random);

// doors and trapdoors
bool useDoor(Player* player, GlobalPos const& pos);
bool useTrapdoor(Player* player, GlobalPos const& pos);
void trackRedstone(GlobalPos const& pos);
void redstoneTick();

// placard
bool usePlacard(Player& player, ItemStack const& item, GlobalPos const& pos);
bool attackPlacard(Player& player, GlobalPos const& pos);
void placardTick();
/// A powered placard without a running pulse (saved mid-pulse) gets a fresh one.
void ensurePlacardPulse(GlobalPos const& pos);
[[nodiscard]] std::optional<std::string> placardItem(GlobalPos const& pos);

// copycats and brackets
bool useCopycat(Player& player, ItemStack const& item, GlobalPos const& pos);
bool useBracket(Player& player, ItemStack const& item, GlobalPos const& pos, int face);
[[nodiscard]] std::optional<std::string> copycatMaterial(GlobalPos const& pos);
[[nodiscard]] std::optional<std::string> bracketAt(GlobalPos const& pos);

// shapes (collision boxes in block pixels, x0 y0 z0 x1 y1 z1)
using Box = std::array<float, 6>;
/// The collision boxes of our block (nullopt: use the block's JSON box). `aboveFeet`: the
/// entity's feet are at or above the block's top; `descending`: it sneaks (scaffolding).
[[nodiscard]] std::optional<std::vector<Box>> collisionBoxes(Info const& info, Block const& block, bool aboveFeet, bool descending);
void installShapes();
void uninstallShapes();
void applyBlockProperties();

void installHooks();
void uninstallHooks();

} // namespace lv::feature::building
