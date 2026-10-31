// World helpers of the packages unit: blocks and states, redstone polling, signs, sounds,
// messages, item drops and inventories.

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/network/packet/TextPacketType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/SignBlockActor.h"
#include "mc/world/level/block/actor/SignTextSide.h"
#include "mc/world/level/dimension/Dimension.h"

#include "core/packages/Address.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"

namespace lv::feature::packages {

namespace items = common::items;

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

bool isLoaded(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    return region && region->hasChunksAt(game::blockPos(pos), 0, false);
}

Block const* blockAt(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region || !region->hasChunksAt(game::blockPos(pos), 0, false)) return nullptr;
    return &region->getBlock(game::blockPos(pos));
}

std::string idAt(GlobalPos const& pos) {
    auto const* b = blockAt(pos);
    return b ? game::blockId(*b) : std::string{};
}

std::optional<int> stateAt(GlobalPos const& pos, std::string_view state) {
    auto const* b = blockAt(pos);
    if (!b) return std::nullopt;
    return game::intState(*b, state);
}

bool setStates(GlobalPos const& pos, std::vector<std::pair<std::string_view, int>> const& states) {
    auto* region = regionOf(pos.dim);
    auto const* b = blockAt(pos);
    if (!region || !b) return false;
    auto id = game::blockId(*b);
    // keep every other integer state of the block
    std::vector<std::pair<std::string_view, int>> all;
    static char const* const kKnown[] = {kFacing, kPowered, kLinked, kDirection, kOpen, kFlag, kGoggles, kRotor, kYaw};
    bool changed = false;
    for (auto const* name : kKnown) {
        auto cur = game::intState(*b, name);
        if (!cur) continue;
        int value = *cur;
        for (auto const& [k, v] : states)
            if (k == name && v != value) {
                value   = v;
                changed = true;
            }
        all.emplace_back(name, value);
    }
    if (!changed) return false;
    auto const* next = game::blockWith(id, all);
    if (!next) return false;
    return game::setBlock(*region, game::blockPos(pos), *next);
}

bool redstonePowered(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    return region && common::redstone::powered(*region, game::blockPos(pos));
}

std::optional<std::string> signText(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region || !region->hasChunksAt(game::blockPos(pos), 0, false)) return std::nullopt;
    auto* actor = region->getBlockEntity(game::blockPos(pos));
    if (!actor) return std::nullopt;
    auto type = actor->getType();
    if (type != BlockActorType::Sign && type != BlockActorType::HangingSign) return std::nullopt;
    auto* sign = static_cast<SignBlockActor*>(actor);
    for (auto side : {SignTextSide::Front, SignTextSide::Back}) {
        std::string const& text = sign->getMessage(side);
        std::string        address;
        std::size_t        start = 0;
        while (start <= text.size()) {
            auto end  = text.find('\n', start);
            auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            // trim
            auto a = line.find_first_not_of(" \t\r");
            auto b = line.find_last_not_of(" \t\r");
            if (a != std::string::npos) address += line.substr(a, b - a + 1) + " ";
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (!cp::isBlank(address)) return address.substr(0, address.size() - 1);
    }
    return std::string{};
}

GlobalPos offset(GlobalPos const& p, cp::Offset o) { return {p.dim, p.x + o.x, p.y + o.y, p.z + o.z}; }

cp::Offset faceOffset(int face) {
    switch (face) {
    case 0: return {0, -1, 0};
    case 1: return {0, 1, 0};
    case 2: return {0, 0, -1};
    case 3: return {0, 0, 1};
    case 4: return {-1, 0, 0};
    case 5: return {1, 0, 0};
    default: return {};
    }
}

GlobalPos  offsetFace(GlobalPos const& p, int face) { return offset(p, faceOffset(face)); }
cp::Offset diff(GlobalPos const& from, GlobalPos const& to) { return {to.x - from.x, to.y - from.y, to.z - from.z}; }
cp::V3     corner(GlobalPos const& p) { return {double(p.x), double(p.y), double(p.z)}; }
cp::V3     centre(GlobalPos const& p) { return {p.x + 0.5, p.y + 0.5, p.z + 0.5}; }
int        oppositeFace(int face) { return face ^ 1; }

std::vector<int> nearestLookingFaces(cp::V3 look) {
    // Java's Direction.orderedByNearest: the axes by how much the look vector points along them
    int    ex = look.x > 0 ? 5 : 4, uy = look.y < 0 ? 0 : 1, sz = look.z > 0 ? 3 : 2;
    double ax = std::abs(look.x), ay = std::abs(look.y), az = std::abs(look.z);
    int    a, b, c;
    if (ax > az) {
        if (ay > ax) a = uy, b = ex, c = sz;
        else if (az > ay) a = ex, b = sz, c = uy;
        else a = ex, b = uy, c = sz;
    } else {
        if (ay > az) a = uy, b = sz, c = ex;
        else if (ax > ay) a = sz, b = ex, c = uy;
        else a = sz, b = uy, c = ex;
    }
    return {a, b, c, oppositeFace(c), oppositeFace(b), oppositeFace(a)};
}

void playSound(int dim, cp::V3 at, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{int(at.x * 8), int(at.y * 8), int(at.z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{int(std::floor(at.x)), int(std::floor(at.y)), int(std::floor(at.z))}, DimensionType(dim));
}

void particle(int dim, cp::V3 at, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(at.x), float(at.y), float(at.z)}, dimension.get());
}

void actionBar(Player& player, std::string const& text) {
    if (text.empty()) return; // an empty tip makes clients drop the connection
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void dropItem(int dim, cp::V3 at, ItemStack const& stack, std::optional<cp::V3> motion) {
    if (items::isEmpty(stack)) return;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(dim);
    if (!level || !region) return;
    auto* actor = level->getSpawner().spawnItem(*region, stack, nullptr, Vec3{float(at.x), float(at.y), float(at.z)}, motion ? 0 : 10);
    if (actor && motion) actor->lerpMotion(Vec3{float(motion->x), float(motion->y), float(motion->z)});
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy) && !items::isEmpty(copy)) player.drop(copy, false);
    player.sendInventory(false);
}

Player* playerById(std::int64_t uniqueId) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uniqueId;
    return level->getPlayer(id);
}

cp::V3 eyePos(Player const& player) {
    auto p = player.getPosition(); // players' position is at eye height
    return {p.x, p.y, p.z};
}

cp::V3 lookVec(Player const& player) {
    auto v = player.getViewVector();
    return {v.x, v.y, v.z};
}

std::vector<std::string> const& chainItemIds() {
    static std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (char const* id : {"minecraft:chain", "minecraft:iron_chain"})
            if (auto s = items::make(id)) {
                auto real = items::idOf(*s);
                if (std::find(out.begin(), out.end(), real) == out.end()) out.push_back(real);
            }
        return out;
    }();
    return ids;
}

bool isChainItem(ItemStack const& stack) {
    auto id = items::idOf(stack);
    if (id.empty()) return false;
    auto const& ids = chainItemIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool takeItems(Player& player, std::vector<std::string> const& itemIds, int count, bool simulate) {
    auto& inv      = player.getInventory();
    int   size     = inv.getContainerSize();
    int   selected = player.getSelectedItemSlot();
    int   found    = 0;
    auto  matches  = [&](ItemStack const& s) {
        auto id = items::idOf(s);
        return !id.empty() && std::find(itemIds.begin(), itemIds.end(), id) != itemIds.end();
    };
    // Create: the other slots first, the selected slot last
    std::vector<int> order;
    for (int i = 0; i < size; ++i)
        if (i != selected) order.push_back(i);
    if (selected >= 0 && selected < size) order.push_back(selected);
    for (int i : order) {
        if (found >= count) break;
        ItemStack s = inv.getItem(i);
        if (!matches(s)) continue;
        int take = std::min(count - found, items::countOf(s));
        found += take;
        if (!simulate) inv.setItem(i, items::withCount(s, items::countOf(s) - take));
    }
    if (!simulate) player.sendInventory(false);
    return found >= count;
}

} // namespace lv::feature::packages
