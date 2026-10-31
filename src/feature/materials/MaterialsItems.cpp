// Materials unit (U9): Create's legacy item entities. A dropped chromatic compound absorbs light
// sources near it (or turns into refined radiance over an active beacon) and becomes shadow steel
// when it falls into the void; shadow steel and refined radiance float (no gravity).

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <string>

#include "ll/api/memory/Hook.h"

#include "mc/common/Brightness.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/item/Item.h"
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

#include "feature/belts/BeltApi.h"
#include "feature/common/Items.h"
#include "feature/materials/Materials.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::materials {

namespace belts = feature::belts;
using core::GlobalPos;

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

constexpr int kEndRod     = 48; // ParticleType::EndRod
constexpr int kWitchSpell = 45; // ParticleType::WitchSpell

/// Legacy particles for the vanilla players near `a` (the client mod draws Create's own, with their
/// motion: client/materials/MaterialsClient.cpp).
void vanillaParticles(Actor& a, std::function<void(std::function<void(int, Vec3 const&)> const&)> const& emit) {
    auto const& at  = a.getPosition();
    int         dim = static_cast<int>(a.getDimensionId());
    a.getLevel().forEachPlayer([&](Player& p) {
        if (p.isSimulated() || net::isCompanion(p) || static_cast<int>(p.getDimensionId()) != dim) return true;
        if (p.getPosition().distanceToSqr(at) > 64.0f * 64.0f) return true;
        emit([&](int type, Vec3 const& pos) {
            LevelEventPacket pkt;
            pkt.mEventId = 0x4000 | type; // ParticleLegacyEvent | type
            pkt.mPos     = pos;
            pkt.mData    = 0;
            pkt.sendTo(p);
        });
        return true;
    });
}

/// Create's NoGravMagicalDohickyItem.onCreated (the "JustCreated" flag is our conversion call): the
/// client effect is a flash with 20 witch and 20 end rod particles bursting upwards.
void onCreated(Actor& a, bool shadow) {
    if (shadow) {
        // Java items accumulate their fall distance; Bedrock items may not: at least the depth below
        // the world (the conversion needs 10 blocks of fall), so the steel always comes back up.
        float fell = std::max(a.getFallDistance(), a.getDimensionBlockSource().getMinHeight() - a.getPosition().y + 10.0f);
        velocity(a) = Vec3{0, (fell + 3) / 50.0f, 0};
    }
    else velocity(a).y += 0.25f;
    Vec3 at = a.getPosition();
    vanillaParticles(a, [&](auto const& send) {
        for (int i = 0; i < 20; ++i) {
            send(kWitchSpell, offsetRandomly(at, 0.5f));
            send(kEndRod, offsetRandomly(at, 0.5f));
        }
    });
    int dim = static_cast<int>(a.getDimensionId());
    a.getLevel().forEachPlayer([&](Player& p) {
        if (net::isCompanion(p) && static_cast<int>(p.getDimensionId()) == dim && p.getPosition().distanceToSqr(at) < 64.0f * 64.0f)
            net::send(p, "materials.created", {{"x", at.x}, {"y", at.y}, {"z", at.z}});
        return true;
    });
}

ItemActor* spawnLike(ItemActor& from, ItemStack const& stack) {
    auto& region = from.getDimensionBlockSource();
    auto* spawned = region.getLevel().getSpawner().spawnItem(region, stack, nullptr, from.getPosition(), 0);
    if (spawned) velocity(*spawned) = from.getPosDelta();
    return spawned;
}

/// Clients only learn an item entity's stack when it spawns: a changed stack is a new entity.
/// Replaces `from` by an entity holding `stack` (nothing when empty); returns the new one.
ItemActor* replaceStack(ItemActor& from, ItemStack const& stack) {
    ItemActor* next = stack.isNull() || stack.mCount <= 0 ? nullptr : spawnLike(from, stack);
    if (stack.isNull() || stack.mCount <= 0 || next) from.remove();
    return next;
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

/// ChromaticCompoundItem.checkLight, the block part: a light source that is neither unbreakable
/// nor a beacon.
bool absorbable(Block const& b) {
    if (b.isAir() || b.getTypeName() == "minecraft:beacon") return false;
    if (b.getBlockType().getLightEmission(b).mValue == 0) return false;
    return b.getBlockType().getDestroySpeed() >= 0;
}

/// ChromaticCompoundItem.checkLight, the sight part: the ray from the item hits `target`.
bool inSight(ItemActor& a, BlockPos const& target) {
    auto&     region = a.getDimensionBlockSource();
    Vec3      from   = a.getPosition() + Vec3{0, 0.5f, 0};
    Vec3      to{target.x + 0.5f, target.y + 0.5f, target.z + 0.5f};
    // (the game calls the block filter unconditionally: an empty one throws)
    static std::function<bool(BlockSource const&, Block const&, bool)> const everyBlock = [](BlockSource const&, Block const&, bool) { return true; };
    HitResult hit = region.clip(from, to, false, ShapeType::Outline, 200, false, false, nullptr, everyBlock, false);
    return hit.mType == HitResultType::Tile && hit.mBlock == target;
}

/// One charge more on one item of the stack (split off as its own entity).
bool charge(ItemActor& a, ItemStack const& stack) {
    ItemStack charged = stack;
    charged.set(1);
    setChromaticLight(charged, chromaticLight(stack) + 1);
    if (!spawnLike(a, charged)) return false;
    ItemStack rest = stack;
    rest.set(stack.mCount - 1);
    replaceStack(a, rest.mCount > 0 ? rest : ItemStack{});
    return true;
}

/// ChromaticCompoundItem.checkLight: absorbs a placed light source in line of sight.
bool tryAbsorbLight(ItemActor& a, ItemStack& stack, BlockPos const& target) {
    auto&        region = a.getDimensionBlockSource();
    Block const& b      = region.getBlock(target);
    if (!absorbable(b) || !inSight(a, target)) return false;
    std::string id = b.getTypeName();
    int         light = chromaticLight(stack) + 1;
    if (!charge(a, stack)) return false;
    game::clearBlock(region, target);
    lv::log().info("materials: chromatic compound absorbed {} ({} light)", id, light);
    return true;
}

/// Create: a belt / depot (TransportedItemStackHandlerBehaviour) at the target gives up one item
/// of the first carried stack whose block emits light.
bool tryAbsorbFromBelt(ItemActor& a, ItemStack& stack, BlockPos const& target) {
    int       dim = static_cast<int>(a.getDimensionId());
    GlobalPos at{dim, target.x, target.y, target.z};
    if (!belts::isTransporter(at)) return false;
    std::string eaten;
    belts::handleProcessingOnAllItems(at, [&](belts::TransportedItem& t) {
        if (!eaten.empty() || t.stack.isNull()) return belts::TransportedResult::doNothing();
        Block const* block = t.stack.mBlock;
        if (!block || !absorbable(*block) || !inSight(a, target)) return belts::TransportedResult::doNothing();
        eaten = t.stack.getTypeName();
        if (t.stack.mCount <= 1) return belts::TransportedResult::removeItem();
        ItemStack rest = t.stack;
        rest.set(t.stack.mCount - 1);
        return belts::TransportedResult::convertTo({rest});
    });
    if (eaten.empty()) return false;
    int light = chromaticLight(stack) + 1;
    if (!charge(a, stack)) {
        lv::log().warn("materials: chromatic compound could not take the light of {}", eaten);
        return false;
    }
    lv::log().info("materials: chromatic compound absorbed {} from a belt ({} light)", eaten, light);
    return true;
}

} // namespace

bool absorbFromTransporter(Actor& itemActor, BlockPos const& target) {
    auto& a = static_cast<ItemActor&>(itemActor);
    return tryAbsorbFromBelt(a, *a.mItem, target);
}

int chromaticLight(ItemStack const& stack) {
    auto d = common::items::getData(stack, "lv:chromatic_compound");
    return d.is_object() ? d.value("light", 0) : 0;
}

void setChromaticLight(ItemStack& stack, int light) {
    common::items::setData(stack, "lv:chromatic_compound", light > 0 ? nlohmann::json{{"light", light}} : nlohmann::json());
    // the charge also goes into the aux value: the client's item renderer only sees id and aux
    // (client mod: the charge bar)
    stack.mAuxValue = static_cast<short>(std::max(0, light));
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
        // (Create: the client's chance per tick, falling at 0.1 blocks per tick; the client mod
        // draws them for its own player)
        if (rnd() < chance) {
            Vec3 at = a.getPosition(), p = offsetRandomly(at, .5f);
            vanillaParticles(a, [&](auto const& send) { send(kEndRod, Vec3{p.x, at.y, p.z}); });
        }
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
            if (auto* s = replaceStack(a, steel)) onCreated(*s, true);
            lv::log().info("materials: chromatic compound turned into shadow steel");
        }
        return;
    }
    if (!cfg.enableRefinedRadianceRecipe) return;
    // enough light collected: one refined radiance
    if (chromaticLight(stack) >= cfg.lightSourceCountForRefinedRadiance) {
        auto one = makeStack(kRadiance, 1);
        if (one.isNull()) return;
        auto* r = spawnLike(a, one);
        if (!r) return;
        onCreated(*r, false);
        ItemStack rest = stack;
        rest.set(stack.mCount - 1);
        setChromaticLight(rest, 0);
        replaceStack(a, rest.mCount > 0 ? rest : ItemStack{});
        lv::log().info("materials: chromatic compound turned into refined radiance");
        return;
    }
    // over an active beacon: the whole stack
    if (overActiveBeacon(a)) {
        auto radiance = makeStack(kRadiance, stack.mCount);
        if (!radiance.isNull()) {
            if (auto* r = replaceStack(a, radiance)) onCreated(*r, false);
            lv::log().info("materials: chromatic compound turned into refined radiance over a beacon");
        }
        return;
    }
    // look for a light source (half of the ticks, within 3 blocks)
    if (rnd() > 0.5f) return;
    Vec3     o = offsetRandomly(pos, 3);
    BlockPos target{static_cast<int>(std::floor(o.x)), static_cast<int>(std::floor(o.y)), static_cast<int>(std::floor(o.z))};
    if (!region.hasChunksAt(target, 0, false)) return;
    if (tryAbsorbFromBelt(a, stack, target)) return;
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

bool isChromatic(Item const* item) {
    static std::string const name = kChromatic;
    return item && item->mFullName->getString() == name;
}

int maxStackSize(ItemStackBase const& stack, int vanilla) {
    // Create's ChromaticCompoundItem.getMaxStackSize: 1 while the charge bar shows
    if (!isChromatic(stack.mItem.get())) return vanilla;
    auto const* tag = stack.mUserData.get();
    // the item data of common::items::setData(stack, "lv:chromatic_compound", ...)
    static std::string const key = "lv:lv:chromatic_compound";
    if (!tag || !tag->contains(key, Tag::Type::String)) return vanilla;
    auto j = nlohmann::json::parse(std::string((*tag)[key].get<StringTag>()), nullptr, false);
    return j.is_object() && j.value("light", 0) > 0 ? 1 : vanilla;
}

LL_TYPE_INSTANCE_HOOK(MaterialsMaxStackHook, ll::memory::HookPriority::Normal, ItemStackBase, &ItemStackBase::getMaxStackSize, uchar) {
    uchar vanilla = origin();
    return static_cast<uchar>(maxStackSize(*this, vanilla));
}

void installMagicalItemHook() {
    MaterialsItemTickHook::hook();
    MaterialsMaxStackHook::hook();
}
void uninstallMagicalItemHook() {
    MaterialsMaxStackHook::unhook();
    MaterialsItemTickHook::unhook();
}

} // namespace lv::feature::materials
