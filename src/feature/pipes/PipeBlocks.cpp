// Blocks of the pipes unit: states, Create's connection rules, placement orientation, wrench,
// encasing and brackets (Create's FluidPipeBlock, AxisPipeBlock / GlassFluidPipeBlock,
// EncasedPipeBlock, PumpBlock, FluidValveBlock, SmartFluidPipeBlock).

#include <array>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlaySoundPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/pipes/Pipes.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::pipes {

namespace bd = common::blockdata;

namespace {

constexpr std::array<char const*, 6> kFaceStates{
    "levitate:down",
    "levitate:up",
    "levitate:north",
    "levitate:south",
    "levitate:west",
    "levitate:east"
};

int stateOf(Block const& b, char const* name, int fallback = 0) { return game::intState(b, name).value_or(fallback); }

Block const* withStates(Block const& block, std::initializer_list<std::pair<char const*, int>> states) {
    Block const* out  = &block;
    auto const&  type = block.getBlockType();
    for (auto const& [name, value] : states) {
        if (auto const* s = type.getBlockState(HashedString(name))) {
            if (auto b = out->setState(*s, value)) out = b.as_ptr();
        }
    }
    return out;
}

Block const* blockOf(std::string_view id) { return game::blockWith(id); }

void playSound(BlockSource& region, BlockPos const& pos, char const* sound, float volume = 1.f, float pitch = 1.f) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(pos, region.getDimensionId());
}

void dropItem(BlockSource& region, BlockPos const& pos, std::string const& id, Player* player) {
    auto stack = common::items::make(id);
    if (!stack) return;
    if (player) {
        if (player->isCreative()) return;
        if (player->add(*stack)) {
            player->sendInventory(false);
            return;
        }
    }
    if (auto level = ll::service::getLevel())
        level->getSpawner().spawnItem(region, *stack, nullptr, Vec3{pos.x + .5f, pos.y + .5f, pos.z + .5f}, 0);
}

std::array<bool, 6> connectable(BlockSource& region, BlockPos const& pos) {
    std::array<bool, 6> can{};
    for (Face f : cp::kFaces) can[static_cast<int>(f)] = canConnectTo(region, pos, f);
    return can;
}

/// A kinetic block next to `pos` on `face` with a shaft end facing `pos`.
bool shaftFacing(BlockSource& region, BlockPos const& pos, Face face) {
    auto o = cp::offsetOf(face);
    BlockPos n{pos.x + o.x, pos.y + o.y, pos.z + o.z};
    if (!region.hasChunksAt(n, 0, false)) return false;
    auto const& b    = region.getBlock(n);
    auto const* spec = kinetics::specOf(b.getTypeName());
    if (!spec) return false;
    kinetics::BlockState s;
    s.id = b.getTypeName();
    for (auto const& name : spec->states) s.values.emplace_back(name, game::intState(b, name).value_or(0));
    return spec->shaftTowards(s, cp::opposite(face));
}

Face faceFromState(int v) { return static_cast<Face>(std::clamp(v, 0, 5)); }
/// Smart pipe `levitate:facing` 0..3 = north, south, west, east.
Face horizontalFromState(int v) { return static_cast<Face>(std::clamp(v, 0, 3) + 2); }
int  horizontalToState(Face f) { return std::clamp(static_cast<int>(f) - 2, 0, 3); }

FaceMask readMask(Block const& b) {
    FaceMask m = 0;
    for (Face f : cp::kFaces)
        if (stateOf(b, kFaceStates[static_cast<int>(f)]) != 0) m |= cp::bit(f);
    return m;
}

} // namespace

// ---- kinds and states ---------------------------------------------------------------------------

BlockKind kindOf(std::string_view id) {
    if (!id.starts_with("levitate:")) return BlockKind::None;
    if (id == kFluidPipe) return BlockKind::Pipe;
    if (id == kGlassPipe) return BlockKind::Glass;
    if (id == kEncasedPipe) return BlockKind::Encased;
    if (id == kPump) return BlockKind::Pump;
    if (id == kValve) return BlockKind::Valve;
    if (id == kSmartPipe) return BlockKind::Smart;
    return BlockKind::None;
}

BlockKind kindOf(Block const& block) { return kindOf(block.getTypeName()); }

bool isPipeLike(BlockKind k) { return k != BlockKind::None; }

FaceMask connections(Block const& block) { return readMask(block); }

Block const* withConnections(Block const& block, FaceMask open) {
    Block const* out = &block;
    for (Face f : cp::kFaces) out = withStates(*out, {{kFaceStates[static_cast<int>(f)], cp::has(open, f) ? 1 : 0}});
    return out;
}

Face pumpFront(Block const& block) { return faceFromState(stateOf(block, "levitate:facing", 1)); }

cp::ValvePlacement valveState(Block const& block) {
    return {faceFromState(stateOf(block, "levitate:facing", 1)), stateOf(block, "levitate:along_first") != 0};
}

cp::SmartPipeState smartState(Block const& block) {
    return {
        static_cast<cp::AttachFace>(std::clamp(stateOf(block, "levitate:face"), 0, 2)),
        horizontalFromState(stateOf(block, "levitate:facing"))
    };
}

FaceMask flowFaces(Block const& block) {
    switch (kindOf(block)) {
    case BlockKind::Pipe:
    case BlockKind::Encased:
        return readMask(block);
    case BlockKind::Glass:
        return cp::axisMask(static_cast<Axis>(std::clamp(stateOf(block, "levitate:axis", 1), 0, 2)));
    case BlockKind::Pump:
        return cp::axisMask(cp::axisOf(pumpFront(block)));
    case BlockKind::Valve: {
        auto v = valveState(block);
        return cp::axisMask(cp::valveAxes(v.facing, v.alongFirst).pipe);
    }
    case BlockKind::Smart:
        return cp::axisMask(cp::smartPipeAxis(smartState(block)));
    default:
        return 0;
    }
}

std::string flowSignature(Block const& block) {
    auto k = kindOf(block);
    if (k == BlockKind::None) return {};
    std::string s = std::to_string(static_cast<int>(k)) + ":" + std::to_string(flowFaces(block));
    if (k == BlockKind::Pump) s += ":" + std::to_string(static_cast<int>(pumpFront(block)));
    if (k == BlockKind::Valve) s += ":" + std::to_string(stateOf(block, "levitate:enabled"));
    return s;
}

// ---- Create's connection rules -------------------------------------------------------------------

namespace {
/// Create's `VanillaFluidTargets.canProvideFluidWithoutCapability`: a full beehive / bee nest.
bool vanillaFluidTarget(Block const& b) {
    auto const& id = b.getTypeName();
    if (id == "minecraft:beehive" || id == "minecraft:bee_nest") return stateOf(b, "honey_level") >= 5;
    return false;
}
} // namespace

bool canConnectTo(BlockSource& region, BlockPos const& pos, Face face) {
    auto     o = cp::offsetOf(face);
    BlockPos n{pos.x + o.x, pos.y + o.y, pos.z + o.z};
    if (!region.hasChunksAt(n, 0, false)) return false;
    auto const& block = region.getBlock(n);
    auto        k     = kindOf(block);
    if (k == BlockKind::None) {
        if (common::fluids::find(common::Access{region, n, static_cast<int>(cp::opposite(face))})) return true;
        return vanillaFluidTarget(block);
    }
    if (k == BlockKind::Pipe) {
        if (bracketOf(game::globalPos(region, n)).empty()) return true;
        auto axis = cp::straightAxis(readMask(block));
        return axis && *axis == cp::axisOf(face);
    }
    return cp::has(flowFaces(block), cp::opposite(face));
}

void updateRims(BlockSource& region, BlockPos const& pos) {
    auto const& block = region.getBlock(pos);
    if (kindOf(block) != BlockKind::Pipe) return;
    FaceMask open = readMask(block);
    int      a = 0, b = 0;
    for (Face f : cp::kFaces) {
        if (!cp::has(open, f)) continue;
        auto     o = cp::offsetOf(f);
        BlockPos n{pos.x + o.x, pos.y + o.y, pos.z + o.z};
        bool     rim = true;
        if (region.hasChunksAt(n, 0, false)) {
            auto const& nb = region.getBlock(n);
            // Create's shouldDrawRim: no rim towards a connected fluid pipe
            if (kindOf(nb) == BlockKind::Pipe) rim = !canConnectTo(region, pos, f);
        }
        if (!rim) continue;
        int i = static_cast<int>(f);
        if (i < 3) a |= 1 << i;
        else b |= 1 << (i - 3);
    }
    auto const* next = withStates(block, {{"levitate:rim_a", a}, {"levitate:rim_b", b}});
    if (next != &block) game::setBlock(region, pos, *next);
}

void updatePipeShape(BlockSource& region, BlockPos const& pos, std::optional<Face> changed) {
    if (!region.hasChunksAt(pos, 0, false)) return;
    auto const& block = region.getBlock(pos);
    if (kindOf(block) != BlockKind::Pipe) return;
    FaceMask prev = readMask(block);
    FaceMask next = prev;
    if (bracketOf(game::globalPos(region, pos)).empty()) {
        std::optional<Face> ignored = changed ? std::optional<Face>(cp::opposite(*changed)) : std::nullopt;
        next                        = cp::updatePipeConnections(prev, connectable(region, pos), changed, ignored);
    }
    if (next != prev) {
        if (auto const* b = withConnections(block, next)) game::setBlock(region, pos, *b);
    }
    updateRims(region, pos);
}

// ---- placement ------------------------------------------------------------------------------------

void orientPlaced(BlockSource& region, BlockPos const& pos, PlaceContext const& ctx) {
    if (!region.hasChunksAt(pos, 0, false)) return;
    auto const& block      = region.getBlock(pos);
    Face        look       = cp::nearestFace(ctx.lookX, ctx.lookY, ctx.lookZ);
    Face        horizontal = cp::horizontalFace(ctx.lookX, ctx.lookZ);
    auto        pipes      = connectable(region, pos);
    Block const* next      = nullptr;
    switch (kindOf(block)) {
    case BlockKind::Pipe: {
        FaceMask m = cp::updatePipeConnections(0, pipes, look, std::nullopt);
        next       = withConnections(block, m);
        break;
    }
    case BlockKind::Pump: {
        std::optional<Face> preferred;
        for (Face side : cp::kFaces) {
            if (!shaftFacing(region, pos, side)) continue;
            if (preferred && cp::axisOf(*preferred) != cp::axisOf(side)) {
                preferred.reset();
                break;
            }
            preferred = side;
        }
        Face front = cp::pumpPlacement(look, ctx.sneaking, preferred, pipes);
        next       = withStates(block, {{"levitate:facing", static_cast<int>(front)}});
        break;
    }
    case BlockKind::Valve: {
        std::array<bool, 6> shafts{};
        for (Face f : cp::kFaces) shafts[static_cast<int>(f)] = shaftFacing(region, pos, f);
        auto v = cp::valvePlacement(look, horizontal, ctx.sneaking, shafts, pipes);
        next   = withStates(block, {{"levitate:facing", static_cast<int>(v.facing)}, {"levitate:along_first", v.alongFirst ? 1 : 0}});
        break;
    }
    case BlockKind::Smart: {
        auto s = cp::smartPipePlacement(look, horizontal, ctx.lookX, ctx.lookZ, pipes);
        next   = withStates(block, {{"levitate:face", static_cast<int>(s.face)}, {"levitate:facing", horizontalToState(s.facing)}});
        break;
    }
    default:
        return;
    }
    if (next && next != &block) game::setBlock(region, pos, *next);
    updateRims(region, pos);
}

// ---- brackets -------------------------------------------------------------------------------------

std::string bracketOf(GlobalPos const& pos) {
    auto const* d = bd::get(pos);
    if (!d) return {};
    auto p = d->find("pipes");
    if (p == d->end() || !p->is_object()) return {};
    return p->value("bracket", std::string{});
}

namespace {
void setBracket(BlockSource& region, BlockPos const& pos, std::string const& id) {
    auto gp = game::globalPos(region, pos);
    if (id.empty()) {
        if (auto const* d = bd::get(gp); d && d->contains("pipes")) {
            auto& e = bd::edit(gp);
            e["pipes"].erase("bracket");
            if (e["pipes"].empty()) e.erase("pipes");
            if (e.empty()) bd::erase(gp);
        }
    } else {
        bd::edit(gp)["pipes"]["bracket"] = id;
    }
    auto const& block = region.getBlock(pos);
    int         look  = id.empty() ? 0 : id == kWoodBracket ? 1 : 2;
    auto const* next  = withStates(block, {{"levitate:bracket", look}});
    if (next != &block) game::setBlock(region, pos, *next);
}

/// Create's `tryRemoveBracket` (wrench on a bracketed pipe).
bool removeBracket(Player* player, BlockSource& region, BlockPos const& pos) {
    auto id = bracketOf(game::globalPos(region, pos));
    if (id.empty()) return false;
    setBracket(region, pos, {});
    dropItem(region, pos, id, player);
    log().info("pipes: bracket removed from {} {} {}", pos.x, pos.y, pos.z);
    return true;
}
} // namespace

bool attachBracket(BlockSource& region, BlockPos const& pos, std::string const& bracketId) {
    auto const& block = region.getBlock(pos);
    auto        k     = kindOf(block);
    // Create: every fluid pipe but the encased one may hold a bracket, on a straight section
    if (k != BlockKind::Pipe && k != BlockKind::Glass) return false;
    if (k == BlockKind::Pipe && !cp::straightAxis(readMask(block))) return false;
    auto previous = bracketOf(game::globalPos(region, pos));
    setBracket(region, pos, bracketId);
    if (!previous.empty()) dropItem(region, pos, previous, nullptr);
    log().info("pipes: {} attached to the pipe at {} {} {}", bracketId, pos.x, pos.y, pos.z);
    return true;
}

// ---- wrench and casing ----------------------------------------------------------------------------

bool wrench(Player* player, BlockSource& region, BlockPos const& pos, Face clicked, double hx, double hy, double hz) {
    if (!region.hasChunksAt(pos, 0, false)) return false;
    auto const&  block = region.getBlock(pos);
    auto         k     = kindOf(block);
    Block const* next  = nullptr;
    char const*  what  = "";
    switch (k) {
    case BlockKind::Pipe: {
        if (removeBracket(player, region, pos)) return true;
        auto axis = cp::wrenchGlassAxis(readMask(block), clicked, hx - .5, hy - .5, hz - .5);
        if (!axis) return false;
        next = game::blockWith(kGlassPipe, {{"levitate:axis", static_cast<int>(*axis)}});
        what = "made a glass pipe";
        break;
    }
    case BlockKind::Glass: {
        if (removeBracket(player, region, pos)) return true;
        auto axis = static_cast<Axis>(std::clamp(stateOf(block, "levitate:axis", 1), 0, 2));
        auto m    = cp::regularPipeFromAxis(axis, connectable(region, pos));
        if (auto const* pipe = blockOf(kFluidPipe)) next = withConnections(*pipe, m);
        what = "made a fluid pipe";
        break;
    }
    case BlockKind::Encased: {
        FaceMask            prev = readMask(block);
        std::optional<Face> first;
        for (Face f : cp::kFaces)
            if (cp::has(prev, f) && !first) first = f;
        auto m = cp::updatePipeConnections(prev, connectable(region, pos), first.value_or(Face::Up), std::nullopt);
        if (auto const* pipe = blockOf(kFluidPipe)) next = withConnections(*pipe, m);
        what = "removed the casing";
        break;
    }
    case BlockKind::Pump:
        next = withStates(block, {{"levitate:facing", static_cast<int>(cp::opposite(pumpFront(block)))}});
        what = "reversed the pump";
        break;
    case BlockKind::Valve: {
        auto v = cp::valveWrenched(valveState(block), clicked);
        next   = withStates(block, {{"levitate:facing", static_cast<int>(v.facing)}, {"levitate:along_first", v.alongFirst ? 1 : 0}});
        what   = "rotated the valve";
        break;
    }
    case BlockKind::Smart: {
        if (cp::axisOf(clicked) != Axis::Y) return false;
        auto s = smartState(block);
        next   = withStates(block, {{"levitate:facing", horizontalToState(cp::clockwise(s.facing))}});
        what   = "rotated the smart pipe";
        break;
    }
    default:
        return false;
    }
    if (!next || next == &block) return false;
    game::setBlock(region, pos, *next);
    updateRims(region, pos);
    playSound(region, pos, "random.click", .6f, 1.4f);
    log().info("pipes: {} wrenched {} {} {}: {}", player ? player->getRealName() : std::string("-"), pos.x, pos.y, pos.z, what);
    return true;
}

bool encase(BlockSource& region, BlockPos const& pos) {
    auto const& block = region.getBlock(pos);
    FaceMask    open  = 0;
    switch (kindOf(block)) {
    case BlockKind::Pipe:
        open = readMask(block);
        break;
    case BlockKind::Glass:
        open = cp::axisMask(static_cast<Axis>(std::clamp(stateOf(block, "levitate:axis", 1), 0, 2)));
        break;
    default:
        return false;
    }
    auto const* encased = blockOf(kEncasedPipe);
    if (!encased) return false;
    game::setBlock(region, pos, *withConnections(*encased, open));
    playSound(region, pos, "dig.copper", 1.f, 1.f);
    log().info("pipes: pipe at {} {} {} encased", pos.x, pos.y, pos.z);
    return true;
}

// ---- helpers ----------------------------------------------------------------------------------------

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

BlockPos toBlockPos(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }

} // namespace lv::feature::pipes
