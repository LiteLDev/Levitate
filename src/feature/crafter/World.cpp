// Crafter unit: game adapters shared by the crafter, the saw and the drill.

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/LevelSoundEventPacket.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/ActorHurtResult.h"
#include "mc/world/actor/HurtParameters.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/crafter/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Config.h"

namespace lv::feature::crafter {

LV_REGISTER_CONFIG("crafter", Config);
Config const& config() { return lv::config::get<Config>(); }

namespace world {

namespace {
bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
constexpr std::array<char const*, 5> kKnownStates{kFacing, kPointing, kCovered, kAlongFirst, kRotor};
} // namespace

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

BlockSource* loadedRegion(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return nullptr;
    auto bp = game::blockPos(pos);
    if (pos.y < region->getMinHeight() || pos.y >= region->getMaxHeight()) return nullptr;
    if (!region->hasChunksAt(bp, 0, false)) return nullptr;
    return region;
}

Block const* blockAt(GlobalPos const& pos) {
    auto* region = loadedRegion(pos);
    if (!region) return nullptr;
    return &region->getBlock(game::blockPos(pos));
}

std::string idAt(GlobalPos const& pos) {
    auto const* b = blockAt(pos);
    return b ? std::string(b->getTypeName()) : std::string{};
}

int intState(Block const& block, char const* state, int fallback) { return game::intState(block, state).value_or(fallback); }

bool setStates(GlobalPos const& pos, std::vector<std::pair<std::string, int>> const& states) {
    auto* region = loadedRegion(pos);
    if (!region) return false;
    auto        bp    = game::blockPos(pos);
    auto const& block = region->getBlock(bp);
    std::vector<std::pair<std::string_view, int>> all;
    for (auto const* s : kKnownStates) {
        if (auto v = game::intState(block, s)) all.emplace_back(s, *v);
    }
    for (auto const& [k, v] : states) {
        bool found = false;
        for (auto& [ak, av] : all)
            if (ak == k) {
                av    = v;
                found = true;
            }
        if (!found) all.emplace_back(k, v);
    }
    auto const* next = game::blockWith(block.getTypeName(), all);
    if (!next || next == &block) return false;
    return game::setBlock(*region, bp, *next);
}

GlobalPos offset(GlobalPos p, Face f) {
    auto o = core::kinetics::offsetOf(f);
    p.x += o.x;
    p.y += o.y;
    p.z += o.z;
    return p;
}

bool powered(GlobalPos const& pos) {
    auto* region = loadedRegion(pos);
    return region && common::redstone::powered(*region, game::blockPos(pos));
}

void playSoundAt(int dim, double x, double y, double z, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{static_cast<int>(x * 8), static_cast<int>(y * 8), static_cast<int>(z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{int(std::floor(x)), int(std::floor(y)), int(std::floor(z))}, DimensionType(dim));
}

void playSound(GlobalPos const& at, std::string const& sound, float volume, float pitch) {
    playSoundAt(at.dim, at.x + .5, at.y + .5, at.z + .5, sound, volume, pitch);
}

void particle(int dim, double x, double y, double z, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(x), float(y), float(z)}, dimension.get());
}

Actor* dropItem(int dim, double x, double y, double z, ItemStack const& stack, std::optional<std::array<double, 3>> motion) {
    if (common::items::isEmpty(stack)) return nullptr;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(dim);
    if (!level || !region) return nullptr;
    auto* actor = level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{float(x), float(y), float(z)}, 0);
    if (!actor) return nullptr;
    actor->mPickupDelay = 10; // Create's setDefaultPickUpDelay
    if (motion) actor->lerpMotion(Vec3{float((*motion)[0]), float((*motion)[1]), float((*motion)[2])});
    return actor;
}

float hardness(Block const& block) { return block.getBlockType().getDestroySpeed(); }

namespace {
bool isLiquidId(std::string_view id) {
    return id == "minecraft:water" || id == "minecraft:flowing_water" || id == "minecraft:lava" || id == "minecraft:flowing_lava";
}
} // namespace

bool breakable(Block const& block) {
    if (block.isAir()) return false;
    std::string_view id = block.getTypeName();
    if (isLiquidId(id)) return false;
    if (hardness(block) < 0) return false;
    // Create's #create:non_breakable (bedrock, barriers, portals, command blocks, ...)
    static std::set<std::string_view> const nonBreakable{
        "minecraft:bedrock",          "minecraft:barrier",       "minecraft:command_block",
        "minecraft:chain_command_block", "minecraft:repeating_command_block", "minecraft:structure_block",
        "minecraft:jigsaw",           "minecraft:end_portal",    "minecraft:end_portal_frame",
        "minecraft:end_gateway",      "minecraft:portal",        "minecraft:light_block",
        "minecraft:moving_block",     "minecraft:reinforced_deepslate",
    };
    return !nonBreakable.contains(id);
}

bool hasCollision(GlobalPos const& pos) {
    auto* region = loadedRegion(pos);
    if (!region) return false;
    auto        bp    = game::blockPos(pos);
    auto const& block = region->getBlock(bp);
    if (block.isAir()) return false;
    AABB box = block.getBlockType().getCollisionShape(block, *region, bp, nullptr);
    return box.max.x - box.min.x > 1e-4f && box.max.y - box.min.y > 1e-4f && box.max.z - box.min.z > 1e-4f;
}

std::vector<Actor*> entitiesIn(int dim, double x0, double y0, double z0, double x1, double y1, double z1) {
    auto* region = regionOf(dim);
    if (!region) return {};
    std::vector<Actor*> out;
    for (auto* a : region->getEntities(AABB{Vec3{float(x0), float(y0), float(z0)}, Vec3{float(x1), float(y1), float(z1)}}, 0.0f)) {
        if (a && a->isAlive()) out.push_back(a);
    }
    return out;
}

bool isItemEntity(Actor const& a) { return a.getEntityTypeId() == ActorType::ItemEntity; }

ItemStack itemOf(Actor const& itemEntity) { return static_cast<ItemActor const&>(itemEntity).mItem.get(); }

void setItem(Actor& itemEntity, ItemStack const& stack) {
    auto& ia = static_cast<ItemActor&>(itemEntity);
    if (common::items::isEmpty(stack)) {
        ia.remove();
        return;
    }
    ia.mItem.get() = stack;
}

void discard(Actor& a) { a.remove(); }

void hurt(Actor& a, float damage, std::string const& deathMessage) {
    ActorDamageSource source(SharedTypes::Legacy::ActorDamageCause::Contact, deathMessage);
    HurtParameters    params{};
    a.hurt(source, damage, params);
}

void playHitSound(GlobalPos const& pos, Block const& block) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::LevelSoundEvent);
    if (!packet) return;
    auto& pkt           = static_cast<LevelSoundEventPacket&>(*packet);
    pkt.mSoundEvent->mSound = SharedTypes::Legacy::LevelSoundEvent::Hit;
    pkt.mPos            = Vec3{float(pos.x) + .5f, float(pos.y) + .5f, float(pos.z) + .5f};
    pkt.mData           = static_cast<int>(block.mNetworkId);
    pkt.mActorIdentifier = std::string();
    pkt.mActor          = ActorUniqueID(-1);
    pkt.mIsGlobal       = false;
    pkt.mIsBaby         = false;
    pkt.sendTo(game::blockPos(pos), DimensionType(pos.dim));
}

void push(Actor& a, double vx, double vy, double vz) {
    auto v = a.getVelocity();
    a.lerpMotion(Vec3{v.x + float(vx), v.y + float(vy), v.z + float(vz)});
}

std::vector<ItemStack> destroyBlock(GlobalPos const& pos, bool collect, float /*effectChance*/) {
    std::vector<ItemStack> drops;
    auto*                  region = loadedRegion(pos);
    auto                   level  = ll::service::getLevel();
    if (!region || !level) return drops;
    auto bp = game::blockPos(pos);
    AABB area{Vec3{pos.x - .5f, pos.y - .5f, pos.z - .5f}, Vec3{pos.x + 1.5f, pos.y + 1.5f, pos.z + 1.5f}};
    std::set<Actor*> before;
    if (collect) {
        for (auto* a : region->getEntities(area, 0.0f, ActorType::ItemEntity)) before.insert(a);
    }
    BlockChangeContext ctx{};
    level->destroyBlock(*region, bp, true, ctx);
    if (!collect) return drops;
    // the vanilla drops were spawned as item entities right now: take them back
    for (auto* a : region->getEntities(area, 0.0f, ActorType::ItemEntity)) {
        if (!a || before.contains(a) || !a->isAlive()) continue;
        drops.push_back(itemOf(*a));
        a->remove();
    }
    return drops;
}

void crack(GlobalPos const& pos, int progress) {
    auto level = ll::service::getLevel();
    auto*      region = regionOf(pos.dim);
    if (!level || !region) return;
    Vec3 at{float(pos.x), float(pos.y), float(pos.z)};
    if (progress < 0) {
        level->broadcastLocalEvent(*region, SharedTypes::Legacy::LevelEvent::StopBlockCracking, at, 0);
        return;
    }
    // Bedrock animates the cracks by itself from a speed; restart it at the reached stage
    level->broadcastLocalEvent(*region, SharedTypes::Legacy::LevelEvent::StartBlockCracking, at, 65535 / std::max(1, 10 - progress) / 2);
}

Player* playerById(std::int64_t uniqueId) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uniqueId;
    return level->getPlayer(id);
}

void forEachCompanionNear(GlobalPos const& pos, double radius, std::function<void(Player&)> const& fn) {
    auto level = ll::service::getLevel();
    if (!level) return;
    std::vector<Player*> players;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) != pos.dim || !net::isCompanion(p)) return true;
        auto const& at = p.getPosition();
        double      dx = at.x - (pos.x + .5), dy = at.y - (pos.y + .5), dz = at.z - (pos.z + .5);
        if (dx * dx + dy * dy + dz * dz <= radius * radius) players.push_back(&p);
        return true;
    });
    for (auto* p : players) fn(*p);
}

void give(Player& player, ItemStack const& stack) {
    if (common::items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy) || !common::items::isEmpty(copy)) {
        if (!common::items::isEmpty(copy)) {
            auto p = player.getFeetPos();
            dropItem(static_cast<int>(player.getDimensionId()), p.x, p.y + .5, p.z, copy);
        }
    }
    player.sendInventory(false);
}

bool acceptsDirectInput(std::string const& id) {
    return id == "levitate:chute" || id == "levitate:smart_chute" || id == "levitate:item_drain";
}

} // namespace world
} // namespace lv::feature::crafter
