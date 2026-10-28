// Mechanical plough (Create's PloughMovementBehaviour). No stationary effect.
//
// Its active area (0.45 blocks in front) breaks blocks without a collision shape (grass,
// flowers, snow layers, torches, rails...) except on farmland, throws entities in its way, and
// uses a hoe on the block below each visited block (vanilla tilling rules).

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "ll/api/service/Bedrock.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/Level.h"

#include "feature/actors/Actors.h"
#include "feature/contraption/ActorsExt.h"
#include "game/Blocks.h"

namespace lv::feature::actors {

namespace ext = ::lv::feature::contraption::ext;

namespace {

/// Create's PloughMovementBehaviour.canBreak.
bool ploughCanBreak(BlockSource& region, BlockPos const& pos, Block const& block) {
    if (block.isAir()) return false;
    BlockPos below{pos.x, pos.y - 1, pos.z};
    if (region.getBlock(below).getTypeName() == "minecraft:farmland") return false;
    std::string id = block.getTypeName();
    if (isLiquid(id)) return false;
    if (id == "levitate:track") return true;
    if (id == "levitate:fake_track") return false;
    if (id == "minecraft:snow_layer") return true; // Create's plough whitelist
    return !hasCollision(region, pos);
}

/// Vanilla hoe use on `pos` (the block below the visited one): grass, dirt and dirt paths
/// become farmland, coarse dirt becomes dirt, rooted dirt becomes dirt and drops hanging roots.
/// Needs air above.
void till(con::MovementContext& ctx, BlockPos const& pos) {
    auto* region = regionOf(ctx.contraption.dimension());
    if (!region || !region->hasChunksAt(pos, 0, false)) return;
    BlockPos above{pos.x, pos.y + 1, pos.z};
    if (!region->getBlock(above).isAir()) return;
    std::string id = region->getBlock(pos).getTypeName();
    char const* result = nullptr;
    if (id == "minecraft:grass_block" || id == "minecraft:dirt" || id == "minecraft:grass_path" || id == "minecraft:dirt_path") {
        result = "minecraft:farmland";
    } else if (id == "minecraft:coarse_dirt" || id == "minecraft:rooted_dirt") {
        result = "minecraft:dirt";
    }
    if (!result) return;
    auto const* block = game::blockWith(result);
    if (!block) return;
    game::setBlock(*region, pos, *block);
    cc::Vec3d c{pos.x + .5, pos.y + 1.0, pos.z + .5};
    ext::playSound(ctx.contraption.dimension(), c, "use.gravel", 1.f, 1.f);
    if (id == "minecraft:rooted_dirt") {
        spawnItem(*region, makeItem("minecraft:hanging_roots"), c, {});
    }
    logAction("plough tilled " + id + " at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z));
}

Breaker const& breaker();

void visit(con::MovementContext& ctx, GlobalPos const& pos) {
    damageEntities(ctx, pos, false, false);
    startBreaking(ctx, breaker(), pos);
    till(ctx, BlockPos{pos.x, pos.y - 1, pos.z});
}

Breaker const& breaker() {
    static Breaker const b{
        [](con::MovementContext const& ctx) { return std::clamp(animationSpeed(ctx) / 500.f, 1.f / 128, 16.f); },
        ploughCanBreak,
        [](con::MovementContext& ctx, GlobalPos const& pos) {
            // the block is gone: plough the ground below now (Create revisits the position)
            till(ctx, BlockPos{pos.x, pos.y - 1, pos.z});
        },
        [](con::MovementContext& ctx, Block const& block, BlockPos const& pos) {
            auto* region = regionOf(ctx.contraption.dimension());
            auto  level  = ll::service::getLevel();
            if (!region || !level) return;
            // snow layers drop as if dug with an iron shovel (Create's plough fake player)
            ItemStack tool  = block.getTypeName() == "minecraft:snow_layer" ? makeItem("minecraft:iron_shovel") : ItemStack::EMPTY_ITEM();
            auto      drops = dropsOf(*region, pos, block, tool);
            std::string id  = block.getTypeName();
            level->destroyBlock(*region, pos, false, BlockChangeContext{});
            for (auto const& s : drops) collectOrDrop(ctx, s);
            logAction("plough broke " + id + " at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z));
        },
    };
    return b;
}

cc::Vec3d areaOffset(con::MovementContext const& ctx) { return cc::Vec3d::of(cc::offset(facingOf(ctx.states))) * .45; }

struct Registrar {
    Registrar() {
        con::MovementBehaviour b;
        b.isActive = [](con::MovementContext const& ctx) {
            return !pointingTowards(relativeMotion(ctx), cc::opposite(facingOf(ctx.states)));
        };
        b.tick = [](con::MovementContext& ctx) {
            tickBreaking(ctx, breaker());
            if (ctx.stall) return;
            if (auto p = newAreaPos(ctx, areaOffset(ctx))) visit(ctx, *p);
        };
        b.stopMoving = [](con::MovementContext& ctx) { cancelBreaking(ctx); };
        registerOwnActor(std::string(kPlough), std::move(b));
        con::registerMovementRules(
            std::string(kPlough),
            con::MovementRules{
                nullptr,
                [](cc::States const&) { return true; },
                [](cc::States const& s) { return cc::bit(cc::opposite(facingOf(s))); },
                nullptr,
            }
        );
    }
};
Registrar const gRegistrar;

} // namespace

} // namespace lv::feature::actors
