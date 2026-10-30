// Actors unit (U22): the module (game hooks, ticks, world lifecycle) and the interaction with
// blocks carried by contraptions (contraption controls and seats are right-clicked while the
// contraption exists: the server casts the player's look against the carried blocks).
//
// Clicks go through the shared interaction dispatch (common/Interactions.h).

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/Level.h"

#include "feature/actors/Actors.h"
#include "feature/common/Interactions.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/ActorsExt.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::actors {

namespace ext = ::lv::feature::contraption::actors_ext;
namespace fs  = ::lv::feature::common::filterslot;
namespace ia  = ::lv::feature::common::interactions;

namespace {

constexpr double kReach = 6.0;

cc::Vec3d ccVec(Vec3 const& v) { return {v.x, v.y, v.z}; }

/// Ray against an axis-aligned box: entry distance (nullopt: miss).
std::optional<double> rayBox(cc::Vec3d const& o, cc::Vec3d const& d, cc::Box const& b) {
    double tmin = -std::numeric_limits<double>::infinity(), tmax = std::numeric_limits<double>::infinity();
    for (auto a : {cc::Axis::X, cc::Axis::Y, cc::Axis::Z}) {
        double oa = o.get(a), da = d.get(a), lo = b.min.get(a), hi = b.max.get(a);
        if (std::abs(da) < 1e-12) {
            if (oa < lo || oa > hi) return std::nullopt;
            continue;
        }
        double t1 = (lo - oa) / da, t2 = (hi - oa) / da;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return std::nullopt;
    }
    if (tmax < 0) return std::nullopt;
    return std::max(0.0, tmin);
}

/// The player's horizontal look direction (Java getHorizontalDirection).
cc::Dir horizontalLook(Player const& player) {
    auto v = player.getViewVector();
    if (std::abs(v.x) > std::abs(v.z)) return v.x > 0 ? cc::Dir::East : cc::Dir::West;
    return v.z > 0 ? cc::Dir::South : cc::Dir::North;
}

/// The nearest looking direction (Java getNearestLookingDirection).
cc::Dir nearestLook(Player const& player) {
    auto v  = player.getViewVector();
    auto ax = std::abs(v.x), ay = std::abs(v.y), az = std::abs(v.z);
    if (ay >= ax && ay >= az) return v.y > 0 ? cc::Dir::Up : cc::Dir::Down;
    if (ax >= az) return v.x > 0 ? cc::Dir::East : cc::Dir::West;
    return v.z > 0 ? cc::Dir::South : cc::Dir::North;
}

bool isAttachedActor(std::string_view id) { return id == kHarvester || id == kPlough || id == kRoller; }

/// Create's AttachedActorBlock#canSurvive: the block behind it (opposite its facing) has a collision shape.
bool supported(BlockSource& region, BlockPos const& pos, cc::Dir facing) {
    auto     o = cc::offset(cc::opposite(facing));
    BlockPos behind{pos.x + o.x, pos.y + o.y, pos.z + o.z};
    if (!region.hasChunksAt(behind, 0, false)) return true;
    return hasCollision(region, behind);
}

/// Create's updateShape: a harvester / plough / roller whose support went drops as an item.
void checkSupportAround(BlockSource& region, BlockPos const& changed) {
    for (auto d : {cc::Dir::North, cc::Dir::South, cc::Dir::East, cc::Dir::West, cc::Dir::Up, cc::Dir::Down}) {
        auto     o = cc::offset(d);
        BlockPos at{changed.x + o.x, changed.y + o.y, changed.z + o.z};
        if (!region.hasChunksAt(at, 0, false)) continue;
        auto const& block = region.getBlock(at);
        std::string id    = block.getTypeName();
        if (!isAttachedActor(id) || facingAt(region, at) != d) continue; // the changed block isn't behind it
        if (supported(region, at, d)) continue;
        auto level = ll::service::getLevel();
        if (!level) return;
        level->destroyBlock(region, at, true, BlockChangeContext{});
        logAction(id + " at " + std::to_string(at.x) + " " + std::to_string(at.y) + " " + std::to_string(at.z) + " dropped: its support is gone");
    }
}

bool isOurs(std::string_view id) { return isAttachedActor(id) || id == kPsi || id == kControls; }

/// Create's getStateForPlacement of our blocks (facing), plus the settings hand-off.
void placed(Player& player, BlockSource& region, BlockPos const& pos, BlockPos const& clicked, cc::Dir clickedFace) {
    std::string id = region.getBlock(pos).getTypeName();
    cc::Dir     facing{};
    if (isAttachedActor(id)) {
        if (clickedFace == cc::Dir::Up || clickedFace == cc::Dir::Down) {
            facing = cc::opposite(horizontalLook(player));
        } else {
            auto const& behind = region.getBlock(clicked);
            facing             = isAttachedActor(behind.getTypeName()) ? facingAt(region, clicked) : clickedFace;
        }
    } else if (id == kPsi) {
        auto look = nearestLook(player);
        facing    = player.isSneaking() ? look : cc::opposite(look);
    } else if (id == kControls) {
        facing = cc::opposite(horizontalLook(player));
    } else {
        return;
    }
    if (isAttachedActor(id) && !supported(region, pos, facing)) {
        // Create's AttachedActorBlock#canSurvive: no placement without a block behind it
        BlockChangeContext ctx{};
        if (auto const* air = game::blockWith("minecraft:air")) region.setBlock(pos, *air, 3, nullptr, ctx);
        if (!player.isCreative()) {
            auto refund = makeItem(id, 1);
            if (!player.add(refund)) player.drop(refund, false);
        }
        player.sendInventory(false);
        logAction(player.getRealName() + " could not place " + id + ": nothing to attach it to");
        return;
    }
    setIntState(region, pos, kFacing, static_cast<int>(facing));
    GlobalPos gp{static_cast<int>(region.getDimensionId()), pos.x, pos.y, pos.z};
    if (id == kRoller) rollerPlaced(gp);
    if (id == kControls) trackStationaryControls(gp);
}

/// Right-click on one of our stationary blocks (`local`: block-local click). True when handled.
bool useOnBlock(Player& player, ItemStack const& item, BlockPos const& at, uchar face, core::common::Vec const& local) {
    auto&       region = player.getDimensionBlockSource();
    auto const& block  = region.getBlock(at);
    std::string id     = block.getTypeName();
    GlobalPos   gp{static_cast<int>(player.getDimensionId()), at.x, at.y, at.z};
    auto        uid    = player.getOrCreateUniqueID().rawID;
    if (isSeat(id)) {
        if (player.isSneaking()) return false;
        return recentlyUsed(uid) || seatUse(player, gp, item);
    }
    if (id == kControls && !player.isSneaking() && (item.isNull() || item.mCount == 0)) {
        if (fs::slotAt(block, face, local)) return false; // the filter slot (U3)
        if (recentlyUsed(uid)) return true;
        controlsClickStationary(player, gp);
        return true;
    }
    return false;
}

} // namespace

// ---- contraption blocks -----------------------------------------------------------------------

std::optional<ContraptionHit> raycastContraption(Player& player, double reach) {
    cc::Vec3d eye = ccVec(player.getEyePos());
    cc::Vec3d dir = ccVec(player.getViewVector());
    int       dim = static_cast<int>(player.getDimensionId());
    std::optional<ContraptionHit> best;
    for (auto id : con::all()) {
        auto* c = con::find(id);
        if (!c || c->dimension() != dim) continue;
        auto const& t  = c->transform();
        cc::Vec3d   lo = t.toLocal(eye);
        cc::Vec3d   ld = t.unrotateVector(dir);
        for (auto const& b : c->blocks()) {
            if (b.id != kControls && !isSeat(b.id)) continue;
            cc::Box box = cc::Box::unit(b.local);
            if (isSeat(b.id)) box.max.y = box.min.y + .5;
            auto d = rayBox(lo, ld, box);
            if (!d || *d > reach) continue;
            if (!best || *d < best->distance) best = ContraptionHit{id, b.local, *d, b.id};
        }
    }
    return best;
}

bool useOnContraption(Player& player) {
    auto hit = raycastContraption(player, kReach);
    return hit && useOnContraption(player, *hit);
}

bool useOnContraption(Player& player, ContraptionHit const& target) {
    auto const* hit = &target;
    auto*       c   = con::find(hit->id);
    if (!c) return false;
    if (player.isSneaking() && isSeat(hit->block)) return false;
    if (recentlyUsed(player.getOrCreateUniqueID().rawID)) return true;
    if (hit->block == kControls) {
        controlsClickOnContraption(player, *c, hit->local);
        return true;
    }
    if (isSeat(hit->block)) {
        // Create: a seated player stays; another rider is ejected for the clicking player
        cc::Vec3d seatPoint = c->toWorld(cc::Vec3d::of(hit->local) + cc::Vec3d{.5, .5, .5});
        auto*     region    = regionOf(c->dimension());
        if (region) {
            for (auto* a : entitiesIn(*region, seatPoint - cc::Vec3d{.4, .4, .4}, seatPoint + cc::Vec3d{.4, .4, .4})) {
                if (!a || a == &player || a->getEntityTypeId() == ActorType::FallingBlock || a->getEntityTypeId() == ActorType::ItemEntity) continue;
                if (a->isPlayer()) return true;
                c->unseat(a->getOrCreateUniqueID().rawID);
                leaveSeat(*a);
            }
        }
        if (c->seat(hit->local, player.getOrCreateUniqueID().rawID)) {
            mountOnContraptionSeat(*c, hit->local, player);
            logAction(player.getRealName() + " sat down on a seat of contraption #" + std::to_string(c->id()));
        }
        return true;
    }
    return false;
}

// ---- hooks ------------------------------------------------------------------------------------

namespace {

/// A contraption block in front of the clicked world block takes the click.
ia::Result useOnContraptionInFront(ia::UseOnContext const& ctx) {
    Player& player = ctx.player;
    try {
        if (auto c = raycastContraption(player, kReach)) {
            cc::Vec3d eye = ccVec(player.getEyePos());
            cc::Vec3d world{ctx.pos.x + ctx.hit[0], ctx.pos.y + ctx.hit[1], ctx.pos.z + ctx.hit[2]};
            if (c->distance < (world - eye).length() && useOnContraption(player, *c)) {
                player.sendInventory(false);
                return ia::Result::Consume;
            }
        }
    } catch (std::exception const& e) {
        log().error("actors: use failed: {}", e.what());
    }
    return ia::Result::Pass;
}

ia::Result useOnStationary(ia::UseOnContext const& ctx) {
    try {
        BlockPos at{ctx.pos.x, ctx.pos.y, ctx.pos.z};
        if (useOnBlock(ctx.player, ctx.item, at, static_cast<uchar>(ctx.face), ctx.hit)) {
            ctx.player.sendInventory(false);
            return ia::Result::Consume;
        }
    } catch (std::exception const& e) {
        log().error("actors: use failed: {}", e.what());
    }
    return ia::Result::Pass;
}

void onPlaced(ia::PlaceContext const& ctx) {
    if (!isOurs(ctx.blockId)) return;
    auto&    region = ctx.player.getDimensionBlockSource();
    BlockPos target{ctx.pos.x, ctx.pos.y, ctx.pos.z};
    BlockPos clicked{ctx.clicked.x, ctx.clicked.y, ctx.clicked.z};
    try {
        placed(ctx.player, region, target, clicked, static_cast<cc::Dir>(std::clamp(ctx.face, 0, 5)));
    } catch (std::exception const& e) {
        log().error("actors: placement failed: {}", e.what());
    }
}

} // namespace

namespace {

class ActorsModule {
public:
    void install() {
        rollerInstall();
        controlsInstall();
        psiInstall();
        seatsInstall();
        mHandlers = {
            ia::onUseOn({}, useOnContraptionInFront, ia::priority::High),
            ia::onUseOn({}, useOnStationary),
            ia::onPlace({}, onPlaced),
            // right-click into the air (a click on a block was handled by the use-on handlers)
            ia::onUse({}, [](ia::UseContext const& c) {
                if (c.onBlock) return ia::Result::Pass;
                return useOnContraption(c.player) ? ia::Result::Consume : ia::Result::Pass;
            }),
        };
        auto& bus = ll::event::EventBus::getInstance();
        mTick     = bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { tick(); });
        // custom blocks get no neighbour updates: watch the blocks the attached actors hang on
        mChanged = bus.emplaceListener<ll::event::BlockChangedEvent>([this](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0) return;
            auto& region = ev.blockSource();
            if (region.getLevel().isClientSide()) return;
            if (ev.previousBlock().isAir()) return; // nothing was there to hang on
            if (hasCollisionShape(ev.newBlock(), region, ev.pos())) return; // still something to hang on
            auto const& p = ev.pos();
            mSupportChecks.emplace(static_cast<int>(region.getDimensionId()), p.x, p.y, p.z);
        });
        // empty-hand right-click into the air (client mod): the server can't see it otherwise
        net::on("actors.use", [](Player& p, net::Json const&) { useOnContraption(p); });
        game::db::onWorldReady([] { controlsLoad(); });
        game::db::onWorldSave([] { controlsSave(); });
        game::db::onWorldClosed([] {
            controlsClear();
            psiClear();
        });
        if (game::db::available()) controlsLoad();
    }
    void uninstall() {
        net::off("actors.use");
        seatsUninstall();
        ll::event::EventBus::getInstance().removeListener(mTick);
        ll::event::EventBus::getInstance().removeListener(mChanged);
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
    }

private:
    static bool hasCollisionShape(Block const& block, BlockSource& region, BlockPos const& pos) {
        if (block.isAir()) return false;
        AABB box = block.getBlockType().getCollisionShape(block, region, pos, nullptr);
        return box.max.x - box.min.x > 1e-4f && box.max.y - box.min.y > 1e-4f && box.max.z - box.min.z > 1e-4f;
    }

    void tick() {
        ++mTicks;
        // (out of the block change: dropping a block inside it would edit the world in a setBlock)
        auto checks = std::exchange(mSupportChecks, {});
        for (auto const& [dim, x, y, z] : checks) {
            try {
                if (auto* region = regionOf(dim)) checkSupportAround(*region, BlockPos{x, y, z});
            } catch (std::exception const& e) {
                log().error("actors: support check failed: {}", e.what());
            }
        }
        try {
            controlsTick(mTicks);
            psiTick();
            seatsTick(mTicks);
        } catch (std::exception const& e) {
            log().error("actors: tick failed: {}", e.what());
        }
    }

    ll::event::ListenerPtr     mTick;
    ll::event::ListenerPtr     mChanged;
    std::set<std::tuple<int, int, int, int>> mSupportChecks; // dim, x, y, z (deduplicated)

    std::uint64_t              mTicks = 0;
    std::vector<ia::HandlerId> mHandlers;
};

} // namespace

} // namespace lv::feature::actors

LV_REGISTER_MODULE(lv::feature::actors::ActorsModule);
