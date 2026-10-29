// World helpers of the transmission unit: block states, kinetic shape queries, redstone power,
// sounds and messages.

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/network/packet/TextPacketType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/Redstone.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"

namespace lv::feature::transmission {

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

namespace {
bool loaded(BlockSource& region, BlockPos const& bp) { return region.hasChunksAt(bp, 0, false); }

Block const* withState(Block const& block, std::string_view state, int value) {
    auto const* s = block.getBlockType().getBlockState(HashedString(std::string(state)));
    if (!s) return nullptr;
    auto b = block.setState(*s, value);
    return b ? b.as_ptr() : nullptr;
}

ck::BlockState snapshotOf(Block const& block, kinetics::KineticSpec const& spec) {
    ck::BlockState s;
    s.id = block.getTypeName();
    for (auto const& name : spec.states) s.values.emplace_back(name, game::intState(block, name).value_or(0));
    return s;
}
} // namespace

std::string idAt(BlockSource& region, GlobalPos const& pos) {
    auto bp = game::blockPos(pos);
    if (!loaded(region, bp)) return {};
    return region.getBlock(bp).getTypeName();
}

int stateAt(BlockSource& region, GlobalPos const& pos, std::string_view state, int fallback) {
    auto bp = game::blockPos(pos);
    if (!loaded(region, bp)) return fallback;
    return game::intState(region.getBlock(bp), state).value_or(fallback);
}

bool place(BlockSource& region, GlobalPos const& pos, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states) {
    auto const* b = game::blockWith(id, states);
    if (!b) return false;
    return game::setBlock(region, game::blockPos(pos), *b);
}

bool setStates(BlockSource& region, GlobalPos const& pos, std::vector<std::pair<std::string_view, int>> const& states) {
    auto bp = game::blockPos(pos);
    if (!loaded(region, bp)) return false;
    Block const* cur  = &region.getBlock(bp);
    Block const* next = cur;
    for (auto const& [name, value] : states) {
        if (game::intState(*next, name).value_or(value) == value) continue;
        auto const* b = withState(*next, name, value);
        if (b) next = b;
    }
    if (next == cur) return false;
    return game::setBlock(region, bp, *next);
}

namespace {
template <class Fn>
auto withSpec(BlockSource& region, GlobalPos const& pos, Fn fn) -> decltype(fn(std::declval<kinetics::KineticSpec const&>(), std::declval<ck::BlockState const&>())) {
    auto bp = game::blockPos(pos);
    if (!loaded(region, bp)) return {};
    auto const& block = region.getBlock(bp);
    auto const* spec  = kinetics::specOf(block.getTypeName());
    if (!spec) return {};
    return fn(*spec, snapshotOf(block, *spec));
}
} // namespace

bool shaftTowards(BlockSource& region, GlobalPos const& pos, Face face) {
    return withSpec(region, pos, [&](kinetics::KineticSpec const& spec, ck::BlockState const& s) { return spec.shaftTowards(s, face); });
}

std::optional<Axis> rotationAxisAt(BlockSource& region, GlobalPos const& pos) {
    return withSpec(region, pos, [&](kinetics::KineticSpec const& spec, ck::BlockState const& s) { return spec.axisOf(s); });
}

ck::CogType cogAt(BlockSource& region, GlobalPos const& pos) {
    auto bp = game::blockPos(pos);
    if (!loaded(region, bp)) return ck::CogType::None;
    auto const& block = region.getBlock(bp);
    auto const* spec  = kinetics::specOf(block.getTypeName());
    if (!spec) return ck::CogType::None;
    return spec->cogOf(snapshotOf(block, *spec));
}

bool replaceableAt(BlockSource& region, GlobalPos const& pos) {
    auto bp = game::blockPos(pos);
    return loaded(region, bp) && game::replaceable(region, bp);
}

int neighbourSignal(BlockSource& region, GlobalPos const& pos) {
    // the strongest signal reaching the block (Java's getBestNeighborSignal)
    auto bp = game::blockPos(pos);
    if (!loaded(region, bp)) return 0;
    return common::redstone::power(region, bp);
}

void playSound(int dim, GlobalPos const& pos, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(game::blockPos(pos), DimensionType(dim));
}

void actionBar(Player& player, std::string const& text) {
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

Player* playerById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

GlobalPos stepPos(GlobalPos p, Face f, int n) {
    auto o = ck::offsetOf(f);
    p.x += o.x * n;
    p.y += o.y * n;
    p.z += o.z * n;
    return p;
}

namespace {
std::array<double, 3> lookVector(Player const& player) {
    // Bedrock rotation: x = pitch (down positive), y = yaw (0 = south, 90 = west)
    auto   rot   = player.getRotation();
    double pitch = rot.x * std::numbers::pi / 180.0, yaw = rot.y * std::numbers::pi / 180.0;
    return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}
} // namespace

Face nearestLooking(Player const& player) {
    auto   look  = lookVector(player);
    Face   best  = Face::North;
    double score = -2;
    for (Face f : ck::kFaces) {
        auto   o = ck::offsetOf(f);
        double s = o.x * look[0] + o.y * look[1] + o.z * look[2];
        if (s > score) {
            score = s;
            best  = f;
        }
    }
    return best;
}

Face horizontalLooking(Player const& player) {
    auto look = lookVector(player);
    if (std::abs(look[0]) > std::abs(look[2])) return look[0] > 0 ? Face::East : Face::West;
    // straight up / down: the yaw decides
    if (look[0] == 0 && look[2] == 0) {
        double yaw = player.getRotation().y * std::numbers::pi / 180.0;
        double x = -std::sin(yaw), z = std::cos(yaw);
        if (std::abs(x) > std::abs(z)) return x > 0 ? Face::East : Face::West;
        return z > 0 ? Face::South : Face::North;
    }
    return look[2] > 0 ? Face::South : Face::North;
}

} // namespace lv::feature::transmission
