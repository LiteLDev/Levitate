#pragma once

// Additions to the contraption contract for linear anchors (added by U20 pistons; the wave-1
// header Contraption.h is unchanged). Pistons, pulleys and gantries need Create's movement checks
// of single blocks (their push line, Create's addToInitialFrontier) and Create's world collision
// test of translating contraptions (ContraptionCollider.isCollidingWithWorld).
//
// Server thread only.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/contraption/Assembly.h"
#include "feature/contraption/Contraption.h"

class BlockSource;

namespace lv::feature::contraption::ext {

/// Create's BlockMovementChecks for the block at `pos` (what the engine's assembly sees).
[[nodiscard]] cc::BlockInfo describe(int dim, cc::Pos const& pos);
/// The block at `pos` has a collision shape.
[[nodiscard]] bool hasCollision(int dim, cc::Pos const& pos);
/// Redstone power reaching `pos` from its neighbours (the sticker's polling, SPEC §8).
[[nodiscard]] bool powered(int dim, cc::Pos const& pos);

/// `maxChassisRange` of `config/contraption.json` (Create also limits a piston's push line by it).
[[nodiscard]] int maxChassisRange();

/// Runs the engine's assembly rules without touching the world (what `assemble(options)` would
/// collect), with `adjust` able to change what a position looks like (e.g. a block an anchor
/// will put there first, or anchoring blocks: `info.anchor = true`).
[[nodiscard]] cc::AssemblyResult
dryRun(AssembleOptions const& options, std::function<void(cc::Pos const&, cc::BlockInfo&)> const& adjust = nullptr);
/// The error message of a failed dry run (translation key, English fallback, arguments), as the
/// engine reports it.
struct AssemblyError {
    std::string              key;
    std::string              fallback;
    std::vector<std::string> args;
};
[[nodiscard]] AssemblyError errorOf(cc::AssemblyResult const& r);

/// Actors whose blocks may enter world blocks (Create's BlockBreakingMovementBehaviour.canBreak,
/// HarvesterMovementBehaviour.isValidCrop): `fn(region, worldPos, worldBlockId)` returns true when
/// the actor passes (it breaks / harvests the block itself). Drills, saws, ploughs, rollers and
/// harvesters register here; other blocks collide with every solid, non-replaceable block.
using PassThrough = std::function<bool(BlockSource& region, cc::Pos const& worldPos, std::string const& worldBlockId)>;
void registerTerrainPassThrough(std::string actorBlockId, PassThrough fn);

/// A block of a (planned or moving) translating structure.
struct StructureBlock {
    cc::Pos     local;
    std::string id;
    bool        solid = true; ///< has a collision shape
};
/// The blocks of a live contraption (local positions, ids, collision).
[[nodiscard]] std::vector<StructureBlock> structureOf(Contraption const& c);
/// The blocks a planned assembly would take (world positions relative to `origin`).
[[nodiscard]] std::vector<StructureBlock> structureAt(int dim, std::vector<cc::Pos> const& world, cc::Pos const& origin);

/// Create's ContraptionCollider.isCollidingWithWorld: the structure's leading blocks in `dir`
/// (local + `grid`) hit a solid, non-replaceable world block (or an unloaded chunk). `ignore`
/// skips world positions (Create: the pulley magnet above the anchor when moving up).
[[nodiscard]] bool collidesWithWorld(
    int                                          dim,
    std::vector<StructureBlock> const&           blocks,
    cc::Pos const&                               grid,
    cc::Dir                                      dir,
    std::function<bool(cc::Pos const&)> const&   ignore = nullptr
);

/// Plays a sound for everyone near `pos` (as the engine does).
void playSound(int dim, cc::Vec3d const& pos, std::string const& sound, float volume = 1.f, float pitch = 1.f);
/// The action bar message of a player.
void actionBar(Player& player, std::string const& text);

} // namespace lv::feature::contraption::ext
