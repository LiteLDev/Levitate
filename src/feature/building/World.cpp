// Building unit: world helpers (blocks, sounds, items, redstone input, player directions).

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockSupportType.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/CachedComponentData.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/building/Building.h"
#include "game/Blocks.h"

namespace lv::feature::building {

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

Block const* blockAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    if (!region) return nullptr;
    BlockPos bp{p.x, p.y, p.z};
    if (bp.y < region->getMinHeight() || bp.y >= region->getMaxHeight()) return nullptr;
    if (!region->hasChunksAt(bp, 0, false)) return nullptr;
    return &region->getBlock(bp);
}

std::string idAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? b->getTypeName() : std::string();
}

bool set(GlobalPos const& p, Block const& block) {
    auto* region = regionOf(p.dim);
    return region && game::setBlock(*region, BlockPos{p.x, p.y, p.z}, block);
}

bool clear(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    return region && game::clearBlock(*region, BlockPos{p.x, p.y, p.z});
}

bool replaceableAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    return region && game::replaceable(*region, BlockPos{p.x, p.y, p.z});
}

bool sturdy(GlobalPos const& p, int face) {
    auto const* b = blockAt(p);
    if (!b || b->isAir()) return false;
    return b->canProvideSupport(static_cast<uchar>(face), BlockSupportType::Edge);
}

bool solidAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b && !b->isAir() && b->mCachedComponentData->mIsSolid;
}

bool canHang(GlobalPos const& p) {
    auto const* b = blockAt(p);
    if (!b || b->isAir()) return false;
    if (b->mCachedComponentData->mIsSolid) return true;
    for (int f = 0; f < 6; ++f)
        if (b->canProvideSupport(static_cast<uchar>(f), BlockSupportType::Any)) return true;
    return infoOf(*b) != nullptr && infoOf(*b)->kind != Kind::Placard;
}

bool mayBuild(Player const& player) { return player.canUseAbility(AbilitiesIndex::Build); }

namespace {

/// The neighbour block powers its neighbours (Java's getSignal > 0 for the usual sources).
bool emitsPower(Block const& block) {
    std::string const& id = block.getTypeName();
    if (id == "minecraft:redstone_block" || id == "minecraft:redstone_torch") return true;
    if (id == "minecraft:powered_repeater" || id == "minecraft:powered_comparator") return true;
    if (id == "minecraft:lever") return game::intState(block, "open_bit").value_or(0) != 0;
    if (game::intState(block, "button_pressed_bit").value_or(0) != 0) return true;
    if (id == "minecraft:observer") return game::intState(block, "powered_bit").value_or(0) != 0;
    if (id == kPlacardId) return game::intState(block, "levitate:powered").value_or(0) != 0;
    return game::intState(block, "redstone_signal").value_or(0) > 0; // wire, plates, detectors, ...
}

/// A solid block is strongly powered by a lever / button attached to it, a powered placard
/// hanging on it, or a redstone torch below it.
bool stronglyPowered(GlobalPos const& p) {
    for (int d = 0; d < 6; ++d) {
        auto const* n = blockAt(offset(p, d));
        if (!n || n->isAir()) continue;
        std::string const& id = n->getTypeName();
        if (id == "minecraft:lever" && game::intState(*n, "open_bit").value_or(0) != 0) return true;
        if (game::intState(*n, "button_pressed_bit").value_or(0) != 0 && game::intState(*n, "facing_direction").value_or(d) == d) return true;
        if (id == kPlacardId && game::intState(*n, "levitate:powered").value_or(0) != 0
            && game::intState(*n, "levitate:facing").value_or(1) == d) {
            return true; // the placard faces away from the block it hangs on
        }
        if (d == Down && id == "minecraft:redstone_torch") return true;
        if ((id == "minecraft:powered_repeater" || id == "minecraft:powered_comparator")) return true;
    }
    return false;
}

} // namespace

bool hasNeighborSignal(GlobalPos const& p) {
    for (int d = 0; d < 6; ++d) {
        auto        np = offset(p, d);
        auto const* n  = blockAt(np);
        if (!n || n->isAir()) continue;
        if (emitsPower(*n)) return true;
        if (n->mCachedComponentData->mIsSolid && !infoOf(*n) && stronglyPowered(np)) return true;
    }
    return false;
}

void playSound(GlobalPos const& p, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{p.x * 8 + 4, p.y * 8 + 4, p.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{p.x, p.y, p.z}, DimensionType(p.dim));
}

void particle(GlobalPos const& p, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(p.dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, dimension.get());
}

ItemStack makeItem(std::string_view id, int count) {
    ItemStack s;
    s.reinit(id, count, 0);
    return s;
}

void drop(GlobalPos const& p, ItemStack const& item) {
    if (item.isNull()) return;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(p.dim);
    if (!level || !region) return;
    level->getSpawner().spawnItem(*region, item, nullptr, Vec3{p.x + .5f, p.y + .5f, p.z + .5f}, 10);
}

void give(Player& player, ItemStack const& item) {
    if (item.isNull()) return;
    ItemStack stack = item;
    if (!player.add(stack) || !stack.isNull()) {
        auto  level = ll::service::getLevel();
        Vec3  at    = player.getPosition();
        if (level) level->getSpawner().spawnItem(player.getDimensionBlockSource(), stack, nullptr, at, 0);
    }
    player.sendInventory(false);
}

void consumeHeld(Player& player) {
    if (player.isCreative()) return;
    ItemStack held = player.getSelectedItem();
    if (held.isNull()) return;
    held.remove(1);
    player.setSelectedItem(held);
    player.sendInventory(false);
}

Player* playerByUid(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

std::array<int, 6> nearestLooking(Player const& player) {
    // Bedrock rotation: x = pitch (down positive), y = yaw (0 = south, 90 = west)
    auto   rot   = player.getRotation();
    double pitch = rot.x * std::numbers::pi / 180.0, yaw = rot.y * std::numbers::pi / 180.0;
    double look[3]{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    std::array<std::pair<double, int>, 6> scored;
    for (int d = 0; d < 6; ++d) scored[d] = {-(kDx[d] * look[0] + kDy[d] * look[1] + kDz[d] * look[2]), d};
    std::sort(scored.begin(), scored.end());
    std::array<int, 6> out{};
    for (int i = 0; i < 6; ++i) out[i] = scored[i].second;
    return out;
}

int horizontalFacing(Player const& player) {
    // Java 2D data: 0 S, 1 W, 2 N, 3 E
    static constexpr int map[4]{South, West, North, East};
    return map[game::playerFacing(player) & 3];
}

} // namespace lv::feature::building
