#pragma once

// Contraption engine (U4): moving block structures, Create style.
//
// An *anchor* (bearing, piston, pulley, gantry, cart assembler, station; the test anchor of this
// unit) collects blocks with `assemble()`. The blocks are removed from the world and carried by a
// contraption: block states, block entity NBT (chests, signs, ...) and per-block data of other
// units (through data carriers). Each tick the anchor sets the contraption's transform
// (translation, rotation about an axis, or following an actor) or lets it move by velocity;
// actors on it run, entities standing on it are carried, players collide with it and seated
// entities ride along. `disassemble()` puts the blocks back aligned to the grid (blocks that
// can't be placed drop as items).
//
// Server thread only. Never keep `Contraption&` across ticks: keep the `Id` (persisted with the
// world) and look it up with `find(id)`.
//
// Client rendering: each block is shown by a display entity (a vanilla falling block entity).
// The client mod moves the displays from the contraption's motion and turns them by its angle;
// vanilla clients see them spawned again in half-block steps (see docs/SPEC.md §3.4 and §8).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "core/contraption/Assembly.h"
#include "core/contraption/Geometry.h"
#include "core/contraption/Glue.h"
#include "core/contraption/StateRotation.h"

class Block;
class BlockSource;
class BlockPos;
class ItemStack;
class Player;
class Actor;

namespace lv::feature::contraption {

namespace cc = ::lv::core::contraption;
using Id     = std::uint64_t;

// ---- per-block data carried with the blocks ---------------------------------------------------

/// Moves per-position data of other units (U3's BlockData plugs in here) along with the blocks.
/// `capture` runs at assembly for every collected block (before it is removed) and returns the
/// data to carry (nullopt: nothing); it must also forget the data at the old position.
/// `restore` runs at disassembly after the block has been placed at its new position;
/// `rotate` (optional) adjusts carried data for a rotation by quarter turns about an axis.
struct DataCarrier {
    std::string                                                                       name;
    std::function<std::optional<nlohmann::json>(BlockSource&, core::GlobalPos const&)> capture;
    std::function<void(BlockSource&, core::GlobalPos const&, nlohmann::json const&)>  restore;
    std::function<void(nlohmann::json&, cc::Axis, int quarterTurns)>                    rotate;
};
void registerDataCarrier(DataCarrier carrier);

/// Rotates the block states of `blockId` for a placement rotated by quarter turns (on top of the
/// common states handled by `core::contraption::rotateStates`).
using RotationHandler = std::function<void(cc::States&, cc::Axis, int quarterTurns)>;
void registerRotationHandler(std::string blockId, RotationHandler handler);

// ---- movement checks (Create's BlockMovementChecks) -------------------------------------------

/// Lets a unit describe how its blocks attach. Each function is optional. `states` are the
/// block's states (`levitate:facing`, ...).
struct MovementRules {
    std::function<bool(cc::States const&)>          movable;         ///< default: true
    std::function<bool(cc::States const&)>          brittle;         ///< default: false
    std::function<cc::DirMask(cc::States const&)>   attachedTowards; ///< faces it hangs on
    std::function<cc::DirMask(cc::States const&)>   notSupportive;   ///< faces that don't carry
};
void registerMovementRules(std::string blockId, MovementRules rules);

// ---- actors (Create's MovementBehaviour) ------------------------------------------------------

class Contraption;

/// What an actor sees each tick.
struct MovementContext {
    Contraption&    contraption;
    cc::Pos         localPos;     ///< the actor block in contraption coordinates
    core::GlobalPos worldPos;     ///< the block it is in now
    cc::Vec3d       position;     ///< its center in world coordinates
    cc::Vec3d       motion;       ///< world-space movement of the center this tick
    cc::States&     states;       ///< its block states (read only by convention)
    nlohmann::json& data;         ///< actor data saved with the contraption
    bool            stall = false; ///< set by an actor to stop the anchor (e.g. a drill breaking)
};

struct MovementBehaviour {
    std::function<void(MovementContext&)>                        startMoving;
    std::function<void(MovementContext&)>                        tick;
    std::function<void(MovementContext&, core::GlobalPos const&)> visitNewPosition;
    std::function<void(MovementContext&)>                        stopMoving;
    std::function<bool(MovementContext const&)>                  isActive; ///< default: true
    /// Create runs every actor's tick each tick; the engine runs `tick` only while the contraption
    /// moves, and also while it stands still when this is set (doors opening at a station).
    bool tickWhileStill = false;
};
/// Registers the movement behaviour of block `blockId`.
void registerActor(std::string blockId, MovementBehaviour behaviour);

// ---- seats ------------------------------------------------------------------------------------

/// Blocks entities can sit on while moving (U22's seats register themselves).
void registerSeat(std::string blockId, double sittingHeight = 0.5);

// ---- mounted storage --------------------------------------------------------------------------

/// A fluid tank carried on a contraption (U17/U18/U19 adapt their tanks: read/write the carried data).
struct FluidTank {
    std::string fluid;
    int         amount   = 0; ///< mB
    int         capacity = 0; ///< mB
};
struct FluidStorageAdapter {
    std::function<std::optional<FluidTank>(nlohmann::json const& carried)> read;
    std::function<void(nlohmann::json& carried, FluidTank const&)>          write;
};
void registerFluidStorage(std::string blockId, FluidStorageAdapter adapter);

/// The merged inventories and tanks of a contraption (Create's MountedStorageManager).
class Storage {
public:
    virtual ~Storage() = default;
    /// Inserts into the merged inventories; returns what didn't fit.
    virtual ItemStack insertItem(ItemStack const& stack, bool simulate) = 0;
    /// Extracts up to `count` items matching `filter` (null: any).
    virtual ItemStack extractItem(std::function<bool(ItemStack const&)> const& filter, int count, bool simulate) = 0;
    [[nodiscard]] virtual int slotCount() const = 0;
    [[nodiscard]] virtual ItemStack const& slot(int i) const = 0;
    /// Fills tanks with `fluid`; returns the amount accepted.
    virtual int fill(std::string const& fluid, int amount, bool simulate) = 0;
    /// Drains up to `amount` of `fluid` ("" = any); returns {fluid, drained}.
    virtual std::pair<std::string, int> drain(std::string const& fluid, int amount, bool simulate) = 0;
};

// ---- the contraption --------------------------------------------------------------------------

/// How the contraption moves when the anchor doesn't set the transform itself.
struct Motion {
    cc::Vec3d                   velocity;        ///< blocks per tick
    double                      angularVelocity = 0; ///< degrees per tick about the transform axis
    std::optional<std::int64_t> follow;          ///< ActorUniqueID: keep the follow offset to it
    cc::Vec3d                   followOffset;    ///< anchor offset from the followed actor
    int                         ticks = -1;      ///< stop after this many ticks (-1: until stopped)
};

struct CarriedBlock {
    cc::Pos                    local;
    std::string                block;      ///< block NBT (SNBT): name + states
    std::string                blockEntity; ///< block entity NBT (SNBT), "" if none
    nlohmann::json             data = nlohmann::json::object(); ///< carrier name -> data
    nlohmann::json             actorData = nlohmann::json::object();
    std::optional<cc::Box>     collision; ///< local collision box (nullopt: walk through)
    std::int64_t               display = 0; ///< display entity (ActorUniqueID)
    bool                       brittle = false; ///< placed after the solid blocks
    std::optional<cc::Pos>     visited;     ///< actors: world block visited last
    std::string                id;          ///< block id (cache of `block`)
    cc::States                 states;      ///< decoded from `block` (cache)
};

class Contraption {
public:
    [[nodiscard]] Id                       id() const { return mId; }
    [[nodiscard]] int                      dimension() const { return mDim; }
    [[nodiscard]] std::string const&       kind() const { return mKind; }
    [[nodiscard]] core::GlobalPos const&   anchor() const { return mAnchor; }
    [[nodiscard]] cc::Transform const&     transform() const { return mTransform; }
    [[nodiscard]] std::vector<CarriedBlock> const& blocks() const { return mBlocks; }
    [[nodiscard]] Motion const&            motion() const { return mMotion; }
    [[nodiscard]] Storage&                 storage();
    [[nodiscard]] nlohmann::json&          anchorData() { return mAnchorData; } ///< saved with it

    /// Sets the transform for the next tick (anchors that compute their own position).
    void setTransform(cc::Transform const& t) { mTarget = t; }
    void setMotion(Motion const& m) { mMotion = m; }
    /// Pauses the actors (no tick / visitNewPosition while moving) without stopping the motion:
    /// Create's isActorActive of a cart in "Pause actors while rotating" mode while it turns.
    void setActorsPaused(bool paused) { mActorsPaused = paused; }
    [[nodiscard]] bool actorsPaused() const { return mActorsPaused; }
    void stop() { mMotion = {}; mTarget.reset(); }
    /// World position of a local point at the current transform.
    [[nodiscard]] cc::Vec3d toWorld(cc::Vec3d const& local) const { return mTransform.toWorld(local); }
    /// The world block a local block would be placed at now (grid aligned).
    [[nodiscard]] core::GlobalPos placement(cc::Pos const& local) const;
    /// Seats `actorUniqueId` on the seat at `local` (false if no free seat there).
    bool seat(cc::Pos const& local, std::int64_t actorUniqueId);
    void unseat(std::int64_t actorUniqueId);
    /// The seated entities (seat block, ActorUniqueID). A rider that rides a vehicle (a seat
    /// entity) is carried by moving that vehicle, so it stays mounted.
    [[nodiscard]] std::vector<std::pair<cc::Pos, std::int64_t>> const& seated() const { return mSeated; }
    /// Writable carried data (carrier name -> data) of the block at `local`, persisted with the
    /// contraption (e.g. contraption controls toggling actors); nullptr when no block is there.
    [[nodiscard]] nlohmann::json* blockData(cc::Pos const& local) {
        for (auto& b : mBlocks)
            if (b.local == local) return &b.data;
        return nullptr;
    }
    /// Writable actor data (MovementContext::data) of the block at `local`; nullptr when none.
    [[nodiscard]] nlohmann::json* actorData(cc::Pos const& local) {
        for (auto& b : mBlocks)
            if (b.local == local) return &b.actorData;
        return nullptr;
    }

private:
    friend class Engine;
    Id                                mId  = 0;
    int                               mDim = 0;
    std::string                       mKind;
    core::GlobalPos                   mAnchor;
    cc::Transform                     mTransform;
    std::optional<cc::Transform>      mTarget;
    Motion                            mMotion;
    std::vector<CarriedBlock>         mBlocks;
    std::vector<cc::GlueBox>          mGlue; ///< local coordinates, re-applied at disassembly
    std::vector<std::pair<cc::Pos, std::int64_t>> mSeated;
    nlohmann::json                    mAnchorData = nlohmann::json::object();
    std::unique_ptr<Storage>          mStorage;
    bool                              mMoving = false;
    bool                              mActorsPaused = false;
    std::vector<cc::Box>              mBoxes;           ///< local collision boxes (cache)
    std::vector<Block const*>         mBoxBlocks;       ///< the block of each collision box (cache)
    bool                              mTicksWhileStill = false; ///< an actor ticks while it stands still (cache)
    cc::Box                           mBounds;          ///< local bounds of all blocks (cache)
    int                               mDisplayQuarter = 0; ///< quarter turns shown by the display entities
};

// ---- anchors' API -----------------------------------------------------------------------------

struct AssembleOptions {
    core::GlobalPos        anchor;     ///< the anchor block (never moved, local origin)
    std::vector<cc::Pos>   start;      ///< world positions to start from (empty: the anchor itself, moved too)
    std::optional<cc::Dir> forced;     ///< linear anchors: movement direction (pistons push the line)
    cc::Axis               axis = cc::Axis::Y; ///< rotation axis for rotating anchors
    cc::Vec3d              pivot{0.5, 0.5, 0.5}; ///< rotation center, local (anchor block) coordinates
    std::string            kind = "test"; ///< owner kind ("bearing", "piston", ...)
    bool                   includeAnchor = false; ///< also move the anchor block itself
};

struct AssembleResult {
    std::optional<Id> id;
    std::string       errorKey;  ///< translation key (create.gui.assembly.exception.*)
    std::string       errorText; ///< translated message (default locale)
    std::vector<std::string> errorArgs;
    [[nodiscard]] bool ok() const { return id.has_value(); }
};

/// Collects and lifts the structure. Errors leave the world untouched.
AssembleResult assemble(AssembleOptions const& options);
/// The contraption (nullptr if gone). Don't keep the pointer across ticks.
[[nodiscard]] Contraption* find(Id id);
/// Places the blocks back (grid aligned); drops what can't be placed. Returns false if unknown.
bool disassemble(Id id);
/// Removes the contraption without placing or dropping anything (Create's entity discard: a
/// picked up minecart contraption, `/create killtrain`). Actors get `stopMoving`. False if unknown.
bool discard(Id id);
/// Changes the states of the carried block at `local` (Create's contraption entity setBlock:
/// a door opening on a train). Its collision and display follow. False if unknown or unchanged.
bool setBlockStates(Id id, cc::Pos const& local, cc::States const& states);
/// Local bounds of the carried blocks (Create's contraption bounds); nullopt if unknown.
[[nodiscard]] std::optional<cc::Box> bounds(Id id);
/// All live contraptions (ids).
[[nodiscard]] std::vector<Id> all();

/// Client mod: the animation state of an actor block (Create's actor visuals: speed, progress,
/// ...), sent to the companions in the contraption's dimension (and again to ones that join);
/// the client reads it with client/contraption/ContraptionClient.h. Call it when it changes, not
/// every tick. Null clears it.
void setActorVisual(Id id, cc::Pos const& local, nlohmann::json visual);

// ---- super glue -------------------------------------------------------------------------------

/// Glue regions of a dimension (world coordinates).
[[nodiscard]] std::vector<cc::GlueBox> glueRegions(int dim);
void addGlue(int dim, cc::GlueBox const& box);
/// Removes every region containing `pos`; returns how many.
int removeGlueAt(int dim, cc::Pos const& pos);
/// Removes every region within `radius` blocks of `center`; returns how many.
int clearGlue(int dim, cc::Pos const& center, int radius);

// ---- chassis ----------------------------------------------------------------------------------

/// The range of the chassis at `pos` (default: maxChassisRange / 2).
[[nodiscard]] int chassisRange(core::GlobalPos const& pos);
/// Sets the range of a chassis and of its connected chassis group (as Create's bulk value).
void setChassisRange(core::GlobalPos const& pos, int range);

} // namespace lv::feature::contraption
