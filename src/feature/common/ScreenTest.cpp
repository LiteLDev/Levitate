// A small native screen to exercise the screen framework end to end (U32): a text field with a
// submit, closing with Esc. `/create screentest` opens it for the player (op only); the client
// e2e scenario `common` drives it. Vanilla players get a form with the same field.

#include "ll/api/command/CommandHandle.h"
#include "ll/api/form/CustomForm.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"

#include "feature/common/Screens.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::common::screens {

namespace {

constexpr char kTextTest[] = "common.text_test";

void submitted(Player& player, std::string const& text) {
    log().info("screen test: {} submitted '{}'", player.getRealName(), text);
}

struct TextTestRegistrar {
    TextTestRegistrar() {
        registerType({
            .id     = kTextTest,
            .data   = [](Player&, Json const&) -> std::optional<Json> { return Json{{"text", ""}}; },
            .action = [](Player& p, Json const&, Json const& action) {
                if (action.value("op", std::string{}) == "submit") submitted(p, action.value("text", std::string{}));
            },
            .form = [](Player& p, Json const&) {
                ll::form::CustomForm form(trFor(p, "lv.common.screentest.title", "Text Field Test"));
                form.appendInput("text", trFor(p, "lv.common.screentest.label", "Text"), "", "");
                form.sendTo(p, [](Player& pl, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
                    if (!r) return;
                    if (auto it = r->find("text"); it != r->end())
                        if (auto const* s = std::get_if<std::string>(&it->second)) submitted(pl, *s);
                });
            },
        });
    }
};
TextTestRegistrar const gTextTest;

} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload().text("screentest").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = commands::playerOf(origin);
        if (!player) return output.error("Only players can open screens.");
        if (!commands::isAdmin(origin)) return output.error("You need operator permission.");
        open(*player, kTextTest);
        output.success("Opened the screen test.");
    });
});

} // namespace lv::feature::common::screens
