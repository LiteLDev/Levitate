// Entities collide with moving contraption blocks (Create's ContraptionCollider, U38 R12).
//
// The game asks a BlockSource for the collision shapes around a moving entity (both the shape
// list and the shape + block list the movement systems use); the shapes of the contraption blocks
// there are added: on the server (every entity, the server's simulation of player movement too)
// from the engine, on the client mod's side (the local player's own movement prediction) from the
// transforms the server sent. Each carried block collides as the bounding box of its turned
// collision box (Create's oriented boxes, approximated). The server still pushes and carries
// entities each tick (Engine.cpp) for clients without the mod.

#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/vanilla_components/block_source_visitor/CollisionShape.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "feature/contraption/Internal.h"
#include "game/Blocks.h"
#include "mod/Module.h"

#ifdef LV_CLIENT
#include "client/contraption/Display.h"
#endif

class GetCollisionShapeInterface;

namespace lv::feature::contraption {

namespace {

void forEachShape(BlockSource const& region, AABB const& query, std::function<void(AABB const&, Block const*)> const& fn) {
    auto& self = const_cast<BlockSource&>(region);
    int   dim  = static_cast<int>(self.getDimensionId());
    if (!self.getLevel().isClientSide()) {
        detail::forEachCollision(dim, query, fn);
        return;
    }
#ifdef LV_CLIENT
    thread_local std::vector<std::pair<cc::Box, std::string>> boxes;
    boxes.clear();
    cc::Box q{{query.min.x, query.min.y, query.min.z}, {query.max.x, query.max.y, query.max.z}};
    client::contraption::appendCollision(dim, q, boxes);
    thread_local std::unordered_map<std::string, Block const*> byId;
    for (auto const& [b, id] : boxes) {
        auto it = byId.find(id);
        if (it == byId.end()) it = byId.emplace(id, game::blockWith(id)).first;
        fn(AABB{Vec3{float(b.min.x), float(b.min.y), float(b.min.z)}, Vec3{float(b.max.x), float(b.max.y), float(b.max.z)}}, it->second);
    }
#endif
}

using ShapesFn = void (BlockSource::*)(std::vector<AABB>&, AABB const&, bool, optional_ref<GetCollisionShapeInterface const>, std::vector<AABB>*) const;

} // namespace

LL_TYPE_INSTANCE_HOOK(
    ContraptionShapesHook,
    ll::memory::HookPriority::Normal,
    BlockSource,
    static_cast<ShapesFn>(&BlockSource::$fetchCollisionShapes),
    void,
    std::vector<AABB>&                             shapes,
    AABB const&                                    intersectTestBox,
    bool                                           withUnloadedChunks,
    optional_ref<GetCollisionShapeInterface const> entity,
    std::vector<AABB>*                             tempShapes
) {
    origin(shapes, intersectTestBox, withUnloadedChunks, entity, tempShapes);
    forEachShape(*this, intersectTestBox, [&](AABB const& box, Block const*) { shapes.push_back(box); });
}

LL_TYPE_INSTANCE_HOOK(
    ContraptionShapesAndBlocksHook,
    ll::memory::HookPriority::Normal,
    BlockSource,
    &BlockSource::$fetchCollisionShapesAndBlocks,
    void,
    std::vector<BlockSourceVisitor::CollisionShape>& shapes,
    AABB const&                                      intersectTestBox,
    bool                                             withUnloadedChunks,
    optional_ref<GetCollisionShapeInterface const>   entity,
    std::vector<AABB>*                               tempShapes
) {
    origin(shapes, intersectTestBox, withUnloadedChunks, entity, tempShapes);
    static Block const* fallback = game::blockWith("minecraft:stone");
    forEachShape(*this, intersectTestBox, [&](AABB const& box, Block const* block) {
        BlockSourceVisitor::CollisionShape s;
        s.mMaybeBlock      = block ? block : fallback;
        s.mAABB            = box;
        s.mBlockPos        = BlockPos{int(std::floor(box.min.x)), int(std::floor(box.min.y)), int(std::floor(box.min.z))};
        s.mIsUnloadedChunk = false;
        shapes.push_back(s);
    });
}

class CollisionModule {
public:
    void install() {
        ContraptionShapesHook::hook();
        ContraptionShapesAndBlocksHook::hook();
    }
    void uninstall() {
        ContraptionShapesAndBlocksHook::unhook();
        ContraptionShapesHook::unhook();
    }
};

} // namespace lv::feature::contraption

LV_REGISTER_MODULE(lv::feature::contraption::CollisionModule);
