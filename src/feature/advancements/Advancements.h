#pragma once

// Create's advancements (U45, SPEC §3.6.45): the tree of Create 6.0.10 (core/advancements),
// progress per player in the world DB (`lv:advancements:player:<uuid>`), a toast (client mod: a
// Create toast; vanilla: the game's toast) and Create's chat line on unlock, the tree screen
// (`/create advancements`: native in the client mod, a form for vanilla players).
//
// Contract for the other units (additive): Create awards most advancements from a block entity's
// AdvancementBehaviour, i.e. to the player who placed the block. This unit remembers that player
// for the blocks that have such advancements (placements seen through the interaction dispatch;
// a unit that places blocks itself calls `setOwner`), and the units call:
//
//     advancements::award(player, "belt");                     // Create's awardTo(player)
//     advancements::awardOwner(pos, "mechanical_press");       // Create's award(...) of a block entity
//     advancements::awardOwnerIfNear(pos, "cuckoo_clock", 32); // Create's awardIfNear
//
// Ids are Create's ("andesite_alloy", "lava_wheel_00000"). Server thread only.

#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"

class Player;

namespace lv::feature::advancements {

using core::GlobalPos;

/// Grants an advancement: saved, toast, chat line (Create's announce types), log line. Returns
/// false when it is unknown or the player has it already.
bool award(Player& player, std::string_view id);
/// The player has the advancement.
[[nodiscard]] bool has(Player const& player, std::string_view id);
/// Ids the player has, in Create's order.
[[nodiscard]] std::vector<std::string> doneOf(Player const& player);
/// Takes an advancement away (selftests, `/create advancements revoke`).
bool revoke(Player& player, std::string_view id);

/// Create's AdvancementBehaviour#setPlayer: the player who placed the block at `pos` (only kept
/// for blocks with block advancements, and only while the player misses one of them).
void setOwner(GlobalPos const& pos, Player const& player);
/// The owner's uuid ("" when none).
[[nodiscard]] std::string ownerOf(GlobalPos const& pos);
/// Create's AdvancementBehaviour#awardPlayer: awards the owner of the block at `pos` if online.
void awardOwner(GlobalPos const& pos, std::string_view id);
/// Create's awardPlayerIfNear: only when the owner is within `maxDistance` blocks of the block.
void awardOwnerIfNear(GlobalPos const& pos, std::string_view id, int maxDistance);

/// The owner of a block riding on a contraption: its carried BlockData document `doc` keeps the
/// owner (Create: the "Owner" of the block entity data in the MovementContext). Awards if online.
void awardCarriedOwner(nlohmann::json const& doc, std::string_view id);
/// Create's DeployerMovementBehaviour#checkForTrackPlacementAdvancement: a deployer on a minecart
/// contraption or a train (`contraptionKind` "cart_assembler" / "train") used a rail or track
/// block item (`usedItemId`, `placed`: the use consumed it): self_deploying for its owner.
void onMovingDeployerUsed(std::string_view contraptionKind, nlohmann::json const& deployerDoc, std::string_view usedItemId, bool placed);

/// Create's MechanicalPressBlockEntity#onItemPressed (one call per result stack): mechanical_press
/// for the owner; tracks made count towards track_crafting_factory (1000, kept per press in its
/// BlockData, like Create's TracksCreated).
void onItemPressed(GlobalPos const& pos, std::string_view resultId, int count);
/// Create's SpoutBlockEntity filling an item: spout for the owner; foods once this spout made
/// chocolate glazed berries, a honeyed apple and a sweet roll while the owner is known.
void onSpoutFilled(GlobalPos const& pos, std::string_view resultId);

/// Opens the advancement tree for the player (native screen or form).
void openTree(Player& player);

} // namespace lv::feature::advancements
