// Redstone unit (U23): ids, directions, block state helpers, effects and the signal reader.
//
// Signals: our blocks emit through `minecraft:redstone_producer` (BP permutations), so vanilla
// redstone sees them natively. Reading the signal *into* one of our blocks uses the game's
// circuit system: every vanilla redstone block owns a circuit component (wire = transporter,
// repeater / comparator / torch = capacitor, lever / button / redstone block = producer, a
// powered solid block = powered block) whose strength is what vanilla redstone uses.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

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
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/redstone/circuit/CircuitSceneGraph.h"
#include "mc/world/redstone/circuit/CircuitSystem.h"
#include "mc/world/redstone/circuit/components/BaseCircuitComponent.h"
#include "mc/world/redstone/circuit/components/CircuitComponentType.h"

#include "feature/common/BlockData.h"
#include "feature/redstone/Internal.h"
#include "feature/redstone/Redstone.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::redstone {

// ---- ids ---------------------------------------------------------------------------------------

std::optional<Kind> kindOf(std::string_view id) {
    static std::unordered_map<std::string_view, Kind> const map{
        {kLink,            Kind::Link           },
        {kLever,           Kind::Lever          },
        {kPulseRepeater,   Kind::PulseRepeater  },
        {kPulseExtender,   Kind::PulseExtender  },
        {kPulseTimer,      Kind::PulseTimer     },
        {kPoweredLatch,    Kind::PoweredLatch   },
        {kToggleLatch,     Kind::ToggleLatch    },
        {kSmartObserver,   Kind::SmartObserver  },
        {kThresholdSwitch, Kind::ThresholdSwitch},
        {kContact,         Kind::Contact        },
    };
    if (!id.starts_with("levitate:")) return std::nullopt;
    auto it = map.find(id);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

char const* idOf(Kind kind) {
    switch (kind) {
    case Kind::Link: return kLink;
    case Kind::Lever: return kLever;
    case Kind::PulseRepeater: return kPulseRepeater;
    case Kind::PulseExtender: return kPulseExtender;
    case Kind::PulseTimer: return kPulseTimer;
    case Kind::PoweredLatch: return kPoweredLatch;
    case Kind::ToggleLatch: return kToggleLatch;
    case Kind::SmartObserver: return kSmartObserver;
    case Kind::ThresholdSwitch: return kThresholdSwitch;
    case Kind::Contact: return kContact;
    }
    return "";
}

cr::DiodeKind diodeKind(Kind k) {
    return k == Kind::PulseExtender ? cr::DiodeKind::Extender : k == Kind::PulseTimer ? cr::DiodeKind::Timer : cr::DiodeKind::Repeater;
}

// ---- directions --------------------------------------------------------------------------------

int opposite(int d) { return d ^ 1; }

std::array<int, 3> offsetOf(int d) {
    switch (d) {
    case kDown: return {0, -1, 0};
    case kUp: return {0, 1, 0};
    case kNorth: return {0, 0, -1};
    case kSouth: return {0, 0, 1};
    case kWest: return {-1, 0, 0};
    default: return {1, 0, 0};
    }
}

BlockPos step(BlockPos const& p, int d) {
    auto o = offsetOf(d);
    return BlockPos{p.x + o[0], p.y + o[1], p.z + o[2]};
}

GP step(GP const& p, int d) {
    auto o = offsetOf(d);
    return {p.dim, p.x + o[0], p.y + o[1], p.z + o[2]};
}

int clockwise(int d) {
    switch (d) {
    case kNorth: return kEast;
    case kEast: return kSouth;
    case kSouth: return kWest;
    case kWest: return kNorth;
    default: return d;
    }
}

int counterClockwise(int d) {
    switch (d) {
    case kNorth: return kWest;
    case kWest: return kSouth;
    case kSouth: return kEast;
    case kEast: return kNorth;
    default: return d;
    }
}

bool isHorizontal(int d) { return d >= kNorth && d <= kEast; }

char const* dirName(int d) {
    static char const* names[]{"down", "up", "north", "south", "west", "east"};
    return d >= 0 && d < 6 ? names[d] : "?";
}

int nearestLooking(float pitch, float yaw) {
    // Bedrock rotation: pitch down positive, yaw 0 = south (+z), 90 = west (-x)
    double p = pitch * std::numbers::pi / 180.0, y = yaw * std::numbers::pi / 180.0;
    double lx = -std::sin(y) * std::cos(p), ly = -std::sin(p), lz = std::cos(y) * std::cos(p);
    int    best  = kUp;
    double score = -2;
    for (int d = 0; d < 6; ++d) {
        auto   o = offsetOf(d);
        double s = o[0] * lx + o[1] * ly + o[2] * lz;
        if (s > score) score = s, best = d;
    }
    return best;
}

int horizontalLooking(float yaw) {
    int idx = static_cast<int>(std::floor(yaw / 90.0f + 0.5f)) & 3; // 0 S, 1 W, 2 N, 3 E
    static constexpr int map[]{kSouth, kWest, kNorth, kEast};
    return map[idx];
}

// ---- blocks ------------------------------------------------------------------------------------

int state(Block const& block, std::string_view name, int fallback) { return game::intState(block, name).value_or(fallback); }

Block const* withStates(Block const& block, std::initializer_list<std::pair<std::string_view, int>> changes) {
    Block const* out  = &block;
    auto const&  type = block.getBlockType();
    for (auto const& [name, value] : changes) {
        if (auto const* s = type.getBlockState(HashedString(std::string(name)))) {
            if (auto b = out->setState(*s, value)) out = b.as_ptr();
        }
    }
    return out;
}

bool setStates(BlockSource& region, BlockPos const& pos, std::initializer_list<std::pair<std::string_view, int>> changes) {
    auto const& block = region.getBlock(pos);
    auto const* next  = withStates(block, changes);
    if (!next || next == &block) return false;
    return game::setBlock(region, pos, *next);
}

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

bool loadedAt(BlockSource& region, BlockPos const& pos) {
    if (pos.y < region.getMinHeight() || pos.y >= region.getMaxHeight()) return false;
    return region.hasChunksAt(pos, 0, false);
}

Block const* vanillaBlock(std::string const& id, Json const& states) {
    Block::BlockStatesType list;
    for (auto const& [k, v] : states.items()) {
        if (v.is_string()) list.emplace_back(k, v.get<std::string>());
        else if (v.is_boolean()) list.emplace_back(k, v.get<bool>());
        else if (v.is_number_integer()) list.emplace_back(k, v.get<int>());
    }
    auto b = list.empty() ? Block::tryGetFromRegistry(HashedString(id)) : Block::tryGetFromRegistry(HashedString(id), list);
    return b ? b.as_ptr() : nullptr;
}

std::string strState(Block const& block, std::string const& name) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return {};
    auto const& states = nbt["states"];
    if (!states.is_object() || !states.contains(name)) return {};
    auto const& v = states[name];
    if (!v.hold(Tag::Type::String)) return {};
    return std::string(v.get<StringTag>());
}

// ---- our emission ------------------------------------------------------------------------------

int targetDirection(Block const& block) {
    switch (state(block, sTarget)) {
    case 1: return kDown;
    case 2: return kUp;
    default: return state(block, sFacing, kNorth);
    }
}

std::optional<int> ourEmission(Block const& block, int d) {
    auto kind = kindOf(block.getTypeName());
    if (!kind) return std::nullopt;
    switch (*kind) {
    case Kind::Link: return state(block, sReceiver) ? state(block, sPower) : 0;
    case Kind::Lever: return state(block, sPower);
    case Kind::PulseRepeater:
    case Kind::PulseExtender:
    case Kind::PulseTimer:
        if (d != opposite(state(block, sFacing, kNorth))) return 0;
        return cr::diodeOutput(state(block, sPowering) != 0, state(block, sInverted) != 0);
    case Kind::PoweredLatch:
    case Kind::ToggleLatch:
        if (d != opposite(state(block, sFacing, kNorth))) return 0;
        return state(block, sPowering) ? 15 : 0;
    case Kind::SmartObserver:
    case Kind::ThresholdSwitch: return state(block, sPowered) && d != targetDirection(block) ? 15 : 0;
    case Kind::Contact: return state(block, sPowered) && d != state(block, sFacing, kUp) ? 15 : 0;
    }
    return 0;
}

// ---- circuit components ------------------------------------------------------------------------

namespace {

std::uint64_t packPos(BlockPos const& p) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x) & 0x3FFFFFF) << 38)
         | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z) & 0x3FFFFFF) << 12)
         | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y) & 0xFFF));
}

std::uint64_t& tickCounter() {
    static std::uint64_t t = 0;
    return t;
}

/// How components are looked up: 0 not decided, 1 the game's map can be searched with our
/// BlockPos hash, 2 it can't (different hash): a per-tick index is built by iterating it.
int& lookupMode() {
    static int m = 0;
    return m;
}

struct ScanIndex {
    std::uint64_t                                          tick = ~0ull;
    std::unordered_map<std::uint64_t, BaseCircuitComponent*> map;
};

/// The dimension's circuit system, whether the member is a plain or a typed-storage unique_ptr.
template <class T>
CircuitSystem* systemOf(std::unique_ptr<T>& p) {
    return p.get();
}
template <class W>
CircuitSystem* systemOf(W& w) {
    return w.get().get();
}

BaseCircuitComponent* componentAt(BlockSource& region, BlockPos const& pos) {
    auto& dimension = region.getDimension();
    auto* system    = systemOf(dimension.mCircuitSystem);
    if (!system) return nullptr;
    auto& all = system->mSceneGraph->mAllComponents;
    if (all.empty()) return nullptr;
    auto& mode = lookupMode();
    if (mode == 0) {
        auto const& [key, value] = *all.begin();
        auto it                  = all.find(key);
        mode                     = it != all.end() && it->second.get() == value.get() ? 1 : 2;
        log().info("redstone: circuit components found by {}", mode == 1 ? "lookup" : "scan");
    }
    BaseCircuitComponent* comp = nullptr;
    if (mode == 1) {
        auto it = all.find(pos);
        if (it != all.end()) comp = it->second.get();
    } else {
        static std::unordered_map<int, ScanIndex> indexes;
        auto& index = indexes[static_cast<int>(region.getDimensionId())];
        if (index.tick != tickCounter()) {
            index.tick = tickCounter();
            index.map.clear();
            for (auto const& [p, c] : all) index.map.emplace(packPos(p), c.get());
        }
        auto it = index.map.find(packPos(pos));
        if (it != index.map.end()) comp = it->second;
    }
    if (comp && comp->mRemoved) return nullptr;
    return comp;
}

/// The strength of the component of a vanilla block at `n` going into its neighbour on side
/// `toward` (FacingID from `n`).
int componentSignal(BlockSource& region, BlockPos const& n, Block const& block, int toward) {
    auto* comp = componentAt(region, n);
    if (!comp) {
        // no circuit (yet): what the block state says
        if (block.getTypeName() == "minecraft:redstone_block") return 15;
        return std::clamp(state(block, "redstone_signal"), 0, 15);
    }
    int  strength = std::clamp(comp->getStrength(), 0, 15);
    auto type     = static_cast<std::uint64_t>(comp->getCircuitComponentType());
    auto group    = type & static_cast<std::uint64_t>(CircuitComponentType::GroupMask);
    if (type == static_cast<std::uint64_t>(CircuitComponentType::TransporterComponent)) {
        // a wire powers the block under it and the blocks beside it, never the one above
        return toward == kUp ? 0 : strength;
    }
    if (group == static_cast<std::uint64_t>(CircuitComponentType::ProducerComponent)
        || group == static_cast<std::uint64_t>(CircuitComponentType::PoweredBlockComponent)) {
        return strength;
    }
    if (group == static_cast<std::uint64_t>(CircuitComponentType::CapacitorComponent)) {
        if (type == static_cast<std::uint64_t>(CircuitComponentType::RedstoneTorchCapacitor)) {
            // a torch powers everything but the block it hangs on
            auto f       = strState(block, "torch_facing_direction");
            int  support = f.empty() || f == "top" || f == "unknown" ? kDown
                         : f == "north"                             ? kSouth
                         : f == "south"                             ? kNorth
                         : f == "west"                              ? kEast
                                                                    : kWest;
            return toward == support ? 0 : strength;
        }
        // repeaters and comparators only power the block they face; their circuit direction is
        // the legacy 2D one (0 south, 1 west, 2 north, 3 east)
        static constexpr int fromLegacy[]{kSouth, kWest, kNorth, kEast};
        int                  d = comp->getDirection();
        int                  out = d >= 0 && d < 4 ? fromLegacy[d] : -1;
        return out == toward ? strength : 0;
    }
    return 0;
}

} // namespace

std::uint64_t& gameTickCounter() { return tickCounter(); }

std::string circuitDebug(BlockSource& region, BlockPos const& pos) {
    if (!loadedAt(region, pos)) return "not loaded";
    auto* comp = componentAt(region, pos);
    if (!comp) return region.getBlock(pos).getTypeName() + ": no component";
    return region.getBlock(pos).getTypeName() + ": type " + std::to_string(static_cast<std::uint64_t>(comp->getCircuitComponentType()))
         + " strength " + std::to_string(comp->getStrength()) + " direction " + std::to_string(comp->getDirection());
}

int signalFrom(BlockSource& region, BlockPos const& pos, int face) {
    auto n = step(pos, face);
    if (!loadedAt(region, n)) return 0;
    auto const& block = region.getBlock(n);
    if (block.isAir()) return 0;
    if (auto ours = ourEmission(block, opposite(face))) return *ours;
    return componentSignal(region, n, block, opposite(face));
}

int undirectedSignal(BlockSource& region, BlockPos const& pos, int face) {
    auto n = step(pos, face);
    if (!loadedAt(region, n)) return 0;
    auto const& block = region.getBlock(n);
    if (block.isAir() || kindOf(block.getTypeName())) return 0;
    auto* comp = componentAt(region, n);
    if (!comp) return 0;
    auto type  = static_cast<std::uint64_t>(comp->getCircuitComponentType());
    auto group = type & static_cast<std::uint64_t>(CircuitComponentType::GroupMask);
    if (type == static_cast<std::uint64_t>(CircuitComponentType::TransporterComponent)
        || group == static_cast<std::uint64_t>(CircuitComponentType::PoweredBlockComponent)) {
        return std::clamp(comp->getStrength(), 0, 15);
    }
    return 0;
}

int bestNeighborSignal(BlockSource& region, BlockPos const& pos) {
    int best = 0;
    for (int d = 0; d < 6 && best < 15; ++d) best = std::max(best, signalFrom(region, pos, d));
    return best;
}

// ---- block data --------------------------------------------------------------------------------

Json const* dataOf(GP const& pos) {
    auto const* d = common::blockdata::get(pos);
    if (!d) return nullptr;
    auto it = d->find("redstone");
    return it == d->end() || !it->is_object() ? nullptr : &*it;
}

Json& dataFor(GP const& pos) {
    auto& d = common::blockdata::edit(pos);
    auto& r = d["redstone"];
    if (!r.is_object()) r = Json::object();
    return r;
}

// ---- effects -----------------------------------------------------------------------------------

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

void spawnParticle(int dim, double x, double y, double z, std::string const& effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(x), float(y), float(z)}, dimension.get());
}

void actionBar(Player& player, std::string const& text) {
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

void giveOrDrop(Player& player, ItemStack const& stack) {
    if (stack.isNull()) return;
    ItemStack copy = stack;
    if (!player.add(copy)) player.drop(copy, false);
    player.sendInventory(false);
}

} // namespace lv::feature::redstone
