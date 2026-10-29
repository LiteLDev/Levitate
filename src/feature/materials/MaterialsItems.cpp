// Materials unit (U9): Create's legacy item entities. A dropped chromatic compound absorbs light
// sources near it (or turns into refined radiance over an active beacon) and becomes shadow steel
// when it falls into the void; shadow steel and refined radiance float (no gravity).

#include <algorithm>
#include <cmath>
#include <random>
#include <string>

#include "ll/api/memory/Hook.h"

#include "mc/common/Brightness.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/ShapeType.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BeaconBlockActor.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/HitResult.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/world/actor/BuiltInActorComponents.h"

#include "feature/common/Items.h"
#include "feature/materials/Materials.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::materials {

namespace {

std::string const kChromatic = "levitate:chromatic_compound";
std::string const kShadow    = "levitate:shadow_steel";
std::string const kRadiance  = "levitate:refined_radiance";
constexpr float   kGravity   = 0.04f; // Bedrock item entity gravity per tick

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
float rnd() { return std::uniform_real_distribution<float>(0, 1)(rng()); }

Vec3& velocity(Actor& a) { return a.mBuiltInComponents->mStateVectorComponent->mPosDelta; }

/// VecHelper.offsetRandomly: every axis moved by up to +-r.
Vec3 offsetRandomly(Vec3 const& v, float r) { return {v.x + (rnd() * 2 - 1) * r, v.y + (rnd() * 2 - 1) * r, v.z + (rnd() * 2 - 1) * r}; }

void particles(Actor& a, Vec3 const& at, int n) {
    for (int i = 0; i < n; ++i) a.getLevel().spawnParticleEffect("minecraft:endrod", offsetRandomly(at, 0.5f), &a.getDimension());
}

/// Create's NoGravMagicalDohickyItem.onCreated (the "JustCreated" flag is our conversion call).
void onCreated(Actor& a, bool shadow) {
    if (shadow) velocity(a) = Vec3{0, (a.getFallDistance() + 3) / 50.0f, 0};
    else velocity(a).y += 0.25f;
    particles(a, a.getPosition(), 20);
}

ItemActor* spawnLike(ItemActor& from, ItemStack const& stack) {
    auto& region = from.getDimensionBlockSource();
    auto* spawned = region.getLevel().getSpawner().spawnItem(region, stack, nullptr, from.getPosition(), 0);
    if (spawned) velocity(*spawned) = from.getPosDelta();
    return spawned;
}

bool overActiveBeacon(ItemActor& a) {
    auto&    region = a.getDimensionBlockSource();
    Vec3     p      = a.getPosition();
    int      x = static_cast<int>(std::floor(p.x)), z = static_cast<int>(std::floor(p.z));
    int      top = region.getAboveTopSolidBlock(x, z, true, true);
    int      y   = std::min(static_cast<int>(std::floor(p.y)), top);
    int      minY = region.getMinHeight();
    while (y > minY) {
        --y;
        BlockPos     bp{x, y, z};
        Block const& b = region.getBlock(bp);
        auto const   id = b.getTypeName();
        if (id == "minecraft:beacon") {
            auto* be = region.getBlockEntity(bp);
            if (!be) return false;
            return !static_cast<BeaconBlockActor*>(be)->mBeamSections->empty();
        }
        if (b.getBlockType().mLightBlock->mValue >= 15 && id != "minecraft:bedrock") return false;
    }
    return false;
}

/// ChromaticCompoundItem.checkLight: absorbs a placed light source in line of sight.
bool tryAbsorbLight(ItemActor& a, ItemStack& stack, BlockPos const& target) {
    auto&        region = a.getDimensionBlockSource();
    Block const& b      = region.getBlock(target);
    if (b.isAir() || b.getTypeName() == "minecraft:beacon") return false;
    if (b.getBlockType().getLightEmission(b).mValue == 0) return false;
    if (b.getBlockType().getDestroySpeed() < 0) return false;
    Vec3 from = a.getPosition() + Vec3{0, 0.5f, 0};
    Vec3 to{target.x + 0.5f, target.y + 0.5f, target.z + 0.5f};
    HitResult hit = region.clip(from, to, false, ShapeType::Outline, 200, false, false, nullptr, {}, false);
    if (!(hit.mType == HitResultType::Tile && hit.mBlock == target)) return false;

    ItemStack charged = stack;
    charged.set(1);
    setChromaticLight(charged, chromaticLight(stack) + 1);
    spawnLike(a, charged);
    stack.set(stack.mCount - 1);
    game::clearBlock(region, target);
    if (stack.mCount <= 0) a.remove();
    lv::log().info("materials: chromatic compound absorbed {} ({} light)", b.getTypeName(), chromaticLight(charged));
    return true;
}

} // namespace

int chromaticLight(ItemStack const& stack) {
    auto d = common::items::getData(stack, "lv:chromatic_compound");
    return d.is_object() ? d.value("light", 0) : 0;
}

void setChromaticLight(ItemStack& stack, int light) {
    common::items::setData(stack, "lv:chromatic_compound", light > 0 ? nlohmann::json{{"light", light}} : nlohmann::json());
}

void tickMagicalItem(Actor& actor) {
    auto& a     = static_cast<ItemActor&>(actor);
    auto& stack = *a.mItem;
    if (stack.isNull() || !a.isAlive()) return;
    auto const id  = stack.getTypeName();
    auto const& cfg = config::get<MaterialsConfig>();

    if (id == kShadow || id == kRadiance) {
        // Create: idle end rod particles, more for bigger stacks
        float chance = std::clamp(stack.mCount - 10.0f, 5.0f, 100.0f) / 64.0f;
        if (id == kShadow) chance = std::clamp(stack.mCount - 10.0f, std::clamp(a.getPosDelta().y * 20, 5.0f, 20.0f), 100.0f) / 64.0f;
        if (rnd() < chance * 0.25f) particles(a, a.getPosition(), 1);
        return;
    }
    if (id != kChromatic) return;
    auto&      region = a.getDimensionBlockSource();
    Vec3 const pos    = a.getPosition();
    float      vy     = a.getPosDelta().y;
    int        minY   = region.getMinHeight();

    // into the void: shadow steel
    if (pos.y < minY && pos.y - vy < minY - 10 && cfg.enableShadowSteelRecipe) {
        auto steel = makeStack(kShadow, stack.mCount);
        if (!steel.isNull()) {
            a.mItem = steel;
            onCreated(a, true);
            lv::log().info("materials: chromatic compound turned into shadow steel");
        }
        return;
    }
    if (!cfg.enableRefinedRadianceRecipe) return;
    // enough light collected: one refined radiance
    if (chromaticLight(stack) >= cfg.lightSourceCountForRefinedRadiance) {
        if (auto* r = spawnLike(a, makeStack(kRadiance, 1))) onCreated(*r, false);
        stack.set(stack.mCount - 1);
        if (stack.mCount <= 0) a.remove();
        else setChromaticLight(stack, 0);
        lv::log().info("materials: chromatic compound turned into refined radiance");
        return;
    }
    // over an active beacon: the whole stack
    if (overActiveBeacon(a)) {
        auto radiance = makeStack(kRadiance, stack.mCount);
        if (!radiance.isNull()) {
            a.mItem = radiance;
            onCreated(a, false);
            lv::log().info("materials: chromatic compound turned into refined radiance over a beacon");
        }
        return;
    }
    // look for a light source (half of the ticks, within 3 blocks)
    if (rnd() > 0.5f) return;
    Vec3     o = offsetRandomly(pos, 3);
    BlockPos target{static_cast<int>(std::floor(o.x)), static_cast<int>(std::floor(o.y)), static_cast<int>(std::floor(o.z))};
    if (!region.hasChunksAt(target, 0, false)) return;
    tryAbsorbLight(a, stack, target);
}

namespace {
/// Item ids of the three items (per world: the client numbers pack items when a world loads).
short chromaticId = 0, shadowId = 0, radianceId = 0;

bool isFloating(ItemStack const& s) {
    if (s.isNull()) return false;
    auto id = s.getTypeName();
    return id == kShadow || id == kRadiance;
}
} // namespace

LL_TYPE_INSTANCE_HOOK(MaterialsItemTickHook, ll::memory::HookPriority::Normal, ItemActor, &ItemActor::postNormalTick, void) {
    ItemStack const& s = *mItem;
    if (s.isNull() || isClientSide()) return origin();
    short const id = s.getId();
    if (!chromaticId) {
        chromaticId = makeStack(kChromatic, 1).getId();
        shadowId    = makeStack(kShadow, 1).getId();
        radianceId  = makeStack(kRadiance, 1).getId();
    }
    if (id != chromaticId && id != shadowId && id != radianceId) return origin();
    // shadow steel / refined radiance ignore gravity (Create sets noGravity): cancel this tick's pull
    if (isFloating(s)) velocity(*this).y += kGravity;
    origin();
    tickMagicalItem(*this);
}

void resetItemCache() { chromaticId = shadowId = radianceId = 0; }

void installMagicalItemHook() { MaterialsItemTickHook::hook(); }
void uninstallMagicalItemHook() { MaterialsItemTickHook::unhook(); }

} // namespace lv::feature::materials
