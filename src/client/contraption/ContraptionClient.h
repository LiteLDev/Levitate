#pragma once

// Client mod (client build only): the blocks of moving contraptions, for units that animate
// their actors (harvester blades, roller drums, drills, ...) on top of the display entities.
//
// The server tells companions which display entity shows which block of which contraption
// (channel "contraption.blocks", sent when displays are spawned), the contraption's transform
// each tick it changes ("contraption.motion"; the client moves the display entities from it) and
// each actor's animation state (`contraption::setActorVisual`, channel "contraption.visual").
// `forEachBlock` gives every known block of the player's dimension with its world transform at
// this frame's partial tick, so drawing follows the contraption exactly as the display entities do:
//
//     lv::client::contraption::forEachBlock("levitate:mechanical_harvester", partialTicks,
//         [&](auto const& b) { drawBlade(b.center, b.axis, b.angle, b.visual.value("speed", 0.f)); });
//
// Client thread only (render passes).

#ifdef LV_CLIENT

#include <cstdint>
#include <functional>
#include <string>

#include "nlohmann/json.hpp"

#include "core/contraption/Geometry.h"

namespace lv::client::contraption {

struct BlockView {
    std::uint64_t                    contraption = 0;
    core::contraption::Pos           local;   ///< the block in contraption coordinates
    std::string                      blockId;
    core::contraption::Vec3d         center;  ///< interpolated world centre of the block this frame
    int                              axis  = 1; ///< contraption rotation axis (0 x, 1 y, 2 z)
    double                           angle = 0; ///< contraption rotation (degrees, right-hand rule)
    int                              tiltAxis = 0; ///< local axis of the tilt applied before the rotation
    double                           tilt     = 0; ///< degrees (a carriage on a slope; see cc::Transform)
    nlohmann::json const*            visual = nullptr; ///< the actor's animation state (nullptr: none)
};

/// Calls `fn` for every contraption block of the player's dimension whose id is `blockId` (""
/// = every block) and whose display entity is loaded, at `partialTicks` (0..1).
void forEachBlock(std::string const& blockId, float partialTicks, std::function<void(BlockView const&)> const& fn);

} // namespace lv::client::contraption

#endif // LV_CLIENT
