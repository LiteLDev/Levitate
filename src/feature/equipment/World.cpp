// World helpers of the equipment unit: blocks, sounds, particles, messages, items, deferred edits.

#include <map>
#include <random>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/network/packet/SetTitlePacket.h"
#include "mc/network/packet/SetTitlePacketPayload.h"
#include "mc/world/actor/Actor.h"
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

#include "core/contraption/Geometry.h"
#include "feature/common/Items.h"
#include "feature/contraption/PistonsExt.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "mod/Config.h"

LV_REGISTER_CONFIG("equipment", lv::feature::equipment::Config);

namespace lv::feature::equipment {

namespace {
std::vector<std::function<void()>>& deferred() {
    static std::vector<std::function<void()>> d;
    return d;
}
std::uint64_t gTick = 0;
} // namespace

Config const& config() { return lv::config::get<Config>(); }

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

Block const* blockAt(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    if (!r) return nullptr;
    BlockPos bp{p.x, p.y, p.z};
    if (p.y < r->getMinHeight() || p.y >= r->getMaxHeight() || !r->hasChunksAt(bp, 0, false)) return nullptr;
    return &r->getBlock(bp);
}

std::string idAt(GlobalPos const& p) {
    auto const* b = blockAt(p);
    return b ? std::string(b->getTypeName()) : std::string();
}

std::optional<int> stateAt(GlobalPos const& p, char const* name) {
    auto const* b = blockAt(p);
    if (!b) return std::nullopt;
    return game::intState(*b, name);
}

bool placeBlock(GlobalPos const& p, Block const& block) {
    auto* r = regionOf(p.dim);
    if (!r) return false;
    BlockPos bp{p.x, p.y, p.z};
    if (!r->hasChunksAt(bp, 0, false)) return false;
    BlockChangeContext ctx{};
    return r->setBlock(bp, block, 3, nullptr, ctx);
}

bool place(GlobalPos const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states) {
    auto const* b = game::blockWith(id, states);
    return b && placeBlock(p, *b);
}

bool clearAt(GlobalPos const& p) {
    auto const* air = game::blockWith("minecraft:air");
    return air && placeBlock(p, *air);
}

bool replaceableAt(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    return r && game::replaceable(*r, BlockPos{p.x, p.y, p.z});
}

void sound(int dim, double x, double y, double z, std::string const& name, float volume, float pitch) {
    contraption::ext::playSound(dim, core::contraption::Vec3d{x, y, z}, name, volume, pitch);
}

void particle(int dim, double x, double y, double z, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(x), float(y), float(z)}, dimension.get());
}

void actionBar(Player& player, std::string const& text) { contraption::ext::actionBar(player, text); }

namespace {
void title(Player& player, SetTitlePacketPayload::TitleType type, std::string const& text) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetTitle);
    if (!packet) return;
    auto& pkt          = static_cast<SetTitlePacket&>(*packet);
    pkt.mType          = type;
    pkt.mTitleText     = text;
    pkt.mFadeInTime    = 10;
    pkt.mStayTime      = 40;
    pkt.mFadeOutTime   = 10;
    pkt.sendTo(player);
}
} // namespace

void subtitle(Player& player, std::string const& text) {
    title(player, SetTitlePacketPayload::TitleType::Times, "");
    title(player, SetTitlePacketPayload::TitleType::Subtitle, text);
    title(player, SetTitlePacketPayload::TitleType::Title, " ");
}

Player* playerById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

std::int64_t uidOf(Actor const& a) { return a.getOrCreateUniqueID().rawID; }

void dropAt(int dim, double x, double y, double z, ItemStack const& stack) {
    auto level = ll::service::getLevel();
    auto* r    = regionOf(dim);
    if (!level || !r || stack.isNull()) return;
    level->getSpawner().spawnItem(*r, stack, nullptr, Vec3{float(x), float(y), float(z)}, 10);
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (stack.isNull() || stack.mCount == 0) return;
    ItemStack copy = stack;
    if (!player.add(copy) || copy.mCount > 0) {
        if (copy.mCount > 0) {
            auto p = player.getPosition();
            dropAt(static_cast<int>(player.getDimensionId()), p.x, p.y, p.z, copy);
        }
    }
    player.sendInventory(false);
}

void setMotion(Actor& a, double x, double y, double z) {
    Vec3 v{float(x), float(y), float(z)};
    const_cast<Vec3&>(a.getPosDelta()) = v;
    if (a.isPlayer()) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetActorMotion);
        if (!packet) return;
        auto& pkt      = static_cast<SetActorMotionPacket&>(*packet);
        pkt.mRuntimeId = a.getRuntimeID();
        pkt.mMotion    = v;
        pkt.sendTo(static_cast<Player&>(a));
    }
}

std::string itemId(ItemStack const& s) { return common::items::idOf(s); }

bool isPaper(std::string_view id) { return id == kSandPaper || id == kRedSandPaper; }

double random01() {
    static std::mt19937                     rng{std::random_device{}()};
    static std::uniform_real_distribution<> dist(0.0, 1.0);
    return dist(rng);
}

std::uint64_t currentTick() { return gTick; }

void defer(std::function<void()> fn) { deferred().push_back(std::move(fn)); }

bool screenDebounced(Player& player) {
    static std::map<std::int64_t, std::uint64_t> last;
    auto  uid = uidOf(player);
    auto  now = gTick + 1;
    auto  it  = last.find(uid);
    if (it != last.end() && now - it->second < 10) return true;
    last[uid] = now;
    return false;
}

void runDeferred() {
    ++gTick;
    auto work = std::move(deferred());
    deferred().clear();
    for (auto& fn : work) fn();
}

void clearDeferred() { deferred().clear(); }

} // namespace lv::feature::equipment
