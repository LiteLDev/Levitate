// Peculiar and haunted bells (U49, SPEC §3.6.31): Create's PeculiarBellBlock, HauntedBellBlock(Entity),
// AbstractBellBlock(Entity), HauntedBellPulser, SoulPulseEffect(Handler), BellMovementBehaviour and
// HauntedBellMovementBehaviour, on top of the vanilla BellBlock rules (core/equipment/Bells.h).
//
// Custom blocks have no block entities: the ringing state lives in memory per position (it only
// lasts 74 ticks); the bells' positions are kept in the world DB ("lv:equipment:bells") so their
// redstone input is watched after a reload. Soul pulses run on the server (Create runs them on each
// client) and mark the dark spawnable spots with the resource pack's soul particles, so vanilla
// clients see them too.

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockSupportType.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/BrightnessPair.h"
#include "mc/world/phys/AABB.h"

#include "core/equipment/Bells.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/contraption/Contraption.h"
#include "feature/equipment/Equipment.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;
namespace ia    = common::interactions;
namespace rs    = common::redstone;
namespace con   = contraption;
namespace cc    = core::contraption;

namespace {

constexpr char kDbKey[]        = "lv:equipment:bells";
constexpr char kPeculiarSound[] = "block.bell.hit";          // AllSoundEvents.PECULIAR_BELL_USE plays the vanilla bell
constexpr char kHauntedSound[]  = "levitate.haunted_bell.use";
constexpr char kConvertSound[]  = "levitate.haunted_bell.convert";

struct Ringing {
    bool ringing      = false;
    int  ringingTicks = 0;
    int  direction    = 3;
    int  effectTicks  = 0;
};

struct Held {
    int           ticks = 0;    ///< HauntedBellPulser's warmup counter (ticks held, 1 = first)
    std::uint64_t last  = 0;    ///< last tick it was held (the cache entry expires after 250 ms)
};

struct State {
    std::set<GlobalPos>                         index;     ///< every bell (persisted)
    bool                                        dirty   = false;
    bool                                        loaded  = false;
    std::map<GlobalPos, rs::WatchId>            watches;
    std::map<GlobalPos, Ringing>                rings;
    std::map<int, ce::SoulPulses>               pulses;    ///< per dimension
    std::unordered_map<std::int64_t, Held>      held;
    std::map<std::pair<std::int64_t, GlobalPos>, std::uint64_t> lastClick;
    std::vector<GlobalPos>                      marks;     ///< selftests
    std::uint64_t                               tick = 0;
};
State& st() {
    static State s;
    return s;
}


ce::BellState stateOf(Block const& b) {
    ce::BellState s;
    s.attach = static_cast<ce::BellAttach>(std::clamp(game::intState(b, kBellAttachment).value_or(0), 0, 3));
    int f    = game::intState(b, kFacing).value_or(3);
    s.facing = ce::horizontalFace(f) ? f : 3;
    return s;
}

GlobalPos neighbour(GlobalPos const& p, int dir) {
    auto o = ce::faceOffset(dir);
    return {p.dim, p.x + o[0], p.y + o[1], p.z + o[2]};
}

ce::BellSupport supportAt(GlobalPos const& pos) {
    ce::BellSupport s;
    s.sturdy = [pos](int dir) {
        auto const* b = blockAt(neighbour(pos, dir));
        return b && !b->isAir() && b->canProvideSupport(static_cast<uchar>(ce::oppositeFace(dir)), BlockSupportType::Center);
    };
    s.centreAbove = [pos] {
        auto const* b = blockAt(neighbour(pos, 1));
        return b && !b->isAir() && b->canProvideSupport(0, BlockSupportType::Center);
    };
    return s;
}

/// Java 2D facing (0 S, 1 W, 2 N, 3 E) -> FacingID.
int faceOf2d(int f) {
    static constexpr int map[4] = {3, 4, 2, 5};
    return map[f & 3];
}

bool setBell(GlobalPos const& pos, std::string_view id, ce::BellState const& s, int powered) {
    return place(pos, id, {{kFacing, s.facing}, {kBellAttachment, static_cast<int>(s.attach)}, {kBellPowered, powered}, {kBellPart, 0}});
}

void playBellSound(int dim, double x, double y, double z, bool haunted) {
    // AbstractBellBlock#playSound: the peculiar bell at volume 2, pitch 0.94; the haunted one at 4, 1
    if (haunted) sound(dim, x, y, z, kHauntedSound, 4.f, 1.f);
    else sound(dim, x, y, z, kPeculiarSound, 2.f, .94f);
}

/// Tells the client mods near the bell to swing it (Create's sendData of the ringing block entity).
void sendSwing(GlobalPos const& pos, int direction) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (static_cast<int>(p.getDimensionId()) != pos.dim || !net::isCompanion(p)) return true;
        auto const& at = p.getPosition();
        double      dx = at.x - pos.x, dz = at.z - pos.z;
        if (dx * dx + dz * dz > 128.0 * 128.0) return true;
        net::send(p, "equipment.bell.ring", {{"pos", {pos.x, pos.y, pos.z}}, {"dir", direction}});
        return true;
    });
}

// ---- index and redstone ----------------------------------------------------------------------------

void onPower(GlobalPos const& at, int power);

void track(GlobalPos const& pos) {
    auto& s = st();
    if (s.index.insert(pos).second) s.dirty = true;
    if (!s.watches.contains(pos)) s.watches[pos] = rs::watch(pos, [](GlobalPos const& at, int power) { onPower(at, power); });
}

void untrack(GlobalPos const& pos) {
    auto& s = st();
    if (s.index.erase(pos)) s.dirty = true;
    if (auto it = s.watches.find(pos); it != s.watches.end()) {
        rs::unwatch(it->second);
        s.watches.erase(it);
    }
    s.rings.erase(pos);
}

void loadIndex() {
    auto& s = st();
    s.index.clear();
    s.watches.clear();
    s.loaded = true;
    if (auto j = game::db::load(kDbKey); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) track(GlobalPos{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
    s.dirty = false;
}

void saveIndex() {
    auto& s = st();
    if (!s.dirty || !game::db::available()) return;
    Json arr = Json::array();
    for (auto const& p : s.index) arr.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbKey, arr);
    s.dirty = false;
}

/// AbstractBellBlock#neighborChanged: a rising signal rings the bell.
void onPower(GlobalPos const& at, int power) {
    auto const* b = blockAt(at);
    if (!b || !isBell(b->getTypeName())) return; // the chunk / bell is gone: the next poll sees it again
    bool should = power > 0;
    bool was    = game::intState(*b, kBellPowered).value_or(0) != 0;
    if (should == was) return;
    std::string id = b->getTypeName();
    auto        s  = stateOf(*b);
    setBell(at, id, s, should ? 1 : 0);
    if (should) ringBell(at, ce::bellRedstoneDirection(s), nullptr);
}

// ---- conversion and support -----------------------------------------------------------------------

bool soulFire(std::string const& id) { return id == "minecraft:soul_fire" || id == "minecraft:soul_campfire"; }

/// PeculiarBellBlock#tryConvert: a peculiar bell right above soul fire becomes a haunted bell.
bool tryConvert(GlobalPos const& pos) {
    auto const* b = blockAt(pos);
    if (!b || b->getTypeName() != kPeculiarBell) return false;
    if (!soulFire(idAt(neighbour(pos, 0)))) return false;
    auto s       = stateOf(*b);
    int  powered = game::intState(*b, kBellPowered).value_or(0);
    // PeculiarBellBlock#spawnConversionParticles (15..24 soul fire flames) and HAUNTED_BELL_CONVERT
    int n = 15 + static_cast<int>(random01() * 10);
    for (int i = 0; i < n; ++i) {
        double pitch = (random01() * 120 - 90) * std::numbers::pi / 180, yaw = random01() * 2 * std::numbers::pi;
        double r     = random01() * 0.3;
        particle(pos.dim, pos.x + .5 - std::sin(yaw) * std::cos(pitch) * r, pos.y + .5 - std::sin(pitch) * r, pos.z + .5 + std::cos(yaw) * std::cos(pitch) * r,
            "minecraft:blue_flame_particle");
    }
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, kConvertSound, 1.f, 1.f);
    setBell(pos, kHauntedBell, s, powered);
    log().info("equipment: peculiar bell at {} became a haunted bell", pos.key());
    return true;
}

/// HauntedBellBlock#onPlace: the souls stir for a second.
void hauntedPlaced(GlobalPos const& pos) { st().rings[pos].effectTicks = ce::kHauntedEffectTicks; }

/// Vanilla BellBlock#updateShape for the block in `changed` of the bell at `pos`.
void neighbourChanged(GlobalPos const& pos, int changed) {
    auto const* b = blockAt(pos);
    if (!b || !isBell(b->getTypeName())) return;
    std::string id  = b->getTypeName();
    auto        s   = stateOf(*b);
    auto        now = ce::bellAfterNeighbourChange(s, changed, supportAt(pos));
    if (!now) {
        // it falls off: dropped like a broken block (its loot table: the bell itself)
        if (auto item = items::make(id, 1)) dropAt(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, *item);
        clearAt(pos);
        log().info("equipment: {} at {} lost its support", id, pos.key());
        return;
    }
    if (!(*now == s)) setBell(pos, id, *now, game::intState(*b, kBellPowered).value_or(0));
    if (changed == 0 && id == kPeculiarBell) tryConvert(pos);
}

// ---- soul pulses ---------------------------------------------------------------------------------------

bool dangerous(std::string const& id) {
    return id == "minecraft:fire" || id == "minecraft:soul_fire" || id == "minecraft:lava" || id == "minecraft:flowing_lava"
        || id == "minecraft:campfire" || id == "minecraft:soul_campfire" || id == "minecraft:sweet_berry_bush" || id == "minecraft:wither_rose"
        || id == "minecraft:cactus" || id == "minecraft:powder_snow" || id == "minecraft:magma";
}

/// Java's isValidEmptySpawnBlock: no full collision, no signal source, no liquid, no rail, not dangerous.
bool emptySpawnBlock(BlockSource& region, BlockPos const& p) {
    auto const& b  = region.getBlock(p);
    if (b.isAir()) return region.getLiquidBlock(p).isAir();
    std::string id = b.getTypeName();
    if (id.ends_with("water") || id.ends_with("lava") || dangerous(id) || id.ends_with("rail")) return false;
    if (b.getBlockType().isSignalSource()) return false;
    if (!region.getLiquidBlock(p).isAir()) return false;
    AABB box = b.getBlockType().getCollisionShape(b, region, p, nullptr);
    bool full = box.max.x - box.min.x > .999f && box.max.y - box.min.y > .999f && box.max.z - box.min.z > .999f;
    return !full;
}

/// Java's BlockState#isValidSpawn for a zombie: a sturdy top, not too bright, not glass / leaves /
/// ice / barriers / bedrock / magma.
bool validSpawnFloor(BlockSource& region, BlockPos const& p) {
    auto const& b = region.getBlock(p);
    if (b.isAir()) return false;
    std::string id = b.getTypeName();
    if (id == "minecraft:bedrock" || id == "minecraft:barrier" || id == "minecraft:magma" || id.find("glass") != std::string::npos
        || id.find("leaves") != std::string::npos || id.ends_with("ice") || id == "minecraft:soul_fire")
        return false;
    if (static_cast<int>(b.getBlockType().mLightEmission->mValue) >= 14) return false;
    return b.canProvideSupport(1, BlockSupportType::Center);
}

} // namespace

bool canSpawnSoulAt(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    BlockPos p{pos.x, pos.y, pos.z};
    if (p.y - 1 < region->getMinHeight() || p.y + 1 >= region->getMaxHeight()) return false;
    if (!region->hasChunksAt(p, 1, false)) return false;
    if (!validSpawnFloor(*region, BlockPos{p.x, p.y - 1, p.z})) return false;
    if (!emptySpawnBlock(*region, p) || !emptySpawnBlock(*region, BlockPos{p.x, p.y + 1, p.z})) return false;
    // Create: a 0.2 x 0.75 dummy at the centre of the spot hits no collision box
    auto const& b = region->getBlock(p);
    if (b.isAir()) return true;
    AABB box = b.getBlockType().getCollisionShape(b, *region, p, nullptr);
    if (box.max.x - box.min.x < 1e-4f || box.max.y - box.min.y < 1e-4f || box.max.z - box.min.z < 1e-4f) return true;
    float x0 = p.x + .4f, x1 = p.x + .6f, y0 = float(p.y), y1 = p.y + .75f, z0 = p.z + .4f, z1 = p.z + .6f;
    bool  hit = box.min.x < x1 && box.max.x > x0 && box.min.y < y1 && box.max.y > y0 && box.min.z < z1 && box.max.z > z0;
    return !hit;
}

bool soulDark(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    return region->getBrightnessPair(BlockPos{pos.x, pos.y, pos.z}).block->mValue < 1;
}

void soulPulse(GlobalPos const& pos, int distance) {
    auto& handler = st().pulses[pos.dim];
    // a pulse of the same origin and size while the first one still expands would only find spots
    // the first one marked (a held bell asks for one nearly every tick): it is not started twice
    if (handler.expanding({pos.x, pos.y, pos.z}, distance)) return;
    handler.add(ce::SoulPulse({pos.x, pos.y, pos.z}, distance, false));
}

std::vector<GlobalPos> takeSoulMarks() { return std::exchange(st().marks, {}); }

std::size_t soulPulseCount() {
    std::size_t n = 0;
    for (auto const& [dim, p] : st().pulses) n += p.size();
    return n;
}

bool isBell(std::string_view id) { return id == kPeculiarBell || id == kHauntedBell; }

BellInfo bellInfo(GlobalPos const& pos) {
    auto it = st().rings.find(pos);
    if (it == st().rings.end()) return {};
    return BellInfo{it->second.ringing, it->second.ringingTicks, it->second.direction, it->second.effectTicks};
}

bool ringBell(GlobalPos const& pos, int direction, Player* player) {
    auto const* b = blockAt(pos);
    if (!b || !isBell(b->getTypeName())) return false;
    bool  haunted = b->getTypeName() == kHauntedBell;
    auto& r       = st().rings[pos];
    if (haunted) {
        // HauntedBellBlockEntity#ring: not again while recharging; a pulse of 10 blocks
        if (r.ringing && r.ringingTicks < ce::kHauntedRecharge) return false;
        soulPulse(pos, ce::kHauntedRingDistance);
        r.effectTicks = ce::kHauntedEffectTicks;
    }
    r.ringing      = true;
    r.ringingTicks = 0;
    r.direction    = direction;
    sendSwing(pos, direction);
    playBellSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, haunted);
    if (player && haunted) advancements::award(*player, "haunted_bell");
    log().info("equipment: {} at {} rang{}", b->getTypeName(), pos.key(), player ? " (" + player->getRealName() + ")" : std::string());
    return true;
}

namespace {

// ---- ticking ---------------------------------------------------------------------------------------

void tickRings() {
    auto& s = st();
    for (auto it = s.rings.begin(); it != s.rings.end();) {
        auto& [pos, r] = *it;
        if (r.ringing && ++r.ringingTicks >= ce::kBellRingDuration) {
            r.ringing      = false;
            r.ringingTicks = 0;
        }
        if (r.effectTicks > 0) {
            --r.effectTicks;
            // HauntedBellBlockEntity#tick (client side in Create): now and then a soul rises with a sigh
            if (random01() <= .25) {
                particle(pos.dim, pos.x + random01(), pos.y + .5, pos.z + random01(), "minecraft:soul_particle");
                float vol = random01() * .4 + random01() > .9 ? .6f : 0.f;
                if (vol > 0) sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "particle.soul_escape", vol, float(.6 + random01() * .4));
            }
        }
        if (!r.ringing && r.effectTicks <= 0) it = s.rings.erase(it);
        else ++it;
    }
}

void tickHeld() {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto& s    = st();
    auto  time = static_cast<long long>(level->getCurrentTick().tickID);
    level->forEachPlayer([&](Player& p) {
        if (p.isClientSide() || !p.isAlive() || p.isSpectator()) return true;
        bool holding = items::idOf(p.getSelectedItem()) == kHauntedBell || items::idOf(p.getOffhandSlot()) == kHauntedBell;
        if (!holding) return true;
        auto& h = s.held[uidOf(p)];
        if (s.tick - h.last > 5) h.ticks = 0; // the warmup entry expired (250 ms)
        h.last = s.tick;
        ++h.ticks;
        if (!ce::heldBellPulses(h.ticks, time)) return true;
        auto bp = game::blockPosOf(p.getFeetPos()); // Player#blockPosition: the feet
        soulPulse(GlobalPos{static_cast<int>(p.getDimensionId()), bp.x, bp.y, bp.z}, ce::kHeldPulseDistance);
        return true;
    });
}

void tickPulses() {
    auto& s = st();
    for (auto& [dim, pulses] : s.pulses) {
        int d = dim;
        pulses.tick(
            [d](ce::Pos const& p) { return canSpawnSoulAt(GlobalPos{d, p[0], p[1], p[2]}); },
            [d, &s](ce::Pos const& p) {
                GlobalPos at{d, p[0], p[1], p[2]};
                // SoulPulseEffect#spawnParticles: only where it is dark
                if (!soulDark(at)) return;
                particle(d, p[0] + .5, p[1] + .5, p[2] + .5, "levitate:soul");
                particle(d, p[0] + .5, p[1] + .01, p[2] + .5, "levitate:soul_base");
                if (s.marks.size() < 4096) s.marks.push_back(at);
            }
        );
    }
}

// ---- contraptions (BellMovementBehaviour / HauntedBellMovementBehaviour) ------------------------

bool movingActive(con::MovementContext const& ctx) { return ctx.contraption.kind() != "train"; }

void movingSound(con::MovementContext const& ctx) {
    bool        haunted = ctx.data.value("haunted", false);
    auto const& p       = ctx.position;
    playBellSound(ctx.contraption.dimension(), std::floor(p.x) + .5, std::floor(p.y) + .5, std::floor(p.z) + .5, haunted);
}

con::MovementBehaviour bellBehaviour(bool haunted) {
    con::MovementBehaviour b;
    b.isActive    = [](con::MovementContext const& ctx) { return movingActive(ctx); };
    b.startMoving = [haunted](con::MovementContext& ctx) {
        ctx.data["haunted"] = haunted;
        // onSpeedChanged with the first movement (not for elevators)
        if (ctx.contraption.kind() != "elevator") movingSound(ctx);
        ctx.data["mx"] = ctx.motion.x;
        ctx.data["my"] = ctx.motion.y;
        ctx.data["mz"] = ctx.motion.z;
    };
    b.tick = [haunted](con::MovementContext& ctx) {
        ctx.data["haunted"] = haunted;
        bool elevator = ctx.contraption.kind() == "elevator";
        // onSpeedChanged: the motion turned around
        double ox = ctx.data.value("mx", 0.0), oy = ctx.data.value("my", 0.0), oz = ctx.data.value("mz", 0.0);
        double dot = ox * ctx.motion.x + oy * ctx.motion.y + oz * ctx.motion.z;
        double len = std::sqrt(ctx.motion.x * ctx.motion.x + ctx.motion.y * ctx.motion.y + ctx.motion.z * ctx.motion.z);
        double olen = std::sqrt(ox * ox + oy * oy + oz * oz);
        if (!elevator && olen > 1e-6 && len > 1e-6 && dot <= 0) movingSound(ctx);
        ctx.data["mx"] = ctx.motion.x;
        ctx.data["my"] = ctx.motion.y;
        ctx.data["mz"] = ctx.motion.z;
        if (haunted) {
            // HauntedBellMovementBehaviour#tick: the recharge runs down
            int recharge = ctx.data.value("Recharge", 0);
            if (recharge > 0) ctx.data["Recharge"] = recharge - 1;
            return;
        }
        // BellMovementBehaviour#tick: an elevator rings once it arrives
        if (elevator) {
            bool moving = len > 1e-6;
            if (moving) ctx.data["moved"] = true;
            else if (ctx.data.value("moved", false)) {
                movingSound(ctx);
                ctx.data["moved"] = false;
            }
        }
    };
    if (haunted) {
        b.visitNewPosition = [](con::MovementContext& ctx, GlobalPos const& pos) {
            if (ctx.data.value("Recharge", 0) != 0) return;
            soulPulse(pos, ce::kMovingPulseDistance);
            ctx.data["Recharge"] = ce::kHauntedRecharge;
            movingSound(ctx);
        };
    }
    b.stopMoving = [](con::MovementContext& ctx) {
        if (movingActive(ctx)) movingSound(ctx);
    };
    b.tickWhileStill = true;
    return b;
}

cc::DirMask bellAttachedTowards(cc::States const& s) {
    auto get = [&](char const* key, int fallback) {
        auto it = s.find(key);
        if (it == s.end()) return fallback;
        if (auto const* v = std::get_if<std::int64_t>(&it->second)) return static_cast<int>(*v);
        return fallback;
    };
    switch (get(kBellAttachment, 0)) {
    case 0: return cc::bit(cc::Dir::Down);
    case 1: return cc::bit(cc::Dir::Up);
    case 2: return cc::bit(static_cast<cc::Dir>(std::clamp(get(kFacing, 3), 2, 5)));
    default: {
        int f = std::clamp(get(kFacing, 3), 2, 5);
        return static_cast<cc::DirMask>(cc::bit(static_cast<cc::Dir>(f)) | cc::bit(static_cast<cc::Dir>(f ^ 1)));
    }
    }
}

// ---- interactions ---------------------------------------------------------------------------------

/// Bedrock repeats use-on while the button is held: one ring per click.
bool repeatedClick(Player& player, GlobalPos const& pos, bool first) {
    auto& s   = st();
    auto  key = std::make_pair(uidOf(player), pos);
    auto  it  = s.lastClick.find(key);
    bool  rep = it != s.lastClick.end() && s.tick - it->second < 5 && !first;
    if (!rep) s.lastClick[key] = s.tick;
    if (s.lastClick.size() > 256) s.lastClick.clear();
    return rep;
}

ia::Result useOnBell(ia::UseOnContext const& c) {
    // vanilla: a sneaking player with something in hand uses the item, not the block
    if (c.sneaking && !c.itemId.empty()) return ia::Result::Pass;
    auto s = stateOf(c.block);
    if (!ce::bellRingsFrom(s, c.face, c.hit[1])) return ia::Result::Pass;
    if (repeatedClick(c.player, c.pos, c.firstEvent)) return ia::Result::Consume;
    ringBell(c.pos, c.face, &c.player);
    // BellBlock#useWithoutItem succeeds even when a haunted bell is still recharging
    return ia::Result::Consume;
}

void placedBell(ia::PlaceContext const& c) {
    auto& player = c.player;
    auto  placed = ce::placeBell(c.face, faceOf2d(game::playerFacing(player)), supportAt(c.pos));
    if (!placed) {
        // BellBlock#getStateForPlacement returned null: no bell here
        clearAt(c.pos);
        if (!player.isCreative())
            if (auto item = items::make(c.blockId, 1)) giveOrDrop(player, *item);
        log().info("equipment: {} can't hang at {}", c.blockId, c.pos.key());
        return;
    }
    setBell(c.pos, c.blockId, *placed, rs::powered(player.getDimensionBlockSource(), BlockPos{c.pos.x, c.pos.y, c.pos.z}) ? 1 : 0);
    track(c.pos);
    if (c.blockId == kPeculiarBell) tryConvert(c.pos);
    log().info("equipment: {} placed at {} by {}", idAt(c.pos), c.pos.key(), player.getRealName());
}

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    if (ev.layer() != 0) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    std::string nowId  = ev.newBlock().getTypeName();
    std::string prevId = ev.previousBlock().getTypeName();
    auto        pos    = game::globalPos(region, ev.pos());
    if (nowId != prevId) {
        if (isBell(prevId) && !isBell(nowId)) untrack(pos);
        if (isBell(nowId)) {
            defer([pos, nowId, prevId] {
                if (idAt(pos) != nowId) return;
                track(pos);
                if (nowId == kHauntedBell && prevId != kHauntedBell) hauntedPlaced(pos);
                if (nowId == kPeculiarBell) tryConvert(pos);
            });
        }
    }
    // the neighbours' support (vanilla BellBlock#updateShape)
    if (st().index.empty()) return;
    for (int dir = 0; dir < 6; ++dir) {
        auto n = neighbour(pos, dir);
        if (!st().index.contains(n)) continue;
        int changed = ce::oppositeFace(dir); // seen from the bell
        defer([n, changed] { neighbourChanged(n, changed); });
    }
}

class BellsModule {
public:
    void install() {
        static bool once = false;
        if (!once) {
            once = true;
            game::db::onWorldReady([] { loadIndex(); });
            game::db::onWorldSave([] { saveIndex(); });
            game::db::onWorldClosed([] { st() = State{}; });
            for (bool haunted : {false, true}) {
                std::string id(haunted ? kHauntedBell : kPeculiarBell);
                con::registerActor(id, bellBehaviour(haunted));
                // brittle (Create's #brittle tag) and hanging on the side it is attached to
                con::registerMovementRules(id, con::MovementRules{nullptr, [](cc::States const&) { return true; }, bellAttachedTowards, nullptr});
            }
        }
        for (auto id : {kPeculiarBell, kHauntedBell}) {
            mIds.push_back(ia::onUseOn({.block = std::string(id)}, useOnBell));
            mIds.push_back(ia::onPlace({.block = std::string(id)}, placedBell));
        }
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            try {
                onBlockChanged(ev);
            } catch (std::exception const& e) {
                log().error("equipment: bell block change failed: {}", e.what());
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            try {
                auto& s = st();
                ++s.tick;
                if (!s.loaded && game::db::available()) loadIndex();
                tickRings();
                tickHeld();
                tickPulses();
            } catch (std::exception const& e) {
                log().error("equipment: bell tick failed: {}", e.what());
            }
        }));
        if (game::db::available() && !st().loaded) loadIndex(); // BDS: the world exists already
    }

    void uninstall() {
        for (auto id : mIds) ia::remove(id);
        mIds.clear();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        saveIndex();
    }

private:
    std::vector<ia::HandlerId>          mIds;
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace

} // namespace lv::feature::equipment

LV_REGISTER_MODULE(lv::feature::equipment::BellsModule);
