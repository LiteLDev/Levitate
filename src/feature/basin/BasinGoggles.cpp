// Basin goggle information (Create's BasinBlockEntity.addToGoggleTooltip): "Basin Contents:", then
// every item stack (gray name, green count) and every fluid (gray name, blue amount). An empty basin
// shows nothing. It replaces Levitate's generic "Contents:" / tank lines for the basin. Blaze
// burners have no goggle information in Create (no fuel or heat line).

#include "mc/world/level/BlockSource.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/Capabilities.h"
#include "feature/common/FluidRegistry.h"
#include "feature/common/Items.h"
#include "feature/goggles/GoggleInfo.h"
#include "game/Blocks.h"
#include "mod/I18n.h"

namespace lv::feature::basin {

namespace {

namespace goggles = feature::goggles;
namespace fmt     = core::goggles::fmt;

constexpr char kBlue[] = "§9"; // ChatFormatting.BLUE

std::string nameOf(ItemStack const& s) {
    auto name = common::items::displayName(s);
    return name.empty() || name.find(".name") != std::string::npos ? common::items::idOf(s) : name;
}

bool basinContents(goggles::InfoContext const& ctx, goggles::Tooltip& t) {
    t.push_back(goggles::line(ctx.tr("create.gui.goggles.basin_contents", "Basin Contents:")));
    bool           empty = true;
    auto           bp    = game::blockPos(ctx.pos);
    common::Access at{ctx.region, bp, -1};
    if (auto h = common::items::find(at); h && h->slots && h->getSlot) {
        int slots = h->slots(at);
        for (int i = 0; i < slots; ++i) {
            auto s = h->getSlot(at, i);
            if (common::items::isEmpty(s)) continue;
            t.push_back(goggles::line(
                std::string(fmt::kGray) + nameOf(s) + fmt::kGreen + " x" + std::to_string(common::items::countOf(s)),
                1
            ));
            empty = false;
        }
    }
    auto mb = ctx.tr("create.generic.unit.millibuckets", "mB");
    for (auto const& tank : common::fluids::tanks(ctx.region, bp)) {
        if (tank.contents.empty()) continue;
        t.push_back(goggles::line(
            std::string(fmt::kGray) + common::fluidtypes::displayName(tank.contents.fluid, localeOf(ctx.player)) + " " + kBlue
                + goggles::number(tank.contents.amount) + mb,
            1
        ));
        empty = false;
    }
    return !empty; // the overlay drops the header again when nothing was added
}

} // namespace

LV_REGISTER_GOGGLE_INFO(kBasin, {.goggle = basinContents, .replaceDefaults = true});
LV_REGISTER_GOGGLE_INFO(kBurner, {.replaceDefaults = true});
LV_REGISTER_GOGGLE_INFO(kLitBurner, {.replaceDefaults = true});
LV_REGISTER_GOGGLE_INFO(kEmptyBurner, {.replaceDefaults = true});

} // namespace lv::feature::basin
