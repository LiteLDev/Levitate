// Building unit: collision shapes Bedrock's JSON collision box can't express (one box per block):
// stairs, sliding doors, panes and bars, girders, and the context-dependent scaffolding shape.
// Hooks the block type's collision query, so the server and the client mod (same code in the
// client build) agree; vanilla clients are corrected by the server's movement authority.
//
// Ladders and scaffolding are made climbable by giving their block types the Climbable
// property (custom blocks have no JSON component for it).

#include <atomic>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockProperty.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/GetCollisionShapeInterface.h"
#include "mc/world/phys/AABB.h"

#include "feature/building/Building.h"

namespace lv::feature::building {

namespace {

Box rotY(Box b, int quarters) {
    for (int i = 0; i < ((quarters % 4) + 4) % 4; ++i) b = {16 - b[5], b[1], b[0], 16 - b[2], b[4], b[3]};
    return b;
}
Box clip(Box b) {
    for (int i = 0; i < 3; ++i) {
        b[i]     = std::clamp(b[i], 0.f, 16.f);
        b[i + 3] = std::clamp(b[i + 3], 0.f, 16.f);
    }
    return b;
}

Info const* infoOfType(BlockType const& type) {
    auto const& n = type.getTypeName();
    if (n.size() <= 9 || n[0] != 'l' || n.compare(0, 9, "levitate:") != 0) return nullptr;
    return infoOf(std::string_view(n));
}

bool climbable(Kind k) { return k == Kind::Ladder || k == Kind::Scaffold; }

void markClimbable(BlockType& type) {
    BlockProperty& props = type.mProperties;
    props       = static_cast<BlockProperty>(static_cast<std::uint64_t>(props) | static_cast<std::uint64_t>(BlockProperty::Climbable));
}

} // namespace

std::optional<std::vector<Box>> collisionBoxes(Info const& info, Block const& block, bool aboveFeet, bool descending) {
    std::vector<Box> out;
    switch (info.kind) {
    case Kind::CopperStairs: {
        int facing = hfacingOf(block), shape = stateOf(block, "levitate:shape"), upside = stateOf(block, "levitate:upside");
        float slab0 = upside ? 8.f : 0.f, step0 = upside ? 0.f : 8.f;
        out.push_back({0, slab0, 0, 16, slab0 + 8, 16});
        // quadrants for facing north (0 nw, 1 ne, 2 se, 3 sw), as the model
        static constexpr int quads[5][3]{{0, 1, -1}, {0, 1, 3}, {0, 1, 2}, {0, -1, -1}, {1, -1, -1}};
        static constexpr Box quadBox[4]{{0, 0, 0, 8, 0, 8}, {8, 0, 0, 16, 0, 8}, {8, 0, 8, 16, 0, 16}, {0, 0, 8, 8, 0, 16}};
        for (int q : quads[std::clamp(shape, 0, 4)]) {
            if (q < 0) continue;
            Box b = quadBox[(q + quarterOf(facing)) % 4];
            b[1]  = step0;
            b[4]  = step0 + 8;
            out.push_back(b);
        }
        return out;
    }
    case Kind::Door: {
        int  q    = quarterOf(hfacingOf(block));
        // a closing door keeps its open shape until the panel is back (Create: !open && visible)
        bool open = stateOf(block, "levitate:open") != 0 || stateOf(block, "levitate:closing") != 0;
        int  hinge = stateOf(block, "levitate:hinge");
        Box  b;
        if (!open) b = {0, 0, 13, 16, 16, 16};
        else if (info.folds) b = hinge ? Box{13, 0, 7, 19, 16, 16} : Box{-3, 0, 7, 3, 16, 16};
        else b = hinge ? Box{13, 0, 13, 29, 16, 16} : Box{-13, 0, 13, 3, 16, 16};
        out.push_back(clip(rotY(b, q)));
        return out;
    }
    case Kind::Pane:
    case Kind::Bars: {
        out.push_back({7, 0, 7, 9, 16, 9});
        if (stateOf(block, "levitate:north")) out.push_back({7, 0, 0, 9, 16, 7});
        if (stateOf(block, "levitate:south")) out.push_back({7, 0, 9, 9, 16, 16});
        if (stateOf(block, "levitate:west")) out.push_back({0, 0, 7, 7, 16, 9});
        if (stateOf(block, "levitate:east")) out.push_back({9, 0, 7, 16, 16, 9});
        return out;
    }
    case Kind::Scaffold: {
        // Create's MetalScaffoldingBlock: the bottom variant is a half block; otherwise vanilla
        // scaffolding: solid top and legs while standing on it, nothing when inside or sneaking
        if (stateOf(block, "levitate:bottom")) {
            out.push_back({0, 8, 0, 16, 16, 16});
            return out;
        }
        if (aboveFeet && !descending) {
            out.push_back({0, 14, 0, 16, 16, 16});
            out.push_back({0, 0, 0, 2, 14, 2});
            out.push_back({14, 0, 0, 16, 14, 2});
            out.push_back({0, 0, 14, 2, 14, 16});
            out.push_back({14, 0, 14, 16, 14, 16});
        }
        return out;
    }
    case Kind::Girder: {
        bool x = stateOf(block, "levitate:x") != 0, z = stateOf(block, "levitate:z") != 0;
        if (x && z) out.push_back({3, 0, 3, 13, 16, 13});
        if (x) out.push_back({0, 2, 4, 16, 14, 12});
        if (z) out.push_back({4, 2, 0, 12, 14, 16});
        if (!x && !z) out.push_back({4, 0, 4, 12, 16, 12});
        return out;
    }
    case Kind::GirderShaft: {
        int axis = stateOf(block, "levitate:axis");
        if (axis == 0) {
            out.push_back({4, 2, 0, 12, 14, 16});
            out.push_back({0, 5, 5, 16, 11, 11});
        } else {
            out.push_back({0, 2, 4, 16, 14, 12});
            out.push_back({5, 5, 0, 11, 11, 16});
        }
        return out;
    }
    default: return std::nullopt;
    }
}

LL_TYPE_INSTANCE_HOOK(
    BuildingCollisionHook,
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
    Info const* info = infoOfType(*this);
    if (!info) return origin(block, region, pos, intersectTestBox, inoutBoxes, entity);
    if (climbable(info->kind) && !block.hasProperty(BlockProperty::Climbable)) markClimbable(const_cast<BlockType&>(static_cast<BlockType const&>(*this)));
    bool aboveFeet = false, descending = false;
    if (entity) {
        aboveFeet  = entity->getFeetAttachPosY() >= static_cast<float>(pos.y) + 1.f - 1e-5f;
        descending = entity->canDescendThroughBlock();
    }
    auto boxes = collisionBoxes(*info, block, aboveFeet, descending);
    if (!boxes) return origin(block, region, pos, intersectTestBox, inoutBoxes, entity);
    bool added = false;
    for (auto const& b : *boxes) {
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

void applyBlockProperties() {
    for (auto const* info : allBlocks()) {
        if (!climbable(info->kind)) continue;
        auto type = BlockType::tryGetFromRegistry(HashedString(info->id));
        if (type) markClimbable(const_cast<BlockType&>(*type));
    }
}

void installShapes() { BuildingCollisionHook::hook(); }
void uninstallShapes() { BuildingCollisionHook::unhook(); }

} // namespace lv::feature::building

// Create's requiresCorrectToolForDrops and the copycats' material rules (U59; an .inc so the build
// adds no object file: the prelink command line is near the Windows limit)
#include "feature/building/Harvest.inc"
