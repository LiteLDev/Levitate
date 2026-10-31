// The wrench on encased shafts and cogwheels (U6, U42): Create's EncasedShaftBlock /
// EncasedCogwheelBlock IWrenchable.
// - Encased cogwheel, wrench on a face along its axis: toggles the shaft end on that side
//   (top_shaft / bottom_shaft); other faces rotate it as every kinetic block.
// - Sneak + wrench: the casing comes off (the block turns back into the plain shaft / cogwheel,
//   with the block's break particles; nothing is given, as encasing does not use the casing up).
// - Encased cogwheels turned on another face swap their shaft ends with the turn
//   (EncasedCogwheelBlock.swapShaftsForRotation), on contraptions too.
// - Chain drives (ChainDriveBlock.getRotatedBlockState / updateAfterWrenched): a connected chain
//   drive turns about its connection axis, then its part is worked out again from part none.
// - The speed controller (a HorizontalAxisKineticBlock) only turns on its top and bottom faces.

#include <algorithm>
#include <variant>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "core/goggles/Rotation.h"
#include "feature/contraption/Contraption.h"
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

namespace cg = core::goggles;

/// `block` with some integer states changed (the others kept).
Block const* withInts(Block const& block, std::initializer_list<std::pair<char const*, int>> states) {
    Block const* out = &block;
    for (auto const& [name, value] : states) {
        auto const* def = out->getBlockType().getBlockState(HashedString(name));
        if (!def) return &block;
        auto next = out->setState(*def, value);
        if (!next) return &block;
        out = next.as_ptr();
    }
    return out;
}

/// The axis after a quarter turn about `faceAxis` (VoxelShaper.axisAsFace(axis).getClockWise(faceAxis)).
int turnedAxis(int axis, int faceAxis) { return cg::axisOfDirection(cg::clockwise(cg::positiveDirection(axis), faceAxis)); }

/// EncasedCogwheelBlock.getRotatedBlockState: the shaft ends follow the turn (the top end is the
/// positive one; they swap when it ends up on the negative side).
Block const* rotatedEncasedCog(Block const& block, int face) {
    if (face < 0 || face > 5) return &block;
    int axis     = std::clamp(game::intState(block, kAxis).value_or(1), 0, 2);
    int faceAxis = cg::axisOfDirection(face);
    int top = game::intState(block, kTopShaft).value_or(0), bottom = game::intState(block, kBottomShaft).value_or(0);
    if (axis != faceAxis && !ck::isPositive(static_cast<Face>(cg::clockwise(cg::positiveDirection(axis), faceAxis)))) std::swap(top, bottom);
    return withInts(block, {{kAxis, turnedAxis(axis, faceAxis)}, {kTopShaft, top}, {kBottomShaft, bottom}});
}

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
        .rotate          = rotatedEncasedCog,
    };
}

// ChainDriveBlock: getRotatedBlockState turns a connected drive about its connection axis;
// updateAfterWrenched starts from the default state (part none) with the new axis, reads the
// redstone again and runs updateShape towards every side.
wrench::Result wrenchChainDrive(wrench::Context& c) {
    std::string id = c.block.getTypeName();
    if (c.face < 0 || c.face > 5) return wrench::Result::Success;
    int  axis = std::clamp(stateAt(c.region, c.pos, kAxis, 1), 0, 2);
    auto part = static_cast<ct::ChainPart>(std::clamp(stateAt(c.region, c.pos, kPart, 0), 0, 3));
    ct::ChainState now{static_cast<Axis>(axis), part, stateAt(c.region, c.pos, kAlongFirst, 0) != 0};
    int turnAbout = part == ct::ChainPart::None ? cg::axisOfDirection(c.face) : static_cast<int>(ct::chainConnectionAxis(now));
    auto next     = chainShapeAt(c.region, c.pos, ct::ChainState{static_cast<Axis>(turnedAxis(axis, turnAbout)), ct::ChainPart::None, false});
    std::vector<std::pair<std::string_view, int>> states{
        {kAxis, static_cast<int>(next.axis)}, {kPart, static_cast<int>(next.part)}, {kAlongFirst, next.alongFirst ? 1 : 0}
    };
    if (id == kChainGearshift) states.emplace_back(kSignal, neighbourSignal(c.region, c.pos));
    if (next == now && (id != kChainGearshift || stateAt(c.region, c.pos, kSignal, 0) == states.back().second)) return wrench::Result::Success;
    if (!place(c.region, c.pos, id, states)) return wrench::Result::Success;
    wrench::playRotateSound(c.pos);
    log().info("wrench: {} rotated {} at {} (face {})", c.player.getRealName(), id, c.pos.key(), c.face);
    return wrench::Result::Success;
}

// HorizontalAxisKineticBlock: only a click on the top or bottom turns the horizontal axis.
Block const* rotatedSpeedController(Block const& block, int face) {
    if (face != 0 && face != 1) return &block;
    int axis = game::intState(block, kAxis).value_or(0);
    if (axis == 1) return &block;
    return withInts(block, {{kAxis, axis == 0 ? 2 : 0}});
}

LV_REGISTER_WRENCHABLE(kAndesiteShaft, encasedShaft());
LV_REGISTER_WRENCHABLE(kBrassShaft, encasedShaft());
LV_REGISTER_WRENCHABLE(kAndesiteCog, encasedCog());
LV_REGISTER_WRENCHABLE(kBrassCog, encasedCog());
LV_REGISTER_WRENCHABLE(kAndesiteLargeCog, encasedCog());
LV_REGISTER_WRENCHABLE(kBrassLargeCog, encasedCog());
LV_REGISTER_WRENCHABLE(kChainDrive, wrench::Handler{.onWrenched = wrenchChainDrive});
LV_REGISTER_WRENCHABLE(kChainGearshift, wrench::Handler{.onWrenched = wrenchChainDrive});
LV_REGISTER_WRENCHABLE(kSpeedController, wrench::Handler{.rotate = rotatedSpeedController});

} // namespace

/// EncasedCogwheelBlock.transform on contraptions: the generic rotation turned levitate:axis; the
/// shaft ends swap when the positive end ends up on the negative side.
void installEncasedCogRotation() {
    namespace cc = contraption::cc;
    for (auto const* id : {kAndesiteCog, kBrassCog, kAndesiteLargeCog, kBrassLargeCog}) {
        contraption::registerRotationHandler(id, [](cc::States& s, cc::Axis about, int quarter) {
            auto axisIt = s.find(kAxis);
            auto topIt = s.find(kTopShaft), bottomIt = s.find(kBottomShaft);
            if (axisIt == s.end() || topIt == s.end() || bottomIt == s.end()) return;
            auto const* now = std::get_if<std::int64_t>(&axisIt->second);
            if (!now || *now < 0 || *now > 2) return;
            auto before = cc::rotate(static_cast<cc::Axis>(*now), about, -quarter); // the axis before the turn
            if (!cc::positive(cc::rotate(cc::dirOf(before, true), about, quarter))) std::swap(topIt->second, bottomIt->second);
        });
    }
}

} // namespace lv::feature::transmission
