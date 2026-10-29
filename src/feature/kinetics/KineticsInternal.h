#pragma once

// Internals of the kinetics unit shared by its own .cpp files (not a contract for other units).

#include <optional>
#include <string>
#include <vector>

#include "core/kinetics/Solver.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsConfig.h"

class Block;
class BlockSource;
class Player;

namespace lv::feature::kinetics::detail {

/// The kinetic spec of a block state (cached per block type; thread safe).
[[nodiscard]] KineticSpec const* specOfBlock(Block const& block);
/// The kinetic state snapshot of a block (`spec.states` read as integers).
[[nodiscard]] BlockState snapshot(Block const& block, KineticSpec const& spec);
/// Drops the per block type cache (block types are registered again for every world).
void clearBlockCaches();
/// The block with `state` set to `value` (nullptr if the block has no such state / value).
[[nodiscard]] Block const* withState(Block const& block, std::string const& state, int value);

/// Server side: the solver (after applying the queued world changes).
[[nodiscard]] core::kinetics::Solver& solver();
/// Applies queued world changes now.
void flush();
/// The overworld / nether / end block source of the server level (nullptr if not loaded).
[[nodiscard]] BlockSource* regionOf(int dim);

/// Persistence (selftests): write dirty chunks / reload the whole network from the world DB.
void saveNow();
void reloadFromDb();

/// Companion protocol channel of the speed sync (server -> client mod), payload
/// {"batches": [<core/kinetics/SyncCodec.h batch text>, ...]}.
inline constexpr char const* kSyncChannel = "kinetics.sync";

/// Sync (KineticsSync.cpp): queue changes for players with the client mod.
void syncChanges(std::vector<core::kinetics::Change> const& changes);
void syncTick();
void syncReset();
/// Builds the full-sync batches of a dimension (selftests).
[[nodiscard]] std::vector<std::string> fullSyncPayloads(int dim);
/// Number of players that announced the client mod.
[[nodiscard]] int companionCount();

/// Text lines of `/create kinetics <pos>` (selftests and the command).
[[nodiscard]] std::vector<std::string> describe(GlobalPos const& pos, Player const* viewer);

} // namespace lv::feature::kinetics::detail
