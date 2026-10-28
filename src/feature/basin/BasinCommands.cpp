// `/create basin demo` (Levitate debug, op only): builds a working mixer, press and burner set in
// front of the player, driven by the kinetic test source, for screenshots and e2e runs.

#include "ll/api/command/CommandHandle.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::basin {

namespace {

constexpr char const* kSource = "levitate:kinetics_test_source";
constexpr char const* kCog    = "levitate:kinetics_test_cog";

bool put(BlockSource& region, BlockPos const& p, char const* id, std::vector<std::pair<std::string_view, int>> states = {}) {
    auto const* b = game::blockWith(id, states);
    return b && game::setBlock(region, p, *b);
}

void buildDemo(Player& player, CommandOutput& output) {
    auto& region = player.getDimensionBlockSource();
    int   dim    = static_cast<int>(player.getDimensionId());
    auto  f      = game::playerFacing(player); // Java 2D facing the player looks along
    auto [dx, dz] = game::facingOffset(f);
    auto feet     = game::blockPosOf(player.getPosition());
    // one row of stations across the look direction, 4 blocks ahead
    BlockPos base{feet.x + dx * 4, feet.y, feet.z + dz * 4};
    int      sx = -dz, sz = dx; // sideways
    auto     at = [&](int side, int up) { return BlockPos{base.x + sx * side, base.y + up, base.z + sz * side}; };

    // mixer over a basin over a seething (creative) blaze burner
    put(region, at(-3, 0), kBurner, {{kBlazeState, 4}});
    if (auto* d = burners::get(game::globalPos(dim, at(-3, 0)))) {
        d->state = {cb::Fuel::None, 0, true};
        burners::save(game::globalPos(dim, at(-3, 0)), *d);
    }
    put(region, at(-3, 1), kBasin);
    put(region, at(-3, 2), "minecraft:air");
    put(region, at(-3, 3), kMixer);
    put(region, at(-4, 2), kSource, {{"levitate:axis", 1}});
    put(region, at(-4, 3), kCog, {{"levitate:axis", 1}});
    kinetics::setGeneratedSpeed(game::globalPos(dim, at(-4, 2)), 64);
    auto mixBasin = game::globalPos(dim, at(-3, 1));
    if (auto* b = basinops::get(region, mixBasin)) {
        if (auto s = common::items::make("minecraft:dirt", 8)) basinops::insertInput(*b, *s, false);
        cb::fillTanks(b->tankIn, {"minecraft:water", 1000}, cb::kTankCapacity, false);
        basinops::notifyChangeOfContents(*b);
        basinops::save(mixBasin, *b);
    }

    // press over a basin of iron ingots (shaft along the row)
    int axis = sx != 0 ? 0 : 2;
    put(region, at(1, 0), kBasin);
    put(region, at(1, 1), "minecraft:air");
    put(region, at(1, 2), kPress, {{kAxisState, axis}});
    put(region, at(2, 2), kSource, {{"levitate:axis", axis}});
    kinetics::setGeneratedSpeed(game::globalPos(dim, at(2, 2)), 128);
    auto pressBasin = game::globalPos(dim, at(1, 0));
    if (auto* b = basinops::get(region, pressBasin)) {
        if (auto s = common::items::make("minecraft:iron_ingot", 63)) basinops::insertInput(*b, *s, false);
        basinops::notifyChangeOfContents(*b);
        basinops::save(pressBasin, *b);
    }

    // burners: kindled, lit, soul and empty
    put(region, at(4, 0), kBurner, {{kBlazeState, 3}});
    if (auto* d = burners::get(game::globalPos(dim, at(4, 0)))) {
        d->state = {cb::Fuel::None, 0, true};
        burners::save(game::globalPos(dim, at(4, 0)), *d);
    }
    put(region, at(5, 0), kLitBurner, {{kFlameState, 0}});
    put(region, at(6, 0), kLitBurner, {{kFlameState, 1}});
    put(region, at(7, 0), kEmptyBurner);
    log().info("basin demo built at {} {} {} by {}", base.x, base.y, base.z, player.getRealName());
    output.success(tr("lv.basin.demo", "Basin demo built at {} {} {}", base.x, base.y, base.z));
}

} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload().text("basin").text("demo").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.basin.need_op", "You need operator permission."));
        auto* player = commands::playerOf(origin);
        if (!player) return output.error(tr("lv.basin.need_player", "Run this as a player."));
        buildDemo(*player, output);
    });
});

} // namespace lv::feature::basin
