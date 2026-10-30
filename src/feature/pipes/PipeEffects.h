#pragma once

// Pipe particles and glass pipe flows for the client mod (internal to the pipes unit); see
// PipeEffects.cpp.

#include <cstdint>
#include <optional>

#include "nlohmann/json.hpp"

#include "core/pipes/Flow.h"

class Player;

namespace lv::feature::pipes {

/// One tick after the flow model's: particles near players, glass pipe flows to companions.
void tickPipeEffects(core::pipes::Model& model);
/// Sends the glass pipe flows of the player's dimension (after the handshake).
void syncGlassTo(Player& player);
void clearPipeEffects();

/// What the effects did since the world was loaded (selftests).
struct EffectCounts {
    std::uint64_t splashes = 0, pours = 0, drips = 0;
    std::size_t   glassPipes = 0; ///< glass pipes whose flows companions know
};
[[nodiscard]] EffectCounts pipeEffectCounts();
/// The flows companions were last told about for a glass pipe ([face, slot, inbound, progress, speed]).
[[nodiscard]] std::optional<nlohmann::json> glassState(core::GlobalPos const& p);

} // namespace lv::feature::pipes
