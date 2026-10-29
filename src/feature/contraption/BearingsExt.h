#pragma once

// Extension of the contraption contract for U21 (bearings): minecart contraptions can be picked
// up as an item and placed again (Create's MinecartContraptionItem). The engine has no API to
// turn a live contraption into data and back, so this file (in the contraption directory, next
// to the engine) does it through the engine's own persistence: a snapshot is the record the
// engine saves for the contraption, and a restore writes a new record and reloads the engine.
//
// Server thread only. Added by U21; no change to Contraption.h or the engine.

#include <optional>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"
#include "feature/contraption/Contraption.h"

namespace lv::feature::contraption::ext {

/// The persisted record of a live contraption (blocks, block entities, carried data, glue,
/// anchor data). nullopt if unknown.
[[nodiscard]] std::optional<nlohmann::json> snapshot(Id id);

/// Creates a contraption from a snapshot at a new anchor with the given transform. The carried
/// blocks are not placed in the world; display entities are spawned on the next tick. Returns
/// the new id.
std::optional<Id> restore(nlohmann::json record, core::GlobalPos const& anchor, std::string const& kind, cc::Transform const& transform);

} // namespace lv::feature::contraption::ext
