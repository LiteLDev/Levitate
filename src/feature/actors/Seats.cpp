// Seats (Create's SeatBlock / SeatEntity / SeatMovementBehaviour), 16 colours.
//
// Stationary: right-click sits the player (an invisible rideable `levitate:seat` entity, as
// Create's SeatEntity; sneak to leave), or seats a mob the player leads on a leash within 10
// blocks; a mob sitting there is replaced by the player. Dye of another colour recolours it (the
// dye is not used). Mobs that stand on a free seat sit down (hostile ones when seatHostileMobs).
// On a contraption the
// engine carries the seated entities (`registerSeat`) by moving the seat entity they ride, so
// they stay seated while it moves; a moving seat picks up mobs it touches and drops its rider on
// top of a solid block it moves into; at disassembly the riders sit down on the placed seats again.
// Seats halve fall damage and bounce what lands on them without sitting down (Create's
// SeatBlock#fallOn / updateEntityAfterFallOn).

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "feature/actors/Actors.h"
#include "feature/contraption/ActorsExt.h"
#include "game/Blocks.h"
#include "mod/Config.h"
#include "mod/Log.h"

namespace lv::feature::actors {

namespace ext = ::lv::feature::contraption::actors_ext;

namespace {

/// The seat entity sits at the seat's top surface; its rider sits on it.
constexpr double kSeatEntityY = 0.3;
/// Bedrock seats a riding player about 2.3 blocks below its vehicle: the players' seat entity
/// floats above the seat so the player ends up on the cushion.
constexpr double kPlayerSeatEntityY = 2.6;
/// The seat block a seat entity belongs to (players' seat entities float above it).
BlockPos seatBlockOf(Actor const& seatEntity) {
    auto bp = seatEntity.getFeetBlockPos();
    if (seatEntity.getTypeName() == kPlayerSeatEntity) bp.y -= 2;
    return bp;
}
/// Height of carried riders above a seat block on a contraption.
constexpr double kCarriedHeight = 0.5;

bool isSeatEntity(Actor const& a) { return isSeatEntityId(a.getTypeName()); }

std::string dyeColourOf(std::string const& itemId) {
    if (!itemId.starts_with("minecraft:") || !itemId.ends_with("_dye")) return {};
    return itemId.substr(10, itemId.size() - 10 - 4);
}

/// The seat entities at a seat block.
std::vector<Actor*> seatEntitiesAt(BlockSource& region, GlobalPos const& pos) {
    std::vector<Actor*> out;
    AABB box{Vec3{float(pos.x), float(pos.y), float(pos.z)}, Vec3{float(pos.x + 1), float(pos.y + 3), float(pos.z + 1)}};
    for (auto* a : region.getEntities(box, 0.1f))
        if (a && isSeatEntity(*a) && !a->isDead() && seatBlockOf(*a) == BlockPos{pos.x, pos.y, pos.z}) out.push_back(a);
    return out;
}

Actor* spawnSeatEntityAt(BlockSource& region, Vec3 const& at, bool forPlayer) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto owner = level->getActorFactory().createSpawnedActor(ActorDefinitionIdentifier(std::string(forPlayer ? kPlayerSeatEntity : kSeatEntity)), nullptr, at, Vec2{0, 0});
    if (!owner) return nullptr;
    return level->addEntity(region, std::move(owner));
}

Actor* spawnSeatEntity(BlockSource& region, GlobalPos const& pos, bool forPlayer) {
    Vec3 at{float(pos.x + .5), float(pos.y + (forPlayer ? kPlayerSeatEntityY : kSeatEntityY)), float(pos.z + .5)};
    return spawnSeatEntityAt(region, at, forPlayer);
}

/// Riders seated on a contraption (their seat entities are carried, not removed).
std::unordered_set<std::int64_t> carriedRiders() {
    std::unordered_set<std::int64_t> out;
    for (auto id : con::all())
        if (auto* c = con::find(id))
            for (auto const& s : c->seated()) out.insert(s.second);
    return out;
}

/// Gets an entity off its seat entity (which goes with it).
void leaveSeatEntity(Actor& rider) {
    auto* v = rider.getVehicle();
    if (!v || !isSeatEntity(*v)) return;
    rider.stopRiding(true, false, false, false);
    v->remove();
}

Actor* fetch(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->fetchEntity(id, false);
}

std::string posText(GlobalPos const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

} // namespace

bool canBePickedUp(Actor const& entity) {
    if (entity.isPlayer() || isSeatEntity(entity)) return false;
    if (!entity.hasCategory(ActorCategory::Mob)) return false; // Create: LivingEntity
    if (entity.getEntityTypeId() == ActorType::Shulker) return false;
    if (entity.getEntityTypeId() == ActorType::ArmorStand) return false;
    if (!config::get<Config>().seatHostileMobs && entity.hasCategory(ActorCategory::Monster)) return false;
    return entity.isAlive();
}

Actor* seatedAt(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return nullptr;
    for (auto* s : seatEntitiesAt(*region, pos))
        if (auto* p = s->getFirstPassenger()) return p;
    return nullptr;
}

bool sitDown(Actor& entity, GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    if (entity.getVehicle()) entity.stopRiding(true, false, false, false);
    auto* seat = spawnSeatEntity(*region, pos, entity.isPlayer());
    if (!seat) return false;
    if (!entity.startRiding(*seat, true)) {
        seat->remove();
        return false;
    }
    if (entity.hasCategory(ActorCategory::TamableAnimal) && entity.isTame()) entity.setSitting(true);
    logAction(entity.getTypeName() + " sat down on the seat at " + posText(pos));
    return true;
}

bool seatUse(Player& player, GlobalPos const& pos, ItemStack const& held) {
    if (player.isSneaking()) return false;
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    auto        bp = toBlockPos(pos);
    std::string id = region->getBlock(bp).getTypeName();
    auto        colour = seatColour(id);
    if (!colour) return false;

    // dyeing (Create: the dye stays in the hand)
    if (!held.isNull() && held.mCount > 0) {
        auto dye = dyeColourOf(held.getTypeName());
        if (!dye.empty() && dye != kColours[*colour]) {
            for (int i = 0; i < 16; ++i) {
                if (kColours[i] != dye) continue;
                if (auto const* b = game::blockWith(seatId(i))) {
                    game::setBlock(*region, bp, *b);
                    logAction("seat at " + posText(pos) + " dyed " + dye);
                }
                return true;
            }
        }
    }

    auto seats = seatEntitiesAt(*region, pos);
    if (!seats.empty()) {
        auto* rider = seats.front()->getFirstPassenger();
        if (rider && rider->isPlayer()) return false;
        if (rider) {
            rider->stopRiding(true, false, false, false);
            logAction(rider->getTypeName() + " was ejected from the seat at " + posText(pos));
        }
        for (auto* s : seats) s->remove();
        return sitDown(player, pos) || true;
    }
    // a mob the player leads on a leash sits instead
    auto level = ll::service::getLevel();
    if (level) {
        auto  feet = player.getFeetPos();
        AABB  area{Vec3{feet.x - 10, feet.y - 10, feet.z - 10}, Vec3{feet.x + 10, feet.y + 10, feet.z + 10}};
        auto  uid  = player.getOrCreateUniqueID();
        for (auto* a : region->getEntities(area, 0.f)) {
            if (!a || !a->isLeashed() || a->getLeashHolder() != uid || !canBePickedUp(*a)) continue;
            return sitDown(*a, pos) || true;
        }
    }
    sitDown(player, pos);
    return true;
}

void leaveSeat(Actor& rider) { leaveSeatEntity(rider); }

void mountOnContraptionSeat(con::Contraption& c, cc::Pos const& local, Actor& rider) {
    auto* region = regionOf(c.dimension());
    if (!region) return;
    if (auto* v = rider.getVehicle()) {
        if (isSeatEntity(*v)) leaveSeatEntity(rider);
        else rider.stopRiding(true, false, false, false);
    }
    bool      player = rider.isPlayer();
    cc::Vec3d at     = c.toWorld(cc::Vec3d::of(local) + cc::Vec3d{.5, player ? kPlayerSeatEntityY : kSeatEntityY, .5});
    auto*     seat   = spawnSeatEntityAt(*region, Vec3{float(at.x), float(at.y), float(at.z)}, player);
    if (!seat) return;
    if (!rider.startRiding(*seat, true)) seat->remove(); // the engine carries it standing then
}

void seatsTick(std::uint64_t tick) {
    if (tick % 2 != 0) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    auto carried = carriedRiders();
    for (auto* a : level->getRuntimeActorList()) {
        if (!a || a->isDead()) continue;
        auto& region = a->getDimensionBlockSource();
        if (isSeatEntity(*a)) {
            // on a moving contraption: the engine moves it with its seat
            if (auto* p = a->getFirstPassenger(); p && carried.contains(p->getOrCreateUniqueID().rawID)) continue;
            // Create's SeatEntity.tick: gone without a rider or without its seat block
            auto bp = seatBlockOf(*a);
            if (!a->hasPassenger() || !region.hasChunksAt(bp, 0, false) || !isSeat(region.getBlock(bp).getTypeName())) {
                if (auto* p = a->getFirstPassenger()) {
                    p->stopRiding(true, false, false, false);
                    // Create: the default dismount spot raised by half a block
                    auto f = p->getFeetPos();
                    if (!p->isPlayer()) p->teleportTo(Vec3{f.x, f.y + .5f, f.z}, false, 0, 0, false);
                    if (p->hasCategory(ActorCategory::TamableAnimal)) p->setSitting(false);
                }
                a->remove();
            }
            continue;
        }
        if (a->getVehicle() || !canBePickedUp(*a) || !a->isOnGround()) continue;
        auto bp = a->getFeetBlockPos();
        if (!isSeat(region.getBlock(bp).getTypeName())) continue;
        GlobalPos pos{static_cast<int>(a->getDimensionId()), bp.x, bp.y, bp.z};
        if (seatedAt(pos)) continue;
        sitDown(*a, pos);
    }
}

// ---- on contraptions --------------------------------------------------------------------------

namespace {

/// The entity carried on the seat at `local` (the engine keeps it at the seat point).
Actor* riderOf(con::Contraption const& c, cc::Pos const& local) {
    auto* region = regionOf(c.dimension());
    if (!region) return nullptr;
    cc::Vec3d p = c.toWorld(cc::Vec3d::of(local) + cc::Vec3d{.5, kCarriedHeight, .5});
    for (auto* a : entitiesIn(*region, p - cc::Vec3d{.3, .3, .3}, p + cc::Vec3d{.3, .3, .3})) {
        if (!a || a->getEntityTypeId() == ActorType::FallingBlock || isSeatEntity(*a) || a->getEntityTypeId() == ActorType::ItemEntity) continue;
        auto f = a->getFeetPos();
        if (std::abs(f.x - p.x) < .25 && std::abs(f.y - p.y) < .25 && std::abs(f.z - p.z) < .25) return a;
    }
    return nullptr;
}

void seatActorTick(con::MovementContext& ctx) {
    // pick up a mob touching the free seat (Create's contraption collision with seats)
    if (riderOf(ctx.contraption, ctx.localPos)) return;
    auto* region = regionOf(ctx.contraption.dimension());
    if (!region) return;
    cc::Vec3d c = ctx.position;
    AABB      box{Vec3{float(c.x - .5), float(c.y - .5), float(c.z - .5)}, Vec3{float(c.x + .5), float(c.y + .5), float(c.z + .5)}};
    for (auto* a : region->getEntities(box, 0.f)) {
        if (!a || a->getVehicle() || !canBePickedUp(*a)) continue;
        if (ctx.contraption.seat(ctx.localPos, a->getOrCreateUniqueID().rawID)) {
            mountOnContraptionSeat(ctx.contraption, ctx.localPos, *a);
            logAction(a->getTypeName() + " picked up by a seat of contraption #" + std::to_string(ctx.contraption.id()));
        }
        return;
    }
}

/// Create's SeatMovementBehaviour.visitNewPosition: a solid block (or a bottom slab) moving
/// into the seat puts its rider on top of it.
void seatVisit(con::MovementContext& ctx, GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    auto        bp    = toBlockPos(pos);
    auto const& block = region->getBlock(bp);
    std::string id    = block.getTypeName();
    bool        slab  = id.ends_with("_slab") && game::intState(block, "minecraft:vertical_half").value_or(0) == 0
              && !id.ends_with("double_slab");
    bool solid = slab || (!block.isAir() && block._isSolid());
    if (!solid) return;
    auto* rider = riderOf(ctx.contraption, ctx.localPos);
    if (!rider) return;
    ctx.contraption.unseat(rider->getOrCreateUniqueID().rawID);
    leaveSeatEntity(*rider);
    Vec3 to{float(pos.x + .5), float(pos.y + (slab ? .5 : 1.0)), float(pos.z + .5)};
    if (rider->isPlayer()) static_cast<Player*>(rider)->teleport(to, rider->getDimensionId());
    else rider->teleportTo(to, true, 0, 0, false);
    logAction(rider->getTypeName() + " dismounted from contraption #" + std::to_string(ctx.contraption.id()));
}

struct Registrar {
    Registrar() {
        for (int i = 0; i < 16; ++i) {
            auto id = seatId(i);
            con::registerSeat(id, kCarriedHeight);
            con::MovementBehaviour b;
            b.tick             = seatActorTick;
            b.visitNewPosition = seatVisit;
            registerOwnActor(id, std::move(b));
        }
        // the settings of this unit's blocks (roller mode / filter, controls state and filter)
        for (auto id : {kHarvester, kPlough, kRoller, kPsi, kControls}) ext::carryBlockData(std::string(id));
        // this unit's per-block hand-off at assembly / disassembly
        con::registerDataCarrier(con::DataCarrier{
            "actors",
            [](BlockSource& region, core::GlobalPos const& pos) -> std::optional<Json> {
                std::string id = region.getBlock(toBlockPos(pos)).getTypeName();
                // (a seat keeps its seat entity and rider: the engine moves them with the seat)
                if (isSeat(id)) return Json{{"seat", true}};
                if (id == kControls) return Json{{"controls", true}};
                return std::nullopt;
            },
            [](BlockSource& region, core::GlobalPos const& pos, Json const& data) {
                if (data.value("controls", false)) {
                    trackStationaryControls(pos);
                    if (data.contains("disabled")) setIntState(region, toBlockPos(pos), "levitate:disabled", data.value("disabled", false) ? 1 : 0);
                }
                if (!data.value("seat", false)) return;
                // Create's addPassengersToWorld: the riders sit on the placed seat
                cc::Vec3d p{pos.x + .5, pos.y + kCarriedHeight, pos.z + .5};
                cc::Vec3d lo{double(pos.x), double(pos.y), double(pos.z)};
                for (auto* a : entitiesIn(region, lo, lo + cc::Vec3d{1, 1.5, 1})) {
                    if (!a || isSeatEntity(*a) || a->getEntityTypeId() == ActorType::FallingBlock
                        || a->getEntityTypeId() == ActorType::ItemEntity) {
                        continue;
                    }
                    if (auto* v = a->getVehicle(); v && !isSeatEntity(*v)) continue;
                    auto f = a->getFeetPos();
                    if (std::abs(f.x - p.x) > .25 || std::abs(f.z - p.z) > .25 || std::abs(f.y - p.y) > .3) continue;
                    // after the block is in place (this runs inside the disassembly)
                    auto uid = a->getOrCreateUniqueID().rawID;
                    ext::defer([uid, pos] {
                        if (auto* e = fetch(uid)) sitDown(*e, pos);
                    });
                    break;
                }
            },
            nullptr,
        });
    }
};
Registrar const gRegistrar;

} // namespace

// ---- landing on a seat (Create's SeatBlock#fallOn / updateEntityAfterFallOn) ------------------

namespace {

/// The seat block an entity stands on (its feet on the cushion or on the block below them).
std::optional<GlobalPos> seatUnder(Actor& a) {
    auto& region = a.getDimensionBlockSource();
    auto  f      = a.getFeetPos();
    for (float dy : {-0.01f, -0.51f}) {
        BlockPos bp{int(std::floor(f.x)), int(std::floor(f.y + dy)), int(std::floor(f.z))};
        if (!region.hasChunksAt(bp, 0, false)) return std::nullopt;
        if (isSeat(region.getBlock(bp).getTypeName())) return GlobalPos{static_cast<int>(a.getDimensionId()), bp.x, bp.y, bp.z};
    }
    return std::nullopt;
}

void bounce(Actor& a, float ya) {
    // Create: vy = -vy * 0.66 (0.8 times that for non-living entities)
    double factor = a.hasCategory(ActorCategory::Mob) ? 1.0 : 0.8;
    Vec3&  delta  = *a.mBuiltInComponents->mStateVectorComponent->mPosDelta;
    delta.y       = static_cast<float>(-ya * 0.66 * factor);
    if (a.isPlayer()) {
        // players move themselves: their client gets the motion
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetActorMotion);
        if (!packet) return;
        auto& pkt      = static_cast<SetActorMotionPacket&>(*packet);
        pkt.mRuntimeId = a.getRuntimeID();
        pkt.mMotion    = delta;
        pkt.sendTo(static_cast<Player&>(a));
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(SeatFallHook, ll::memory::HookPriority::Normal, Actor, &Actor::checkFallDamage, void, float ya, bool onGround, bool recheckLiquid) {
    // a real landing (not the gravity tick of an entity standing still) on a seat
    if (onGround && ya < -0.1f && !isClientSide() && !isSeatEntity(*this) && !getVehicle()) {
        try {
            if (auto seat = seatUnder(*this)) {
                float fall = getFallDistance();
                if (fall > 0) setFallDistance(fall * 0.5f); // Create: fallOn(..., fallDistance * 0.5F)
                bool sits = !isPlayer() && canBePickedUp(*this) && !seatedAt(*seat); // sits down (seatsTick)
                bool suppress = isPlayer() && isSneaking(); // Entity#isSuppressingBounce
                if (!sits && !suppress) {
                    origin(ya, onGround, recheckLiquid);
                    bounce(*this, ya);
                    return;
                }
            }
        } catch (std::exception const& e) {
            log().error("actors: seat landing failed: {}", e.what());
        }
    }
    origin(ya, onGround, recheckLiquid);
}

void seatsInstall() { SeatFallHook::hook(); }
void seatsUninstall() { SeatFallHook::unhook(); }


} // namespace lv::feature::actors
