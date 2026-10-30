// The wrench on encased shafts and cogwheels (U6, U42): Create's EncasedShaftBlock /
// EncasedCogwheelBlock IWrenchable.
// - Encased cogwheel, wrench on a face along its axis: toggles the shaft end on that side
//   (top_shaft / bottom_shaft); other faces rotate it as every kinetic block.
// - Sneak + wrench: the casing comes off (the block turns back into the plain shaft / cogwheel,
//   with the block's break particles; nothing is given, as encasing does not use the casing up).

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/goggles/Wrenchable.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::transmission {

namespace {

namespace wrench = goggles::wrench;

/// Level event 2001: the break particles and sound of `block` at `pos`.
void breakEffect(GlobalPos const& pos, Block const& block) {
    auto level = ll::service::getLevel();
    if (!level) return;
    Vec3 at{static_cast<float>(pos.x) + .5f, static_cast<float>(pos.y) + .5f, static_cast<float>(pos.z) + .5f};
    level->broadcastLevelEvent(SharedTypes::Legacy::LevelEvent::ParticlesDestroyBlock, at, static_cast<int>(block.mNetworkId), nullptr);
}

/// The plain block an encased one turns back into.
char const* uncased(std::string_view id) {
    if (id == kAndesiteShaft || id == kBrassShaft) return kShaft;
    if (id == kAndesiteLargeCog || id == kBrassLargeCog) return kLargeCogwheel;
    return kCogwheel;
}

wrench::Result removeCasing(wrench::Context& c) {
    std::string id   = c.block.getTypeName();
    int         axis = stateAt(c.region, c.pos, kAxis, 1);
    breakEffect(c.pos, c.block);
    if (!place(c.region, c.pos, uncased(id), {{kAxis, axis}})) return wrench::Result::Success;
    log().info("transmission: {} removed the casing of {} at {}", c.player.getRealName(), id, c.pos.key());
    return wrench::Result::Success;
}

wrench::Handler encasedShaft() { return {.onSneakWrenched = removeCasing}; }

wrench::Handler encasedCog() {
    return {
        .onWrenched =
            [](wrench::Context& c) {
                int  axis = stateAt(c.region, c.pos, kAxis, 1);
                Face face = static_cast<Face>(c.face);
                if (c.face < 0 || c.face > 5 || static_cast<int>(ck::axisOf(face)) != axis) return wrench::defaultOnWrenched(c);
                bool        positive = ck::isPositive(face);
                char const* state    = positive ? kTopShaft : kBottomShaft;
                int         now      = stateAt(c.region, c.pos, state, 0);
                std::string id       = c.block.getTypeName();
                place(c.region, c.pos, id, {{kAxis, axis}, {kTopShaft, positive ? 1 - now : stateAt(c.region, c.pos, kTopShaft, 0)},
                                            {kBottomShaft, positive ? stateAt(c.region, c.pos, kBottomShaft, 0) : 1 - now}});
                wrench::playRotateSound(c.pos);
                log().info("transmission: {} toggled the {} shaft of {} at {} to {}", c.player.getRealName(), positive ? "top" : "bottom", id, c.pos.key(), 1 - now);
                return wrench::Result::Success;
            },
        .onSneakWrenched = removeCasing,
    };
}

LV_REGISTER_WRENCHABLE(kAndesiteShaft, encasedShaft());
LV_REGISTER_WRENCHABLE(kBrassShaft, encasedShaft());
LV_REGISTER_WRENCHABLE(kAndesiteCog, encasedCog());
LV_REGISTER_WRENCHABLE(kBrassCog, encasedCog());
LV_REGISTER_WRENCHABLE(kAndesiteLargeCog, encasedCog());
LV_REGISTER_WRENCHABLE(kBrassLargeCog, encasedCog());

} // namespace

} // namespace lv::feature::transmission
