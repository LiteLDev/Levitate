// `/create display ...` (Levitate debug, op): write text onto a display board / nixie tube row and
// bind a display link without the item, for e2e scenarios and testing from the console.

#include <algorithm>

#include "ll/api/command/CommandHandle.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"

#include "feature/display/Internal.h"
#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::display {

// Command parameter types need external linkage.
struct DisplayTextParams {
    int         x = 0, y = 0, z = 0;
    int         line = 0;
    std::string text;
};
struct DisplayBindParams {
    int         x = 0, y = 0, z = 0;
    int         tx = 0, ty = 0, tz = 0;
    std::string source;
};

namespace {
int dimOf(CommandOrigin const& origin) {
    if (auto* e = origin.getEntity()) return static_cast<int>(e->getDimensionId());
    return 0;
}
} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    using Origin = CommandOrigin const;
    cmd.overload<DisplayTextParams>()
        .text("display")
        .text("text")
        .required("x")
        .required("y")
        .required("z")
        .required("line")
        .required("text")
        .execute([](Origin& origin, CommandOutput& output, DisplayTextParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            GlobalPos pos{dimOf(origin), p.x, p.y, p.z};
            auto*     region = regionOf(pos.dim);
            if (!region) return output.error("no world");
            auto id   = idAt(*region, pos);
            auto text = p.text;
            std::replace(text.begin(), text.end(), '_', ' ');
            if (board::isBoard(id)) {
                auto s = board::structureOf(*region, pos);
                board::setLine(*region, s->controller, p.line, text, true);
            } else if (nixie::isNixie(id)) {
                nixie::track(pos);
                nixie::displayCustomText(*region, pos, text);
            } else {
                return output.error("not a display board or nixie tube: " + id);
            }
            log().info("display text '{}' written at {}", text, pos.key());
            output.success("display text written");
        });
    cmd.overload<DisplayBindParams>()
        .text("display")
        .text("bind")
        .required("x")
        .required("y")
        .required("z")
        .required("tx")
        .required("ty")
        .required("tz")
        .required("source")
        .execute([](Origin& origin, CommandOutput& output, DisplayBindParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            GlobalPos pos{dimOf(origin), p.x, p.y, p.z};
            auto*     region = regionOf(pos.dim);
            if (!region || idAt(*region, pos) != kLinkId) return output.error("not a display link");
            link::LinkData d;
            if (auto old = link::read(pos)) d = *old;
            d.targetOffset = {p.tx - p.x, p.ty - p.y, p.tz - p.z};
            link::write(pos, d);
            link::track(pos);
            link::selectSource(pos, p.source);
            bool ok = link::update(pos);
            log().info("display link at {} bound to {} {} {} ({}), update {}", pos.key(), p.tx, p.ty, p.tz, p.source, ok ? "sent" : "failed");
            output.success(ok ? "display link bound" : "display link bound (nothing sent)");
        });
});

} // namespace lv::feature::display
