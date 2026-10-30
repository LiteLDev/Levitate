// `/create highlight <pos>` (SPEC §6, op): outlines a block for everyone with particles along
// its edges (Create's highlight command draws an outline on clients; Bedrock clients see the
// particle outline, which needs no client mod).

#include "ll/api/command/CommandHandle.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/server/commands/CommandVersion.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/Log.h"

namespace lv::feature::common {

// Command parameter types need external linkage.
struct HighlightParams {
    CommandPosition pos{};
};

namespace {
constexpr int kLatestCommandVersion = 0x7FFFFFFF; ///< newest rules for relative positions

void outline(Level& level, Dimension& dim, BlockPos const& p) {
    constexpr int kSteps = 4;
    for (int axis = 0; axis < 3; ++axis) {
        for (int a = 0; a < 2; ++a) {
            for (int b = 0; b < 2; ++b) {
                for (int i = 0; i <= kSteps; ++i) {
                    float t = static_cast<float>(i) / kSteps;
                    Vec3  v{0, 0, 0};
                    float* c[3] = {&v.x, &v.y, &v.z};
                    *c[axis]           = t;
                    *c[(axis + 1) % 3] = static_cast<float>(a);
                    *c[(axis + 2) % 3] = static_cast<float>(b);
                    level.spawnParticleEffect("minecraft:villager_happy", Vec3{p.x + v.x, p.y + v.y, p.z + v.z}, &dim);
                }
            }
        }
    }
}
} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload<HighlightParams>().text("highlight").required("pos").execute(
        [](CommandOrigin const& origin, CommandOutput& output, HighlightParams const& params) {
            if (!commands::isAdmin(origin)) return output.error("You need operator permission.");
            auto  level = ll::service::getLevel();
            auto* dim   = origin.getDimension();
            if (!level || !dim) return output.error("No dimension.");
            auto pos = game::blockPosOf(params.pos.getPosition(kLatestCommandVersion, origin, Vec3{0, 0, 0}));
            outline(*level, *dim, pos);
            log().info("highlight at {} {} {}", pos.x, pos.y, pos.z);
            output.success("Highlighted " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z));
        }
    );
});

} // namespace lv::feature::common
