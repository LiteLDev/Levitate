// Steam unit: game adapters (block states, regions, redstone, sounds, particles, items).

#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ShortTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/network/packet/TextPacketPayload.h"
#include "mc/network/packet/TextPacketType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/steam/SteamInternal.h"
#include "game/Blocks.h"
#include "mod/Config.h"

namespace lv::feature::steam {

LV_REGISTER_CONFIG("steam", SteamConfig);

cs::TankParams tankParams(bool creative) {
    auto const&    cfg = config::get<SteamConfig>();
    cs::TankParams p;
    p.maxWidth         = 3;
    p.maxHeight        = std::max(1, cfg.fluidTankMaxHeight);
    p.capacityPerBlock = std::max(1, cfg.fluidTankCapacity) * 1000;
    p.creative         = creative;
    return p;
}

bool isTankId(std::string_view id) { return id == kTank || id == kCreativeTank; }
bool isCreativeTankId(std::string_view id) { return id == kCreativeTank; }

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

bool loaded(BlockSource& region, GlobalPos const& pos) {
    auto bp = game::blockPos(pos);
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return false;
    return region.hasChunksAt(bp, 0, false);
}

std::string idAt(BlockSource& region, GlobalPos const& pos) {
    if (!loaded(region, pos)) return {};
    return region.getBlock(game::blockPos(pos)).getTypeName();
}

std::optional<int> intState(Block const& block, std::string_view name) { return game::intState(block, name); }

std::string strState(Block const& block, std::string_view name) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return {};
    auto const& states = nbt["states"];
    if (!states.is_object()) return {};
    std::string key(name);
    if (!states.contains(key)) return {};
    auto const& v = states[key];
    if (v.hold(Tag::Type::String)) return std::string(v.get<StringTag>());
    return {};
}

Block const* withStates(Block const& block, std::vector<std::pair<std::string, int>> const& ints,
                        std::vector<std::pair<std::string, std::string>> const& strings) {
    CompoundTag nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return &block;
    auto& tagStates = nbt["states"];
    bool  any       = false;
    for (auto const& [key, value] : ints) {
        if (!tagStates.contains(key)) continue;
        auto& slot = tagStates[key];
        if (slot.hold(Tag::Type::Byte)) slot = static_cast<uchar>(value);
        else if (slot.hold(Tag::Type::Int)) slot = static_cast<int>(value);
        else if (slot.hold(Tag::Type::Short)) slot = static_cast<short>(value);
        else continue;
        any = true;
    }
    for (auto const& [key, value] : strings) {
        if (!tagStates.contains(key)) continue;
        auto& slot = tagStates[key];
        if (!slot.hold(Tag::Type::String)) continue;
        slot = value;
        any  = true;
    }
    if (!any) return &block;
    auto result = Block::tryGetFromRegistry(nbt);
    return result ? result.as_ptr() : &block;
}

bool setStates(BlockSource& region, GlobalPos const& pos, std::vector<std::pair<std::string, int>> const& ints,
               std::vector<std::pair<std::string, std::string>> const& strings) {
    if (!loaded(region, pos)) return false;
    auto        bp    = game::blockPos(pos);
    auto const& block = region.getBlock(bp);
    auto const* next  = withStates(block, ints, strings);
    if (!next || next == &block) return false;
    return game::setBlock(region, bp, *next);
}

void popBlock(BlockSource& region, GlobalPos const& pos) {
    auto level = ll::service::getLevel();
    if (!level || !loaded(region, pos)) return;
    BlockChangeContext ctx{};
    level->destroyBlock(region, game::blockPos(pos), true, ctx);
}

void removeBlock(BlockSource& region, GlobalPos const& pos) {
    if (!loaded(region, pos)) return;
    game::clearBlock(region, game::blockPos(pos));
}

bool redstonePowered(BlockSource& region, GlobalPos const& pos) {
    static constexpr cs::Dir dirs[]{cs::Dir::Down, cs::Dir::Up, cs::Dir::North, cs::Dir::South, cs::Dir::West, cs::Dir::East};
    for (auto d : dirs) {
        GlobalPos n = offset(pos, d);
        if (!loaded(region, n)) continue;
        auto const& block = region.getBlock(game::blockPos(n));
        if (block.isAir()) continue;
        std::string const& id = block.getTypeName();
        if (id == "minecraft:redstone_block") return true;
        if (intState(block, "redstone_signal").value_or(0) > 0) return true;
        if (id == "minecraft:lever" && intState(block, "open_bit").value_or(0)) return true;
        if (intState(block, "button_pressed_bit").value_or(0)) return true;
        if (id == "minecraft:observer" && intState(block, "powered_bit").value_or(0)) return true;
        if (id == "minecraft:powered_repeater" || id == "minecraft:powered_comparator") return true;
        if (id == "minecraft:redstone_torch") {
            // a torch powers every neighbour but the block it stands on
            auto t       = strState(block, "torch_facing_direction");
            auto support = (t.empty() || t == "top" || t == "unknown") ? cs::Dir::Down : dirFromName(t, cs::Dir::Down);
            if (!(offset(n, support) == pos)) return true;
        }
    }
    return false;
}

cs::Dir dirFromName(std::string_view name, cs::Dir fallback) {
    if (name == "down") return cs::Dir::Down;
    if (name == "up") return cs::Dir::Up;
    if (name == "north") return cs::Dir::North;
    if (name == "south") return cs::Dir::South;
    if (name == "west") return cs::Dir::West;
    if (name == "east") return cs::Dir::East;
    return fallback;
}

char const* dirName(cs::Dir d) {
    static constexpr char const* names[]{"down", "up", "north", "south", "west", "east"};
    return names[static_cast<int>(d)];
}

GlobalPos offset(GlobalPos p, cs::Dir d, int n) {
    switch (d) {
    case cs::Dir::Down:
        p.y -= n;
        break;
    case cs::Dir::Up:
        p.y += n;
        break;
    case cs::Dir::North:
        p.z -= n;
        break;
    case cs::Dir::South:
        p.z += n;
        break;
    case cs::Dir::West:
        p.x -= n;
        break;
    case cs::Dir::East:
        p.x += n;
        break;
    }
    return p;
}

void playSound(int dim, double x, double y, double z, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
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

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (stack.isNull() || stack.mCount == 0) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
    player.sendInventory(false);
}

void actionBar(Player& player, std::string const& text) {
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

cs::Pos3 pos3(GlobalPos const& p) { return {p.x, p.y, p.z}; }
GlobalPos globalOf(int dim, cs::Pos3 const& p) { return {dim, p.x, p.y, p.z}; }

} // namespace lv::feature::steam
