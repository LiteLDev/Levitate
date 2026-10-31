// Goggle information of the crafting tools: Create's mechanical crafter and saw are plain kinetic
// blocks (kinetic stats only, no contents line).

#include <string>

#include "feature/crafter/Internal.h"
#include "feature/goggles/GoggleInfo.h"

namespace lv::feature::crafter {

namespace goggles = feature::goggles;

LV_REGISTER_GOGGLE_INFO(std::string(kCrafter), {.goggle = goggles::kineticGoggleTooltip, .hover = goggles::kineticHoverTooltip, .replaceDefaults = true});
LV_REGISTER_GOGGLE_INFO(std::string(kSaw), {.goggle = goggles::kineticGoggleTooltip, .hover = goggles::kineticHoverTooltip, .replaceDefaults = true});

} // namespace lv::feature::crafter
