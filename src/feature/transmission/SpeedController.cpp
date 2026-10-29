// Rotation speed controller (U6): Create's SpeedControllerBlockEntity. The target speed is a
// kinetic value box on the two side faces (Create's ControllerValueBoxTransform: 8 / 11 px on the
// faces across the shaft), default 16 RPM, -maxRotationSpeed..maxRotationSpeed. A large
// cogwheel on top (axis across the controller's shaft) is driven at the target speed (Specs.cpp).

#include <cmath>
#include <unordered_map>

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/common/BlockData.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::transmission {

namespace blockdata = feature::common::blockdata;
namespace valuebox  = feature::common::valuebox;

namespace {

constexpr char kKey[] = "speed_controller";

std::unordered_map<GlobalPos, int, ck::GlobalPosHash>& cache() {
    static std::unordered_map<GlobalPos, int, ck::GlobalPosHash> m;
    return m;
}

int maxSpeed() { return std::max(1, static_cast<int>(config::get<kinetics::KineticsConfig>().maxRotationSpeed)); }

} // namespace

int targetSpeed(GlobalPos const& pos) {
    auto& c = cache();
    if (auto it = c.find(pos); it != c.end()) return it->second;
    int  v = kDefaultTargetSpeed;
    if (auto const* d = blockdata::get(pos); d && d->contains(kKey)) v = (*d)[kKey].value("target", kDefaultTargetSpeed);
    c[pos] = v;
    return v;
}

void setTargetSpeed(GlobalPos const& pos, int rpm) {
    int max = maxSpeed();
    rpm     = std::clamp(rpm, -max, max);
    if (targetSpeed(pos) == rpm) return;
    cache()[pos]                         = rpm;
    blockdata::edit(pos)[kKey]["target"] = rpm;
    // Create's updateTargetRotation: leave the network and attach again
    if (auto* region = regionOf(pos.dim); region && idAt(*region, pos) == kSpeedController) {
        setStates(*region, pos, {{kRevision, stateAt(*region, pos, kRevision, 0) ^ 1}});
    }
    log().info("transmission: speed controller at {} targets {} RPM", pos.key(), rpm);
}

void clearSpeedControllers() { cache().clear(); }

void registerSpeedControllerBox() {
    blockdata::onErase([](GlobalPos const& pos, blockdata::Json const&, std::string const&) { cache().erase(pos); });
    valuebox::registerBox(kSpeedController, {
        .id            = "target_speed",
        .label         = "create.kinetics.speed_controller.rotation_speed",
        .labelFallback = "Targeted Speed in RPM",
        .board         = [](Block const&) { return core::common::signedBoard(maxSpeed(), 32, "⟳", "⟲"); },
        .centre        = [](Block const& b, int face) -> std::optional<core::common::Vec> {
            if (face < 2) return std::nullopt;
            int axis = game::intState(b, kAxis).value_or(0);
            if (static_cast<int>(ck::axisOf(static_cast<Face>(face))) == axis) return std::nullopt;
            return core::common::faceLocalPoint(face, 8, 11, -.5);
        },
        .scale  = 0.5,
        .get    = [](GlobalPos const& p) { return core::common::signedToSettings(targetSpeed(p)); },
        .set    = [](GlobalPos const& p, core::common::ValueSettings s, Player&) { setTargetSpeed(p, core::common::signedFromSettings(s, maxSpeed())); },
        .format = [](Block const&, core::common::ValueSettings s) {
            return std::to_string(std::max(1, s.value)) + (s.row == 0 ? "⟳" : "⟲");
        },
    });
}

} // namespace lv::feature::transmission
