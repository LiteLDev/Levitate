// Goggle information of the bulk processing blocks: Create's millstone is a plain kinetic block
// (kinetic stats only, no contents line) and the crushing wheel controller has none.

#include <string>

#include "feature/bulk/Bulk.h"
#include "feature/goggles/GoggleInfo.h"

namespace lv::feature::bulk {

namespace goggles = feature::goggles;

LV_REGISTER_GOGGLE_INFO(std::string(kMillstone), {.goggle = goggles::kineticGoggleTooltip, .hover = goggles::kineticHoverTooltip, .replaceDefaults = true});
LV_REGISTER_GOGGLE_INFO(std::string(kController), {.replaceDefaults = true});

} // namespace lv::feature::bulk
