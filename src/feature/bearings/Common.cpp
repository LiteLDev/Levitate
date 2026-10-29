// Bearings unit: shared helpers (ids, sails, block states, redstone, players, messages, sounds).

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
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/bearings/Bearings.h"
#include "feature/common/Items.h"
#include "game/Blocks.h"

namespace lv::feature::bearings {

std::optional<Kind> kindOf(std::string_view id) {
    if (id == kMechanicalBearing) return Kind::Mechanical;
    if (id == kWindmillBearing) return Kind::Windmill;
    if (id == kClockworkBearing) return Kind::Clockwork;
    if (id == kTurntable) return Kind::Turntable;
    if (id == kCartAssembler) return Kind::CartAssembler;
    if (id == kControllerRail) return Kind::ControllerRail;
    return std::nullopt;
}

// ---- sails --------------------------------------------------------------------------------------

std::vector<std::string> const& dyeColours() {
    static std::vector<std::string> const colours{
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
    };
    return colours;
}

std::string sailIdOf(std::string_view colour) { return "levitate:" + std::string(colour) + "_sail"; }

bool isSailFrame(std::string_view id) { return id == kSailFrame; }

bool isSail(std::string_view id) {
    if (isSailFrame(id)) return true;
    if (!id.starts_with("levitate:") || !id.ends_with("_sail")) return false;
    auto colour = id.substr(9, id.size() - 9 - 5);
    auto const& all = dyeColours();
    return std::find(all.begin(), all.end(), colour) != all.end();
}

bool isWindmillSail(std::string_view id) {
    if (isSail(id)) return true;
    // #minecraft:wool
    return id.starts_with("minecraft:") && id.ends_with("_wool");
}

std::optional<std::string> dyeColourOf(std::string_view itemId) {
    if (itemId == "minecraft:ink_sac") return "black";
    if (itemId == "minecraft:bone_meal") return "white";
    if (itemId == "minecraft:lapis_lazuli") return "blue";
    if (itemId == "minecraft:cocoa_beans") return "brown";
    if (!itemId.starts_with("minecraft:") || !itemId.ends_with("_dye")) return std::nullopt;
    auto colour = std::string(itemId.substr(10, itemId.size() - 10 - 4));
    if (colour == "silver") colour = "light_gray";
    auto const& all = dyeColours();
    if (std::find(all.begin(), all.end(), colour) == all.end()) return std::nullopt;
    return colour;
}

// ---- blocks -------------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(DimensionType(dim)).lock();
    return d ? &d->getBlockSourceFromMainChunkSource() : nullptr;
}

BlockPos bp(GP const& p) { return BlockPos{p.x, p.y, p.z}; }
BlockPos bp(cc::Pos const& p) { return BlockPos{p.x, p.y, p.z}; }
GP       gp(int dim, cc::Pos const& p) { return {dim, p.x, p.y, p.z}; }
cc::Pos  ccp(GP const& p) { return {p.x, p.y, p.z}; }

bool loaded(BlockSource& region, GP const& p) {
    return p.y >= region.getMinHeight() && p.y < region.getMaxHeight() && region.hasChunksAt(bp(p), 0, false);
}

std::string idAt(BlockSource& region, GP const& p) {
    if (!loaded(region, p)) return {};
    return region.getBlock(bp(p)).getTypeName();
}

int stateAt(BlockSource& region, GP const& p, std::string_view state, int fallback) {
    if (!loaded(region, p)) return fallback;
    return game::intState(region.getBlock(bp(p)), state).value_or(fallback);
}

bool setStates(BlockSource& region, GP const& p, std::vector<std::pair<std::string_view, int>> const& states) {
    if (!loaded(region, p)) return false;
    Block const* block = &region.getBlock(bp(p));
    Block const* start = block;
    for (auto const& [name, value] : states) {
        auto const* s = block->getBlockType().getBlockState(HashedString(std::string(name)));
        if (!s) continue;
        if (auto b = block->setState(*s, value)) block = b.as_ptr();
    }
    if (block == start) return false;
    return game::setBlock(region, bp(p), *block);
}

bool placeBlock(BlockSource& region, GP const& p, std::string_view id, std::vector<std::pair<std::string_view, int>> const& states) {
    auto const* block = game::blockWith(id, states);
    if (!block) return false;
    return game::setBlock(region, bp(p), *block);
}

cc::Dir facingOf(Block const& block) {
    return static_cast<cc::Dir>(std::clamp(game::intState(block, "levitate:facing").value_or(1), 0, 5));
}

namespace {
std::string stringState(Block const& block, char const* key) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return {};
    auto const& states = nbt["states"];
    if (!states.is_object()) return {};
    for (auto const& [name, v] : states.items()) {
        if (name != key) continue;
        if (!v.hold(Tag::Type::String)) return {};
        return std::string(v.get<StringTag>());
    }
    return {};
}

/// Signal a neighbour block gives to `towards` (state based).
int signalFrom(BlockSource& region, cc::Pos const& n, cc::Pos const& towards) {
    auto bpos = bp(n);
    if (!region.hasChunksAt(bpos, 0, false)) return 0;
    auto const& block = region.getBlock(bpos);
    if (block.isAir()) return 0;
    std::string const& id = block.getTypeName();
    if (id == "minecraft:redstone_block") return 15;
    if (id == "minecraft:lever") return game::intState(block, "open_bit").value_or(0) ? 15 : 0;
    if (id.ends_with("_button")) return game::intState(block, "button_pressed_bit").value_or(0) ? 15 : 0;
    if (id == "minecraft:observer") return game::intState(block, "powered_bit").value_or(0) ? 15 : 0;
    if (id == "minecraft:powered_repeater" || id == "minecraft:powered_comparator") return 15;
    if (id == "minecraft:redstone_torch") {
        // a torch powers every neighbour except the block it stands on
        auto t = stringState(block, "torch_facing_direction");
        cc::Pos support = n + cc::Pos{0, -1, 0};
        if (t == "west") support = n + cc::Pos{1, 0, 0};
        else if (t == "east") support = n + cc::Pos{-1, 0, 0};
        else if (t == "north") support = n + cc::Pos{0, 0, 1};
        else if (t == "south") support = n + cc::Pos{0, 0, -1};
        return support == towards ? 0 : 15;
    }
    if (auto s = game::intState(block, "redstone_signal")) return std::clamp(*s, 0, 15);
    // a solid block standing on a lit torch is strongly powered
    if (block.getBlockType().mSolid) {
        auto below = bp(n + cc::Pos{0, -1, 0});
        if (region.hasChunksAt(below, 0, false) && region.getBlock(below).getTypeName() == "minecraft:redstone_torch") {
            auto t = stringState(region.getBlock(below), "torch_facing_direction");
            if (t.empty() || t == "top" || t == "unknown") return 15;
        }
    }
    return 0;
}
} // namespace

int redstoneSignal(BlockSource& region, cc::Pos const& p) {
    int best = 0;
    for (auto d : cc::kAllDirs) best = std::max(best, signalFrom(region, p + cc::offset(d), p));
    return best;
}

// ---- players and actors -----------------------------------------------------------------------

Player* playerById(std::int64_t uniqueId) {
    auto level = ll::service::getLevel();
    if (!level || !uniqueId) return nullptr;
    ActorUniqueID id;
    id.rawID = uniqueId;
    return level->getPlayer(id);
}

Actor* actorById(std::int64_t uniqueId) {
    auto level = ll::service::getLevel();
    if (!level || !uniqueId) return nullptr;
    ActorUniqueID id;
    id.rawID = uniqueId;
    auto* a  = level->fetchEntity(id, false);
    return (a && a->isAlive()) ? a : nullptr;
}

std::int64_t uidOf(Actor const& actor) { return actor.getOrCreateUniqueID().rawID; }

void actionBar(Player& player, std::string const& text) {
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void playSound(int dim, cc::Vec3d const& at, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{static_cast<int>(at.x * 8), static_cast<int>(at.y * 8), static_cast<int>(at.z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{int(std::floor(at.x)), int(std::floor(at.y)), int(std::floor(at.z))}, DimensionType(dim));
}

void assembleSound(int dim, cc::Vec3d const& at) {
    playSound(dim, at, "open.wooden_trapdoor", .5f, .5f);
    playSound(dim, at, "random.chestopen", .045f, .74f);
}

void disassembleSound(int dim, cc::Vec3d const& at) { playSound(dim, at, "close.wooden_trapdoor", .35f, .75f); }

void give(Player& player, ItemStack const& stack) {
    if (common::items::isEmpty(stack)) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
    player.sendInventory(false);
}

void consumeSelected(Player& player) {
    if (player.isCreative()) return;
    auto const& held = player.getSelectedItem();
    player.setSelectedItem(common::items::withCount(held, common::items::countOf(held) - 1));
    player.sendInventory(false);
}

} // namespace lv::feature::bearings
