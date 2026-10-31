// Game adapters of the contraption unit: block states <-> core types, Create's movement checks
// for vanilla blocks, collision boxes, redstone power, messages, sounds and particles.

#include <algorithm>
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
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/Redstone.h"
#include "feature/contraption/Internal.h"
#include "feature/contraption/Registries.h"
#include "mod/Config.h"

namespace lv::feature::contraption::detail {

namespace {

bool endsWith(std::string_view s, std::string_view suffix) { return s.size() >= suffix.size() && s.ends_with(suffix); }

std::optional<std::int64_t> intState(cc::States const& s, char const* key) {
    auto it = s.find(key);
    if (it == s.end()) return std::nullopt;
    if (auto const* v = std::get_if<std::int64_t>(&it->second)) return *v;
    return std::nullopt;
}
std::optional<std::string> strState(cc::States const& s, char const* key) {
    auto it = s.find(key);
    if (it == s.end()) return std::nullopt;
    if (auto const* v = std::get_if<std::string>(&it->second)) return *v;
    return std::nullopt;
}

std::optional<cc::Dir> facingDir(cc::States const& s, char const* key = "facing_direction") {
    auto v = intState(s, key);
    if (!v || *v < 0 || *v > 5) return std::nullopt;
    return static_cast<cc::Dir>(*v);
}

/// Java's HORIZONTAL FACING: `minecraft:cardinal_direction`, else the legacy `direction` (S W N E).
std::optional<cc::Dir> horizontalFacing(cc::States const& s) {
    if (auto c = strState(s, "minecraft:cardinal_direction")) return cc::dirFromName(*c);
    if (auto v = intState(s, "direction"); v && *v >= 0 && *v <= 3) {
        static constexpr cc::Dir kSwne[4]{cc::Dir::South, cc::Dir::West, cc::Dir::North, cc::Dir::East};
        return kSwne[*v];
    }
    return std::nullopt;
}

bool isVine(std::string const& id) {
    return id == "minecraft:vine" || id == "minecraft:twisting_vines" || id == "minecraft:weeping_vines" || id.starts_with("minecraft:cave_vines");
}
bool isCoral(std::string const& id) {
    return id.starts_with("minecraft:") && id.find("coral") != std::string::npos && !endsWith(id, "coral_block");
}

/// Create's isBrittle / isBlockAttachedTowards / isNotSupportive for vanilla blocks.
void vanillaRules(std::string const& id, cc::States const& s, cc::BlockInfo& info, cc::Pos const& p) {
    using cc::bit;
    using cc::Dir;
    auto attach = [&](Dir d) { info.attachedTowards |= bit(d); };
    if (auto t = strState(s, "torch_facing_direction")) {
        info.brittle = true;
        auto d       = *t == "top" || *t == "unknown" ? std::optional<Dir>(Dir::Down) : cc::dirFromName(*t);
        if (d) attach(*d);
        return;
    }
    if (endsWith(id, "ladder")) {
        info.brittle = true;
        if (auto f = facingDir(s)) attach(cc::opposite(*f));
        return;
    }
    if (endsWith(id, "wall_sign") || endsWith(id, "wall_banner")) {
        info.brittle = true;
        if (auto f = facingDir(s)) attach(cc::opposite(*f));
        return;
    }
    if (endsWith(id, "standing_sign") || id == "minecraft:standing_banner" || endsWith(id, "pressure_plate")
        || id == "minecraft:redstone_wire" || endsWith(id, "_repeater") || endsWith(id, "_comparator")
        || endsWith(id, "rail")) {
        info.brittle = true;
        attach(Dir::Down);
        return;
    }
    if (endsWith(id, "carpet")) {
        info.brittle = true;
        attach(Dir::Down);
        info.notSupportive |= bit(Dir::Up);
        return;
    }
    if (endsWith(id, "_hanging_sign")) {
        info.brittle = true;
        if (intState(s, "hanging").value_or(1)) attach(Dir::Up);
        else if (auto f = facingDir(s)) attach(cc::opposite(*f));
        return;
    }
    if (endsWith(id, "_door") && !endsWith(id, "trapdoor")) {
        info.brittle = true;
        bool upper   = intState(s, "upper_block_bit").value_or(0) != 0;
        attach(Dir::Down);
        if (!upper) attach(Dir::Up);
        info.linked.push_back(p + cc::offset(upper ? Dir::Down : Dir::Up));
        return;
    }
    if (id == "minecraft:bed") {
        info.brittle = true;
        Dir  toHead  = cc::fromHorizontalIndex(static_cast<int>(intState(s, "direction").value_or(0)));
        bool head    = intState(s, "head_piece_bit").value_or(0) != 0;
        Dir  partner = head ? cc::opposite(toHead) : toHead;
        attach(partner);
        info.linked.push_back(p + cc::offset(partner));
        return;
    }
    if (auto lever = strState(s, "lever_direction")) {
        info.brittle = true;
        if (lever->starts_with("down")) attach(Dir::Up);
        else if (lever->starts_with("up")) attach(Dir::Down);
        else if (auto d = cc::dirFromName(*lever)) attach(cc::opposite(*d));
        return;
    }
    if (endsWith(id, "button")) {
        info.brittle = true;
        if (auto f = facingDir(s)) attach(cc::opposite(*f));
        return;
    }
    if (id == "minecraft:lantern" || id == "minecraft:soul_lantern" || endsWith(id, "copper_lantern")) {
        info.brittle = true;
        attach(intState(s, "hanging").value_or(0) ? Dir::Up : Dir::Down);
        return;
    }
    if (id == "minecraft:bell") {
        info.brittle = true;
        auto a       = strState(s, "attachment").value_or("standing");
        if (a == "standing") attach(Dir::Down);
        else if (a == "hanging") attach(Dir::Up);
        else if (auto f = horizontalFacing(s)) attach(*f); // on a wall: towards its FACING (Create)
        return;
    }
    if (auto hanging = intState(s, "hanging")) {
        // Create's isBrittleFallback: every block with a HANGING property (mangrove propagules, ...)
        info.brittle = true;
        attach(*hanging ? Dir::Up : Dir::Down);
        return;
    }
    if (id == "minecraft:flower_pot") {
        info.brittle = true;
        attach(Dir::Down);
        return;
    }
    // #create:brittle (cocoa, cake, big dripleaf, bamboo sapling, vines, corals, cave vines)
    if (id == "minecraft:cocoa" || id == "minecraft:cake" || id == "minecraft:big_dripleaf" || id == "minecraft:bamboo_sapling"
        || isVine(id) || isCoral(id)) {
        info.brittle = true;
    }
}

/// Create's #create:movable_empty_collider: moved although they have no collision shape.
bool movableEmptyCollider(std::string const& id) {
    return id == "minecraft:web" || id == "minecraft:powder_snow" || id == "minecraft:trip_wire" || id == "minecraft:tripwire_hook"
        || id == "minecraft:bamboo_sapling" || isVine(id) || isCoral(id) || endsWith(id, "fence_gate") || endsWith(id, "banner");
}

/// Create's ContraptionMovementSetting config value.
MovementSetting settingOf(std::string const& value) {
    if (value == "MOVABLE") return MovementSetting::Movable;
    if (value == "NO_PICKUP") return MovementSetting::NoPickup;
    return MovementSetting::Unmovable;
}

} // namespace

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

cc::Pos  toPos(BlockPos const& p) { return {p.x, p.y, p.z}; }
BlockPos toBlockPos(cc::Pos const& p) { return BlockPos{p.x, p.y, p.z}; }

cc::States statesOf(Block const& block) {
    cc::States out;
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return out;
    auto const& states = nbt["states"];
    if (!states.is_object()) return out;
    for (auto const& [key, value] : states.items()) {
        if (value.hold(Tag::Type::String)) out[key] = std::string(value.get<StringTag>());
        else if (value.hold(Tag::Type::Byte)) out[key] = std::int64_t{value.get<ByteTag>().data};
        else if (value.hold(Tag::Type::Int)) out[key] = std::int64_t{value.get<IntTag>().data};
        else if (value.hold(Tag::Type::Short)) out[key] = std::int64_t{value.get<ShortTag>().data};
    }
    return out;
}

Block const* withStates(Block const& block, cc::States const& states) {
    CompoundTag nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return &block;
    auto& tagStates = nbt["states"];
    for (auto const& [key, value] : states) {
        if (!tagStates.contains(key)) continue;
        auto& slot = tagStates[key];
        if (auto const* s = std::get_if<std::string>(&value)) {
            if (slot.hold(Tag::Type::String)) slot = *s;
        } else if (auto const* i = std::get_if<std::int64_t>(&value)) {
            if (slot.hold(Tag::Type::Byte)) slot = static_cast<uchar>(*i);
            else if (slot.hold(Tag::Type::Int)) slot = static_cast<int>(*i);
            else if (slot.hold(Tag::Type::Short)) slot = static_cast<short>(*i);
        }
    }
    auto result = Block::tryGetFromRegistry(nbt);
    return result ? result.as_ptr() : &block;
}

std::string snbtOf(Block const& block) { return block.mSerializationId->toSnbt(SnbtFormat::Minimize, 0); }

Block const* blockFromSnbt(std::string const& snbt) {
    auto nbt = CompoundTag::fromSnbt(snbt);
    if (!nbt) return nullptr;
    auto block = Block::tryGetFromRegistry(*nbt);
    return block ? block.as_ptr() : nullptr;
}

std::optional<std::int64_t> intStateOf(Block const& block, std::string const& name) {
    auto s = statesOf(block);
    return intState(s, name.c_str());
}

std::optional<cc::Box> collisionOf(BlockSource& region, cc::Pos const& p) { return collisionOfBlock(region, region.getBlock(toBlockPos(p)), p); }

std::optional<cc::Box> collisionOfBlock(BlockSource& region, Block const& block, cc::Pos const& p) {
    auto        bp    = toBlockPos(p);
    if (block.isAir()) return std::nullopt;
    AABB box = block.getBlockType().getCollisionShape(block, region, bp, nullptr);
    if (box.max.x - box.min.x < 1e-4f || box.max.y - box.min.y < 1e-4f || box.max.z - box.min.z < 1e-4f) {
        return std::nullopt;
    }
    cc::Vec3d base = cc::Vec3d::of(p); // the game gives the shape in world coordinates
    return cc::Box{
        cc::Vec3d{box.min.x, box.min.y, box.min.z} - base,
        cc::Vec3d{box.max.x, box.max.y, box.max.z} - base,
    };
}

cc::BlockInfo describe(BlockSource& region, int /*dim*/, cc::Pos const& p) {
    cc::BlockInfo info;
    auto          bp = toBlockPos(p);
    if (p.y < region.getMinHeight() || p.y >= region.getMaxHeight()) return info;
    if (!region.hasChunksAt(bp, 0, false)) {
        info.loaded    = false;
        info.necessary = true;
        return info;
    }
    auto const& block = region.getBlock(bp);
    info.id           = block.getTypeName();
    if (block.isAir()) return info;
    auto states = statesOf(block);
    auto const& cfg = config::get<Config>();

    vanillaRules(info.id, states, info, p);

    // our own blocks
    if (auto kind = chassisKind(info.id); kind != cc::ChassisKind::None) {
        info.chassis     = kind;
        info.chassisAxis = static_cast<cc::Axis>(std::clamp<std::int64_t>(intState(states, "levitate:axis").value_or(1), 0, 2));
        if (kind == cc::ChassisKind::Radial) {
            auto sides = cc::radialSides(info.chassisAxis);
            char const* keys[4]{"levitate:sticky_north", "levitate:sticky_south", "levitate:sticky_east", "levitate:sticky_west"};
            for (int i = 0; i < 4; ++i)
                if (intState(states, keys[i]).value_or(0)) info.stickyFaces |= cc::bit(sides[i]);
        } else {
            if (intState(states, "levitate:sticky_top").value_or(0)) info.stickyFaces |= cc::bit(cc::dirOf(info.chassisAxis, true));
            if (intState(states, "levitate:sticky_bottom").value_or(0))
                info.stickyFaces |= cc::bit(cc::dirOf(info.chassisAxis, false));
        }
        info.chassisRange = storedChassisRange(static_cast<int>(region.getDimensionId()), p);
    }
    if (info.id == kSticker) {
        auto facing = static_cast<cc::Dir>(std::clamp<std::int64_t>(intState(states, "levitate:facing").value_or(1), 0, 5));
        if (intState(states, "levitate:extended").value_or(0)) {
            info.stickerFacing = facing;
            info.attachedTowards |= cc::bit(facing);
        } else {
            info.notSupportive |= cc::bit(facing);
        }
    }
    if (info.id == "minecraft:slime") info.sticky = cc::StickyKind::Slime;
    if (info.id == "minecraft:honey_block") info.sticky = cc::StickyKind::Honey;
    if (endsWith(info.id, "_glazed_terracotta")) info.pushOnly = true;

    // registered rules of other units
    if (auto const* rules = movementRules(info.id)) {
        if (rules->brittle) info.brittle = rules->brittle(states);
        if (rules->attachedTowards) info.attachedTowards |= rules->attachedTowards(states);
        if (rules->notSupportive) info.notSupportive |= rules->notSupportive(states);
        if (rules->pulls) info.pulls |= rules->pulls(states);
    }
    // Create's moveBlock: "Cart assemblers attach themselves" to the block they carry
    if (p.y - 1 >= region.getMinHeight() && region.getBlock(toBlockPos(p + cc::offset(cc::Dir::Down))).getTypeName() == "levitate:cart_assembler") {
        info.pulls |= cc::bit(cc::Dir::Down);
    }

    // isMovementNecessary
    if (info.brittle || info.chassis != cc::ChassisKind::None || info.id == kSticker || movableEmptyCollider(info.id)) {
        info.necessary = true;
    } else if (block.getBlockType().canBeBuiltOver(block, region, bp)) {
        info.necessary = false;
    } else {
        info.necessary = collisionOf(region, p).has_value() || block.getBlockType().mBlockEntityType != BlockActorType::Undefined;
    }

    // movementAllowed
    if (info.chassis == cc::ChassisKind::None) {
        if (block.getBlockType().getDestroySpeed() < 0) info.movable = false;
        if (std::find(cfg.unmovableBlocks.begin(), cfg.unmovableBlocks.end(), info.id) != cfg.unmovableBlocks.end()) {
            info.movable = false;
        }
        if (movementSetting(info.id) == MovementSetting::Unmovable) info.movable = false;
        // Create: ITrackBlock and StationBlock never move
        if (info.id == "levitate:track" || info.id == "levitate:fake_track" || info.id == "levitate:track_station") info.movable = false;
        if (auto const* rules = movementRules(info.id); rules && rules->movable && !rules->movable(states)) info.movable = false;
        if (auto const* at = movableAt(info.id); info.movable && at && !(*at)(region, {static_cast<int>(region.getDimensionId()), p.x, p.y, p.z}, states)) {
            info.movable = false;
        }
    }
    // Create's isNotSupportive fallback: brittle blocks carry nothing (carpets: only their top)
    if (info.brittle && !endsWith(info.id, "carpet")) info.notSupportive = 0x3F;
    return info;
}

bool powered(BlockSource& region, cc::Pos const& p) { return common::redstone::powered(region, toBlockPos(p)); }

void actionBar(Player& player, std::string const& text) {
    auto                            pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void playSound(int dim, cc::Vec3d const& pos, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{static_cast<int>(pos.x * 8), static_cast<int>(pos.y * 8), static_cast<int>(pos.z * 8)};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{int(std::floor(pos.x)), int(std::floor(pos.y)), int(std::floor(pos.z))}, DimensionType(dim));
}

void particle(int dim, cc::Vec3d const& pos, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(pos.x), float(pos.y), float(pos.z)}, dimension.get());
}

void dropItemAt(BlockSource& region, cc::Vec3d const& pos, Block const& block, std::string const& /*blockEntitySnbt*/) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto instance = block.asItemInstance(region, BlockPos{int(pos.x), int(pos.y), int(pos.z)}, false);
    auto tag      = instance.save(*SaveContextFactory::createCloneSaveContext());
    if (!tag) return;
    auto item = ItemStack::fromTag(*tag);
    if (item.isNull()) return;
    level->getSpawner().spawnItem(region, item, nullptr, Vec3{float(pos.x), float(pos.y), float(pos.z)}, 10);
}

} // namespace lv::feature::contraption::detail

namespace lv::feature::contraption {

MovementSetting movementSetting(std::string_view blockId) {
    auto const& cfg = config::get<Config>();
    if (blockId == "minecraft:mob_spawner") return detail::settingOf(cfg.movableSpawners);
    if (blockId == "minecraft:budding_amethyst") return detail::settingOf(cfg.amethystMovement);
    if (blockId == "minecraft:obsidian" || blockId == "minecraft:crying_obsidian" || blockId == "minecraft:respawn_anchor") {
        return detail::settingOf(cfg.movableObsidian);
    }
    if (blockId == "minecraft:reinforced_deepslate") return detail::settingOf(cfg.movableReinforcedDeepslate);
    return MovementSetting::Movable;
}

} // namespace lv::feature::contraption
