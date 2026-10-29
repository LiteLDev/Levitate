// Shared game helpers of the assembly unit: regions, players, messages, sounds, particles, drops,
// redstone polling, block states.

#include <cmath>
#include <numbers>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/network/packet/TextPacketType.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Config.h"

namespace lv::feature::assembly {

LV_REGISTER_CONFIG("assembly", AssemblyConfig);

AssemblyConfig const& config() { return lv::config::get<AssemblyConfig>(); }

namespace detail {

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(DimensionType(dim)).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

Player* playerById(std::int64_t uniqueId) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uniqueId;
    return level->getPlayer(id);
}

void actionBar(Player& player, std::string const& text) {
    TextPacket                     pkt{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void playSound(int dim, double x, double y, double z, std::string const& name, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = name;
    pkt.mPos    = BlockPos{static_cast<int>(x * 8), static_cast<int>(y * 8), static_cast<int>(z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{int(std::floor(x)), int(std::floor(y)), int(std::floor(z))}, DimensionType(dim));
}

void particle(int dim, double x, double y, double z, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(x), float(y), float(z)}, dimension.get());
}

void dropItem(BlockSource& region, double x, double y, double z, ItemStack const& stack) {
    if (common::items::isEmpty(stack)) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    level->getSpawner().spawnItem(region, stack, nullptr, Vec3{float(x), float(y), float(z)}, 10);
}

std::optional<int> intState(Block const& block, std::string_view state) { return game::intState(block, state); }

Block const* withStates(Block const& block, std::vector<std::pair<std::string, int>> const& states) {
    Block const* cur = &block;
    for (auto const& [name, value] : states) {
        auto const* s = cur->getBlockType().getBlockState(HashedString(name));
        if (!s) return nullptr;
        auto next = cur->setState(*s, value);
        if (!next) return nullptr;
        cur = next.as_ptr();
    }
    return cur;
}

bool powered(BlockSource& region, BlockPos const& pos) { return common::redstone::powered(region, pos); }

bool shaftTowards(BlockSource& region, BlockPos const& pos, Face face) {
    if (!region.hasChunksAt(pos, 0, false)) return false;
    auto const& block = region.getBlock(pos);
    auto const* spec  = kinetics::specOf(block.getTypeName());
    if (!spec) return false;
    core::kinetics::BlockState s;
    s.id = block.getTypeName();
    for (auto const& name : spec->states) s.values.emplace_back(name, game::intState(block, name).value_or(0));
    return spec->shaftTowards(s, face);
}

BlockPos offsetPos(BlockPos const& p, Face f, int n) {
    auto o = core::kinetics::offsetOf(f);
    return BlockPos{p.x + o.x * n, p.y + o.y * n, p.z + o.z * n};
}

Face nearestFace(double x, double y, double z) {
    Face   best  = Face::North;
    double score = -1e9;
    for (Face f : core::kinetics::kFaces) {
        auto   o = core::kinetics::offsetOf(f);
        double s = o.x * x + o.y * y + o.z * z;
        if (s > score) {
            score = s;
            best  = f;
        }
    }
    return best;
}

Face lookFace(Actor const& actor) {
    // Bedrock rotation: x = pitch (down positive), y = yaw (0 = south, 90 = west)
    auto   rot   = actor.getRotation();
    double pitch = rot.x * std::numbers::pi / 180.0, yaw = rot.y * std::numbers::pi / 180.0;
    return nearestFace(-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch));
}

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }

std::optional<GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

} // namespace detail

} // namespace lv::feature::assembly
