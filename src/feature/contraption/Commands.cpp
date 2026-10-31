// `/create contraption ...` (the test anchor: assemble, move, rotate, stop, disassemble without a
// bearing or piston), `/create glue <from> <to>` (Create's GlueCommand: adds a super glue region)
// and `/create glue clear [radius]` (Levitate only: removes the regions near the caller). Op only.

#include <cmath>

#include "ll/api/command/CommandHandle.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/BlockPos.h"

#include "feature/contraption/Internal.h"
#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::contraption {

// Command parameter types need external linkage.
struct ContraptionPosParams {
    int x = 0, y = 0, z = 0;
};
struct ContraptionPosAxisParams {
    int         x = 0, y = 0, z = 0;
    std::string axis;
};
struct ContraptionIdParams {
    int id = 0;
};
struct ContraptionMoveParams {
    int   id = 0;
    float dx = 0, dy = 0, dz = 0;
    int   ticks = 20;
};
struct ContraptionRotateParams {
    int         id = 0;
    std::string axis;
    float       degrees = 90;
    int         ticks   = 20;
};
struct ContraptionChassisParams {
    int x = 0, y = 0, z = 0;
    int range = 8;
};
struct GlueClearParams {
    int radius = 16;
};
struct GlueAddParams {
    CommandPosition from{};
    CommandPosition to{};
};

namespace {

int dimOf(CommandOrigin const& origin) {
    if (auto* e = origin.getEntity()) return static_cast<int>(e->getDimensionId());
    return 0;
}

void assembleAt(CommandOrigin const& origin, CommandOutput& output, int x, int y, int z, std::string const& axisName) {
    if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
    auto axis = cc::axisFromName(axisName.empty() ? "y" : axisName);
    if (!axis) return output.error(tr("lv.contraption.bad_axis", "The axis must be x, y or z"));
    AssembleOptions o;
    o.anchor        = {dimOf(origin), x, y, z};
    o.includeAnchor = true;
    o.axis          = *axis;
    o.kind          = "test";
    auto r          = assemble(o);
    if (!r.ok()) return output.error(tr("create.gui.assembly.exception", "This Contraption was unable to assemble:") + " " + r.errorText);
    auto* c = find(*r.id);
    output.success(tr("lv.contraption.assembled", "Contraption #{} assembled ({} blocks)", *r.id, c ? c->blocks().size() : 0));
}

} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    using Origin = CommandOrigin const;
    cmd.overload<ContraptionPosParams>()
        .text("contraption")
        .text("assemble")
        .required("x")
        .required("y")
        .required("z")
        .execute([](Origin& origin, CommandOutput& output, ContraptionPosParams const& p) {
            assembleAt(origin, output, p.x, p.y, p.z, "y");
        });
    cmd.overload<ContraptionPosAxisParams>()
        .text("contraption")
        .text("assemble")
        .required("x")
        .required("y")
        .required("z")
        .required("axis")
        .execute([](Origin& origin, CommandOutput& output, ContraptionPosAxisParams const& p) {
            assembleAt(origin, output, p.x, p.y, p.z, p.axis);
        });
    cmd.overload<ContraptionMoveParams>()
        .text("contraption")
        .text("move")
        .required("id")
        .required("dx")
        .required("dy")
        .required("dz")
        .optional("ticks")
        .execute([](Origin& origin, CommandOutput& output, ContraptionMoveParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            auto* c = find(static_cast<Id>(p.id));
            if (!c) return output.error(tr("lv.contraption.unknown", "No contraption #{}", p.id));
            int    ticks = std::max(1, p.ticks);
            Motion m;
            m.velocity = cc::Vec3d{p.dx, p.dy, p.dz} * (1.0 / ticks);
            m.ticks    = ticks;
            c->setMotion(m);
            output.success(tr("lv.contraption.moving", "Contraption #{} moving", p.id));
        });
    cmd.overload<ContraptionRotateParams>()
        .text("contraption")
        .text("rotate")
        .required("id")
        .required("axis")
        .required("degrees")
        .optional("ticks")
        .execute([](Origin& origin, CommandOutput& output, ContraptionRotateParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            auto* c = find(static_cast<Id>(p.id));
            if (!c) return output.error(tr("lv.contraption.unknown", "No contraption #{}", p.id));
            auto axis = cc::axisFromName(p.axis);
            if (!axis) return output.error(tr("lv.contraption.bad_axis", "The axis must be x, y or z"));
            if (*axis != c->transform().axis) {
                if (std::abs(c->transform().angle) > 1e-6) return output.error(tr("lv.contraption.other_axis", "The contraption already turns about another axis"));
                auto t = c->transform();
                t.axis = *axis;
                c->setTransform(t);
            }
            int    ticks = std::max(1, p.ticks);
            Motion m;
            m.angularVelocity = p.degrees / ticks;
            m.ticks           = ticks;
            c->setMotion(m);
            output.success(tr("lv.contraption.moving", "Contraption #{} moving", p.id));
        });
    cmd.overload<ContraptionIdParams>().text("contraption").text("stop").required("id").execute(
        [](Origin& origin, CommandOutput& output, ContraptionIdParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            auto* c = find(static_cast<Id>(p.id));
            if (!c) return output.error(tr("lv.contraption.unknown", "No contraption #{}", p.id));
            c->stop();
            output.success(tr("lv.contraption.stopped", "Contraption #{} stopped", p.id));
        }
    );
    cmd.overload<ContraptionIdParams>().text("contraption").text("disassemble").required("id").execute(
        [](Origin& origin, CommandOutput& output, ContraptionIdParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            if (!disassemble(static_cast<Id>(p.id))) return output.error(tr("lv.contraption.unknown", "No contraption #{}", p.id));
            output.success(tr("lv.contraption.disassembled", "Contraption #{} disassembled", p.id));
        }
    );
    // A small glued structure built 4 blocks in front of the player and assembled (e2e / demos).
    cmd.overload().text("contraption").text("demo").execute([](Origin& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
        auto* player = commands::playerOf(origin);
        if (!player) return output.error(tr("lv.contraption.players_only", "Only players can build the demo."));
        auto& region = player->getDimensionBlockSource();
        int   dim    = static_cast<int>(player->getDimensionId());
        auto  feet   = game::blockPosOf(player->getFeetPos());
        auto [fx, fz] = game::facingOffset(game::playerFacing(*player));
        cc::Pos base{feet.x + 4 * fx, feet.y, feet.z + 4 * fz};
        cc::Pos side{fz, 0, -fx}; // sideways (perpendicular to the view)
        struct Part {
            cc::Pos     at;
            char const* id;
        };
        Part parts[]{
            {base,                              "minecraft:stone"       },
            {base + cc::Pos{0, 1, 0},           "minecraft:gold_block"  },
            {base + cc::Pos{0, 2, 0},           "minecraft:diamond_block"},
            {base + cc::Pos{0, 2, 0} + side,    "minecraft:emerald_block"},
            {base + cc::Pos{0, 2, 0} + side * 2, "minecraft:redstone_block"},
        };
        for (auto const& part : parts) {
            if (auto const* b = game::blockWith(part.id)) game::setBlock(region, detail::toBlockPos(part.at), *b);
        }
        addGlue(dim, cc::GlueBox::of(base, base + cc::Pos{0, 2, 0} + side * 2));
        AssembleOptions o;
        o.anchor        = {dim, base.x, base.y, base.z};
        o.includeAnchor = true;
        o.axis          = cc::Axis::Y;
        o.kind          = "test";
        auto r          = assemble(o);
        if (!r.ok()) return output.error(r.errorText);
        output.success(tr("lv.contraption.assembled", "Contraption #{} assembled ({} blocks)", *r.id, find(*r.id)->blocks().size()));
    });
    cmd.overload().text("contraption").text("list").execute([](Origin& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
        auto ids = all();
        std::string text = tr("lv.contraption.list", "{} contraption(s)", ids.size());
        for (auto id : ids) {
            auto* c = find(id);
            if (!c) continue;
            auto const& t = c->transform();
            text += "\n#" + std::to_string(id) + " " + c->kind() + " " + std::to_string(c->blocks().size()) + " blocks at "
                  + std::to_string(c->anchor().x) + " " + std::to_string(c->anchor().y) + " " + std::to_string(c->anchor().z)
                  + " offset " + std::to_string(t.offset.x) + " " + std::to_string(t.offset.y) + " " + std::to_string(t.offset.z)
                  + " angle " + std::to_string(t.angle);
        }
        output.success(text);
    });
    cmd.overload<ContraptionChassisParams>()
        .text("contraption")
        .text("chassis")
        .required("x")
        .required("y")
        .required("z")
        .required("range")
        .execute([](Origin& origin, CommandOutput& output, ContraptionChassisParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            setChassisRange({dimOf(origin), p.x, p.y, p.z}, p.range);
            output.success(tr("create.contraptions.chassis.range", "Range of Sticky Sides") + ": " + std::to_string(chassisRange({dimOf(origin), p.x, p.y, p.z})));
        });
    cmd.overload<GlueAddParams>().text("glue").required("from").required("to").execute(
        [](Origin& origin, CommandOutput& output, GlueAddParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            constexpr int kLatest = 0x7FFFFFFF; // newest rules for relative positions
            auto from = game::blockPosOf(p.from.getPosition(kLatest, origin, Vec3{0, 0, 0}));
            auto to   = game::blockPosOf(p.to.getPosition(kLatest, origin, Vec3{0, 0, 0}));
            int  dim  = dimOf(origin);
            auto box  = cc::GlueBox::of({from.x, from.y, from.z}, {to.x, to.y, to.z});
            addGlue(dim, box);
            detail::playSound(dim, cc::Vec3d::center(box.min), "mob.slime.big", 0.5f, 0.9f);
            log().info("super glue added by command: {} {} {} .. {} {} {}", box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
            output.success(tr("lv.contraption.glue_added", "Glued {} {} {} .. {} {} {}", box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z));
        }
    );
    cmd.overload<GlueClearParams>().text("glue").text("clear").optional("radius").execute(
        [](Origin& origin, CommandOutput& output, GlueClearParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            auto* e = origin.getEntity();
            BlockPos center = e ? game::blockPosOf(e->getPosition()) : BlockPos{0, 0, 0};
            int      n      = clearGlue(dimOf(origin), {center.x, center.y, center.z}, std::max(0, p.radius));
            output.success(tr("lv.contraption.glue_cleared", "Removed {} glue region(s)", n));
        }
    );
});

} // namespace lv::feature::contraption
