#pragma once

// Contraption actors (U22, SPEC §2.18 / §3.6.22): mechanical harvester, mechanical plough,
// mechanical roller, portable storage interface, contraption controls and the 16 seats.
//
// Internal header of the unit (not a contract for other units). Every actor registers itself
// with `contraption::registerActor` (Create's MovementBehaviour); stationary behaviour (PSI
// transfers, seats, controls) runs in the unit's module.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "feature/contraption/Contraption.h"

class Actor;
class Block;
class BlockPos;
class BlockSource;
class ItemStack;
class Player;

namespace lv::feature::actors {

namespace cc  = ::lv::core::contraption;
namespace con = ::lv::feature::contraption;
using Json    = nlohmann::json;
using core::GlobalPos;

inline constexpr std::string_view kHarvester = "levitate:mechanical_harvester";
inline constexpr std::string_view kPlough    = "levitate:mechanical_plough";
inline constexpr std::string_view kRoller    = "levitate:mechanical_roller";
inline constexpr std::string_view kPsi       = "levitate:portable_storage_interface";
inline constexpr std::string_view kControls  = "levitate:contraption_controls";
inline constexpr std::string_view kSeatEntity = "levitate:seat";
/// The seat entity players ride (another rider offset: Bedrock places a riding player lower).
inline constexpr std::string_view kPlayerSeatEntity = "levitate:player_seat";
[[nodiscard]] inline bool isSeatEntityId(std::string_view id) { return id == kSeatEntity || id == kPlayerSeatEntity; }
inline constexpr char             kFacing[]  = "levitate:facing";

/// Create's 16 dye colours in their order (white first).
inline constexpr std::string_view kColours[16]{
    "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
    "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
};

/// `config/actors.json`: Create's keys (CKinetics / CLogistics) for this unit's blocks.
struct Config {
    int  version               = 1;
    bool harvestPartiallyGrown = false; ///< CKinetics.harvestPartiallyGrown
    bool harvesterReplants     = true;  ///< CKinetics.harvesterReplants
    bool moveItemsToStorage    = true;  ///< CKinetics.moveItemsToStorage
    int  rollerFillDepth       = 12;    ///< CKinetics.rollerFillDepth
    int  psiTimeout            = 60;    ///< CLogistics.psiTimeout (ticks)
    bool seatHostileMobs       = true;  ///< CLogistics.seatHostileMobs
};

// ---- blocks -----------------------------------------------------------------------------------

[[nodiscard]] bool isSeat(std::string_view id);
/// "levitate:<colour>_seat" -> colour index (nullopt for other blocks).
[[nodiscard]] std::optional<int> seatColour(std::string_view id);
[[nodiscard]] std::string        seatId(int colour);
/// The front of an actor block (`levitate:facing`, Bedrock facing order).
[[nodiscard]] cc::Dir facingOf(cc::States const& states, cc::Dir fallback = cc::Dir::North);
[[nodiscard]] cc::Dir facingAt(BlockSource& region, BlockPos const& pos, cc::Dir fallback = cc::Dir::North);
[[nodiscard]] BlockPos toBlockPos(cc::Pos const& p);
[[nodiscard]] BlockPos toBlockPos(GlobalPos const& p);
[[nodiscard]] cc::Pos  toPos(BlockPos const& p);
[[nodiscard]] BlockSource* regionOf(int dim);
/// Entities (players included: the region's entity query leaves them out) whose box meets
/// the box `lo`..`hi`.
[[nodiscard]] std::vector<Actor*> entitiesIn(BlockSource& region, cc::Vec3d const& lo, cc::Vec3d const& hi);
/// Debounce of repeated use packets while the button is held: true when `key` acted within
/// the last 250 ms (and remembers this use otherwise).
[[nodiscard]] bool recentlyUsed(std::int64_t key);
/// Sets one integer state of the block at `pos` (keeps the others).
bool setIntState(BlockSource& region, BlockPos const& pos, std::string const& state, int value);

// ---- items ------------------------------------------------------------------------------------

[[nodiscard]] ItemStack makeItem(std::string const& id, int count = 1);
/// Spawns an item entity at `pos` with `velocity`.
void spawnItem(BlockSource& region, ItemStack const& item, cc::Vec3d const& pos, cc::Vec3d const& velocity);
/// The drops of a block broken with `tool` (empty stack: no tool).
[[nodiscard]] std::vector<ItemStack> dropsOf(BlockSource& region, BlockPos const& pos, Block const& block, ItemStack const& tool);

// ---- actor helpers (Create's MovementBehaviour / BlockBreakingMovementBehaviour) ---------------

/// Front of the actor in world space (the contraption's rotation applied).
[[nodiscard]] cc::Vec3d worldFacing(con::MovementContext const& ctx);
/// Movement in the contraption's own frame (Create's relativeMotion).
[[nodiscard]] cc::Vec3d relativeMotion(con::MovementContext const& ctx);
/// Create's VecHelper.isVecPointingTowards(vec, dir).
[[nodiscard]] bool pointingTowards(cc::Vec3d const& vec, cc::Dir dir);
/// Create's MovementContext.getAnimationSpeed (magnitude).
[[nodiscard]] float animationSpeed(con::MovementContext const& ctx);
/// Create's active area: the block at the actor's center + `localOffset` (contraption frame,
/// rotated with it). Returns it when it differs from the one visited last (kept in ctx.data
/// under `key`), nullopt otherwise.
[[nodiscard]] std::optional<GlobalPos> newAreaPos(con::MovementContext& ctx, cc::Vec3d const& localOffset, char const* key = "Area");
/// The world point of the active area.
[[nodiscard]] cc::Vec3d areaPoint(con::MovementContext const& ctx, cc::Vec3d const& localOffset);
/// Create's collectOrDropItem:into contraption storage (moveItemsToStorage), the rest dropped.
void collectOrDrop(con::MovementContext& ctx, ItemStack const& stack);
/// Create's damageEntities: hurts (damage > 0) and/or throws the entities in a block.
void damageEntities(con::MovementContext& ctx, GlobalPos const& pos, bool hurt, bool roller);
/// Death message of the roller's damage source (Create's CreateDamageSources.roller), a
/// translation key of the resource pack (actors.lang).
inline constexpr char kRollerDeathMessage[] = "death.attack.create.mechanical_roller";
/// Plays the block's own hit sound at `pos` (Create's getSoundType().getHitSound()) for the
/// players nearby: the game's hit sound event, resolved by every client from the block.
void playHitSound(int dim, BlockPos const& pos, Block const& block);

/// Create's BlockBreakingMovementBehaviour state in the actor data.
struct Breaker {
    /// Speed of breaking (Create: |animation speed| / 500, clamped to [1/128, 16]); the roller
    /// multiplies by 1.5.
    std::function<float(con::MovementContext const&)>                     speed;
    std::function<bool(BlockSource&, BlockPos const&, Block const&)>         canBreak;
    /// Called when the block broke (drops already handled by `onDrops`), e.g. to revisit.
    std::function<void(con::MovementContext&, GlobalPos const&)>            onBroken;
    /// What happens to the drops (default: collectOrDrop each).
    std::function<void(con::MovementContext&, Block const&, BlockPos const&)> breakBlock;
};
/// Starts breaking at `pos` when `canBreak` says so (sets ctx.stall). Returns true if it did.
bool startBreaking(con::MovementContext& ctx, Breaker const& b, GlobalPos const& pos);
/// Progresses the current break (call from `tick`). Keeps ctx.stall while breaking.
void tickBreaking(con::MovementContext& ctx, Breaker const& b);
void cancelBreaking(con::MovementContext& ctx);
/// Default block breaking: break particles and drops collected or dropped (no tool).
void destroyAndCollect(con::MovementContext& ctx, Block const& block, BlockPos const& pos);
/// Whether a block can be broken at all (not air, not a liquid, not unbreakable).
[[nodiscard]] bool isBreakable(Block const& block);
[[nodiscard]] bool isLiquid(std::string const& id);
[[nodiscard]] bool hasCollision(BlockSource& region, BlockPos const& pos);

// ---- contraption controls (Controls.cpp) ------------------------------------------------------

/// Whether actors of `blockId` are disabled on contraption `id` by its controls.
[[nodiscard]] bool actorDisabled(con::Id id, std::string const& blockId);
/// Registers an actor of this unit, wrapped with the controls check (Create: isActive = !disabled).
void registerOwnActor(std::string const& blockId, con::MovementBehaviour behaviour);
/// Wraps the actors of other units found on `c` with the controls check.
void wrapActorsOf(con::Contraption const& c);
/// A player's click on contraption controls carried by a contraption.
void controlsClickOnContraption(Player& player, con::Contraption& c, cc::Pos const& local);
/// A player's click on stationary controls.
void controlsClickStationary(Player& player, GlobalPos const& pos);
void controlsTick(std::uint64_t tick);
/// Starts polling the redstone power of stationary controls at `pos`.
void trackStationaryControls(GlobalPos const& pos);
void controlsInstall();
void controlsLoad();
void controlsSave();
void controlsClear();

// ---- portable storage interface (Psi.cpp) -----------------------------------------------------

void psiTick();
void psiInstall();
void psiClear();
/// Whether the stationary PSI at `pos` is connected (selftests).
[[nodiscard]] bool psiConnected(GlobalPos const& pos);

// ---- seats (Seats.cpp) ------------------------------------------------------------------------

/// Right-click on a seat block. Returns true when handled (the vanilla use is cancelled).
bool seatUse(Player& player, GlobalPos const& pos, ItemStack const& held);
/// Seats `entity` on the seat block at `pos` (Create's SeatBlock.sitDown).
bool sitDown(Actor& entity, GlobalPos const& pos);
/// Create's SeatBlock.canBePickedUp.
[[nodiscard]] bool canBePickedUp(Actor const& entity);
/// Whether an entity sits on the seat block at `pos`.
[[nodiscard]] Actor* seatedAt(GlobalPos const& pos);
void seatsTick(std::uint64_t tick);
/// Puts a rider seated on a contraption seat on a seat entity there (the engine moves it with
/// the seat, so the rider stays seated while the contraption moves).
void mountOnContraptionSeat(con::Contraption& c, cc::Pos const& local, Actor& rider);
/// Gets an entity off the seat entity it rides (removing that seat entity).
void leaveSeat(Actor& rider);
void seatsInstall();
void seatsUninstall();

// ---- roller (Roller.cpp) ----------------------------------------------------------------------

enum class RollingMode : int { TunnelPave = 0, StraightFill = 1, WideFill = 2 };
[[nodiscard]] RollingMode rollerMode(GlobalPos const& pos);
void                      setRollerMode(GlobalPos const& pos, RollingMode mode, bool share = true);
void                      rollerPlaced(GlobalPos const& pos);
void                      rollerInstall();

// ---- interaction with blocks carried by contraptions (Interact.cpp) ---------------------------

struct ContraptionHit {
    con::Id   id = 0;
    cc::Pos   local;
    double    distance = 0;
    std::string block;
};
/// The nearest contraption block of interest (controls / seats) the player looks at within reach.
[[nodiscard]] std::optional<ContraptionHit> raycastContraption(Player& player, double reach);
/// Handles a use on a contraption block (controls, seats). Returns true if handled.
bool useOnContraption(Player& player);
bool useOnContraption(Player& player, ContraptionHit const& hit);

// ---- stats --------------------------------------------------------------------------------------

/// Logs one line per user-visible action (`actors: ...`).
void logAction(std::string const& text);

} // namespace lv::feature::actors
