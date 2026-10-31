#pragma once

// What a printed schematic may carry into the world (pure C++, unit-tested), Create's
// BlockHelper.prepareBlockEntityData + NBTProcessors + SchematicProcessor on Levitate's data:
//
// - Vanilla block entities: only blocks tagged `create:safe_nbt` keep their data (signs and
//   banners on Bedrock: Sign, HangingSign, Banner). Chests, spawners, command blocks, ... are
//   printed empty, by the schematicannon and by the creative instant print alike.
// - Levitate's BlockData (our block entities): Create's SAFE_NBT blocks (links, levers, diodes,
//   funnels, chassis, pistons, bearings, motors, ...) keep their whole document but the kinetic
//   network record and the advancement owner; every other block keeps only its filter slots
//   (Create's FilteringBehaviour.writeSafe) and the settings of its unit's PartialSafeNBT part
//   when listed below (the threshold switch's thresholds, a copycat's material, a track's curves).
// - Entities: `onlyOpCanSetNbt` ones (command block minecarts, falling blocks, NPCs) are dropped
//   for players who can't use operator blocks (SchematicProcessor.processEntity).

#include <optional>
#include <string_view>

#include "nlohmann/json.hpp"

#include "core/schematics/Nbt.h"

namespace lv::core::schematics {

/// The block id is tagged `create:safe_nbt` (its block entity data is printed whole).
[[nodiscard]] bool isSafeNbtBlock(std::string_view blockId);
/// The part of a saved Bedrock block entity a print keeps (nullopt: printed without data).
[[nodiscard]] std::optional<nbt::Tag> safeBlockEntity(nbt::Tag const& tag);
/// The part of a block's Levitate BlockData a print keeps (null: none).
[[nodiscard]] nlohmann::json safeBlockData(std::string_view blockId, nlohmann::json const& doc);
/// Java's Entity.onlyOpCanSetNbt for a Bedrock entity identifier.
[[nodiscard]] bool onlyOpCanSetNbt(std::string_view entityId);
/// The identifier of a saved Bedrock entity ("" when missing).
[[nodiscard]] std::string entityIdOf(nbt::Tag const& tag);

} // namespace lv::core::schematics
