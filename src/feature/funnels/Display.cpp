// Items moving through chutes (SPEC §3.6.12, §8).
//
// Create draws the item inside a chute with its block entity renderer:
// - players with the client mod get a display item (feature/itemrender): the stack drawn by the
//   mod, lying at its height in the chute, interpolated between ticks;
// - vanilla players get an item entity that exists only on their client (AddItemActor /
//   MoveActorAbsolute / RemoveActor packets with runtime ids of our own range, never a server
//   entity, so nobody can pick it up). Their clients interpolate its movement.
// Every player within view distance gets it (also players who arrive later).

#include <cmath>
#include <set>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/NetworkItemStackDescriptor.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/common/Items.h"
#include "feature/funnels/FunnelsInternal.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/net/Net.h"
#include "game/WorldDb.h"

namespace lv::feature::funnels::detail {

namespace items = common::items;

namespace {

/// Players further away than this don't get display items.
constexpr float kViewDistance = 64.f;

struct Shown {
    std::uint64_t          id = 0;
    int                    dim = 0;
    std::string            item; ///< what is shown ("id x count")
    ItemStack              stack;
    Vec3                   pos{0, 0, 0};
    bool                   moved = false;
    std::set<std::int64_t> viewers; ///< players that have the entity
};

std::unordered_map<std::string, Shown>& shown() {
    static std::unordered_map<std::string, Shown> m;
    return m;
}

/// Runtime ids far above the server's own counter (it starts at 1 and counts up).
std::uint64_t nextId() {
    static std::uint64_t id = 0x4C56000000000000ull; // "LV"
    return ++id;
}

Player* playerById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

void sendRemove(Player& to, Shown const& s) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::RemoveActor);
    if (!packet) return;
    auto& pkt     = static_cast<RemoveActorPacket&>(*packet);
    pkt.mEntityId = ActorUniqueID{static_cast<int64>(s.id)};
    pkt.sendTo(to);
}

void sendAdd(Player& to, Shown const& s) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::AddItemActor);
    if (!packet) return;
    auto& pkt      = static_cast<AddItemActorPacket&>(*packet);
    pkt.mId        = ActorUniqueID{static_cast<int64>(s.id)};
    pkt.mRuntimeId = ActorRuntimeID{s.id};
    // NetworkItemStackDescriptor has no usable assignment: rebuild it in place
    std::destroy_at(&pkt.mItem.get());
    std::construct_at(&pkt.mItem.get(), s.stack);
    pkt.mPos      = s.pos;
    pkt.mVelocity = Vec3{0, 0, 0};
    pkt.sendTo(to);
}

void sendMove(Player& to, Shown const& s) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::MoveAbsoluteActor);
    if (!packet) return;
    auto& pkt                    = static_cast<MoveActorAbsolutePacket&>(*packet);
    pkt.mMoveData->mRuntimeId    = ActorRuntimeID{s.id};
    pkt.mMoveData->mHeader->mRaw = 0;
    pkt.mMoveData->mPos          = s.pos;
    pkt.mMoveData->mRotX         = 0;
    pkt.mMoveData->mRotY         = 0;
    pkt.mMoveData->mRotYHead     = 0;
    pkt.mMoveData->mRotYBody     = 0;
    pkt.sendTo(to);
}

void removeFromViewers(Shown const& s) {
    for (auto uid : s.viewers)
        if (auto* p = playerById(uid)) sendRemove(*p, s);
}

struct Lifecycle {
    Lifecycle() {
        game::db::onWorldClosed([] { shown().clear(); });
    }
} const gLifecycle;

} // namespace

void showItem(GlobalPos const& owner, ItemStack const& stack, Vec3 const& pos) {
    if (items::isEmpty(stack)) {
        hideItem(owner);
        return;
    }
    itemrender::DisplayItem d; // the client mod's item (Create's ChuteRenderer: half size)
    d.stack     = stack;
    d.at        = {pos.x, pos.y, pos.z};
    d.at.scale  = .5f;
    d.style     = itemrender::Style::Lying;
    d.lerpTicks = 1;
    itemrender::set(owner, 0, d);
    auto key  = owner.key();
    auto what = items::idOf(stack) + " x" + std::to_string(items::countOf(stack));
    auto it   = shown().find(key);
    if (it != shown().end() && it->second.item != what) {
        removeFromViewers(it->second);
        shown().erase(it);
        it = shown().end();
    }
    if (it == shown().end()) {
        Shown s;
        s.id    = nextId();
        s.dim   = owner.dim;
        s.item  = what;
        s.stack = stack;
        s.pos   = pos;
        shown().emplace(key, std::move(s));
        return;
    }
    auto& s = it->second;
    if (std::abs(s.pos.x - pos.x) + std::abs(s.pos.y - pos.y) + std::abs(s.pos.z - pos.z) < 1e-3f) return;
    s.pos   = pos;
    s.moved = true;
}

void hideItem(GlobalPos const& owner) {
    itemrender::remove(owner, 0);
    auto it = shown().find(owner.key());
    if (it == shown().end()) return;
    removeFromViewers(it->second);
    shown().erase(it);
}

void flushDisplays() {
    if (shown().empty()) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    struct Viewer {
        Player*      player;
        std::int64_t uid;
        int          dim;
        Vec3         pos;
    };
    std::vector<Viewer> players;
    level->forEachPlayer([&](Player& p) {
        // players with the client mod see the display item instead
        if (!p.isSimulatedPlayer() && !net::isCompanion(p)) players.push_back({&p, p.getOrCreateUniqueID().rawID, static_cast<int>(p.getDimensionId()), p.getPosition()});
        return true;
    });
    for (auto& [key, s] : shown()) {
        std::set<std::int64_t> inRange;
        for (auto const& v : players) {
            float dx = v.pos.x - s.pos.x, dy = v.pos.y - s.pos.y, dz = v.pos.z - s.pos.z;
            if (v.dim != s.dim || dx * dx + dy * dy + dz * dz > kViewDistance * kViewDistance) continue;
            inRange.insert(v.uid);
            if (!s.viewers.contains(v.uid)) sendAdd(*v.player, s);
            else if (s.moved) sendMove(*v.player, s);
        }
        for (auto uid : s.viewers)
            if (!inRange.contains(uid))
                if (auto* p = playerById(uid)) sendRemove(*p, s);
        s.viewers = std::move(inRange);
        s.moved   = false;
    }
}

std::size_t shownItems() { return shown().size(); }

} // namespace lv::feature::funnels::detail
