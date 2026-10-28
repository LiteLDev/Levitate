#pragma once

// Server side of the screen framework (U3, SPEC §3.3 / §4).
//
// Every screen has a native version (drawn by the client mod, src/client/screen/) and a form
// fallback for vanilla clients. A unit registers a screen *type* once:
//
//     screens::registerType({
//         .id      = "common.filter",
//         .data    = [](Player& p, Json const& ctx) -> std::optional<Json> { ... },  // state to show
//         .action  = [](Player& p, Json const& ctx, Json const& action) { ... },     // native clicks
//         .form    = [](Player& p, Json const& ctx) { ... },                         // vanilla fallback
//     });
//     screens::open(player, "common.filter", {{"slot", 3}});
//
// `open` sends "screen.open" {type, context, data} to companions (the client builds the screen
// registered for `type`), or calls `form` for vanilla players. Native actions arrive as
// "screen.action" {type, context, action}; after the handler runs, the server sends
// "screen.update" {type, context, data} with fresh data (or "screen.close" when `data` returns
// nullopt, e.g. the filter is no longer in hand). The context identifies what the screen edits
// (a hand slot, a block position) and is re-validated by `data` on every action.

#include <functional>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"

class Player;

namespace lv::feature::common::screens {

using Json = nlohmann::json;

struct ScreenType {
    std::string id; ///< "<unit>.<screen>"
    /// The data the native screen shows; nullopt when the screen can't be (or stay) open.
    std::function<std::optional<Json>(Player&, Json const& context)> data;
    /// A native screen action (companions only).
    std::function<void(Player&, Json const& context, Json const& action)> action;
    /// The vanilla fallback (server forms).
    std::function<void(Player&, Json const& context)> form;
};

void registerType(ScreenType type);

/// Opens a screen: native for companions, the form otherwise. Returns false when `data`
/// refuses (nothing opened).
bool open(Player& player, std::string const& type, Json const& context = Json::object());
/// Opens the form fallback even for a companion (e.g. a "use forms" preference).
bool openForm(Player& player, std::string const& type, Json const& context = Json::object());
/// Sends fresh data to an open native screen (no-op for vanilla players).
void refresh(Player& player, std::string const& type, Json const& context);
/// Closes the player's native screen of `type`.
void close(Player& player, std::string const& type);

} // namespace lv::feature::common::screens
