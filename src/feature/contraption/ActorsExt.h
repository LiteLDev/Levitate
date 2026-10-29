#pragma once

// Contraption extension added by U22 (actors): compatible additions to the contraption contract
// that the actors unit needs. Nothing here changes the engine's behaviour.
//
// - `blockdata` data carrier: U3's BlockData documents of the registered block ids
//   (`carryBlockData`) travel with their blocks (Create keeps block entity data on contraptions).
//   Registered at static init under the carrier name "blockdata"; the carried document is
//   `CarriedBlock::data["blockdata"]`.
// - Lookups of the registries (an actor's behaviour, a seat's height), so a unit can wrap the
//   behaviour of other units' actors (contraption controls disable actors by item type).
// - Thin forwards to the engine's game adapters (redstone power, action bar, sounds, deferral).

#include <functional>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"

#include "feature/contraption/Contraption.h"

class Block;
class BlockSource;
class Player;

namespace lv::feature::contraption::actors_ext {

/// Name of the BlockData carrier ("blockdata").
inline constexpr char kBlockDataCarrier[] = "blockdata";

/// Lets the "blockdata" carrier move the BlockData documents of `blockId` (static init or install).
void carryBlockData(std::string blockId);
/// The carried BlockData document of a block on a contraption (nullptr when it has none).
[[nodiscard]] nlohmann::json const* carriedBlockData(Contraption const& c, cc::Pos const& local);
/// The carried block at `local` (nullptr when none).
[[nodiscard]] CarriedBlock const* carriedAt(Contraption const& c, cc::Pos const& local);

/// The registered behaviour of an actor block (nullptr when it isn't an actor).
[[nodiscard]] MovementBehaviour const* actorBehaviour(std::string const& blockId);
/// Sitting height of a registered seat block.
[[nodiscard]] std::optional<double> seatHeight(std::string const& blockId);

/// Redstone power reaching `p` from its neighbours (the engine's sticker poll).
[[nodiscard]] bool powered(BlockSource& region, cc::Pos const& p);
/// Action bar text for a player.
void actionBar(Player& player, std::string const& text);
/// Plays a sound for players near `pos`.
void playSound(int dim, cc::Vec3d const& pos, std::string const& sound, float volume = 1.f, float pitch = 1.f);
/// Runs `fn` at the start of the next server tick.
void defer(std::function<void()> fn);
/// The block states of a block (as carried: `levitate:facing`, ...).
[[nodiscard]] cc::States statesOf(Block const& block);
/// `block` with `states` applied (unknown states ignored).
[[nodiscard]] Block const* withStates(Block const& block, cc::States const& states);

} // namespace lv::feature::contraption::actors_ext
