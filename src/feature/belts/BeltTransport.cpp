// Belts unit: items riding belts (Create's BeltInventory with the world around it), items and
// entities landing on / standing on belts (BeltBlock.entityInside, BeltMovementHandler), inserting
// from the sides (DirectBeltInputBehaviour) and the item capability of a segment.

#include <algorithm>
#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/phys/AABB.h"

#include "feature/belts/BeltsInternal.h"
#include "feature/common/FunnelsExt.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::belts {

namespace citems = common::items;

float beltSpeedRpm(Belt const& belt) { return kinetics::speedAt(belt.controller); }

namespace {

cb::SegmentState controllerState(Belt const& belt) { return segmentAt(belt.controller).value_or(cb::SegmentState{}); }

GlobalPos segmentPos(Belt const& belt, cb::SegmentState const& cs, int index) {
    return offset(belt.controller, cb::beltVector(cs) * index);
}

/// The world as one belt's inventory sees it during a tick.
class BeltEnv : public cb::InventoryEnv {
public:
    BeltEnv(Belt& belt) : mBelt(belt), mState(controllerState(belt)) {
        mRpm   = beltSpeedRpm(belt);
        mSpeed = cb::directionAwareSpeed(mRpm, mState.facing);
        mFace  = cb::faceOfFacing(cb::movementFacing(mRpm, mState.facing));
    }

    float speed() const override { return mSpeed; }
    int   length() const override { return mBelt.length; }
    bool  horizontal() const override { return mState.slope == cb::Slope::Horizontal; }
    bool  emptyStack(cb::Item const& item) const override {
        auto it = mBelt.stacks.find(item.id);
        return it == mBelt.stacks.end() || citems::isEmpty(it->second.stack);
    }

    /// The block after the end the items move to (Create's getPositionForOffset).
    GlobalPos nextPosition() const {
        bool positive = mSpeed > 0;
        return offset(mBelt.controller, cb::beltVector(mState) * (positive ? mBelt.length : -1));
    }
    /// For slopes, the block level with the last segment also takes items (a horizontal belt
    /// continuing at the top / bottom of a slope).
    std::optional<GlobalPos> levelNextPosition() const {
        if (mState.slope != cb::Slope::Upward && mState.slope != cb::Slope::Downward) return std::nullopt;
        bool      positive = mSpeed > 0;
        auto      v        = cb::beltVector(mState);
        GlobalPos last     = segmentPos(mBelt, mState, positive ? mBelt.length - 1 : 0);
        cb::Vec3i h{v.x, 0, v.z};
        return offset(last, positive ? h : cb::Vec3i{-h.x, 0, -h.z});
    }
    std::optional<GlobalPos> inputTarget() const {
        GlobalPos n = nextPosition();
        if (hasDirectInput(n)) return n;
        if (auto l = levelNextPosition(); l && hasDirectInput(*l)) return *l;
        return std::nullopt;
    }

    cb::Ending resolveEnding() override {
        if (inputTarget()) return cb::Ending::Insert;
        GlobalPos n = nextPosition();
        auto*     r = regionOf(n.dim);
        if (!r || !loadedAt(n)) return cb::Ending::Blocked;
        return game::replaceable(*r, game::blockPos(n)) ? cb::Ending::Eject : cb::Ending::Blocked;
    }

    cb::EndInsert insertAtEnd(cb::Item& item) override {
        auto target = inputTarget();
        if (!target) return cb::EndInsert::Unchanged;
        auto& sd   = mBelt.stacks[item.id];
        auto  api  = toApi(item, sd);
        auto  rest = insertDirect(*target, api, mFace, false);
        if (citems::countOf(rest) == citems::countOf(sd.stack) && citems::sameItemSameData(rest, sd.stack)) return cb::EndInsert::Unchanged;
        sd.stack = rest;
        if (citems::isEmpty(rest)) {
            removeDisplay(sd);
            mBelt.stacks.erase(item.id);
            return cb::EndInsert::Consumed;
        }
        return cb::EndInsert::Partial;
    }

    void eject(cb::Item& item) override {
        auto it = mBelt.stacks.find(item.id);
        if (it == mBelt.stacks.end()) return;
        // Create's BeltInventory.eject: off the end along the belt, a little upwards
        auto   pos   = itemWorldPos(mBelt, item);
        auto   v     = cb::beltVector(mState);
        double sign  = mSpeed > 0 ? 1 : -1;
        double speed = std::max(std::abs(double(cb::beltMovementSpeed(mRpm))), 1 / 8.0);
        spawnItem(mBelt.controller, pos.x, pos.y + 1 / 16.0, pos.z, it->second.stack, v.x * sign * speed, v.y * sign * speed + 1 / 8.0, v.z * sign * speed);
        removeDisplay(it->second);
        mBelt.stacks.erase(it);
    }

    bool processing(cb::Item& item, float nextOffset, bool noMovement) override {
        auto ctxFor = [&](int segment, Processor const*& proc) -> std::optional<ProcessorContext> {
            if (segment < 0 || segment >= mBelt.length) return std::nullopt;
            GlobalPos seg  = segmentPos(mBelt, mState, segment);
            GlobalPos above = offset(seg, {0, 2, 0});
            proc            = processorFor(idAt(above));
            if (!proc) return std::nullopt;
            return ProcessorContext{above, seg, false};
        };
        auto run = [&](auto const& fn, ProcessorContext const& c) {
            auto& sd     = mBelt.stacks[item.id];
            auto  api    = toApi(item, sd);
            auto  result = fn(c, api);
            fromApi(api, item, sd);
            return result;
        };
        int current = int(item.beltPosition);
        if (item.locked) {
            Processor const* proc = nullptr;
            auto             c    = ctxFor(current, proc);
            if (!c || !proc->held) {
                item.locked = false;
                return false;
            }
            auto result = run(proc->held, *c);
            if (result == ProcessingResult::Remove) return removeItem(item);
            if (result == ProcessingResult::Hold) return false;
            item.locked = false;
            return false;
        }
        if (noMovement) return false;
        bool positive = mSpeed > 0;
        if (item.beltPosition > .5f || positive) {
            int first = int(item.beltPosition + (positive ? .5f : -.5f));
            int step  = positive ? 1 : -1;
            for (int segment = first; positive ? segment + .5f <= nextOffset : segment + .5f >= nextOffset; segment += step) {
                Processor const* proc = nullptr;
                auto             c    = ctxFor(segment, proc);
                if (!c || !proc->received) continue;
                if (processingBlocked(c->transporter)) continue;
                auto result = run(proc->received, *c);
                if (result == ProcessingResult::Remove) return removeItem(item);
                if (result == ProcessingResult::Hold) {
                    item.beltPosition = segment + .5f + (positive ? 1 / 512.f : -1 / 512.f);
                    item.locked       = true;
                    return false;
                }
            }
        }
        return false;
    }

    bool funnels(cb::Item& item, float nextOffset) override {
        bool positive = mSpeed > 0;
        int  first    = std::clamp(int(std::floor(item.beltPosition)), 0, mBelt.length - 1);
        int  step     = positive ? 1 : -1;
        for (int segment = first; positive ? segment <= nextOffset : segment + 1 >= nextOffset; segment += step) {
            if (segment < 0 || segment >= mBelt.length) break;
            GlobalPos     seg   = segmentPos(mBelt, mState, segment);
            GlobalPos     above = offset(seg, {0, 1, 0});
            PassFn const* fn    = interactorFor(idAt(above));
            if (!fn) continue;
            auto&       sd  = mBelt.stacks[item.id];
            auto        api = toApi(item, sd);
            PassContext ctx{seg, above, segment, mBelt.length, mFace, positive, nextOffset, false, api};
            auto        r = (*fn)(ctx);
            fromApi(api, item, sd);
            if (citems::isEmpty(sd.stack)) {
                removeDisplay(sd);
                return true; // removed next tick (empty stack)
            }
            if (r == PassResult::Stop) return true;
        }
        return false;
    }

    /// Create's BeltTunnelInteractionHandler.flapTunnelsAndCheckIfStuck: an item crossing into
    /// another segment is reported to the block above it (tunnels, beltlink in FunnelsExt.h).
    bool obstacles(cb::Item& item, float nextOffset) override {
        bool positive = mSpeed > 0;
        int  current  = int(item.beltPosition);
        int  upcoming = int(nextOffset);
        if (!positive && nextOffset == 0) upcoming = -1;
        if (current == upcoming) return false;
        GlobalPos seg    = segmentPos(mBelt, mState, upcoming);
        auto*     region = regionOf(seg.dim);
        if (!region) return false;
        auto it = mBelt.stacks.find(item.id);
        if (it == mBelt.stacks.end()) return false;
        auto& sd      = it->second;
        auto  passing = common::beltlink::itemEntering(*region, game::blockPos(seg), sd.stack, mFace);
        if (passing == common::beltlink::Passing::Stuck) {
            item.beltPosition = float(current) + (positive ? .99f : .01f);
            return true;
        }
        if (passing == common::beltlink::Passing::Taken || citems::isEmpty(sd.stack)) {
            removeDisplay(sd);
            sd.stack = ItemStack();
            return true; // removed next tick (empty stack)
        }
        return false;
    }

    bool removeItem(cb::Item& item) {
        if (auto it = mBelt.stacks.find(item.id); it != mBelt.stacks.end()) {
            removeDisplay(it->second);
            mBelt.stacks.erase(it);
        }
        return true;
    }

    float rpm() const { return mRpm; }
    int   face() const { return mFace; }
    cb::SegmentState const& seg() const { return mState; }

private:
    Belt&            mBelt;
    cb::SegmentState mState;
    float            mRpm   = 0;
    float            mSpeed = 0;
    int              mFace  = 2;
};

// ---- entities --------------------------------------------------------------------------------------

/// The belt segment an entity stands on (its feet block or the one below).
std::optional<std::pair<GlobalPos, int>> segmentUnder(Belt const& belt, cb::SegmentState const& cs, Vec3 const& feet, int dim) {
    for (int dy : {0, -1}) {
        GlobalPos p{dim, int(std::floor(feet.x)), int(std::floor(feet.y)) + dy, int(std::floor(feet.z))};
        for (int i = 0; i < belt.length; ++i) {
            if (segmentPos(belt, cs, i) == p) return std::pair{p, i};
        }
    }
    return std::nullopt;
}

void sendMotion(Player& player, Vec3 const& motion) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SetActorMotion);
    if (!packet) return;
    auto& pkt      = static_cast<SetActorMotionPacket&>(*packet);
    pkt.mRuntimeId = player.getRuntimeID();
    pkt.mMotion    = motion;
    pkt.sendTo(player);
}

/// Create's BeltMovementHandler.transportEntity.
void transportEntity(Belt& belt, BeltEnv const& env, Actor& entity, GlobalPos const& seg) {
    auto state = segmentAt(seg);
    if (!state) return;
    float rpm = env.rpm();
    if (std::abs(rpm) < 1) return; // too slow
    Vec3 const& feet = entity.getFeetPos();
    if (feet.y - .25f < seg.y) return; // not on top
    bool isPlayer = entity.isPlayer();
    if (isPlayer) {
        auto& p = static_cast<Player&>(entity);
        if (p.isSneaking() || p.isFlying()) return;
    }
    int        beltFacing = state->facing;
    auto       slope      = state->slope;
    auto       axis       = cb::facingAxis(beltFacing);
    float      movementSpeed = cb::beltMovementSpeed(rpm);
    int        movementDir   = axis == cb::Axis::X ? 1 /*west*/ : 0 /*south*/;
    cb::Vec3i  mv            = cb::facingNormal(movementDir);
    double     mx = mv.x * movementSpeed, my = 0, mz = mv.z * movementSpeed;
    double     diffCenter = axis == cb::Axis::Z ? (seg.x + .5 - feet.x) : (seg.z + .5 - feet.z);
    if (std::abs(diffCenter) > 48 / 64.0) return;
    int  movementFacing = cb::movementFacing(rpm, beltFacing);
    auto part           = state->part;
    float top           = 13 / 16.f;
    bool notHorizontal  = slope != cb::Slope::Horizontal;
    double rel          = feet.y - seg.y;
    bool onSlope        = notHorizontal
                  && (part == cb::Part::Middle || part == cb::Part::Pulley
                      || (part == (slope == cb::Slope::Upward ? cb::Part::End : cb::Part::Start) && rel < top)
                      || (part == (slope == cb::Slope::Upward ? cb::Part::Start : cb::Part::End) && rel > top));
    bool movingDown = onSlope && slope == (movementFacing == beltFacing ? cb::Slope::Downward : cb::Slope::Upward);
    bool movingUp   = onSlope && slope == (movementFacing == beltFacing ? cb::Slope::Upward : cb::Slope::Downward);
    // (Create swaps these on the z axis to undo its own movement facing there; ours is the real one)
    double along = axis == cb::Axis::X ? mx : mz;
    if (movingUp) my += std::abs(along);
    if (movingDown) my -= std::abs(along);
    // centering towards the middle of the belt (not while the entity walks)
    cb::Vec3i centeringDir = cb::facingNormal(cb::clockwise(beltFacing));
    centeringDir           = {std::abs(centeringDir.x), 0, std::abs(centeringDir.z)};
    double centering       = diffCenter * std::min(std::abs(movementSpeed), .1f) * 4;
    bool   walking         = isPlayer && entity.isMoving();
    if (!walking) {
        mx += centeringDir.x * centering;
        mz += centeringDir.z * centering;
    }
    Vec3 motion{float(mx), float(my), float(mz)};
    if (movingUp) motion.y = float(std::max(std::abs(my), .13));
    if (isPlayer && !entity.isSimulatedPlayer()) {
        // real players move themselves: push them along
        entity.mBuiltInComponents->mStateVectorComponent->mPosDelta = motion;
        sendMotion(static_cast<Player&>(entity), motion);
    } else {
        entity.move(motion);
    }
    (void)belt;
}

} // namespace

// ---- tick --------------------------------------------------------------------------------------------

void tickBelt(Belt& belt) {
    BeltEnv env(belt);
    auto    cs = env.seg();
    if (!cb::canTransportObjects(cs.slope)) {
        if (!belt.inv.empty()) ejectAll(belt);
        return;
    }
    belt.inv.tick(env);

    auto* region = regionOf(belt.controller.dim);
    auto  level  = ll::service::getLevel();
    if (!region || !level) return;

    // the belt's box (all segments), a little above
    double lx = 1e9, ly = 1e9, lz = 1e9, hx = -1e9, hy = -1e9, hz = -1e9;
    for (int i = 0; i < belt.length; ++i) {
        GlobalPos p = segmentPos(belt, cs, i);
        lx = std::min(lx, double(p.x)), ly = std::min(ly, double(p.y)), lz = std::min(lz, double(p.z));
        hx = std::max(hx, p.x + 1.0), hy = std::max(hy, p.y + 1.0), hz = std::max(hz, p.z + 1.0);
    }
    AABB box{Vec3{float(lx), float(ly), float(lz)}, Vec3{float(hx), float(hy + 1.5), float(hz)}};

    // items and mobs
    std::vector<Actor*> actors = region->getEntities(box, 0.f);
    level->forEachPlayer([&](Player& p) {
        if (int(p.getDimensionId()) != belt.controller.dim) return true;
        auto f = p.getFeetPos();
        if (f.x >= lx - .5 && f.x <= hx + .5 && f.y >= ly - .5 && f.y <= hy + 1.5 && f.z >= lz - .5 && f.z <= hz + .5) actors.push_back(&p);
        return true;
    });
    for (auto* a : actors) {
        if (!a || !a->isAlive() || isDisplayActor(*a)) continue;
        auto under = segmentUnder(belt, cs, a->getFeetPos(), belt.controller.dim);
        if (!under) continue;
        auto [seg, index] = *under;
        if (a->getEntityTypeId() == ActorType::ItemEntity) {
            // Create's entityInside for items: dropped items land on the belt
            auto& item = static_cast<ItemActor&>(*a);
            if (a->getPosDelta().y > 0) continue;
            if (a->getFeetPos().y - seg.y > cb::kBeltTop + .3) continue;
            if (common::beltlink::hasAbove(*region, game::blockPos(seg))) continue; // a tunnel on the segment
            ItemStack stack = item.mItem;
            ItemStack rest  = insertOnSegment(seg, stack, false);
            if (citems::isEmpty(rest)) a->remove();
            else if (citems::countOf(rest) != citems::countOf(stack)) item.mItem = rest;
            continue;
        }
        transportEntity(belt, env, *a, seg);
    }

    // display entities
    for (auto& it : belt.inv.items) {
        auto sd = belt.stacks.find(it.id);
        if (sd == belt.stacks.end()) continue;
        auto p = itemWorldPos(belt, it);
        syncDisplay(belt.controller, sd->second, p);
    }
    // stacks that left the inventory (taken by the API) lose their display
    for (auto it = belt.stacks.begin(); it != belt.stacks.end();) {
        if (!belt.inv.find(it->first)) {
            removeDisplay(it->second);
            it = belt.stacks.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- insertion / extraction ----------------------------------------------------------------------------

ItemStack insertOnSegment(GlobalPos const& segment, ItemStack const& stack, bool simulate) {
    if (citems::isEmpty(stack)) return stack;
    auto seg = segmentAt(segment);
    if (!seg || !cb::canTransportObjects(seg->slope)) return stack;
    auto  info = segmentInfo(segment);
    auto* belt = beltOfSegment(segment);
    if (!info || !belt) return stack;
    auto  cs   = controllerState(*belt);
    float rpm  = beltSpeedRpm(*belt);
    int   face = cb::faceOfFacing(cb::movementFacing(rpm, cs.facing));
    if (!belt->inv.canInsertAt(info->index, face)) return stack;
    if (simulate) return ItemStack();
    cb::Item it;
    it.id               = newItemId();
    it.insertedAt       = info->index;
    it.insertedFrom     = 1;
    it.beltPosition     = info->index + .5f + (belt->inv.movementPositive ? -1 : 1) / 16.f;
    it.prevBeltPosition = it.beltPosition;
    belt->stacks[it.id].stack = stack;
    belt->inv.addItem(it);
    return ItemStack();
}

ItemStack extractFromSegment(GlobalPos const& segment, int amount, std::function<bool(ItemStack const&)> const& filter, bool simulate) {
    auto  info = segmentInfo(segment);
    auto* belt = beltOfSegment(segment);
    if (!info || !belt || amount <= 0) return ItemStack();
    auto* it = belt->inv.stackAtOffset(info->index);
    if (!it) return ItemStack();
    auto sd = belt->stacks.find(it->id);
    if (sd == belt->stacks.end() || citems::isEmpty(sd->second.stack)) return ItemStack();
    if (filter && !filter(sd->second.stack)) return ItemStack();
    int  n   = std::min(amount, citems::countOf(sd->second.stack));
    auto out = citems::withCount(sd->second.stack, n);
    if (simulate) return out;
    int left = citems::countOf(sd->second.stack) - n;
    if (left <= 0) {
        belt->inv.toRemove.push_back(it->id);
        removeDisplay(sd->second);
        sd->second.stack = ItemStack();
    } else {
        sd->second.stack = citems::withCount(sd->second.stack, left);
    }
    return out;
}

bool canInsertFromSide(GlobalPos const& segment, int side) {
    auto seg = segmentAt(segment);
    if (!seg || !cb::canTransportObjects(seg->slope)) return false;
    auto  info = segmentInfo(segment);
    auto* belt = beltOfSegment(segment);
    if (!info || !belt) return false;
    float rpm = beltSpeedRpm(*belt);
    if (rpm == 0) return false;
    int movementFace = cb::faceOfFacing(cb::movementFacing(rpm, controllerState(*belt).facing));
    return movementFace != cb::oppositeFace(side);
}

ItemStack insertFromSide(GlobalPos const& segment, TransportedItem const& item, int side, bool simulate) {
    auto  info = segmentInfo(segment);
    auto* belt = beltOfSegment(segment);
    if (!info || !belt) return item.stack;
    float rpm = beltSpeedRpm(*belt);
    auto  cs  = controllerState(*belt);
    if (rpm == 0) return item.stack;
    int movementFace = cb::faceOfFacing(cb::movementFacing(rpm, cs.facing));
    // a distributing brass tunnel above takes the item instead (Create's tryInsertingFromSide)
    if (auto* region = regionOf(segment.dim)) {
        if (auto taken = common::beltlink::sideInputAbove(*region, game::blockPos(segment), item.stack, side, simulate)) return *taken;
    }
    // Create's isOccupied
    if (movementFace == cb::oppositeFace(side)) return item.stack;
    if (!belt->inv.canInsertAtFromSide(info->index, side, movementFace)) return item.stack;
    if (simulate) return ItemStack();
    float    dirSpeed = cb::directionAwareSpeed(rpm, cs.facing);
    cb::Item it;
    it.id           = newItemId();
    it.beltPosition = info->index + .5f - (dirSpeed > 0 ? 1.f : dirSpeed < 0 ? -1.f : 0.f) / 16.f;
    if (side != 0 && side != 1) {
        if (movementFace != side) {
            // from the side: starts off-centre and slides to the middle
            auto  sf = cb::facingOfFace(side).value_or(0);
            float so = cb::facingStep(sf) * .35f;
            if (cb::facingAxis(sf) == cb::Axis::X) so *= -1;
            it.sideOffset = so;
        } else {
            it.beltPosition = dirSpeed > 0 ? float(info->index) : float(info->index + 1);
        }
    }
    it.prevSideOffset   = it.sideOffset;
    it.insertedAt       = info->index;
    it.insertedFrom     = side;
    it.prevBeltPosition = it.beltPosition;
    auto& sd            = belt->stacks[it.id];
    sd.stack            = item.stack;
    sd.processingTime   = item.processingTime;
    sd.processedBy      = item.processedBy;
    sd.extra            = item.extra;
    belt->inv.addItem(it);
    return ItemStack();
}

bool applyOnBelt(Belt& belt, float position, float maxDistance, ProcessFn const& fn) {
    bool dirty    = false;
    bool positive = belt.inv.movementPositive;
    for (auto& it : belt.inv.items) {
        if (std::find(belt.inv.toRemove.begin(), belt.inv.toRemove.end(), it.id) != belt.inv.toRemove.end()) continue;
        if (std::abs(position - it.beltPosition) >= maxDistance) continue;
        auto sd = belt.stacks.find(it.id);
        if (sd == belt.stacks.end()) continue;
        auto api    = toApi(it, sd->second);
        auto result = fn(api);
        if (result.kind == TransportedResult::Kind::DoNothing) {
            // processors may still have updated their own fields
            sd->second.processingTime = api.processingTime;
            sd->second.processedBy    = api.processedBy;
            sd->second.extra          = api.extra;
            continue;
        }
        dirty = true;
        if (result.heldOutput && !citems::isEmpty(*result.heldOutput)) {
            cb::Item held      = it;
            held.id            = newItemId();
            held.beltPosition  = float(int(position)) + .5f - (positive ? 1 / 512.f : -1 / 512.f);
            held.locked        = true;
            belt.stacks[held.id].stack = *result.heldOutput;
            belt.inv.toInsert.push_back(held);
        }
        for (auto const& out : result.outputs) {
            if (citems::isEmpty(out)) continue;
            cb::Item o   = it;
            o.id         = newItemId();
            o.locked     = false;
            belt.stacks[o.id].stack = out;
            belt.inv.toInsert.push_back(o);
        }
        belt.inv.toRemove.push_back(it.id);
    }
    return dirty;
}

} // namespace lv::feature::belts
