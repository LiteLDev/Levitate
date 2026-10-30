#pragma once

// Internals of the advancements unit (U45). Not a contract for other units.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/GlobalPos.h"

namespace lv::feature::advancements {

/// Create's registerAwardables of the block entity of `blockId` (empty: no owner kept).
[[nodiscard]] std::vector<std::string_view> const& blockAdvancements(std::string_view blockId);
/// The id of the block at `pos` ("" when not loaded).
[[nodiscard]] std::string blockIdAt(core::GlobalPos const& pos);
/// Blocks with an owner (position, uuid).
[[nodiscard]] std::vector<std::pair<core::GlobalPos, std::string>> ownedBlocks();

void installProgress();
void installTriggers();
void uninstallTriggers();
void tickTriggers();
void resetTriggers();
void resetState();
/// Writes dirty progress / owners to the world DB.
void flush();

} // namespace lv::feature::advancements
