// `/create ponder [id]` (SPEC §6, anyone): opens Ponder on the player's client mod. Without an id
// it opens the Ponder index; the id is an item (`shaft`, `levitate:shaft`, `minecraft:chest`) or
// a tag (`kinetic_relays`). Players without the client mod get a message (Ponder is client-side,
// SPEC §8). Runs on BDS and on the integrated server of local worlds.

#include <filesystem>
#include <fstream>

#include "ll/api/command/CommandHandle.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"

#include "core/ponder/Scene.h"
#include "feature/net/Net.h"
#include "feature/ponder/Ponder.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Main.h"

namespace lv::feature::ponder {

// Command parameter types need external linkage.
struct PonderParams {
    std::string id;
};

core::ponder::Library const& index() {
    static core::ponder::Library lib = [] {
        core::ponder::Library l;
        auto                  dir = lv::Mod::getInstance().getSelf().getModDir() / "ponder";
        std::ifstream         in(dir / "index.json");
        auto                  j = nlohmann::json::parse(in, nullptr, false);
        for (auto const& e : l.setIndex(j)) log().warn("ponder: {}", e);
        return l;
    }();
    return lib;
}

std::optional<std::string> resolve(std::string const& id) {
    if (id.empty()) return std::string{};
    auto const& lib = index();
    if (lib.tag(id)) return id;
    auto full = core::ponder::normaliseId(id);
    if (lib.component(full)) return full;
    return std::nullopt;
}

Result request(Player& player, std::string const& id) {
    auto target = resolve(id);
    if (!target) return {false, trFor(player, "lv.ponder.unknown", "There is nothing to ponder about {0}.", id)};
    if (!net::isCompanion(player)) return {false, trFor(player, "lv.ponder.no_client", "Ponder needs the Levitate client mod.")};
    net::send(player, "ponder.open", {{"id", *target}});
    log().info("ponder: {} opens {}", player.getRealName(), target->empty() ? "the index" : *target);
    return {true, trFor(player, "lv.ponder.opening", "Opening Ponder: {0}", target->empty() ? trFor(player, "ponder.ui.index_title", "Ponder Index") : *target)};
}

namespace {
void run(CommandOrigin const& origin, CommandOutput& output, std::string const& id) {
    auto* player = commands::playerOf(origin);
    if (!player) return output.error(tr("lv.ponder.players_only", "Only players can ponder."));
    auto r = request(*player, id);
    if (r.ok) output.success(r.message);
    else output.error(r.message);
}
} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload().text("ponder").execute([](CommandOrigin const& origin, CommandOutput& output) { run(origin, output, ""); });
    cmd.overload<PonderParams>().text("ponder").required("id").execute(
        [](CommandOrigin const& origin, CommandOutput& output, PonderParams const& params) { run(origin, output, params.id); }
    );
});

} // namespace lv::feature::ponder
