#pragma once

// Internals of the contraption unit shared by its .cpp files (not a contract for other units).

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/contraption/Assembly.h"
#include "feature/contraption/Contraption.h"

class Block;
class BlockPos;
class BlockSource;
class Player;
class Vec3;

namespace lv::feature::contraption {

/// `config/contraption.json` (Create's CKinetics keys where they exist).
struct Config {
    int  version           = 1;
    int  maxBlocksMoved    = 2048; ///< Create: maxBlocksMoved
    int  maxChassisRange   = 16;   ///< Create: maxChassisRange
    int  maxSuperGlueRange = 24;   ///< largest side of a super glue selection
    bool glueOutlines      = true; ///< particle outlines of glue regions for players holding super glue
    /// Blocks that never move, on top of the game's unbreakable blocks (Create's NON_MOVABLE tag).
    std::vector<std::string> unmovableBlocks{
        "minecraft:portal",
        "minecraft:end_portal",
        "minecraft:end_gateway",
        "minecraft:moving_block",
        "minecraft:piston_arm_collision",
        "minecraft:sticky_piston_arm_collision",
        "minecraft:structure_void",
    };
};

inline constexpr std::string_view kSuperGlue      = "levitate:super_glue";
inline constexpr std::string_view kLinearChassis  = "levitate:linear_chassis";
inline constexpr std::string_view kSecondaryChassis = "levitate:secondary_linear_chassis";
inline constexpr std::string_view kRadialChassis  = "levitate:radial_chassis";
inline constexpr std::string_view kSticker        = "levitate:sticker";

/// Display entities (falling blocks, which never rotate in vanilla) carry the contraption's
/// rotation in their own rotation: pitch = 20 * (axis + 1) marks them (and gives the axis),
/// yaw = the angle left after the quarter turns their block shows (-45..45 while a vanilla
/// player watches, any angle otherwise). The game sends rotations in 1/256 turns, so the pitch
/// is decoded by rounding.
inline constexpr float kDisplayPitchStep = 20.0f;
struct DisplayRotation {
    float pitch = 0, yaw = 0;
};
[[nodiscard]] inline DisplayRotation encodeDisplay(int axis, double residualDegrees) {
    return {kDisplayPitchStep * static_cast<float>(axis + 1), static_cast<float>(residualDegrees)};
}
/// The axis (0..2) of a display entity's rotation, or -1 if it isn't one.
[[nodiscard]] inline int displayAxis(float pitch) {
    int step = static_cast<int>(pitch / kDisplayPitchStep + (pitch >= 0 ? 0.5f : -0.5f));
    return step >= 1 && step <= 3 ? step - 1 : -1;
}
inline constexpr char kDisplayTag[] = "lv_contraption";

namespace detail {

// ---- game adapters (World.cpp) ----
[[nodiscard]] BlockSource* regionOf(int dim);
[[nodiscard]] cc::Pos      toPos(BlockPos const& p);
[[nodiscard]] BlockPos     toBlockPos(cc::Pos const& p);
[[nodiscard]] cc::States   statesOf(Block const& block);
[[nodiscard]] Block const* withStates(Block const& block, cc::States const& states);
[[nodiscard]] std::string  snbtOf(Block const& block);
[[nodiscard]] Block const* blockFromSnbt(std::string const& snbt);
[[nodiscard]] std::optional<std::int64_t> intStateOf(Block const& block, std::string const& name);
/// Create's movement checks for the block at `p`.
[[nodiscard]] cc::BlockInfo describe(BlockSource& region, int dim, cc::Pos const& p);
/// Collision box of a block relative to its position (nullopt: none).
[[nodiscard]] std::optional<cc::Box> collisionOf(BlockSource& region, cc::Pos const& p);
/// Collision box of `block` relative to its position, as if it stood at `at` (carried blocks).
[[nodiscard]] std::optional<cc::Box> collisionOfBlock(BlockSource& region, Block const& block, cc::Pos const& at);
/// Redstone power reaching `p` from its neighbours.
[[nodiscard]] bool powered(BlockSource& region, cc::Pos const& p);

void actionBar(Player& player, std::string const& text);
void playSound(int dim, cc::Vec3d const& pos, std::string const& sound, float volume = 1.f, float pitch = 1.f);
void particle(int dim, cc::Vec3d const& pos, std::string const& effect);
void dropItemAt(BlockSource& region, cc::Vec3d const& pos, Block const& block, std::string const& blockEntitySnbt);

// ---- glue (Glue.cpp) ----
[[nodiscard]] bool glued(int dim, cc::Pos const& a, cc::Dir d);
/// Glue regions holding any of `positions` (they move with the contraption as a whole, like
/// Create's glue entities), removed from the world.
std::vector<cc::GlueBox> takeGlue(int dim, std::unordered_set<cc::Pos, cc::PosHash> const& positions);
/// Super glue item use on a block: two-click selection (selftests call it directly).
void glueClick(Player& player, cc::Pos const& pos, bool sneaking);
/// Attack with super glue: removes the regions containing the block.
bool glueAttack(Player& player, cc::Pos const& pos);
/// Particle outlines of glue regions near a player holding super glue.
void glueOutlines(Player& player);
/// Runs `fn` at the start of the next server tick (world edits out of cancelled events).
void defer(std::function<void()> fn);

// ---- chassis (Chassis.cpp) ----
[[nodiscard]] cc::ChassisKind chassisKind(std::string_view id);
[[nodiscard]] int            storedChassisRange(int dim, cc::Pos const& p);
/// Slime ball / super glue / sneaking empty hand on a chassis face (Create's AbstractChassisBlock#use).
bool chassisUse(Player& player, int dim, cc::Pos const& pos, cc::Dir face, std::string const& heldId, bool sneaking);
/// Sets the axis / facing of a freshly placed chassis or sticker.
void orientPlaced(BlockSource& region, cc::Pos const& pos, cc::Dir clickedFace, Player& player);
/// Re-evaluates a sticker's power (selftests and the tracker).
void updateSticker(BlockSource& region, int dim, cc::Pos const& pos);
void trackSticker(int dim, cc::Pos const& pos);

// ---- engine (Engine.cpp) ----
/// Saves every contraption now (selftests).
void saveAll();
/// Drops in-memory contraptions and loads them again from the world DB (selftests).
void reloadAll();
/// Whether a display entity with this id belongs to a live contraption.
[[nodiscard]] bool ownsDisplay(std::int64_t uniqueId);
/// Server side: the falling block is one of our display entities (owned or orphaned).
[[nodiscard]] bool isDisplayActor(::Actor const& actor);
/// Selftests: behave as if a player without the client mod were in every dimension (displays
/// are spawned again as they move, see Engine::needsRespawn).
void forceVanillaViewers(bool on);

} // namespace detail
} // namespace lv::feature::contraption
