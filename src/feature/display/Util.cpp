// Display unit (U24): shared helpers (dyes, block states, redstone, feedback, deferred edits) and
// the floating name-tag text vanilla clients see on display boards and nixie tubes.

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
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
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/display/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::display {

// ---- dyes -------------------------------------------------------------------------------------

std::optional<int> dyeOf(std::string_view itemId) {
    for (int i = 0; i < 16; ++i)
        if (itemId == std::string("minecraft:") + kDyeNames[static_cast<std::size_t>(i)] + "_dye") return i;
    return std::nullopt;
}

unsigned dyeRgb(int dye) {
    // Java DyeColor text colours ⚠ (Create's DyeHelper uses its own pairs; SPEC §3.6 Display)
    static constexpr unsigned rgb[16]{0xFFFFFF, 0xFF681F, 0xFF00FF, 0x9AC0CD, 0xFFFF00, 0xBFFF00, 0xFF69B4, 0x808080,
                                      0xD3D3D3, 0x00FFFF, 0xA020F0, 0x0000FF, 0x8B4513, 0x00FF00, 0xFF0000, 0x000000};
    return rgb[std::clamp(dye, 0, 15)];
}

std::string dyeFormatCode(int dye) {
    static constexpr char const* codes[16]{"§f", "§6", "§d", "§b", "§e", "§a", "§d", "§8",
                                           "§7", "§3", "§5", "§9", "§n", "§2", "§c", "§0"};
    return codes[std::clamp(dye, 0, 15)];
}

// ---- block helpers ----------------------------------------------------------------------------

std::optional<int> intState(Block const& block, std::string_view name) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return std::nullopt;
    auto const& states = nbt["states"];
    std::string key(name);
    if (!states.contains(key)) return std::nullopt;
    auto const& v = states[key];
    if (v.hold(Tag::Type::Byte)) return int(v.get<ByteTag>().data);
    if (v.hold(Tag::Type::Int)) return v.get<IntTag>().data;
    if (v.hold(Tag::Type::Short)) return int(v.get<ShortTag>().data);
    return std::nullopt;
}

std::optional<std::string> strState(Block const& block, std::string_view name) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return std::nullopt;
    auto const& states = nbt["states"];
    std::string key(name);
    if (!states.contains(key) || !states[key].hold(Tag::Type::String)) return std::nullopt;
    return std::string(states[key].get<StringTag>());
}

Block const* withStates(Block const& block, std::vector<std::pair<std::string, StateValue>> const& states) {
    CompoundTag nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return &block;
    auto& tagStates = nbt["states"];
    for (auto const& [key, value] : states) {
        if (!tagStates.contains(key)) continue;
        auto& slot = tagStates[key];
        if (auto const* s = std::get_if<std::string>(&value)) {
            if (slot.hold(Tag::Type::String)) slot = *s;
        } else if (auto const* i = std::get_if<int>(&value)) {
            if (slot.hold(Tag::Type::Byte)) slot = static_cast<uchar>(*i);
            else if (slot.hold(Tag::Type::Int)) slot = *i;
            else if (slot.hold(Tag::Type::Short)) slot = static_cast<short>(*i);
        }
    }
    auto result = Block::tryGetFromRegistry(nbt);
    return result ? result.as_ptr() : nullptr;
}

Block const* loadedBlock(BlockSource& region, BlockPos const& pos) {
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return nullptr;
    if (!region.hasChunksAt(pos, 0, false)) return nullptr;
    return &region.getBlock(pos);
}

std::string idAt(BlockSource& region, GlobalPos const& pos) {
    auto const* b = loadedBlock(region, game::blockPos(pos));
    return b ? b->getTypeName() : std::string{};
}

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

GlobalPos offsetPos(GlobalPos p, int face, int n) {
    if (face < 0 || face > 5) return p;
    p.x += kDx[face] * n;
    p.y += kDy[face] * n;
    p.z += kDz[face] * n;
    return p;
}

int faceOfHorizontal(int facing) {
    static constexpr int faces[4]{3, 4, 2, 5}; // S, W, N, E
    return faces[facing & 3];
}

// ---- redstone ---------------------------------------------------------------------------------

namespace {
bool endsWith(std::string_view s, std::string_view suffix) { return s.size() >= suffix.size() && s.ends_with(suffix); }

/// Power a vanilla emitter at `n` gives towards the block at `n - offset(face)` (face = from the
/// block to the emitter).
int emitted(Block const& b, int face) {
    std::string const& id = b.getTypeName();
    if (id == "minecraft:redstone_block") return 15;
    if (id == "minecraft:lever") return intState(b, "open_bit").value_or(0) ? 15 : 0;
    if (endsWith(id, "_button")) return intState(b, "button_pressed_bit").value_or(0) ? 15 : 0;
    if (id == "minecraft:observer") return intState(b, "powered_bit").value_or(0) ? 15 : 0;
    if (id == "minecraft:powered_repeater") return 15;
    if (id == "minecraft:powered_comparator" || id == "minecraft:unpowered_comparator") {
        return intState(b, "output_lit_bit").value_or(0) ? 15 : 0;
    }
    if (id == "minecraft:redstone_torch") {
        // a torch powers every neighbour except the block it stands on
        auto t = strState(b, "torch_facing_direction").value_or("top");
        int  support = t == "top" || t == "unknown" ? 0 : t == "north" ? 3 : t == "south" ? 2 : t == "west" ? 5 : t == "east" ? 4 : -1;
        // support is the face of the torch block pointing at its supporting block
        return support == (face ^ 1) ? 0 : 15;
    }
    if (id == "minecraft:redstone_wire") {
        // wire powers the block below it and the blocks beside it
        return face == 0 ? 0 : intState(b, "redstone_signal").value_or(0);
    }
    if (auto s = intState(b, "redstone_signal")) return *s; // plates, daylight detectors, ...
    return 0;
}
} // namespace

int signalAt(BlockSource& region, BlockPos const& pos, int exceptFace) {
    int best = 0;
    for (int f = 0; f < 6; ++f) {
        if (f == exceptFace) continue;
        BlockPos n{pos.x + kDx[f], pos.y + kDy[f], pos.z + kDz[f]};
        auto const* b = loadedBlock(region, n);
        if (!b || b->isAir()) continue;
        best = std::max(best, emitted(*b, f));
        if (best >= 15) return 15;
    }
    return best;
}

// ---- feedback ---------------------------------------------------------------------------------

void actionBar(Player& player, std::string const& text) {
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void playSound(BlockSource& region, GlobalPos const& pos, std::string const& sound, float volume, float pitch) {
    (void)region;
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(BlockPos{pos.x, pos.y, pos.z}, DimensionType(pos.dim));
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (stack.isNull()) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
    player.sendInventory(false);
}

void dropAt(BlockSource& region, GlobalPos const& pos, ItemStack const& stack) {
    auto level = ll::service::getLevel();
    if (!level || stack.isNull()) return;
    level->getSpawner().spawnItem(region, stack, nullptr, Vec3{pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f}, 10);
}

std::uint64_t gameTime() {
    auto level = ll::service::getLevel();
    return level ? static_cast<std::uint64_t>(level->getCurrentServerTick().tickID) : 0;
}

namespace {
std::vector<std::function<void()>>& deferred() {
    static std::vector<std::function<void()>> v;
    return v;
}
} // namespace

void defer(std::function<void()> fn) { deferred().push_back(std::move(fn)); }

Player* playerById(std::int64_t id) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID uid;
    uid.rawID = id;
    return level->getPlayer(uid);
}

void broadcast(int dim, std::string const& channel, Json const& payload) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer(std::function<bool(Player&)>([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) == dim && net::isCompanion(p)) net::send(p, channel, payload);
        return true;
    }));
}

// ---- floating text ----------------------------------------------------------------------------

namespace {
constexpr char kTextTag[] = "lv_display_text";

struct FloatingText {
    int                   dim = 0;
    std::array<double, 3> pos{};
    std::string           text;
    std::int64_t          actor = 0;
    bool                  dirty = true;
};

std::unordered_map<std::string, FloatingText>& texts() {
    static std::unordered_map<std::string, FloatingText> m;
    return m;
}

Actor* fetch(std::int64_t id) {
    if (!id) return nullptr;
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID uid;
    uid.rawID = id;
    return level->fetchEntity(uid, false);
}

void removeActor(std::int64_t id) {
    if (auto* a = fetch(id)) a->remove();
}

std::int64_t spawnText(FloatingText const& t) {
    auto level  = ll::service::getLevel();
    auto region = regionOf(t.dim);
    if (!level || !region) return 0;
    BlockPos bp{int(std::floor(t.pos[0])), int(std::floor(t.pos[1])), int(std::floor(t.pos[2]))};
    if (!region->hasChunksAt(bp, 0, false)) return 0;
    Vec3 at{float(t.pos[0]), float(t.pos[1]), float(t.pos[2])};
    auto owner = level->getActorFactory().createSpawnedActor(ActorDefinitionIdentifier(kTextEntityId), nullptr, at, Vec2{0, 0});
    if (!owner) return 0;
    auto* added = level->addEntity(*region, std::move(owner));
    if (!added) return 0;
    added->addTag(kTextTag);
    added->setNameTag(t.text);
    return added->getOrCreateUniqueID().rawID;
}

void tickTexts(std::uint64_t tick) {
    if (tick % 10 == 0) {
        for (auto& [key, t] : texts()) {
            auto* a = fetch(t.actor);
            if (!a) {
                t.actor = spawnText(t);
                t.dirty = false;
                continue;
            }
            if (t.dirty) {
                a->setNameTag(t.text);
                t.dirty = false;
            }
        }
    }
    if (tick % 100 == 50) {
        // entities saved with their chunk (or left by a crash) are not ours any more
        auto level = ll::service::getLevel();
        if (!level) return;
        std::unordered_set<std::int64_t> live;
        for (auto const& [k, t] : texts()) live.insert(t.actor);
        for (auto* a : level->getRuntimeActorList()) {
            if (!a || !a->hasTag(kTextTag)) continue;
            if (!live.contains(a->getOrCreateUniqueID().rawID)) a->remove();
        }
    }
}
} // namespace

void setFloatingText(std::string const& key, int dim, std::array<double, 3> pos, std::string const& text) {
    if (text.empty()) {
        removeFloatingText(key);
        return;
    }
    auto& t = texts()[key];
    if (t.actor && (t.dim != dim || t.pos != pos)) {
        removeActor(t.actor);
        t.actor = 0;
    }
    if (t.text != text) t.dirty = true;
    t.dim  = dim;
    t.pos  = pos;
    t.text = text;
    if (auto* a = fetch(t.actor); a && t.dirty) {
        a->setNameTag(text);
        t.dirty = false;
    }
}

void removeFloatingText(std::string const& key) {
    auto it = texts().find(key);
    if (it == texts().end()) return;
    removeActor(it->second.actor);
    texts().erase(it);
}

int floatingTextCount() { return static_cast<int>(texts().size()); }

// ---- module -----------------------------------------------------------------------------------

namespace link {
void tickAll(std::uint64_t tick);
}
namespace board {
void tickAll(std::uint64_t tick);
}
namespace nixie {
void tickAll(std::uint64_t tick);
}

class DisplayModule {
public:
    void install() {
        game::db::onWorldClosed([] {
            for (auto& [k, t] : texts()) t.actor = 0;
            texts().clear();
            deferred().clear();
        });
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) {
            ++mTicks;
            if (!deferred().empty()) {
                auto run = std::exchange(deferred(), {});
                for (auto& fn : run) fn();
            }
            link::tickAll(mTicks);
            board::tickAll(mTicks);
            nixie::tickAll(mTicks);
            tickTexts(mTicks);
        });
        installHooks();
    }
    void uninstall() {
        uninstallHooks();
        ll::event::EventBus::getInstance().removeListener(mTick);
        for (auto& [k, t] : texts()) removeActor(t.actor);
        texts().clear();
    }

private:
    ll::event::ListenerPtr mTick;
    std::uint64_t          mTicks = 0;
};

} // namespace lv::feature::display

LV_REGISTER_MODULE(lv::feature::display::DisplayModule);
