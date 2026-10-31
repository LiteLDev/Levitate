// Portable storage interface (Create's PortableStorageInterfaceMovement / BlockEntity /
// PortableItemInterfaceBlockEntity).
//
// A PSI carried by a contraption looks 1.85 blocks ahead of its front: when a stationary PSI
// of the same kind faces it there (or one block further) and is not powered, the stationary
// one connects. While connected (transfer timer between 4 and psiTimeout + 4) it exposes the
// contraption's merged inventories through the item capability (funnels, chutes, arms, ... and
// vanilla hoppers pointing into it / below it); every transfer restarts the timeout. The
// moving PSI holds the contraption once its active point passes the stationary PSI's centre
// until the timer runs out (psiTimeout ticks after the last transfer). Redstone power
// disconnects / prevents a connection.

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/service/Bedrock.h"

#include "mc/world/Container.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"

#include "feature/actors/Actors.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/contraption/ActorsExt.h"
#include "game/Blocks.h"
#include "mod/Config.h"

namespace lv::feature::actors {

namespace ext  = ::lv::feature::contraption::actors_ext;
namespace caps = ::lv::feature::common;

namespace {

constexpr int    kAnimation    = 4;
constexpr double kActiveOffset = 1.85;

struct PsiState {
    int                    timer     = 0;
    float                  distance  = 0;
    std::optional<con::Id> connected;
    int                    keepAlive = 0;
    bool                   powered   = false;
    std::optional<con::Id> announced; ///< contraption whose connection was logged
};

std::map<GlobalPos, PsiState>& states() {
    static std::map<GlobalPos, PsiState> s;
    return s;
}

int timeout() { return std::max(1, config::get<Config>().psiTimeout); }

bool isConnected(PsiState const& s) { return s.timer >= kAnimation && s.timer <= timeout() + kAnimation; }

bool canTransfer(PsiState& s) {
    if (s.connected && !con::find(*s.connected)) s.connected.reset();
    return s.connected && isConnected(s);
}

void onContentTransferred(PsiState& s) {
    s.timer = timeout() + kAnimation;
    for (auto const& [pos, st] : states())
        if (&st == &s) advancements::awardOwner(pos, "portable_storage_interface");
}

void startTransferringTo(GlobalPos const& pos, PsiState& s, con::Id id, float distance) {
    if (s.connected == id) return;
    s.distance  = std::min(2.f, distance);
    s.connected = id;
    s.timer     = 6 + kAnimation * 2;
    if (s.announced == id) return; // the idle connection cycles while the contraption stays
    s.announced = id;
    logAction("psi at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z) + " connected to contraption #"
              + std::to_string(id));
}

/// `left`: the contraption went away (or a redstone signal ended it), not just an idle cycle.
void stopTransferring(GlobalPos const& pos, PsiState& s, bool left = false) {
    if (!s.connected && !(left && s.announced)) return;
    s.connected.reset();
    if (!left || !s.announced) return;
    s.announced.reset();
    logAction("psi at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z) + " disconnected");
}

/// The state of a stationary PSI that still exists (nullptr otherwise).
PsiState* stationary(GlobalPos const& pos) {
    auto it = states().find(pos);
    return it == states().end() ? nullptr : &it->second;
}

/// The world facing of a carried PSI when it is axis aligned (Create: off by less than half).
std::optional<cc::Dir> alignedFacing(con::Contraption const& c, cc::States const& states) {
    auto v = c.transform().rotateVector(cc::Vec3d::of(cc::offset(facingOf(states))));
    for (auto d : cc::kAllDirs) {
        if ((cc::Vec3d::of(cc::offset(d)) - v).length() < .5) return d;
    }
    return std::nullopt;
}

struct Found {
    GlobalPos pos;
    cc::Dir   facing;
    cc::Vec3d active;
};

/// Create's findInterface: a stationary PSI facing back at the visited block or one further.
std::optional<Found> findInterface(con::Contraption const& c, con::CarriedBlock const& b) {
    auto facing = alignedFacing(c, b.states);
    if (!facing) return std::nullopt;
    auto* region = regionOf(c.dimension());
    if (!region) return std::nullopt;
    cc::Vec3d center = c.toWorld(cc::Vec3d::center(b.local));
    cc::Vec3d active = center + cc::Vec3d::of(cc::offset(*facing)) * kActiveOffset;
    cc::Pos   visited{int(std::floor(active.x)), int(std::floor(active.y)), int(std::floor(active.z))};
    for (int i = 0; i < 2; ++i) {
        cc::Pos p  = visited + cc::offset(*facing) * i;
        auto    bp = toBlockPos(p);
        if (!region->hasChunksAt(bp, 0, false)) continue;
        if (region->getBlock(bp).getTypeName() != kPsi) continue;
        if (facingAt(*region, bp) != cc::opposite(*facing)) continue;
        if (ext::powered(*region, p)) continue;
        return Found{{c.dimension(), p.x, p.y, p.z}, *facing, active};
    }
    return std::nullopt;
}

/// The moving side, every tick for every contraption (Create ticks actors also when still).
void connectMoving() {
    for (auto id : con::all()) {
        auto* c = con::find(id);
        if (!c) continue;
        for (auto const& b : c->blocks()) {
            if (b.id != kPsi) continue;
            auto found = findInterface(*c, b);
            if (!found) continue;
            auto& s = states()[found->pos];
            if (s.connected && !con::find(*s.connected)) s.connected.reset();
            cc::Vec3d target = cc::Vec3d::center(cc::Pos{found->pos.x, found->pos.y, found->pos.z});
            cc::Vec3d axis   = cc::Vec3d::of(cc::offset(found->facing));
            cc::Vec3d diff   = target - found->active;
            float     dist   = static_cast<float>(diff.x * axis.x + diff.y * axis.y + diff.z * axis.z + kActiveOffset - 1);
            if (!s.connected) startTransferringTo(found->pos, s, id, dist);
            if (s.connected == id) s.keepAlive = 2;
        }
    }
}

/// Vanilla hoppers next to a connected PSI: one pointing into it pushes, one below pulls (the
/// hopper rules Java hoppers follow with Create's interface), one item per 8 ticks.
void hoppers(GlobalPos const& pos, PsiState& s) {
    auto* region = regionOf(pos.dim);
    auto* c      = s.connected ? con::find(*s.connected) : nullptr;
    if (!region || !c) return;
    for (auto d : cc::kAllDirs) {
        cc::Pos n  = cc::Pos{pos.x, pos.y, pos.z} + cc::offset(d);
        auto    bp = toBlockPos(n);
        auto const& block = region->getBlock(bp);
        if (block.getTypeName() != "minecraft:hopper") continue;
        if (game::intState(block, "toggle_bit").value_or(0)) continue; // locked by redstone
        auto container = region->tryGetContainer(bp);
        if (!container) continue;
        int  hopperFacing = game::intState(block, "facing_direction").value_or(0);
        bool pushesIn     = static_cast<int>(cc::opposite(d)) == hopperFacing;
        if (pushesIn) {
            for (int i = 0; i < container->getContainerSize(); ++i) {
                ItemStack item = container->getItem(i);
                if (item.isNull() || item.mCount == 0) continue;
                ItemStack one = item;
                one.mCount    = 1;
                auto rest     = c->storage().insertItem(one, false);
                if (!rest.isNull() && rest.mCount > 0) continue;
                container->removeItem(i, 1);
                onContentTransferred(s);
                break;
            }
        } else if (d == cc::Dir::Down) {
            // a hopper below pulls from the interface
            for (int i = 0; i < c->storage().slotCount(); ++i) {
                ItemStack const& item = c->storage().slot(i);
                if (item.isNull() || item.mCount == 0) continue;
                ItemStack one = item;
                one.mCount    = 1;
                if (!container->hasRoomForItem(one)) continue;
                auto got = c->storage().extractItem([&](ItemStack const& x) { return common::items::sameItemSameData(x, one); }, 1, false);
                if (got.isNull() || got.mCount == 0) continue;
                if (!container->addItem(got)) {
                    c->storage().insertItem(got, false); // no room after all: put it back
                    continue;
                }
                onContentTransferred(s);
                break;
            }
        }
    }
}

void tickStationary(std::uint64_t tick) {
    for (auto it = states().begin(); it != states().end();) {
        auto const& pos    = it->first;
        auto&       s      = it->second;
        auto*       region = regionOf(pos.dim);
        auto        bp     = toBlockPos(pos);
        if (!region || !region->hasChunksAt(bp, 0, false)) {
            ++it;
            continue;
        }
        if (region->getBlock(bp).getTypeName() != kPsi) {
            it = states().erase(it);
            continue;
        }
        bool powered = ext::powered(*region, {pos.x, pos.y, pos.z});
        if (powered != s.powered) {
            s.powered = powered;
            if (powered) stopTransferring(pos, s, true);
        }
        s.timer = std::min(s.timer, timeout() + kAnimation * 2);
        bool expired = false;
        if (s.keepAlive > 0 && --s.keepAlive == 0) {
            stopTransferring(pos, s, true);
            s.timer = 3;
            expired = true;
        }
        if (!expired && s.timer > 0) --s.timer;
        if (s.timer == 0 || s.powered) stopTransferring(pos, s);
        if (canTransfer(s) && tick % 8 == 0) hoppers(pos, s);
        setIntState(*region, bp, "levitate:extended", canTransfer(s) ? 1 : 0);
        if (!s.connected && s.timer == 0 && s.keepAlive == 0) {
            it = states().erase(it);
            continue;
        }
        ++it;
    }
}

/// Item capability of a stationary PSI: the connected contraption's storage.
caps::items::Handler handler() {
    auto stateAt = [](caps::Access const& a) -> PsiState* {
        GlobalPos p{static_cast<int>(a.region.getDimensionId()), a.pos.x, a.pos.y, a.pos.z};
        auto*     s = stationary(p);
        return s && canTransfer(*s) ? s : nullptr;
    };
    caps::items::Handler h;
    h.slots = [stateAt](caps::Access const& a) {
        auto* s = stateAt(a);
        return s ? con::find(*s->connected)->storage().slotCount() : 0;
    };
    h.getSlot = [stateAt](caps::Access const& a, int slot) {
        auto* s = stateAt(a);
        if (!s) return ItemStack::EMPTY_ITEM();
        auto& st = con::find(*s->connected)->storage();
        return slot >= 0 && slot < st.slotCount() ? st.slot(slot) : ItemStack::EMPTY_ITEM();
    };
    h.insert = [stateAt](caps::Access const& a, ItemStack const& stack, bool simulate) {
        auto* s = stateAt(a);
        if (!s) return stack;
        auto rest = con::find(*s->connected)->storage().insertItem(stack, simulate);
        int  restCount = rest.isNull() ? 0 : rest.mCount;
        if (!simulate && restCount != stack.mCount) onContentTransferred(*s);
        return rest;
    };
    h.extract = [stateAt](caps::Access const& a, caps::items::ItemPredicate const& filter, int amount, bool simulate) {
        auto* s = stateAt(a);
        if (!s) return ItemStack::EMPTY_ITEM();
        auto got = con::find(*s->connected)->storage().extractItem(filter, amount, simulate);
        if (!simulate && !got.isNull() && got.mCount > 0) onContentTransferred(*s);
        return got;
    };
    return h;
}

/// The moving PSI's actor: holds the contraption once its active point passes the stationary
/// PSI's centre, until the stationary timer drops to the animation length.
void actorTick(con::MovementContext& ctx) {
    auto const* b = ext::carriedAt(ctx.contraption, ctx.localPos);
    if (!b) return;
    auto found = findInterface(ctx.contraption, *b);
    if (!found) {
        ctx.data.erase("PsiStall");
        return;
    }
    auto* s = stationary(found->pos);
    if (!s || s->connected != ctx.contraption.id()) return;
    if (ctx.data.value("PsiStall", false)) {
        if (s->timer <= kAnimation) {
            ctx.data.erase("PsiStall");
            ctx.data["PsiDone"] = Json::array({found->pos.x, found->pos.y, found->pos.z});
            return;
        }
        ctx.stall = true;
        return;
    }
    if (ctx.data.contains("PsiDone") && ctx.data["PsiDone"] == Json::array({found->pos.x, found->pos.y, found->pos.z})) return;
    // the active point of the attempted position vs. the stationary centre along the facing
    cc::Vec3d axis   = cc::Vec3d::of(cc::offset(found->facing));
    cc::Vec3d target = cc::Vec3d::center(cc::Pos{found->pos.x, found->pos.y, found->pos.z});
    auto      along  = [&](cc::Vec3d const& v) { return v.x * axis.x + v.y * axis.y + v.z * axis.z; };
    // (the contraption's transform is still the current one: the attempted move is ctx.motion)
    double    before = along(found->active - target);
    double    next   = along(found->active + ctx.motion - target);
    if (before < 0 && next >= 0 && s->timer > kAnimation) {
        ctx.data["PsiStall"] = true;
        ctx.stall            = true;
        logAction("psi holds contraption #" + std::to_string(ctx.contraption.id()));
    }
}

struct Registrar {
    Registrar() {
        con::MovementBehaviour b;
        b.tick       = actorTick;
        b.startMoving = [](con::MovementContext& ctx) { ctx.data.erase("PsiDone"); };
        b.stopMoving = [](con::MovementContext& ctx) { ctx.data.erase("PsiStall"); };
        registerOwnActor(std::string(kPsi), std::move(b));
        // Create's isNotSupportiveFallback: nothing sticks to the interface's front
        con::MovementRules rules;
        rules.notSupportive = [](cc::States const& s) { return cc::bit(facingOf(s)); };
        con::registerMovementRules(std::string(kPsi), std::move(rules));
    }
};
Registrar const gRegistrar;

} // namespace

bool psiConnected(GlobalPos const& pos) {
    auto* s = stationary(pos);
    return s && canTransfer(*s);
}

void psiTick() {
    static std::uint64_t ticks = 0;
    ++ticks;
    connectMoving();
    tickStationary(ticks);
}

void psiInstall() {
    caps::items::registerBlock(std::string(kPsi), handler());
    // Create's PortableStorageInterfaceBlock.getAnalogOutputSignal: 15 while connected, else 0
    common::redstone::registerAnalog(std::string(kPsi), [](BlockSource& region, BlockPos const& pos, Block const&) -> std::optional<int> {
        auto* s = stationary(game::globalPos(region, pos));
        return s && s->connected && isConnected(*s) ? 15 : 0;
    });
}

void psiClear() { states().clear(); }

} // namespace lv::feature::actors
