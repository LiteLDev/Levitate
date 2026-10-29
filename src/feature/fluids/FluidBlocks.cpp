// Honey and chocolate in the world (SPEC §3.6.19, §8): Bedrock has no data-driven fluids, so a
// bucket places a non-flowing source block (`levitate:honey` / `levitate:chocolate`) that an
// empty bucket, a hose pulley or a pump picks up again. Entities inside are slowed down as by
// Create's viscous fluids and take no fall damage; lava next to them turns into obsidian (lava
// source) or limestone / scoria (flowing lava), as Create's fluid interactions.

#include <array>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/fluids/FluidsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::fluids {

namespace items = common::items;

namespace {

constexpr unsigned kSlowness = 2; ///< Bedrock MobEffect id

bool isFluidBlock(std::string const& id) { return id == kHoneyBlock || id == kChocolateBlock; }

std::string fluidOfBlock(std::string const& id) {
    if (id == kHoneyBlock) return std::string(cf::kHoney);
    if (id == kChocolateBlock) return std::string(cf::kChocolate);
    return {};
}

std::string blockOfBucket(std::string const& itemId) {
    if (itemId == kHoneyBucket) return kHoneyBlock;
    if (itemId == kChocolateBucket) return kChocolateBlock;
    return {};
}

/// Slows an entity in honey / chocolate (Create's fluid viscosity) and resets its fall.
void affect(Actor& a) {
    if (!a.isAlive()) return;
    a.setFallDistance(0);
    if (a.getEntityTypeId() == ActorType::ItemEntity) return;
    MobEffectInstance e(kSlowness, EffectDuration{10});
    e.mAmplifier     = 1;
    e.mEffectVisible = false;
    e.mAmbient       = true;
    a.addEffect(e);
}

bool inFluid(BlockSource& region, Vec3 const& feet, float height) {
    for (float dy : {0.1f, height * 0.5f, height - 0.1f}) {
        BlockPos bp = game::blockPosOf(Vec3{feet.x, feet.y + dy, feet.z});
        if (!region.hasChunksAt(bp, 0, false)) continue;
        if (isFluidBlock(region.getBlock(bp).getTypeName())) return true;
    }
    return false;
}

} // namespace

bool useBucket(Player& player, GlobalPos const& clicked, int face, ItemStack const& held) {
    auto* region = regionOf(clicked.dim);
    if (!region) return false;
    auto heldId   = items::idOf(held);
    bool creative = player.isCreative();
    if (!player.canUseAbility(AbilitiesIndex::Build)) return false; // adventure mode, no build permission
    // an empty bucket on a honey / chocolate source picks it up
    if (heldId == "minecraft:bucket") {
        auto id = blockIdAt(clicked);
        if (!isFluidBlock(id)) return false;
        auto bucket = bucketOf(fluidOfBlock(id));
        auto filled = items::make(bucket);
        if (!filled) return false;
        game::clearBlock(*region, toBlockPos(clicked));
        if (!creative) player.setSelectedItem(items::withCount(held, items::countOf(held) - 1));
        giveOrDrop(player, *filled);
        playSound(clicked, "bucket.fill_water", 1.f, 0.8f);
        log().info("{} picked up {} at {} {} {}", player.getRealName(), fluidOfBlock(id), clicked.x, clicked.y, clicked.z);
        return true;
    }
    // a honey / chocolate bucket places a source block
    auto block = blockOfBucket(heldId);
    if (block.empty()) return false;
    GlobalPos target = clicked;
    auto      bp     = toBlockPos(clicked);
    if (!game::replaceable(*region, bp) || isFluidBlock(blockIdAt(clicked))) target = offsetPos(clicked, face);
    if (!game::replaceable(*region, toBlockPos(target)) || isFluidBlock(blockIdAt(target))) return false;
    auto const* b = game::blockWith(block);
    if (!b) return false;
    game::setBlock(*region, toBlockPos(target), *b);
    if (!creative) {
        auto empty = items::make("minecraft:bucket");
        player.setSelectedItem(empty ? *empty : items::empty());
    }
    playSound(target, "bucket.empty_water", 1.f, 0.8f);
    log().info("{} placed {} at {} {} {}", player.getRealName(), fluidOfBlock(block), target.x, target.y, target.z);
    return true;
}

void tickSources(std::uint64_t tick) {
    if (tick % 10 != 0) return;
    for (auto const& p : tracked(Kind::Source)) {
        auto* region = regionOf(p.dim);
        if (!region || !region->hasChunksAt(toBlockPos(p), 1, false)) continue;
        auto own = blockIdAt(p);
        if (!isFluidBlock(own)) continue;
        for (int f = 0; f < 6; ++f) {
            if (f == 0) continue; // lava flows down onto it, never up out of it
            GlobalPos   q  = offsetPos(p, f);
            auto const& b  = region->getBlock(toBlockPos(q));
            auto        id = b.getTypeName();
            if (id != "minecraft:lava" && id != "minecraft:flowing_lava") continue;
            bool        source = game::intState(b, "liquid_depth").value_or(0) == 0;
            std::string into   = source ? "minecraft:obsidian" : (own == kHoneyBlock ? "levitate:limestone" : "levitate:scoria");
            auto const* nb     = game::blockWith(into);
            if (!nb) continue; // the stone of another unit is not there (yet)
            game::setBlock(*region, toBlockPos(q), *nb);
            playSound(q, "random.fizz", 0.5f, 2.6f);
            log().info("lava next to {} at {} {} {} became {}", own, q.x, q.y, q.z, into);
        }
    }
}

void applyFluidEffects() {
    auto level = ll::service::getLevel();
    if (!level) return;
    // players (not returned by entity queries)
    level->forEachPlayer([](Player& pl) {
        auto* region = regionOf(static_cast<int>(pl.getDimensionId()));
        if (region && inFluid(*region, pl.getFeetPos(), 1.8f)) affect(pl);
        return true;
    });
    // other entities: around our source blocks (bounded)
    int budget = 512;
    for (auto const& p : tracked(Kind::Source)) {
        if (--budget < 0) break;
        auto* region = regionOf(p.dim);
        if (!region || !region->hasChunksAt(toBlockPos(p), 0, false)) continue;
        AABB box{
            Vec3{float(p.x), float(p.y), float(p.z)},
            Vec3{float(p.x + 1), float(p.y + 1), float(p.z + 1)}
        };
        for (auto* a : region->getEntities(box, 0.0f)) {
            if (a && !a->isPlayer()) affect(*a);
        }
    }
}

void registerSources() {}

bool poweredAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    return region && common::redstone::powered(*region, toBlockPos(p));
}

} // namespace lv::feature::fluids
