// Goggle information of the assembly blocks. The mechanical arm is a plain kinetic block in Create
// (kinetic stats only, no held item line).
//
// Deployer (Create's DeployerBlockEntity.addToGoggleTooltip): "Deployer
// Information", the mode (yellow), the held item (green), then the stress impact without the
// "Kinetic Stats:" header (addStressImpactStats). It replaces the generic kinetic / "Contents:"
// lines; the overstressed hint (hover) stays.

#include <cmath>

#include "mc/world/item/ItemStack.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/common/Items.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsConfig.h"
#include "mod/Config.h"

namespace lv::feature::assembly {

namespace {

namespace goggles = feature::goggles;
namespace fmt     = core::goggles::fmt;
namespace items   = common::items;

bool deployerInfo(goggles::InfoContext const& ctx, goggles::Tooltip& t) {
    t.push_back(goggles::line(ctx.tr("create.tooltip.deployer.header", "Deployer Information")));
    bool use = deployer::mode(ctx.pos) == ca::DeployerMode::Use;
    t.push_back(goggles::line(
        std::string(fmt::kYellow)
        + (use ? ctx.tr("create.tooltip.deployer.using", "Mode: Use") : ctx.tr("create.tooltip.deployer.punching", "Mode: Attack"))
    ));
    auto held = deployer::held(ctx.pos);
    if (!items::isEmpty(held)) {
        auto name = items::displayName(held);
        if (name.empty() || name.find(".name") != std::string::npos) name = items::idOf(held);
        t.push_back(goggles::line(std::string(fmt::kGreen) + ctx.tr("create.tooltip.deployer.contains", "Item: {0} x{1}", name, items::countOf(held))));
    }
    // Create: StressImpact.isEnabled() && stress at base != 0
    auto node = kinetics::nodeAt(ctx.pos);
    if (!config::get<kinetics::KineticsConfig>().disableStress && node && std::fabs(node->impact) > 1e-5) {
        t.emplace_back();
        t.push_back(goggles::line(std::string(fmt::kGray) + ctx.tr("create.tooltip.stressImpact", "Kinetic Stress Impact:")));
        double total = node->impact * std::fabs(node->theoreticalSpeed);
        t.push_back(goggles::line(
            std::string(fmt::kAqua) + goggles::number(total) + ctx.tr("create.generic.unit.stress", "su") + " " + fmt::kDarkGray
                + ctx.tr("create.gui.goggles.at_current_speed", "at current speed"),
            1
        ));
    }
    return true;
}

} // namespace

LV_REGISTER_GOGGLE_INFO(kDeployerId, {.goggle = deployerInfo, .hover = goggles::kineticHoverTooltip, .replaceDefaults = true});
LV_REGISTER_GOGGLE_INFO(kArmId, {.goggle = goggles::kineticGoggleTooltip, .hover = goggles::kineticHoverTooltip, .replaceDefaults = true});

} // namespace lv::feature::assembly
