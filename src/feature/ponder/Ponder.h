#pragma once

// Server side of Ponder (U30): `/create ponder [id]` asks a companion client to open Ponder.

#include <optional>
#include <string>

#include "core/ponder/Scene.h"

class Player;

namespace lv::feature::ponder {

struct Result {
    bool        ok = false;
    std::string message;
};

/// The Ponder index shipped in the mod folder (`ponder/index.json`), loaded once.
[[nodiscard]] core::ponder::Library const& index();
/// A typed id as the client opens it: "" (the index), a tag id, or a namespaced component id;
/// nullopt when unknown.
[[nodiscard]] std::optional<std::string> resolve(std::string const& id);
/// Sends `ponder.open` to the player's client mod (what `/create ponder` does).
Result request(Player& player, std::string const& id);

} // namespace lv::feature::ponder
