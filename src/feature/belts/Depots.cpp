// Belts unit: depots and weighted ejectors (Create's DepotBehaviour, SharedDepotBlockMethods,
// DepotItemHandler, WeightedEjectorBlockEntity, EntityLauncher and EjectorTargetHandler).

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/level/ShapeType.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"

#include "core/common/ValueSettings.h"
#include "feature/advancements/Advancements.h"
#include "feature/belts/BeltsInternal.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/common/ValueBox.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::belts {

namespace citems = common::items;

namespace {

std::string posText(GlobalPos const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

int maxStackOf(ItemStack const& s) { return citems::isEmpty(s) ? 64 : std::max(1, int(s.getMaxStackSize())); }

ItemStack const& heldStack(Depot const& d) {
    if (!d.held) return citems::empty();
    auto it = d.stacks.find(d.held->id);
    return it == d.stacks.end() ? citems::empty() : it->second.stack;
}

bool outputsEmpty(Depot const& d) {
    return std::all_of(d.outputs.begin(), d.outputs.end(), [](ItemStack const& s) { return citems::isEmpty(s); });
}

/// Create's EjectorBlockEntity enables merging on its depot behaviour; plain depots don't merge.
bool canMergeItems(Depot const& d) { return d.ejector; }

/// Create's DepotBehaviour.getPresentStackSize: the held stack and the output buffer.
int presentStackSize(Depot const& d) {
    int n = citems::countOf(heldStack(d));
    for (auto const& o : d.outputs) n += citems::countOf(o);
    return n;
}

/// Create's DepotBehaviour.getRemainingSpace: min(setting or 64, the held stack's max stack size)
/// minus what is present and incoming.
int remainingSpace(Depot const& d) {
    int n = presentStackSize(d);
    for (auto const& i : d.incoming)
        if (auto it = d.stacks.find(i.id); it != d.stacks.end()) n += citems::countOf(it->second.stack);
    int cap = std::min(d.stackSize > 0 ? d.stackSize : 64, maxStackOf(heldStack(d)));
    return cap - n;
}

void setHeld(Depot& d, cb::Item item, StackData data) {
    if (d.held) {
        if (auto it = d.stacks.find(d.held->id); it != d.stacks.end()) {
            removeDisplay(it->second);
            d.stacks.erase(it);
        }
    }
    d.held             = item;
    d.stacks[item.id]  = std::move(data);
}

void removeHeld(Depot& d) {
    if (!d.held) return;
    if (auto it = d.stacks.find(d.held->id); it != d.stacks.end()) {
        removeDisplay(it->second);
        d.stacks.erase(it);
    }
    d.held.reset();
}

/// Create's DepotBehaviour.insert.
ItemStack insertInternal(Depot& d, TransportedItem const& t, bool simulate) {
    ItemStack const& inserted = t.stack;
    int              count    = citems::countOf(inserted);
    auto             make     = [&](int n) {
        cb::Item it;
        it.id               = newItemId();
        it.beltPosition     = t.beltPosition;
        it.prevBeltPosition = t.beltPosition;
        it.sideOffset       = t.sideOffset;
        it.insertedFrom     = t.insertedFrom;
        StackData sd;
        sd.stack          = citems::withCount(inserted, n);
        sd.processingTime = t.processingTime;
        sd.processedBy    = t.processedBy;
        sd.extra          = t.extra;
        return std::pair{it, sd};
    };
    auto add = [&](int n) {
        auto [it, sd] = make(n);
        if (d.held) {
            d.stacks[it.id] = std::move(sd);
            d.incoming.push_back(it);
        } else {
            setHeld(d, it, std::move(sd));
        }
    };
    if (canMergeItems(d)) {
        int remaining = remainingSpace(d);
        if (remaining <= 0) return inserted;
        if (d.held && !citems::sameItemSameData(heldStack(d), inserted)) return inserted;
        if (remaining < count) {
            if (!simulate) add(remaining);
            return citems::withCount(inserted, count - remaining);
        }
        if (!simulate) add(count);
        return ItemStack();
    }
    int       max      = maxStackOf(inserted);
    ItemStack returned = count > max ? citems::withCount(inserted, count - max) : ItemStack();
    if (simulate) return returned;
    if (!d.held && outputsEmpty(d)) {
        if (t.insertedFrom >= 2) playSound(d.pos, "dig.sand", .125f, 1.5f);           // Create's DEPOT_SLIDE
        else playSound(d.pos, "block.itemframe.add_item", .25f, 1.25f);               // Create's DEPOT_PLOP
    }
    auto [it, sd] = make(std::min(count, max));
    setHeld(d, it, std::move(sd));
    return returned;
}

ItemStack insertOutput(Depot& d, ItemStack stack) {
    for (auto& o : d.outputs) {
        if (citems::isEmpty(stack)) break;
        if (citems::isEmpty(o)) {
            o     = stack;
            stack = ItemStack();
        } else if (citems::sameItemSameData(o, stack)) {
            int room = maxStackOf(o) - citems::countOf(o);
            int n    = std::min(room, citems::countOf(stack));
            if (n <= 0) continue;
            o     = citems::withCount(o, citems::countOf(o) + n);
            stack = citems::withCount(stack, citems::countOf(stack) - n);
        }
    }
    return stack;
}

// ---- ejector --------------------------------------------------------------------------------------

int ejectorFacing(Depot const& d) {
    auto const* b = blockAt(d.pos);
    return b ? game::intState(*b, kFacing).value_or(0) & 3 : 0;
}

GlobalPos ejectorTarget(Depot const& d) {
    auto n = cb::facingNormal(ejectorFacing(d));
    return offset(d.pos, {n.x * d.targetH, d.targetV, n.z * d.targetH});
}

void setLidState(Depot& d, int lid) {
    auto const* b = blockAt(d.pos);
    if (!b || game::intState(*b, kLid).value_or(0) == lid) return;
    if (auto const* nb = game::blockWith(kEjectorId, {{kFacing, game::intState(*b, kFacing).value_or(0)}, {kLid, lid}, {kRotor, game::intState(*b, kRotor).value_or(0)}}))
        setBlockQuiet(d.pos, *nb);
}

/// Redstone power next to the ejector (custom blocks get no neighbour updates: polled).
bool poweredAt(GlobalPos const& p) {
    auto* region = regionOf(p.dim);
    return region && common::redstone::powered(*region, game::blockPos(p));
}

std::vector<Actor*> entitiesOnPlate(Depot const& d) {
    std::vector<Actor*> out;
    auto*               region = regionOf(d.pos.dim);
    auto                level  = ll::service::getLevel();
    if (!region || !level) return out;
    double x0 = d.pos.x + 1 / 16.0, x1 = d.pos.x + 15 / 16.0, z0 = d.pos.z + 1 / 16.0, z1 = d.pos.z + 15 / 16.0;
    double y0 = d.pos.y + .5, y1 = d.pos.y + 1.4;
    auto   inside = [&](Vec3 const& f) { return f.x >= x0 && f.x <= x1 && f.z >= z0 && f.z <= z1 && f.y >= y0 && f.y <= y1; };
    AABB   box{Vec3{float(x0), float(y0), float(z0)}, Vec3{float(x1), float(y1), float(z1)}};
    for (auto* a : region->getEntities(box, 0.f)) {
        if (!a || !a->isAlive() || isDisplayActor(*a) || a->getEntityTypeId() == ActorType::ItemEntity) continue;
        if (inside(a->getFeetPos())) out.push_back(a);
    }
    level->forEachPlayer([&](Player& p) {
        if (int(p.getDimensionId()) == d.pos.dim && inside(p.getFeetPos()) && !p.isSpectator()) out.push_back(&p);
        return true;
    });
    return out;
}

void launchEntity(Depot& d, Actor& a, cb::Launch const& l, int facing) {
    auto n = cb::facingNormal(facing);
    // Create's activateDeferred: the entity is put on top of the ejector, then EntityLauncher.applyMotion
    Vec3 feet = {float(d.pos.x + .5), float(d.pos.y + 1), float(d.pos.z + .5)};
    Vec3 motion{float(n.x * l.vx * .91), float(l.vy * .98), float(n.z * l.vx * .91)};
    // Create: a player wearing an elytra starts gliding (motion scaled by .75)
    bool glide = a.isPlayer() && citems::idOf(a.getArmor(SharedTypes::Legacy::ArmorSlot::Torso)) == "minecraft:elytra";
    if (glide) motion = Vec3{motion.x * .75f, motion.y * .75f, motion.z * .75f};
    if (a.isPlayer()) static_cast<Player&>(a).teleport(feet, DimensionType(d.pos.dim));
    else a.teleportTo(feet, false, 0, 0, false);
    a.mBuiltInComponents->mStateVectorComponent->mPosDelta = motion;
    if (a.isPlayer() && !a.isSimulatedPlayer()) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetActorMotion);
        if (packet) {
            auto& pkt      = static_cast<SetActorMotionPacket&>(*packet);
            pkt.mRuntimeId = a.getRuntimeID();
            pkt.mMotion    = motion;
            pkt.sendTo(static_cast<Player&>(a));
        }
    }
    if (glide) static_cast<Player&>(a).startGliding();
    // Create's EjectorAwardPacket: a player launched 25 blocks or further
    if (a.isPlayer() && l.horizontal * l.horizontal + l.vertical * l.vertical >= 25 * 25)
        advancements::award(static_cast<Player&>(a), "ejector_maxed");
}

/// Create's getTargetOpenInv: the block an obstacle scan found, else the target (at least 1 ahead).
GlobalPos targetBlockOf(Depot const& d) {
    if (d.earlyTarget) return d.earlyTarget->block;
    auto n = cb::facingNormal(ejectorFacing(d));
    int  h = std::max(1, d.targetH);
    return offset(d.pos, {n.x * h, d.targetV, n.z * h});
}

/// The target accepts the held stack (Create waits while a targeted inventory is full).
bool targetAccepts(Depot& d) {
    if (!d.held) return true;
    GlobalPos target = targetBlockOf(d);
    if (!hasDirectInput(target)) return true;
    auto it = d.stacks.find(d.held->id);
    if (it == d.stacks.end()) return false;
    auto api  = toApi(*d.held, it->second);
    auto rest = insertDirect(target, api, 0, true);
    return citems::countOf(rest) < citems::countOf(api.stack);
}

/// Create's addToLaunchedItems: the first item launched while no scan cools down is tracked.
void addToLaunched(Depot& d, StackData data, cb::Launch const& l, int facing) {
    if (citems::isEmpty(data.stack)) return;
    Airborne ab;
    ab.data   = std::move(data);
    ab.launch = l;
    ab.facing = facing;
    ab.origin = d.pos;
    ab.id     = newItemId();
    if (d.trackedItem == 0 && d.scanCooldown == 0) {
        d.scanCooldown = std::max(1, config().ejectorScanInterval);
        d.trackedItem  = ab.id;
    }
    log().info("belts: ejector at {} launched {} x{} to {}", posText(d.pos), citems::idOf(ab.data.stack), citems::countOf(ab.data.stack), posText(ejectorTarget(d)));
    d.airborne.push_back(std::move(ab));
}

/// Create's EjectorBlockEntity.activateDeferred / launchItems: the held stack, the incoming ones
/// and the 8 output slots fly; entities on the plate are thrown too.
void activate(Depot& d) {
    int  facing = ejectorFacing(d);
    auto l      = cb::planLaunch(d.targetH, d.targetV);
    for (auto* a : entitiesOnPlate(d)) launchEntity(d, *a, l, facing);
    if (d.held) {
        if (auto it = d.stacks.find(d.held->id); it != d.stacks.end()) {
            addToLaunched(d, std::move(it->second), l, facing);
            d.stacks.erase(it);
        }
        d.held.reset();
    }
    for (auto const& i : d.incoming) {
        if (auto it = d.stacks.find(i.id); it != d.stacks.end()) {
            removeDisplay(it->second);
            addToLaunched(d, std::move(it->second), l, facing);
            d.stacks.erase(it);
        }
    }
    d.incoming.clear();
    for (auto& o : d.outputs) {
        if (citems::isEmpty(o)) continue;
        StackData sd;
        sd.stack = o;
        o        = ItemStack();
        addToLaunched(d, std::move(sd), l, facing);
    }
    d.state       = EjectorState::Launching;
    d.launchTicks = 0;
    d.lid         = 1;
    setLidState(d, 1);
    // Create: wooden trapdoor close (.35, 1) + chest open (.1, 1.4)
    playSound(d.pos, "close.wooden_trapdoor", .35f, 1.f);
    playSound(d.pos, "random.chestopen", .1f, 1.4f);
}

/// Where a launched item is after `t` ticks (Create's getLaunchedItemLocation: from the centre).
cb::Vec3d launchedAt(Depot const& d, Airborne const& a, double t) {
    auto n           = cb::facingNormal(a.facing);
    auto [along, up] = a.launch.at(t);
    return cb::Vec3d{d.pos.x + .5, d.pos.y + .5, d.pos.z + .5} + cb::Vec3d{n.x * along, up, n.z * along};
}

/// Create's scanTrajectoryForObstacles: a block in the arc between `t` and `t + 1` ends the flight
/// early (extracting funnels let items through).
bool scanTrajectory(Depot& d, Airborne const& a, double t) {
    if (t <= 2) return false;
    auto* region = regionOf(d.pos.dim);
    if (!region) return false;
    auto source = launchedAt(d, a, t), target = launchedAt(d, a, t + 1);
    Vec3 from{float(source.x), float(source.y), float(source.z)}, to{float(target.x), float(target.y), float(target.z)};
    // (the game calls the block filter unconditionally: an empty one throws)
    static std::function<bool(BlockSource const&, Block const&, bool)> const everyBlock = [](BlockSource const&, Block const&, bool) { return true; };
    HitResult hit  = region->clip(from, to, false, ShapeType::Collision, 200, false, false, nullptr, everyBlock, false);
    bool      miss = hit.mType != HitResultType::Tile;
    if (!miss) {
        auto const& b = region->getBlock(hit.mBlock);
        auto        id = std::string(b.getTypeName());
        if ((id == "levitate:andesite_funnel" || id == "levitate:brass_funnel") && game::intState(b, "levitate:extracting").value_or(0) != 0) miss = true;
    }
    if (miss) {
        if (d.earlyTarget && d.earlyTarget->time < t + 1) d.earlyTarget.reset();
        return false;
    }
    auto       fn = cb::faceNormal(hit.mFacing);
    cb::Vec3d  vec{hit.mPos.x, hit.mPos.y, hit.mPos.z};
    auto       dist = [](cb::Vec3d const& p, cb::Vec3d const& q) {
        auto e = p - q;
        return std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
    };
    double full = dist(source, target);
    d.earlyTarget = EarlyTarget{vec + cb::Vec3d{fn.x * .25, fn.y * .25, fn.z * .25}, GlobalPos{d.pos.dim, hit.mBlock.x, hit.mBlock.y, hit.mBlock.z},
                                t + (full > 0 ? dist(source, vec) / full : 0)};
    log().info("belts: ejector at {} hits {} on the way", posText(d.pos), posText(d.earlyTarget->block));
    return true;
}

/// Create's placeItemAtTarget: into the target's belt input, else dropped there.
void placeAtTarget(Depot& d, Airborne& a, double maxTime) {
    if (a.id == d.trackedItem) d.trackedItem = 0;
    removeDisplay(a.data);
    GlobalPos target = targetBlockOf(d);
    ItemStack rest   = a.data.stack;
    if (hasDirectInput(target)) {
        TransportedItem t;
        t.stack          = a.data.stack;
        t.insertedFrom   = 0;
        t.processingTime = a.data.processingTime;
        t.processedBy    = a.data.processedBy;
        t.extra          = a.data.extra;
        rest             = insertDirect(target, t, 0, false);
    }
    if (citems::isEmpty(rest)) return;
    auto at = d.earlyTarget ? d.earlyTarget->at : launchedAt(d, a, maxTime);
    auto n  = cb::facingNormal(a.facing);
    auto [va, vu] = a.launch.velocity(maxTime);
    spawnItem(d.pos, at.x, at.y, at.z, rest, n.x * va * .5, vu * .5, n.z * va * .5);
}

void tickAirborne(Depot& d) {
    if (d.scanCooldown > 0) --d.scanCooldown;
    for (auto it = d.airborne.begin(); it != d.airborne.end();) {
        auto&  a         = *it;
        double totalTime = a.launch.itemTicks();
        bool   hit       = a.id != 0 && a.id == d.trackedItem && scanTrajectory(d, a, a.t);
        double maxTime   = d.earlyTarget ? std::min(d.earlyTarget->time, totalTime) : totalTime;
        if (hit || a.t > maxTime) {
            placeAtTarget(d, a, maxTime);
            it = d.airborne.erase(it);
            continue;
        }
        syncDisplay(d.pos, a.data, launchedAt(d, a, a.t));
        a.t += 1;
        ++it;
    }
}

void tickEjector(Depot& d) {
    // Create's EjectorBlock.neighborChanged -> updateSignal: the shared redstone watch reports changes
    if (d.redstoneWatch == 0) {
        d.redstoneWatch = common::redstone::watch(d.pos, [](GlobalPos const& at, int power) {
            auto& depots = state().depots;
            if (auto it = depots.find(at); it != depots.end() && it->second->ejector) {
                bool now = power > 0;
                if (now != it->second->powered) log().info("belts: ejector at {} {}", posText(at), now ? "powered" : "unpowered");
                it->second->powered = now;
            }
        });
        d.powered = poweredAt(d.pos);
    }
    float speed = kinetics::speedAt(d.pos);
    switch (d.state) {
    case EjectorState::Charged: {
        if (d.powered || speed == 0) break;
        bool trigger = !entitiesOnPlate(d).empty();
        // Create's ejectIfTriggered: something present (held + outputs), at least the stack size
        // setting, the held stack centred and the target taking it
        int present = presentStackSize(d);
        if (!trigger && present > 0 && present >= d.stackSize && (!d.held || d.held->beltPosition >= .49f)) trigger = targetAccepts(d);
        if (trigger) activate(d);
        break;
    }
    case EjectorState::Launching:
        if (++d.launchTicks >= 2) d.state = EjectorState::Retracting;
        break;
    case EjectorState::Retracting: {
        float windUp = cb::ejectorWindUp(speed, d.targetH, d.targetV);
        d.lid -= windUp;
        // Create: a wooden button clicks while it winds up (.125, 1.5 - lid)
        int soundRate = windUp > 0 ? int(1 / (windUp * 5)) + 1 : 0;
        if (soundRate > 0 && state().tick % std::uint64_t(soundRate) == 0)
            playSound(d.pos, "click_off.wooden_button", .125f, 1.5f - std::max(0.f, d.lid));
        if (d.lid < .5f) setLidState(d, 0);
        if (d.lid <= 0) {
            d.lid   = 0;
            d.state = EjectorState::Charged;
        }
        break;
    }
    }
    tickAirborne(d);
}

} // namespace

void releaseDepot(Depot& d) {
    if (d.redstoneWatch != 0) common::redstone::unwatch(d.redstoneWatch);
    d.redstoneWatch = 0;
}

bool depotCanAccept(Depot const& d) { return !d.ejector || d.state == EjectorState::Charged; }

ItemStack depotInsert(Depot& d, TransportedItem const& item, int side, bool simulate) {
    // Create's DepotBehaviour.tryInsertingFromSide
    if (!citems::isEmpty(heldStack(d)) && !canMergeItems(d)) return item.stack;
    if (!outputsEmpty(d) && !canMergeItems(d)) return item.stack;
    if (!depotCanAccept(d)) return item.stack;
    TransportedItem t = item;
    t.beltPosition    = side == 0 || side == 1 ? .5f : 0.f;
    t.insertedFrom    = side;
    t.locked          = false;
    return insertInternal(d, t, simulate);
}

bool applyOnDepot(Depot& d, float maxDistance, ProcessFn const& fn) {
    if (!d.held) return false;
    if (.5f - d.held->beltPosition > maxDistance) return false;
    auto it = d.stacks.find(d.held->id);
    if (it == d.stacks.end()) return false;
    auto api    = toApi(*d.held, it->second);
    auto result = fn(api);
    if (result.kind == TransportedResult::Kind::DoNothing) {
        it->second.processingTime = api.processingTime;
        it->second.processedBy    = api.processedBy;
        it->second.extra          = api.extra;
        return false;
    }
    cb::Item base = *d.held;
    removeHeld(d);
    auto put = [&](ItemStack const& stack, bool locked) {
        cb::Item i     = base;
        i.id           = newItemId();
        i.locked       = locked;
        i.beltPosition = .5f;
        StackData sd;
        sd.stack = stack;
        setHeld(d, i, std::move(sd));
    };
    if (result.heldOutput && !citems::isEmpty(*result.heldOutput)) put(*result.heldOutput, true);
    for (auto const& out : result.outputs) {
        if (citems::isEmpty(out)) continue;
        if (!d.held) {
            put(out, false);
            continue;
        }
        auto rest = insertOutput(d, out);
        spawnItem(d.pos, d.pos.x + .5, d.pos.y + 1, d.pos.z + .5, rest);
    }
    return true;
}

void tickDepot(Depot& d) {
    // incoming items slide in and merge into the held stack
    for (auto it = d.incoming.begin(); it != d.incoming.end();) {
        if (!cb::depotSlide(*it)) {
            ++it;
            continue;
        }
        auto sd = d.stacks.find(it->id);
        if (sd == d.stacks.end()) {
            it = d.incoming.erase(it);
            continue;
        }
        if (!d.held) {
            d.held = *it;
        } else if (auto hs = d.stacks.find(d.held->id); hs != d.stacks.end() && citems::sameItemSameData(hs->second.stack, sd->second.stack)
                                                         && citems::countOf(hs->second.stack) + citems::countOf(sd->second.stack) <= maxStackOf(hs->second.stack)) {
            hs->second.stack = citems::withCount(hs->second.stack, citems::countOf(hs->second.stack) + citems::countOf(sd->second.stack));
            removeDisplay(sd->second);
            d.stacks.erase(sd);
        } else {
            spawnItem(d.pos, d.pos.x + .5, d.pos.y + 1.5, d.pos.z + .5, sd->second.stack);
            removeDisplay(sd->second);
            d.stacks.erase(sd);
        }
        it      = d.incoming.erase(it);
    }
    if (d.held && citems::isEmpty(heldStack(d))) removeHeld(d);

    if (d.held && cb::depotSlide(*d.held)) {
        // a belt funnel on top (Create's DepotBehaviour.handleBeltFunnelOutput): the outputs
        // first, then the held stack; one transfer per tick
        bool      exported = false;
        GlobalPos above    = offset(d.pos, {0, 1, 0});
        if (auto const* fn = interactorFor(idAt(above))) {
            for (auto& out : d.outputs) {
                if (citems::isEmpty(out)) continue;
                TransportedItem t;
                t.stack          = out;
                t.beltPosition   = .5f;
                int         before = citems::countOf(out);
                PassContext ctx{d.pos, above, 0, 1, 1, true, .5f, true, t};
                (*fn)(ctx);
                if (citems::countOf(t.stack) != before) {
                    out      = t.stack;
                    exported = true;
                    break;
                }
            }
            if (!exported && d.held) {
                auto        heldId = d.held->id;
                auto        api    = toApi(*d.held, d.stacks[heldId]);
                int         before = citems::countOf(api.stack);
                PassContext ctx{d.pos, above, 0, 1, 1, true, .5f, true, api};
                (*fn)(ctx);
                if (d.held && d.held->id == heldId) {
                    exported = citems::countOf(api.stack) != before;
                    fromApi(api, *d.held, d.stacks[heldId]);
                    if (citems::isEmpty(d.stacks[heldId].stack)) removeHeld(d);
                }
            }
        }
        // a processor 2 above (Create's BeltProcessingBehaviour on depots)
        GlobalPos procPos = offset(d.pos, {0, 2, 0});
        if (d.held && !exported) {
            if (auto const* proc = processorFor(idAt(procPos))) {
                if (d.held->locked || !processingBlocked(d.pos)) {
                    auto             heldId    = d.held->id;
                    auto             api       = toApi(*d.held, d.stacks[heldId]);
                    bool             wasLocked = d.held->locked;
                    ProcessorContext ctx{procPos, d.pos, true};
                    auto const&      fn     = wasLocked ? proc->held : proc->received;
                    auto             result = fn ? fn(ctx, api) : ProcessingResult::Pass;
                    // the processor may have replaced the held item (handleCenteredProcessing)
                    bool same = d.held && d.held->id == heldId;
                    if (same) fromApi(api, *d.held, d.stacks[heldId]);
                    if (result == ProcessingResult::Remove) {
                        if (same) removeHeld(d);
                    } else if (d.held) {
                        d.held->locked = same ? result == ProcessingResult::Hold : d.held->locked;
                    }
                }
            } else if (d.held->locked) {
                d.held->locked = false;
            }
        }
    }
    if (d.ejector) tickEjector(d);

    // displays: the held stack on top, the outputs are not shown
    cb::Vec3d centre{d.pos.x + .5, d.pos.y + cb::kBeltTop, d.pos.z + .5};
    if (d.held) {
        auto      from = cb::faceNormal(cb::oppositeFace(d.held->insertedFrom)); // the side it came in through
        double    k    = .5 - d.held->beltPosition; // 0.5 at the edge, 0 at the centre
        cb::Vec3d at   = centre + cb::Vec3d{from.x * k, 0, from.z * k};
        syncDisplay(d.pos, d.stacks[d.held->id], at);
    }
    for (auto& i : d.incoming) {
        auto      from = cb::faceNormal(cb::oppositeFace(i.insertedFrom));
        double    k    = .5 - i.beltPosition;
        syncDisplay(d.pos, d.stacks[i.id], centre + cb::Vec3d{from.x * k, 0, from.z * k});
    }
}

// ---- interaction -------------------------------------------------------------------------------------

bool onDepotUse(Player& player, GlobalPos const& pos, int face, cb::Vec3d const& /*hit*/) {
    if (player.isSneaking()) return false;
    // Create's SharedDepotBlockMethods.onUse: only a click on the top face (blocks can be placed
    // against the sides)
    if (face != 1) return false;
    auto* d = depotFor(pos);
    if (!d) return false;
    if (!depotCanAccept(*d)) return true;
    ItemStack held     = player.getSelectedItem();
    bool      wasEmpty = citems::isEmpty(held) || citems::idOf(held) == "levitate:mechanical_arm"; // shouldntPlaceItem
    int       took     = 0;
    if (!citems::isEmpty(heldStack(*d))) {
        giveToPlayer(player, heldStack(*d));
        removeHeld(*d);
        playSound(pos, "random.pop", .2f, 1.f);
        ++took;
    }
    for (auto& i : d->incoming) {
        if (auto it = d->stacks.find(i.id); it != d->stacks.end()) {
            giveToPlayer(player, it->second.stack);
            removeDisplay(it->second);
            d->stacks.erase(it);
            ++took;
        }
    }
    d->incoming.clear();
    for (auto& o : d->outputs) {
        if (citems::isEmpty(o)) continue;
        giveToPlayer(player, o);
        o = ItemStack();
        ++took;
    }
    if (!wasEmpty) {
        cb::Item it;
        it.id               = newItemId();
        it.beltPosition     = .25f;
        it.prevBeltPosition = .25f;
        it.insertedFrom     = cb::faceOfFacing(game::playerFacing(player)); // travels away from the player
        StackData sd;
        sd.stack = held;
        setHeld(*d, it, std::move(sd));
        player.setSelectedItem(ItemStack());
        player.sendInventory(false);
        playSound(pos, "dig.sand", .125f, 1.5f);
        log().info("belts: {} put {} x{} on the {} at {}", player.getRealName(), citems::idOf(held), citems::countOf(held), d->ejector ? "ejector" : "depot", posText(pos));
    } else if (took) {
        log().info("belts: {} took {} stack(s) from the {} at {}", player.getRealName(), took, d->ejector ? "ejector" : "depot", posText(pos));
    }
    return true;
}

void sendEjectorSelection(Player& player, std::optional<GlobalPos> const& sel) {
    if (!net::isCompanion(player)) return;
    Json msg = Json::object();
    if (sel) msg = {{"pos", Json::array({sel->x, sel->y, sel->z})}, {"dim", sel->dim}, {"max", config().maxEjectorDistance}};
    net::send(player, "belts.ejector", msg);
}

void clearEjectorSelection(Player& player) { sendEjectorSelection(player, std::nullopt); }

bool onEjectorItemUse(Player& player, GlobalPos const& clicked) {
    if (!player.isSneaking()) return false;
    state().ejectorTargets[uuidOf(player)] = clicked;
    sendEjectorSelection(player, clicked);
    actionbar(player, "§6" + trFor(player, "create.weighted_ejector.target_set", "Target Selected"));
    log().info("belts: {} selected the ejector target {}", player.getRealName(), posText(clicked));
    return true;
}

void onEjectorPlaced(GlobalPos const& pos, State::PlaceIntent const& intent) {
    auto& s      = state();
    auto* d      = depotFor(pos);
    auto  level  = ll::service::getLevel();
    Player* player = level ? level->getPlayer(mce::UUID::fromString(intent.uuid)) : nullptr;
    if (!d) return;
    int  facing = intent.lookFacing;
    auto sel    = s.ejectorTargets.find(intent.uuid);
    if (sel == s.ejectorTargets.end()) {
        // Create's EjectorTargetHandler.flushSettings without a selection
        if (player) actionbar(*player, "§f" + trFor(*player, "create.weighted_ejector.no_target", "Ejecting to Adjacent block (No Target was Selected)"));
    } else {
        GlobalPos target = sel->second;
        s.ejectorTargets.erase(sel);
        if (player) sendEjectorSelection(*player, std::nullopt);
        auto f = target.dim == pos.dim ? cb::ejectorTargetFacing(vecOf(pos), vecOf(target), config().maxEjectorDistance) : std::nullopt;
        if (f) {
            facing     = *f;
            d->targetH = std::abs(target.x - pos.x) + std::abs(target.z - pos.z);
            d->targetV = target.y - pos.y;
            if (player)
                actionbar(*player, "§a" + trFor(*player, "create.weighted_ejector.targeting", "Ejecting to [{0},{1},{2}]", target.x, target.y, target.z));
        } else {
            d->targetH = 1;
            d->targetV = 0;
            if (player)
                actionbar(*player, "§f" + trFor(*player, "create.weighted_ejector.target_not_valid", "Ejecting to Adjacent block (Target was not Valid)"));
        }
    }
    if (auto const* b = game::blockWith(kEjectorId, {{kFacing, facing}, {kLid, 0}, {kRotor, 0}})) setBlockQuiet(pos, *b);
    common::blockdata::edit(pos)["depot"]["target"] = Json::array({d->targetH, d->targetV});
    log().info("belts: ejector placed at {} facing {} (target {} ahead, {} up)", posText(pos), facing, d->targetH, d->targetV);
}

// ---- registrations -------------------------------------------------------------------------------------

void registerDepotCapabilities() {
    // Create's DepotItemHandler: slot 0 the held stack, 1..8 the processing outputs
    auto handler = common::items::Handler{
        .slots = [](common::Access const&) { return 9; },
        .getSlot =
            [](common::Access const& a, int slot) {
                auto* d = depotFor(game::globalPos(a.region, a.pos));
                if (!d || slot < 0 || slot > 8) return ItemStack();
                return slot == 0 ? ItemStack(heldStack(*d)) : d->outputs[std::size_t(slot - 1)];
            },
        .insert =
            [](common::Access const& a, ItemStack const& stack, bool simulate) {
                auto* d = depotFor(game::globalPos(a.region, a.pos));
                if (!d) return stack;
                TransportedItem t;
                t.stack = stack;
                return depotInsert(*d, t, a.face >= 0 ? cb::oppositeFace(a.face) : 0, simulate);
            },
        .extract =
            [](common::Access const& a, common::items::ItemPredicate const& filter, int amount, bool simulate) {
                auto* d = depotFor(game::globalPos(a.region, a.pos));
                if (!d || amount <= 0) return ItemStack();
                // outputs first, then the held stack when it isn't being processed
                for (auto& o : d->outputs) {
                    if (citems::isEmpty(o) || (filter && !filter(o))) continue;
                    int  n   = std::min(amount, citems::countOf(o));
                    auto out = citems::withCount(o, n);
                    if (!simulate) {
                        o       = citems::withCount(o, citems::countOf(o) - n);
                    }
                    return out;
                }
                if (!d->held || d->held->locked) return ItemStack();
                auto& sd = d->stacks[d->held->id];
                if (citems::isEmpty(sd.stack) || (filter && !filter(sd.stack))) return ItemStack();
                int  n   = std::min(amount, citems::countOf(sd.stack));
                auto out = citems::withCount(sd.stack, n);
                if (!simulate) {
                    int left = citems::countOf(sd.stack) - n;
                    if (left <= 0) removeHeld(*d);
                    else sd.stack = citems::withCount(sd.stack, left);
                }
                return out;
            },
    };
    common::items::registerBlock(kDepotId, handler);
    common::items::registerBlock(kEjectorId, handler);
    DirectInput input{
        .canInsertFromSide = [](GlobalPos const&, int) { return true; },
        .insert = [](GlobalPos const& p, TransportedItem const& item, int side, bool simulate) {
            auto* d = depotFor(p);
            return d ? depotInsert(*d, item, side, simulate) : item.stack;
        },
    };
    registerDirectInput(kDepotId, input);
    registerDirectInput(kEjectorId, input);
}

void registerEjectorBox() {
    using core::common::ValueSettings;
    common::valuebox::registerBox(
        kEjectorId,
        common::valuebox::BoxSpec{
            .id            = "stack_size",
            .label         = "create.weighted_ejector.stack_size",
            .labelFallback = "Ejected Stack Size",
            .board         = [](Block const&) { return core::common::plainBoard(64, 16, ""); },
            .centre =
                [](Block const& b, int face) -> std::optional<core::common::Vec> {
                    // Create's EjectorSlot: the front and back faces, 6 px up
                    int f = game::intState(b, kFacing).value_or(0) & 3;
                    if (face < 2 || cb::facingAxis(*cb::facingOfFace(face)) != cb::facingAxis(f)) return std::nullopt;
                    return core::common::faceLocalPoint(face, 8, 6, .5);
                },
            .scale = .5,
            .get =
                [](GlobalPos const& p) {
                    auto* d = depotFor(p);
                    return core::common::plainToSettings(d ? d->stackSize : 0);
                },
            .set =
                [](GlobalPos const& p, ValueSettings v, Player& editor) {
                    auto* d = depotFor(p);
                    if (!d) return;
                    d->stackSize = core::common::plainFromSettings(v, 0, 64);
                    common::blockdata::edit(p)["depot"]["stackSize"] = d->stackSize;
                    log().info("belts: {} set the ejector at {} to stack size {}", editor.getRealName(), posText(p), d->stackSize);
                },
            .format = [](Block const&, ValueSettings v) { return v.value == 0 ? std::string("*") : std::to_string(v.value); },
        }
    );
}

} // namespace lv::feature::belts
