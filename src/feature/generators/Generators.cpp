// Generators (U7): server-side behaviour of Create 6.0.10's hand crank, valve handles, water
// wheels, large water wheel structure and creative motor (see docs/SPEC.md §3.6.7).

#include "feature/generators/Generators.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
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
#include "mc/world/level/block/BubbleColumnBlock.h"
#include "mc/world/level/block/LiquidBlockBase.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/material/Material.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("generators", lv::feature::generators::Config);

namespace lv::feature::generators {

namespace k = core::kinetics;
using Json  = nlohmann::json;

namespace {

constexpr char kDbWheels[] = "lv:generators:wheels";
constexpr char kDbActive[] = "lv:generators:active";

/// Turning state of a hand crank / valve handle (Create's HandCrankBlockEntity fields).
struct Handle {
    int  inUse     = 0;
    bool backwards = false;
    int  cooldown  = 0; ///< valve handles
};

/// A water wheel the unit re-checks (Create's lazy tick).
struct Wheel {
    std::optional<int> score; ///< last applied flow score (nullopt: not applied since load)
};

struct State {
    std::map<GlobalPos, Handle>                 handles;
    std::map<GlobalPos, Wheel>                  wheels;
    std::map<GlobalPos, std::vector<GlobalPos>> watch;    ///< checked position -> wheels
    std::map<GlobalPos, std::vector<GlobalPos>> watching; ///< wheel -> its checked positions
    std::set<GlobalPos>                         dirtyWheels;
    std::set<GlobalPos>                         refresh; ///< generators whose speed must be re-applied
    std::vector<GlobalPos>                      build;   ///< large wheels that need their structure
    std::vector<std::pair<GlobalPos, Axis>>     clear;   ///< removed large wheels
    std::vector<std::pair<GlobalPos, Block const*>> structureGone; ///< removed structure blocks (old block)
    std::set<GlobalPos>                         woodSync;
    std::set<GlobalPos>                         extensionCheck;
    std::set<GlobalPos>                         creativeBreaks;
    std::vector<GlobalPos>                      stopOnLoad;
    std::uint64_t                               tick = 0;
    std::unordered_map<BlockType const*, Kind>  kinds;
};

State& state() {
    static State s;
    return s;
}

Config const& config() { return config::get<Config>(); }

BlockPos bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
using k::shifted;
std::string posText(GlobalPos const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }
std::optional<GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

void playSound(GlobalPos const& pos, std::string const& sound, float volume, float pitch) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlaySound);
    if (!packet) return;
    auto& pkt   = static_cast<PlaySoundPacket&>(*packet);
    pkt.mName   = sound;
    pkt.mPos    = BlockPos{pos.x * 8 + 4, pos.y * 8 + 4, pos.z * 8 + 4};
    pkt.mVolume = volume;
    pkt.mPitch  = pitch;
    pkt.sendTo(bp(pos), DimensionType(pos.dim));
}

void actionBar(Player& player, std::string const& text) {
    auto                           pkt = TextPacket{};
    TextPacketPayload::MessageOnly message;
    message.mType = TextPacketType::Tip;
    message.mMessage->assign(text);
    pkt.mBody = message;
    pkt.sendTo(player);
}

/// Create's AllSoundEvents.CRANKING (wood place + wooden button click, played every 10 ticks).
void crankSound(GlobalPos const& pos, int inUse) {
    float volume = static_cast<float>(inUse) / 2.5f;
    float pitch  = .65f + static_cast<float>(10 - inUse) / 10.f;
    playSound(pos, "use.wood", .075f * volume, .5f * pitch);
    playSound(pos, "click_off.wooden_button", .025f * volume, .5f * pitch);
}

// ---- wheels index -------------------------------------------------------------------------------------------

std::optional<std::pair<Axis, bool>> wheelShape(Block const& block) {
    switch (kindOf(block)) {
    case Kind::WaterWheel:
        return std::pair{k::axisOf(facingOf(block)), false};
    case Kind::LargeWheel:
        return std::pair{axisStateOf(block), true};
    default:
        return std::nullopt;
    }
}

void unwatch(GlobalPos const& wheel) {
    auto& s  = state();
    auto  it = s.watching.find(wheel);
    if (it == s.watching.end()) return;
    for (auto const& cell : it->second) {
        auto w = s.watch.find(cell);
        if (w == s.watch.end()) continue;
        std::erase_if(w->second, [&](GlobalPos const& p) { return p == wheel; });
        if (w->second.empty()) s.watch.erase(w);
    }
    s.watching.erase(it);
}

void watchWheel(GlobalPos const& wheel, Block const& block) {
    auto shape = wheelShape(block);
    if (!shape) return;
    unwatch(wheel);
    auto& s     = state();
    auto& cells = s.watching[wheel];
    for (auto const& o : cg::wheelOffsets(shape->first, shape->second)) cells.push_back(shifted(wheel, o));
    cells.push_back(wheel);
    for (auto const& c : cells) s.watch[c].push_back(wheel);
}

void trackWheel(GlobalPos const& pos, Block const& block, bool shapeChanged) {
    auto& s = state();
    s.wheels.try_emplace(pos);
    if (shapeChanged || !s.watching.contains(pos)) watchWheel(pos, block);
    s.dirtyWheels.insert(pos);
}

void untrackWheel(GlobalPos const& pos) {
    auto& s = state();
    if (s.wheels.erase(pos)) unwatch(pos);
    s.dirtyWheels.erase(pos);
}

// ---- handles ------------------------------------------------------------------------------------------------------

bool clockwiseOf(GlobalPos const& pos, Kind kind, Handle const& h) {
    if (kind == Kind::Valve) return (valveAngle(pos) < 0) != h.backwards; // ValveHandleBlockEntity.clockwise
    return h.backwards;
}

void applyHandleSpeed(GlobalPos const& pos, Block const& block, Handle const& h) {
    auto kind = kindOf(block);
    kinetics::setGeneratedSpeed(pos, cg::crankSpeed(h.inUse > 0, clockwiseOf(pos, kind, h), facingOf(block)));
}

/// Tells nearby companions to animate a valve handle turn (client/generators).
void sendValveTurn(GlobalPos const& pos, cg::ValveTurn const& t, bool backwards, Face facing) {
    auto level = ll::service::getLevel();
    if (!level) return;
    Json msg{
        {"pos",       posJson(pos)            },
        {"target",    t.targetAngle            },
        {"ticks",     t.useTicks               },
        {"backwards", backwards                },
        {"facing",    static_cast<int>(facing) },
    };
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) != pos.dim || !net::isCompanion(p)) return true;
        auto const& at = p.getPosition();
        double      dx = at.x - pos.x, dz = at.z - pos.z;
        if (dx * dx + dz * dz <= 192.0 * 192.0) net::send(p, "generators.valve", msg);
        return true;
    });
}

// ---- large water wheel structure ----------------------------------------------------------------------------

Block const* structureBlock(int segment, Axis axis, int wood) {
    return game::blockWith(kStructure, {{kSegment, segment}, {kAxisState, static_cast<int>(axis)}, {kWood, wood}});
}

void destroy(GlobalPos const& pos, bool drop) {
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(pos.dim);
    if (!level || !region || !region->hasChunksAt(bp(pos), 0, false)) return;
    BlockChangeContext ctx{};
    level->destroyBlock(*region, bp(pos), drop, ctx);
}

/// Create's LargeWaterWheelBlock.tick: fills the 8 cells around the wheel with structure blocks;
/// a cell taken by something solid breaks the wheel.
void buildStructure(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    auto* block  = blockAt(pos);
    if (!region || !block || kindOf(*block) != Kind::LargeWheel) return;
    Axis axis = axisStateOf(*block);
    int  wood = stateOf(*block, kWood, cg::kDefaultWood);
    for (auto const& cell : cg::structureCells(axis)) {
        auto  at       = shifted(pos, cell.offset);
        auto* occupied = blockAt(at);
        if (!occupied) continue;
        if (kindOf(*occupied) == Kind::Structure && stateOf(*occupied, kSegment) == cell.segment
            && static_cast<int>(axisStateOf(*occupied)) == static_cast<int>(axis)) {
            if (stateOf(*occupied, kWood) != wood) setStates(at, {{kWood, wood}});
            continue;
        }
        if (!game::replaceable(*region, bp(at))) {
            log().info("large water wheel at {} broke: its cell {} is blocked", posText(pos), posText(at));
            destroy(pos, false);
            return;
        }
        if (auto const* req = structureBlock(cell.segment, axis, wood)) game::setBlock(*region, bp(at), *req);
    }
}

/// The large wheel a structure block belongs to (its position from the segment and axis).
GlobalPos masterOf(GlobalPos const& structure, Block const& block) {
    auto o = cg::segmentOffset(stateOf(block, kSegment), axisStateOf(block));
    return shifted(structure, {-o.x, -o.y, -o.z});
}

/// Create's WaterWheelStructuralBlock: losing a structure block breaks the wheel (with its drop,
/// unless a creative player broke it).
void structureRemoved(GlobalPos const& pos, Block const& old) {
    auto  master = masterOf(pos, old);
    auto* block  = blockAt(master);
    if (!block || kindOf(*block) != Kind::LargeWheel || axisStateOf(*block) != axisStateOf(old)) return;
    bool creative = state().creativeBreaks.contains(pos);
    log().info("large water wheel at {} broke with its structure at {}", posText(master), posText(pos));
    destroy(master, !creative);
}

/// The wheel is gone: its structure blocks become air (Create: `stillValid` fails).
void clearStructure(GlobalPos const& pos, Axis axis) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    for (auto const& cell : cg::structureCells(axis)) {
        auto  at = shifted(pos, cell.offset);
        auto* b  = blockAt(at);
        if (!b || kindOf(*b) != Kind::Structure || stateOf(*b, kSegment) != cell.segment || axisStateOf(*b) != axis) continue;
        game::clearBlock(*region, bp(at));
    }
}

/// LargeWaterWheelBlock.EXTENSION: set when the block behind (negative axis side) is a large wheel.
void updateExtension(GlobalPos const& pos) {
    auto* block = blockAt(pos);
    if (!block || kindOf(*block) != Kind::LargeWheel) return;
    Axis  axis   = axisStateOf(*block);
    auto  behind = shifted(pos, k::offsetOf(k::faceOf(axis, false)));
    auto* b      = blockAt(behind);
    int   ext    = b && kindOf(*b) == Kind::LargeWheel ? 1 : 0;
    if (stateOf(*block, kExtension) != ext) setStates(pos, {{kExtension, ext}});
}

bool neighbourShaftTowards(GlobalPos const& pos, Face side) {
    auto* nb = blockAt(shifted(pos, k::offsetOf(side)));
    if (!nb) return false;
    auto const* spec = kinetics::specOf(nb->getTypeName());
    if (!spec) return false;
    k::BlockState s;
    s.id = nb->getTypeName();
    for (auto const& name : spec->states) s.set(name, stateOf(*nb, name.c_str()));
    return spec->shaftTowards(s, k::opposite(side));
}

Face lookOf(Player const& player) {
    Vec3 v = player.getViewVector();
    return cg::nearestLooking(v.x, v.y, v.z);
}

// ---- persistence --------------------------------------------------------------------------------------------------

void saveWorld() {
    if (!game::db::available()) return;
    Json wheels = Json::array();
    for (auto const& [p, w] : state().wheels) wheels.push_back(posJson(p));
    game::db::save(kDbWheels, wheels);
    Json active = Json::array();
    for (auto const& [p, h] : state().handles)
        if (h.inUse > 0) active.push_back(posJson(p));
    game::db::save(kDbActive, active);
}

void loadWorld() {
    auto& s = state();
    s     = State{};
    if (!game::db::available()) return;
    if (auto j = game::db::load(kDbWheels); j && j->is_array()) {
        for (auto const& e : *j)
            if (auto p = posFrom(e)) {
                s.wheels.try_emplace(*p);
                s.dirtyWheels.insert(*p);
            }
    }
    // cranks that were turning when the world was saved: their kinetic speed was saved with them
    if (auto j = game::db::load(kDbActive); j && j->is_array()) {
        for (auto const& e : *j)
            if (auto p = posFrom(e)) s.stopOnLoad.push_back(*p);
    }
    log().info("generators: {} water wheels tracked", s.wheels.size());
}

// ---- events -----------------------------------------------------------------------------------------------------------

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    auto& s   = state();
    auto  pos = game::globalPos(region, ev.pos());
    // a change next to a wheel: re-check its flow (Create: neighbour update / lazy tick)
    if (auto it = s.watch.find(pos); it != s.watch.end())
        for (auto const& w : it->second) s.dirtyWheels.insert(w);
    if (ev.layer() != 0) return;
    auto const& now  = ev.newBlock();
    auto const& prev = ev.previousBlock();
    Kind        kn = kindOf(now), kp = kindOf(prev);
    if (kn == Kind::None && kp == Kind::None) return;

    bool sameType = &now.getBlockType() == &prev.getBlockType();
    if (kn == Kind::WaterWheel || kn == Kind::LargeWheel) trackWheel(pos, now, !sameType || wheelShape(now) != wheelShape(prev));
    else if (kp == Kind::WaterWheel || kp == Kind::LargeWheel) untrackWheel(pos);

    if (kn == Kind::Crank || kn == Kind::Valve || kn == Kind::Motor) s.refresh.insert(pos);
    if ((kp == Kind::Crank || kp == Kind::Valve) && !sameType) s.handles.erase(pos);

    if (kn == Kind::LargeWheel) {
        if (!sameType || axisStateOf(now) != axisStateOf(prev)) s.build.push_back(pos);
        else if (stateOf(now, kWood) != stateOf(prev, kWood)) s.woodSync.insert(pos);
    }
    if (kp == Kind::LargeWheel && (!sameType || axisStateOf(now) != axisStateOf(prev))) s.clear.emplace_back(pos, axisStateOf(prev));
    if (kp == Kind::LargeWheel || kn == Kind::LargeWheel) {
        Axis axis = kn == Kind::LargeWheel ? axisStateOf(now) : axisStateOf(prev);
        s.extensionCheck.insert(pos);
        s.extensionCheck.insert(shifted(pos, k::offsetOf(k::faceOf(axis, true))));
    }
    if (kp == Kind::Structure && kn != Kind::Structure) s.structureGone.emplace_back(pos, &prev);
}

void onTick() {
    auto& s = state();
    ++s.tick;

    // cranks that were turning when the world was saved
    if (!s.stopOnLoad.empty()) {
        for (auto const& p : std::exchange(s.stopOnLoad, {}))
            if (!s.handles.contains(p)) kinetics::setGeneratedSpeed(p, 0);
    }

    // structure blocks, extensions, wood
    for (auto const& [p, old] : std::exchange(s.structureGone, {})) structureRemoved(p, *old);
    for (auto const& [p, axis] : std::exchange(s.clear, {})) clearStructure(p, axis);
    for (auto const& p : std::exchange(s.build, {})) buildStructure(p);
    for (auto const& p : std::exchange(s.woodSync, {})) buildStructure(p);
    for (auto const& p : std::exchange(s.extensionCheck, {})) updateExtension(p);
    s.creativeBreaks.clear();

    // generators whose block changed (placed, turned, moved by a contraption)
    for (auto const& p : std::exchange(s.refresh, {})) {
        auto* b = blockAt(p);
        if (!b) continue;
        switch (kindOf(*b)) {
        case Kind::Motor:
            kinetics::setGeneratedSpeed(p, cg::convertToDirection(static_cast<float>(motorSpeed(p)), facingOf(*b)));
            break;
        case Kind::Crank:
        case Kind::Valve: {
            auto it = s.handles.find(p);
            applyHandleSpeed(p, *b, it == s.handles.end() ? Handle{} : it->second);
            break;
        }
        default:
            break;
        }
    }

    // hand cranks / valve handles (HandCrankBlockEntity.tick, ValveHandleBlockEntity.tick)
    for (auto it = s.handles.begin(); it != s.handles.end();) {
        auto& [p, h] = *it;
        auto* b      = blockAt(p);
        if (b && kindOf(*b) != Kind::Crank && kindOf(*b) != Kind::Valve) {
            kinetics::setSequenceContext(p, std::nullopt);
            it = s.handles.erase(it);
            continue;
        }
        if (h.inUse > 0) {
            if (b && kindOf(*b) == Kind::Crank && s.tick % cg::kCrankSoundPeriod == 0) crankSound(p, h.inUse);
            if (--h.inUse == 0) {
                if (b) applyHandleSpeed(p, *b, h);
                else kinetics::setGeneratedSpeed(p, 0);
                if (!b || kindOf(*b) == Kind::Valve) kinetics::setSequenceContext(p, std::nullopt);
            }
        } else if (h.cooldown > 0) {
            --h.cooldown;
        }
        it = h.inUse == 0 && h.cooldown == 0 ? s.handles.erase(it) : std::next(it);
    }

    // water wheels: the changed ones now, every wheel every 60 ticks (staggered)
    for (auto const& [p, w] : s.wheels) {
        auto h = static_cast<std::uint64_t>(std::abs(p.x * 31 + p.z * 17 + p.y * 7));
        if ((s.tick + h) % cg::kWheelLazyTicks == 0) s.dirtyWheels.insert(p);
    }
    for (auto const& p : std::exchange(s.dirtyWheels, {})) evaluateWheel(p);
}

} // namespace

// ---- ids and world access ----------------------------------------------------------------------------------------------

std::string valveId(int colour) {
    if (colour < 0 || colour > 15) return kCopperValve;
    return "levitate:" + std::string(cg::kDyeColours[static_cast<std::size_t>(colour)]) + "_valve_handle";
}

int valveColour(std::string_view id) {
    if (id == kCopperValve) return -1;
    constexpr std::string_view prefix = "levitate:";
    constexpr std::string_view suffix = "_valve_handle";
    if (!id.starts_with(prefix) || !id.ends_with(suffix)) return -2;
    auto colour = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    for (std::size_t i = 0; i < cg::kDyeColours.size(); ++i)
        if (cg::kDyeColours[i] == colour) return static_cast<int>(i);
    return -2;
}

Kind kindOf(std::string_view id) {
    if (!id.starts_with("levitate:")) return Kind::None;
    if (id == kHandCrank) return Kind::Crank;
    if (id == kWaterWheel) return Kind::WaterWheel;
    if (id == kLargeWheel) return Kind::LargeWheel;
    if (id == kStructure) return Kind::Structure;
    if (id == kMotor) return Kind::Motor;
    if (valveColour(id) != -2) return Kind::Valve;
    return Kind::None;
}

Kind kindOf(Block const& block) {
    auto& kinds = state().kinds;
    auto* type  = &block.getBlockType();
    if (auto it = kinds.find(type); it != kinds.end()) return it->second;
    return kinds[type] = kindOf(block.getTypeName());
}

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return nullptr;
    return &dimension->getBlockSourceFromMainChunkSource();
}

Block const* blockAt(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region || !region->hasChunksAt(bp(pos), 0, false)) return nullptr;
    return &region->getBlock(bp(pos));
}

int stateOf(Block const& block, char const* name, int fallback) { return game::intState(block, name).value_or(fallback); }

Face facingOf(Block const& block) {
    int v = stateOf(block, kFacing, 1);
    return static_cast<Face>(v >= 0 && v <= 5 ? v : 1);
}

Axis axisStateOf(Block const& block) { return static_cast<Axis>(std::clamp(stateOf(block, kAxisState, 1), 0, 2)); }

bool setStates(GlobalPos const& pos, std::initializer_list<std::pair<char const*, int>> states) {
    auto* region = regionOf(pos.dim);
    auto* block  = blockAt(pos);
    if (!region || !block) return false;
    Block const* b = block;
    for (auto const& [name, value] : states) {
        auto const* st = b->getBlockType().getBlockState(HashedString(name));
        if (!st) return false;
        auto next = b->setState(*st, value);
        if (!next) return false;
        b = next.as_ptr();
    }
    if (b == block) return true;
    return game::setBlock(*region, bp(pos), *b);
}

// ---- actions --------------------------------------------------------------------------------------------------------------

bool useCrank(Player& player, GlobalPos const& pos, bool backwards, bool costHunger) {
    auto* b = blockAt(pos);
    if (!b || kindOf(*b) != Kind::Crank) return false;
    auto& h      = state().handles[pos];
    bool  update = h.inUse == 0 || backwards != h.backwards; // HandCrankBlockEntity.turn
    bool  start  = h.inUse == 0;
    h.inUse      = cg::kCrankUseTicks;
    h.backwards  = backwards;
    if (update) applyHandleSpeed(pos, *b, h);
    if (costHunger && !player.isCreative()) player.causeFoodExhaustion(static_cast<float>(cg::kCrankSpeed) * config().crankHungerMultiplier);
    if (start) log().info("hand crank at {} turned by {}{}", posText(pos), player.getRealName(), backwards ? " (backwards)" : "");
    return true;
}

bool useValve(Player& player, GlobalPos const& pos, std::string const& heldId, bool sneaking) {
    auto* b = blockAt(pos);
    if (!b || kindOf(*b) != Kind::Valve) return false;
    int colour = cg::dyeColourOf(heldId);
    if (colour >= 0 && colour != valveColour(b->getTypeName())) {
        // ValveHandleBlock.clicked: BlockHelper.copyProperties onto the dyed handle (the dye stays)
        auto* region = regionOf(pos.dim);
        auto* dyed   = game::blockWith(valveId(colour), {{kFacing, static_cast<int>(facingOf(*b))}});
        if (!region || !dyed) return false;
        game::setBlock(*region, bp(pos), *dyed);
        log().info("valve handle at {} dyed {} by {}", posText(pos), cg::kDyeColours[static_cast<std::size_t>(colour)], player.getRealName());
        return true;
    }
    // ValveHandleBlockEntity.activate
    auto& handles = state().handles;
    if (kinetics::theoreticalSpeedAt(pos) != 0) return true;
    if (auto it = handles.find(pos); it != handles.end() && (it->second.inUse > 0 || it->second.cooldown > 0)) return true;
    int  angle = valveAngle(pos);
    auto turn  = cg::valveTurn(angle, sneaking);
    auto& h    = handles[pos];
    h.inUse    = turn.useTicks;
    h.backwards = sneaking;
    // Create's sequenceContext of the valve: bearings / turntables it drives stop after exactly the
    // set angle (scaled by the speed ratio)
    kinetics::setSequenceContext(pos, kinetics::SequenceContext{kinetics::SequenceContext::Kind::TurnAngle, static_cast<double>(std::abs(angle))});
    applyHandleSpeed(pos, *b, h);
    h.cooldown = cg::kValveCooldown;
    sendValveTurn(pos, turn, sneaking, facingOf(*b));
    log().info("valve handle at {} turned {} degrees by {}", posText(pos), turn.clockwise ? -std::abs(angle) : std::abs(angle), player.getRealName());
    return true;
}

bool applyMaterial(GlobalPos const& pos, std::string const& heldId) {
    int wood = cg::woodOfPlanks(heldId);
    if (wood < 0) return false;
    auto* b = blockAt(pos);
    if (!b) return false;
    GlobalPos wheel = pos;
    if (kindOf(*b) == Kind::Structure) {
        wheel = masterOf(pos, *b);
        b     = blockAt(wheel);
        if (!b) return false;
    }
    if (kindOf(*b) != Kind::WaterWheel && kindOf(*b) != Kind::LargeWheel) return false;
    if (stateOf(*b, kWood, cg::kDefaultWood) == wood) return false;
    if (!setStates(wheel, {{kWood, wood}})) return false;
    // level event 2001: the planks' break particles and sound
    if (auto level = ll::service::getLevel()) {
        if (auto const* planks = game::blockWith(heldId)) {
            Vec3 at{static_cast<float>(wheel.x) + .5f, static_cast<float>(wheel.y) + .5f, static_cast<float>(wheel.z) + .5f};
            level->broadcastLevelEvent(SharedTypes::Legacy::LevelEvent::ParticlesDestroyBlock, at, static_cast<int>(planks->mNetworkId), nullptr);
        }
    }
    log().info("water wheel at {} now uses {} wood", posText(wheel), cg::kWoods[static_cast<std::size_t>(wood)]);
    return true;
}

void orientPlaced(Player& player, GlobalPos const& pos, Face clickedFace) {
    auto* b = blockAt(pos);
    if (!b) return;
    auto kind = kindOf(*b);
    if (kind != Kind::Crank && kind != Kind::Valve && kind != Kind::WaterWheel && kind != Kind::Motor) return;
    bool sneaking  = player.isSneaking();
    auto preferred = cg::preferredFacing([&](Face side) { return neighbourShaftTowards(pos, side); });
    Face facing;
    switch (kind) {
    case Kind::Crank:
    case Kind::Valve:
        facing = cg::crankFacing(preferred, clickedFace, sneaking);
        break;
    case Kind::WaterWheel:
        facing = cg::directionalFacing(preferred, lookOf(player), sneaking);
        break;
    default:
        facing = cg::motorFacing(preferred, lookOf(player), sneaking);
        break;
    }
    setStates(pos, {{kFacing, static_cast<int>(facing)}});
    log().info("{} placed at {} facing {} by {}", b->getTypeName(), posText(pos), k::faceName(facing), player.getRealName());
}

bool placeLargeWheel(Player& player, ItemStack& held, GlobalPos const& placement, Face clickedFace) {
    auto* region = regionOf(placement.dim);
    if (!region) return false;
    bool sneaking = player.isSneaking();
    Face look     = lookOf(player);
    auto axisAt   = [&](GlobalPos const& p) {
        auto pref = cg::preferredAxis([&](Face side) { return neighbourShaftTowards(p, side); });
        return cg::pillarAxis(pref, clickedFace, look, sneaking);
    };
    // LargeWaterWheelBlock.getStateForPlacement: the wheel and its 8 cells must be free
    auto fits = [&](GlobalPos const& p, Axis axis) {
        if (!game::replaceable(*region, bp(p))) return false;
        for (auto const& cell : cg::structureCells(axis))
            if (!game::replaceable(*region, bp(shifted(p, cell.offset)))) return false;
        return true;
    };
    GlobalPos at   = placement;
    Axis      axis = axisAt(at);
    if (!fits(at, axis)) {
        // LargeWaterWheelBlockItem.place: one block further along the clicked face
        if (k::axisOf(clickedFace) == axis) {
            actionBar(player, "§c" + trFor(player, "create.large_water_wheel.not_enough_space", "Clear Blocks for Placement"));
            log().info("large water wheel at {}: not enough space", posText(at));
            return false;
        }
        at   = shifted(placement, k::offsetOf(clickedFace));
        axis = axisAt(at);
        if (!fits(at, axis)) {
            actionBar(player, "§c" + trFor(player, "create.large_water_wheel.not_enough_space", "Clear Blocks for Placement"));
            log().info("large water wheel at {}: not enough space", posText(placement));
            return false;
        }
    }
    // BlockItem.place: nobody in the way, and other mods (land claims) may refuse it
    AABB box{Vec3{static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(at.z)}, Vec3{at.x + 1.f, at.y + 1.f, at.z + 1.f}};
    if (!region->isUnobstructedByEntities(box, nullptr)) return false;
    uchar                               face = static_cast<uchar>(clickedFace);
    ll::event::PlayerPlacingBlockEvent placing(player, bp(at), face);
    ll::event::EventBus::getInstance().publish(placing);
    if (placing.isCancelled()) return false;
    auto const* block = game::blockWith(kLargeWheel, {{kAxisState, static_cast<int>(axis)}, {kWood, cg::kDefaultWood}});
    if (!block || !game::setBlock(*region, bp(at), *block)) return false;
    ll::event::PlayerPlacedBlockEvent placed(player, bp(at), *block);
    ll::event::EventBus::getInstance().publish(placed);
    if (!player.isCreative()) {
        auto rest = common::items::withCount(held, common::items::countOf(held) - 1);
        if (&held == &player.getOffhandSlot()) player.setOffhandSlot(rest);
        else player.setSelectedItem(rest);
    }
    player.sendInventory(false);
    playSound(at, "use.wood", 1.f, .8f);
    log().info("large water wheel placed at {} (axis {}) by {}", posText(at), k::axisName(axis), player.getRealName());
    return true;
}

void noteCreativeBreak(GlobalPos const& pos) { state().creativeBreaks.insert(pos); }

// ---- value settings ------------------------------------------------------------------------------------------------------

int motorSpeed(GlobalPos const& pos) {
    if (auto const* d = common::blockdata::get(pos)) {
        auto g = d->find("generators");
        if (g != d->end() && g->is_object()) return std::clamp(g->value("speed", cg::kMotorDefaultSpeed), -cg::kMotorMaxSpeed, cg::kMotorMaxSpeed);
    }
    return cg::kMotorDefaultSpeed;
}

void setMotorSpeed(GlobalPos const& pos, int rpm) {
    rpm = std::clamp(rpm, -cg::kMotorMaxSpeed, cg::kMotorMaxSpeed);
    auto& d = common::blockdata::edit(pos);
    if (!d["generators"].is_object()) d["generators"] = Json::object();
    d["generators"]["speed"] = rpm;
    if (auto* b = blockAt(pos); b && kindOf(*b) == Kind::Motor) {
        kinetics::setGeneratedSpeed(pos, cg::convertToDirection(static_cast<float>(rpm), facingOf(*b)));
    }
}

int valveAngle(GlobalPos const& pos) {
    if (auto const* d = common::blockdata::get(pos)) {
        auto g = d->find("generators");
        if (g != d->end() && g->is_object()) {
            int v = std::clamp(g->value("angle", cg::kValveDefaultAngle), -cg::kValveMaxAngle, cg::kValveMaxAngle);
            return v == 0 ? cg::kValveDefaultAngle : v;
        }
    }
    return cg::kValveDefaultAngle;
}

void setValveAngle(GlobalPos const& pos, int degrees) {
    degrees = std::clamp(degrees, -cg::kValveMaxAngle, cg::kValveMaxAngle);
    if (degrees == 0) degrees = 1;
    auto& d = common::blockdata::edit(pos);
    if (!d["generators"].is_object()) d["generators"] = Json::object();
    d["generators"]["angle"] = degrees;
}

// ---- wheels ----------------------------------------------------------------------------------------------------------------

cg::Vec3d flowAt(BlockSource& region, GlobalPos const& pos) {
    auto p = bp(pos);
    if (!region.hasChunksAt(p, 0, false)) return {};
    auto const& block = region.getBlock(p);
    if (block.getTypeName() == "minecraft:bubble_column") return {0, BubbleColumnBlock::shouldDragDown(block) ? -1.0 : 1.0, 0};
    Block const* liquid = &block;
    if (!block.getMaterial().mLiquid) {
        liquid = &region.getExtraBlock(p);
        if (!liquid->getMaterial().mLiquid) return {};
    }
    Vec3 v = LiquidBlockBase::_getFlow(region, p, liquid->getMaterial());
    return {v.x, v.y, v.z};
}

void evaluateWheel(GlobalPos const& pos) {
    auto& s  = state();
    auto  it = s.wheels.find(pos);
    if (it == s.wheels.end()) return;
    auto* region = regionOf(pos.dim);
    if (!region || !region->hasChunksAt(bp(pos), 0, false)) return;
    auto const& block = region->getBlock(bp(pos));
    auto        shape = wheelShape(block);
    if (!shape) {
        untrackWheel(pos);
        return;
    }
    if (!s.watching.contains(pos)) watchWheel(pos, block);
    int score = cg::flowScore(shape->first, shape->second, [&](k::Offset const& o) { return flowAt(*region, shifted(pos, o)); });
    if (it->second.score == score) return;
    bool first        = !it->second.score.has_value();
    it->second.score  = score;
    float speed       = cg::wheelSpeed(score, shape->second);
    kinetics::setGeneratedSpeed(pos, speed);
    if (!first || speed != 0) {
        log().info("{} at {}: flow score {}, {} RPM", shape->second ? "large water wheel" : "water wheel", posText(pos), score, speed);
    }
}

int crankInUse(GlobalPos const& pos) {
    auto it = state().handles.find(pos);
    return it == state().handles.end() ? 0 : it->second.inUse;
}

int wheelFlowScore(GlobalPos const& pos) {
    auto it = state().wheels.find(pos);
    return it == state().wheels.end() ? 0 : it->second.score.value_or(0);
}

bool tracksWheel(GlobalPos const& pos) { return state().wheels.contains(pos); }

// ---- module -------------------------------------------------------------------------------------------------------------------

class GeneratorsModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) { onBlockChanged(ev); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { onTick(); }));
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldReady([] { loadWorld(); });
            game::db::onWorldSave([] { saveWorld(); });
            game::db::onWorldClosed([] { state() = State{}; });
        }
        detail::registerValueBoxes();
        detail::installHooks();
        if (game::db::available()) loadWorld(); // BDS: the world exists before mods are enabled
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        detail::uninstallHooks();
        saveWorld();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::feature::generators

LV_REGISTER_MODULE(lv::feature::generators::GeneratorsModule);
