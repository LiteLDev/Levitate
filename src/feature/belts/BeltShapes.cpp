// Belts unit: the collision shape of sloped belts. Bedrock's JSON collision box is one box per
// block; a sloped belt collides as a staircase of eight 2 px steps following its band (Create's
// BeltShapes for the diagonal belt), so entities walk up and down it instead of standing on a
// half slab. Hooks the block type's collision query like the building unit does (server and client
// mod run the same code; vanilla clients are corrected by the server's movement authority).

#include <algorithm>
#include <array>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/GetCollisionShapeInterface.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltsInternal.h"

namespace lv::feature::belts {

using Box = std::array<float, 6>; ///< x0 y0 z0 x1 y1 z1 in pixels of the block

std::vector<Box> slopeCollision(cb::SegmentState const& s) {
    std::vector<Box> out;
    if (s.slope != cb::Slope::Upward && s.slope != cb::Slope::Downward) return out;
    // the band rises along the facing (upward) or against it (downward); its top is ~6 px above
    // the lower edge of the block and leaves the block at the top 10 px further (belt.geo.json)
    cb::Vec3i n = cb::facingNormal(s.facing);
    int       dx = s.slope == cb::Slope::Upward ? n.x : -n.x;
    int       dz = s.slope == cb::Slope::Upward ? n.z : -n.z;
    for (int i = 0; i < 8; ++i) {
        float a0 = 2.f * i, a1 = a0 + 2.f;
        float y0 = std::max(0.f, a0 - 2.f), y1 = std::min(16.f, a1 + 6.f);
        // along the rising direction: [a0, a1]; across: the band's 14 px
        auto along = [&](int d) -> std::pair<float, float> { return d > 0 ? std::pair{a0, a1} : std::pair{16.f - a1, 16.f - a0}; };
        Box  b;
        if (dx != 0) {
            auto [x0, x1] = along(dx);
            b             = {x0, y0, 1.f, x1, y1, 15.f};
        } else {
            auto [z0, z1] = along(dz);
            b             = {1.f, y0, z0, 15.f, y1, z1};
        }
        out.push_back(b);
    }
    return out;
}

LL_TYPE_INSTANCE_HOOK(
    BeltsCollisionHook,
    ll::memory::HookPriority::Normal,
    BlockType,
    &BlockType::$addCollisionShapes,
    bool,
    ::Block const&                                     block,
    ::IConstBlockSource const&                         region,
    ::BlockPos const&                                  pos,
    ::AABB const*                                      intersectTestBox,
    ::std::vector<::AABB>&                             inoutBoxes,
    ::optional_ref<::GetCollisionShapeInterface const> entity
) {
    if (getTypeName() != kBeltId) return origin(block, region, pos, intersectTestBox, inoutBoxes, entity);
    auto seg = segmentOf(block);
    if (!seg) return origin(block, region, pos, intersectTestBox, inoutBoxes, entity);
    auto boxes = slopeCollision(*seg);
    if (boxes.empty()) return origin(block, region, pos, intersectTestBox, inoutBoxes, entity);
    bool added = false;
    for (auto const& b : boxes) {
        AABB box{
            Vec3{pos.x + b[0] / 16.f, pos.y + b[1] / 16.f, pos.z + b[2] / 16.f},
            Vec3{pos.x + b[3] / 16.f, pos.y + b[4] / 16.f, pos.z + b[5] / 16.f}
        };
        if (intersectTestBox && !intersectTestBox->intersects(box)) continue;
        inoutBoxes.push_back(box);
        added = true;
    }
    return added;
}

void installShapes() { BeltsCollisionHook::hook(); }
void uninstallShapes() { BeltsCollisionHook::unhook(); }

} // namespace lv::feature::belts
