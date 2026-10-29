// Contract additions for linear anchors (U20): see PistonsExt.h.

#include "feature/contraption/PistonsExt.h"

#include <map>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "core/pistons/Linear.h"
#include "feature/contraption/Internal.h"
#include "mod/Config.h"

namespace lv::feature::contraption::ext {

namespace {
std::map<std::string, PassThrough, std::less<>>& passThroughs() {
    static std::map<std::string, PassThrough, std::less<>> m;
    return m;
}
} // namespace

cc::BlockInfo describe(int dim, cc::Pos const& pos) {
    auto* region = detail::regionOf(dim);
    if (!region) {
        cc::BlockInfo info;
        info.loaded = false;
        return info;
    }
    return detail::describe(*region, dim, pos);
}

bool hasCollision(int dim, cc::Pos const& pos) {
    auto* region = detail::regionOf(dim);
    if (!region || !region->hasChunksAt(detail::toBlockPos(pos), 0, false)) return false;
    return detail::collisionOf(*region, pos).has_value();
}

bool powered(int dim, cc::Pos const& pos) {
    auto* region = detail::regionOf(dim);
    return region && detail::powered(*region, pos);
}

int maxChassisRange() { return config::get<Config>().maxChassisRange; }

cc::AssemblyResult dryRun(AssembleOptions const& o, std::function<void(cc::Pos const&, cc::BlockInfo&)> const& adjust) {
    cc::AssemblyResult r;
    auto*              region = detail::regionOf(o.anchor.dim);
    if (!region) {
        r.error = cc::AssemblyError::UnloadedChunk;
        return r;
    }
    cc::Pos anchor{o.anchor.x, o.anchor.y, o.anchor.z};
    int     dim   = o.anchor.dim;
    auto    start = o.start;
    if (start.empty()) start.push_back(anchor);
    bool includeAnchor = o.includeAnchor || o.start.empty();
    auto query         = [&](cc::Pos const& p) {
        auto info = detail::describe(*region, dim, p);
        if (p == anchor && !includeAnchor) info.anchor = true;
        if (adjust) adjust(p, info);
        return info;
    };
    auto glue = [&](cc::Pos const& p, cc::Dir d) { return detail::glued(dim, p, d); };
    cc::AssemblyLimits limits;
    limits.maxBlocks = config::get<Config>().maxBlocksMoved;
    limits.forced    = o.forced;
    limits.minY      = region->getMinHeight();
    limits.maxY      = region->getMaxHeight();
    return cc::assemble(start, query, glue, limits);
}

AssemblyError errorOf(cc::AssemblyResult const& r) {
    auto x = std::to_string(r.errorPos.x), y = std::to_string(r.errorPos.y), z = std::to_string(r.errorPos.z);
    switch (r.error) {
    case cc::AssemblyError::Unmovable:
        return {"create.gui.assembly.exception.unmovableBlock", "Unmovable Block ({3}) at [{0},{1},{2}]", {x, y, z, r.errorBlock}};
    case cc::AssemblyError::UnloadedChunk:
        return {"create.gui.assembly.exception.chunkNotLoaded", "The Block at [{0},{1},{2}] was not in a loaded chunk", {x, y, z}};
    case cc::AssemblyError::TooLarge:
        return {
            "create.gui.assembly.exception.structureTooLarge",
            "There are too many Blocks included in the contraption.\nThe configured maximum is: {0}",
            {std::to_string(config::get<Config>().maxBlocksMoved)}
        };
    default:
        return {"create.gui.assembly.exception", "This Contraption was unable to assemble:", {}};
    }
}

void registerTerrainPassThrough(std::string actorBlockId, PassThrough fn) { passThroughs()[std::move(actorBlockId)] = std::move(fn); }

std::vector<StructureBlock> structureOf(Contraption const& c) {
    std::vector<StructureBlock> out;
    out.reserve(c.blocks().size());
    for (auto const& b : c.blocks()) out.push_back({b.local, b.id, b.collision.has_value()});
    return out;
}

std::vector<StructureBlock> structureAt(int dim, std::vector<cc::Pos> const& world, cc::Pos const& origin) {
    std::vector<StructureBlock> out;
    auto*                       region = detail::regionOf(dim);
    if (!region) return out;
    for (auto const& p : world) {
        auto bp = detail::toBlockPos(p);
        if (!region->hasChunksAt(bp, 0, false)) continue;
        out.push_back({p - origin, region->getBlock(bp).getTypeName(), detail::collisionOf(*region, p).has_value()});
    }
    return out;
}

bool collidesWithWorld(
    int                                        dim,
    std::vector<StructureBlock> const&         blocks,
    cc::Pos const&                             grid,
    cc::Dir                                    dir,
    std::function<bool(cc::Pos const&)> const& ignore
) {
    auto* region = detail::regionOf(dim);
    if (!region) return true;
    std::vector<core::pistons::ColliderBlock> cb;
    std::map<cc::Pos, std::string const*>      ids;
    cb.reserve(blocks.size());
    for (auto const& b : blocks) {
        cb.push_back({b.local, b.solid});
        ids[b.local] = &b.id;
    }
    for (auto const& local : core::pistons::colliders(cb, dir)) {
        cc::Pos world = local + grid;
        auto    bp    = detail::toBlockPos(world);
        if (world.y < region->getMinHeight() || world.y >= region->getMaxHeight()) return true;
        if (!region->hasChunksAt(bp, 0, false)) return true;
        if (ignore && ignore(world)) continue;
        auto const& block = region->getBlock(bp);
        if (block.isAir()) continue;
        std::string const& worldId = block.getTypeName();
        if (worldId == "minecraft:cocoa") continue;
        bool empty = !detail::collisionOf(*region, world).has_value();
        if (auto it = ids.find(local); it != ids.end()) {
            if (auto p = passThroughs().find(*it->second); p != passThroughs().end()) {
                if (!p->second(*region, world, worldId) && !empty) return true;
                continue;
            }
        }
        bool replaceable = block.getBlockType().canBeBuiltOver(block, *region, bp);
        if (!replaceable && !empty) return true;
    }
    return false;
}

void playSound(int dim, cc::Vec3d const& pos, std::string const& sound, float volume, float pitch) {
    detail::playSound(dim, pos, sound, volume, pitch);
}

void actionBar(Player& player, std::string const& text) { detail::actionBar(player, text); }

} // namespace lv::feature::contraption::ext
