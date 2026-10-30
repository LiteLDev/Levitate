// Bulk unit: world, entity, particle and item helpers.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <unordered_map>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/network/packet/SpawnParticleEffectPacket.h"
#include "mc/util/MolangScriptArg.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/ActorHurtResult.h"
#include "mc/world/actor/HurtParameters.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/storage/loot/LootTableContext.h"
#include "mc/world/level/storage/loot/functions/LootingEnchantFunction.h"
#include "mc/world/item/crafting/Recipes.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/bulk/Bulk.h"
#include "feature/common/Items.h"
#include "feature/common/Recipes.h"
#include "game/Blocks.h"
#include "mod/Config.h"

// The game doesn't export MolangVariableMap's default constructor; its members (LeviLamina typed
// storage) construct themselves empty.
inline MolangVariableMap::MolangVariableMap() {}

namespace lv::feature::bulk {

namespace items = common::items;

LV_REGISTER_CONFIG("bulk", BulkConfig);

BulkConfig const& config() { return lv::config::get<BulkConfig>(); }

cb::FanConfig fanConfig() {
    auto const& c = config();
    return {c.fanPushDistance, c.fanPullDistance, c.fanRotationArgmax};
}

std::optional<Kind> kindOf(std::string_view id) {
    if (id == kMillstone) return Kind::Millstone;
    if (id == kWheel) return Kind::Wheel;
    if (id == kController) return Kind::Controller;
    if (id == kFan) return Kind::Fan;
    if (id == kNozzle) return Kind::Nozzle;
    return std::nullopt;
}

// ---- blocks ------------------------------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(DimensionType(dim)).lock();
    return d ? &d->getBlockSourceFromMainChunkSource() : nullptr;
}

BlockPos  toBlockPos(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
GlobalPos toGlobal(int dim, BlockPos const& p) { return GlobalPos{dim, p.x, p.y, p.z}; }

bool loaded(BlockSource& region, BlockPos const& p) {
    if (p.y < region.getMinHeight() || p.y >= region.getMaxHeight()) return false;
    return region.hasChunksAt(p, 0, false);
}

std::string idAt(BlockSource& region, BlockPos const& p) {
    if (!loaded(region, p)) return {};
    return region.getBlock(p).getTypeName();
}

int stateOf(Block const& block, std::string_view state, int fallback) {
    return game::intState(block, state).value_or(fallback);
}

bool setStates(BlockSource& region, BlockPos const& p, std::vector<std::pair<std::string, int>> const& states) {
    if (!loaded(region, p)) return false;
    Block const* block = &region.getBlock(p);
    Block const* next  = block;
    for (auto const& [name, value] : states) {
        auto const* s = next->getBlockType().getBlockState(HashedString(name));
        if (!s) continue;
        if (auto b = next->setState(*s, value)) next = b.as_ptr();
    }
    if (next == block) return false;
    return game::setBlock(region, p, *next);
}

void breakBlock(BlockSource& region, BlockPos const& p) {
    if (!loaded(region, p)) return;
    auto id = idAt(region, p);
    game::clearBlock(region, p);
    if (id.empty() || id == "minecraft:air") return;
    if (auto stack = items::make(id)) dropStacks(region, p, {*stack});
}

cb::Face faceOf(int v) { return static_cast<cb::Face>(std::clamp(v, 0, 5)); }

BlockPos offset(BlockPos p, cb::Face f, int n) {
    auto o = core::kinetics::offsetOf(f);
    return BlockPos{p.x + o.x * n, p.y + o.y * n, p.z + o.z * n};
}

// ---- entities --------------------------------------------------------------------------------------------------

std::vector<Actor*> entitiesIn(BlockSource& region, cb::Box const& box) {
    std::vector<Actor*> out;
    if (box.empty()) return out;
    AABB aabb{
        Vec3{float(box.min.x), float(box.min.y), float(box.min.z)},
        Vec3{float(box.max.x), float(box.max.y), float(box.max.z)}
    };
    for (auto* a : region.getEntities(aabb, 0.0f)) {
        if (!a || !a->isAlive()) continue;
        if (!boxOf(*a).intersects(box)) continue;
        out.push_back(a);
    }
    return out;
}

void hurtBy(Actor& e, float damage, int cause, std::string const& deathMessage) {
    ActorDamageSource source(static_cast<SharedTypes::Legacy::ActorDamageCause>(cause), deathMessage);
    HurtParameters    params{};
    e.hurt(source, damage, params);
}

namespace {
/// Entities killed by crushing wheels (unique id -> tick of the kill): Bedrock rolls a mob's loot
/// when its death animation ends, not in the killing hit.
std::unordered_map<std::int64_t, std::uint64_t> gCrushed;
int                                             gLootingBoosts = 0;
} // namespace

void setCrushingVictim(Actor const* victim) {
    std::erase_if(gCrushed, [](auto const& kv) { return currentTick() - kv.second > 200; });
    if (victim) gCrushed[const_cast<Actor*>(victim)->getOrCreateUniqueID().rawID] = currentTick();
}
int lootingBoosts() { return gLootingBoosts; }

// Create's crushingIsFortunate: the drops of an entity killed by crushing wheels roll with
// looting level 2 (LootingEnchantFunction: count += round(level * uniform(min, max))).
LL_TYPE_INSTANCE_HOOK(
    CrushingLootingHook,
    ll::memory::HookPriority::Normal,
    LootingEnchantFunction,
    &LootingEnchantFunction::$applyPreVersion,
    void,
    ::ItemStackBase&    item,
    ::Random&           random,
    ::LootTableContext& context
) {
    Actor* killed = context.mKilledActor;
    if (gCrushed.empty() || !killed || !gCrushed.contains(killed->getOrCreateUniqueID().rawID)) return origin(item, random, context);
    float bounds[2];
    std::memcpy(bounds, &mValue, sizeof(bounds));
    if (!(bounds[0] >= 0 && bounds[1] >= bounds[0] && bounds[1] <= 64)) return origin(item, random, context);
    float f     = 2.f * (bounds[0] + random01() * (bounds[1] - bounds[0]));
    int   count = item.mCount + static_cast<int>(std::lround(f));
    item.mCount = static_cast<unsigned char>(std::clamp(count, 0, 255));
    ++gLootingBoosts;
}

void installLootingHook() { CrushingLootingHook::hook(); }
void uninstallLootingHook() { CrushingLootingHook::unhook(); }

Actor* entityById(std::int64_t uniqueId) {
    if (!uniqueId) return nullptr;
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uniqueId;
    return level->fetchEntity(id, false);
}

std::int64_t idOf(Actor const& a) { return a.getOrCreateUniqueID().rawID; }

cb::Vec3d feetOf(Actor const& a) {
    auto p = a.getFeetPos();
    return {p.x, p.y, p.z};
}

cb::Box boxOf(Actor const& a) {
    auto const& b = a.getAABB();
    return {
        {b.min.x, b.min.y, b.min.z},
        {b.max.x, b.max.y, b.max.z}
    };
}

cb::Vec3d motionOf(Actor const& a) {
    auto const& d = a.getPosDelta();
    return {d.x, d.y, d.z};
}

void setMotion(Actor& a, cb::Vec3d const& m) {
    Vec3 v{float(m.x), float(m.y), float(m.z)};
    const_cast<Vec3&>(a.getPosDelta()) = v;
    if (a.isPlayer()) {
        // players move themselves: their client applies the motion
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetActorMotion);
        if (!packet) return;
        auto& pkt       = static_cast<SetActorMotionPacket&>(*packet);
        pkt.mRuntimeId  = a.getRuntimeID();
        pkt.mMotion     = v;
        pkt.sendTo(static_cast<Player&>(a));
    }
}

void moveTo(Actor& a, cb::Vec3d const& feet) {
    if (a.isPlayer()) return;
    a.teleportTo(Vec3{float(feet.x), float(feet.y), float(feet.z)}, false, 0, 0, true);
}

bool isItem(Actor const& a) { return a.isType(ActorType::ItemEntity); }
bool isLiving(Actor const& a) { return a.hasCategory(ActorCategory::Mob); }

bool isCreativeFlying(Actor const& a) {
    if (!a.isPlayer()) return false;
    auto const& p = static_cast<Player const&>(a);
    return p.isCreative() && p.isFlying();
}

bool isSneaking(Actor const& a) { return a.isSneaking(); }

ItemStack* itemOf(Actor& a) {
    if (!isItem(a)) return nullptr;
    return &static_cast<ItemActor&>(a).item();
}

std::int64_t spawnItem(BlockSource& region, ItemStack const& stack, cb::Vec3d const& pos, cb::Vec3d const& motion, int pickupDelay) {
    auto level = ll::service::getLevel();
    if (!level || items::isEmpty(stack)) return 0;
    auto* actor = level->getSpawner().spawnItem(region, stack, nullptr, Vec3{float(pos.x), float(pos.y), float(pos.z)}, 0);
    if (!actor) return 0;
    setMotion(*actor, motion);
    if (pickupDelay >= 0) actor->pickupDelay() = pickupDelay;
    return idOf(*actor);
}

void dropStacks(BlockSource& region, BlockPos const& p, std::vector<ItemStack> const& stacks) {
    auto level = ll::service::getLevel();
    if (!level) return;
    for (auto const& s : stacks) {
        if (items::isEmpty(s)) continue;
        level->getSpawner().spawnItem(region, s, nullptr, Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, 10);
    }
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy) || !items::isEmpty(copy)) {
        if (!items::isEmpty(copy)) player.drop(copy, false);
    }
    player.refreshInventory();
}

// ---- particles and sounds ------------------------------------------------------------------------------------

void particle(int dim, cb::Vec3d const& pos, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto d = level->getDimension(DimensionType(dim)).lock();
    if (!d) return;
    level->spawnParticleEffect(effect, Vec3{float(pos.x), float(pos.y), float(pos.z)}, d.get());
}

void legacyParticle(int dim, cb::Vec3d const& pos, int particleType, int data) {
    LevelEventPacket pkt;
    pkt.mEventId = 0x4000 | particleType; // LevelEvent::ParticleLegacyEvent | type
    pkt.mPos     = Vec3{float(pos.x), float(pos.y), float(pos.z)};
    pkt.mData    = data;
    pkt.sendTo(BlockPos{int(std::floor(pos.x)), int(std::floor(pos.y)), int(std::floor(pos.z))}, DimensionType(dim));
}

void particleWith(int dim, cb::Vec3d const& pos, std::string const& effect, std::vector<std::pair<std::string, float>> const& vars) {
    MolangVariableMap map;
    for (auto const& [name, value] : vars) {
        auto full = "variable." + name;
        map.setMolangVariable(HashedString::computeHash(full), full.c_str(), MolangScriptArg(value), false);
    }
    SpawnParticleEffectPacket pkt(SpawnParticleEffectPacketPayload(
        Vec3{float(pos.x), float(pos.y), float(pos.z)},
        effect,
        static_cast<uchar>(dim),
        std::optional<MolangVariableMap>(std::move(map))
    ));
    pkt.sendTo(BlockPos{int(std::floor(pos.x)), int(std::floor(pos.y)), int(std::floor(pos.z))}, DimensionType(dim));
}

void sound(int dim, cb::Vec3d const& pos, std::string const& name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = name;
    pkt.mPos    = BlockPos{static_cast<int>(pos.x * 8), static_cast<int>(pos.y * 8), static_cast<int>(pos.z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{int(std::floor(pos.x)), int(std::floor(pos.y)), int(std::floor(pos.z))}, DimensionType(dim));
}

float random01() {
    static std::mt19937                   rng{std::random_device{}()};
    static std::uniform_real_distribution d(0.f, 1.f);
    return d(rng);
}

// ---- items -------------------------------------------------------------------------------------------------------

int slotLimit(ItemStack const& s) { return std::min<int>(64, std::max<int>(1, s.getMaxStackSize())); }

bool sameItem(ItemStack const& a, ItemStack const& b) { return items::sameItemSameData(a, b); }

void addToList(ItemStack stack, std::vector<ItemStack>& list) {
    for (auto& s : list) {
        if (items::isEmpty(stack)) return;
        if (!sameItem(s, stack)) continue;
        int move = std::min(slotLimit(s) - items::countOf(s), items::countOf(stack));
        if (move <= 0) continue;
        s     = items::withCount(s, items::countOf(s) + move);
        stack = items::withCount(stack, items::countOf(stack) - move);
    }
    if (!items::isEmpty(stack)) list.push_back(stack);
}

ItemStack insertStacked(std::vector<ItemStack>& slots, ItemStack stack, bool simulate) {
    auto work = slots;
    for (auto& s : work) {
        if (items::isEmpty(stack)) break;
        if (items::isEmpty(s) || !sameItem(s, stack)) continue;
        int move = std::min(slotLimit(s) - items::countOf(s), items::countOf(stack));
        if (move <= 0) continue;
        s     = items::withCount(s, items::countOf(s) + move);
        stack = items::withCount(stack, items::countOf(stack) - move);
    }
    for (auto& s : work) {
        if (items::isEmpty(stack)) break;
        if (!items::isEmpty(s)) continue;
        int move = std::min(slotLimit(stack), items::countOf(stack));
        s        = items::withCount(stack, move);
        stack    = items::withCount(stack, items::countOf(stack) - move);
    }
    if (!simulate) slots = std::move(work);
    return items::isEmpty(stack) ? items::empty() : stack;
}

std::vector<ItemStack> rollRecipe(core::common::ProcessingRecipe const& recipe, int count) {
    std::vector<ItemStack> out;
    for (int i = 0; i < count; ++i) {
        for (auto const& s : common::recipes::rollResults(recipe)) addToList(s, out);
    }
    return out;
}

std::vector<ItemStack> multipliedOutput(ItemStack const& in, ItemStack const& out) {
    std::vector<ItemStack> stacks;
    int                    total = items::countOf(in) * std::max(1, items::countOf(out));
    int                    max   = slotLimit(out);
    while (total > 0) {
        int n = std::min(total, max);
        stacks.push_back(items::withCount(out, n));
        total -= n;
    }
    return stacks;
}

std::optional<ItemStack> furnaceResult(ItemStack const& in, char const* tag) {
    auto level = ll::service::getLevel();
    if (!level || items::isEmpty(in)) return std::nullopt;
    ItemStack one    = items::withCount(in, 1);
    auto      result = level->getRecipes().getFurnaceRecipeResult(one, HashedString(tag));
    if (result.isNull() || result.mCount == 0) return std::nullopt;
    return items::make(result.getTypeName(), std::max<int>(1, result.mCount), result.getAuxValue());
}

core::common::ProcessingRecipe const* findRecipe(std::string const& type, ItemStack const& stack) {
    if (items::isEmpty(stack)) return nullptr;
    return common::recipes::findFirst(type, stack);
}

Json      stackJson(ItemStack const& s) { return items::isEmpty(s) ? Json(nullptr) : items::toJson(s); }
ItemStack stackOf(Json const& j) { return j.is_null() ? items::empty() : items::fromJson(j); }

} // namespace lv::feature::bulk
