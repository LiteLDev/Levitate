// Items shown to vanilla clients (U56, SPEC §8): Create draws hanging packages, the box on the
// packager's tray and the factory gauge's monitored items with block entity renderers, which
// vanilla clients don't have. Players without the client mod get an item entity that exists only
// on their client instead (AddItemActor / MoveActorAbsolute / RemoveActor packets with runtime ids
// of our own range, never a server entity, so nobody can pick it up), sent without gravity so it
// stays where it is put. Players with the client mod draw the real thing and get nothing here.

#include <cmath>
#include <map>
#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/world/actor/DataItem.h"
#include "mc/world/actor/DataItem2.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/NetworkItemStackDescriptor.h"
#include "mc/world/level/Level.h"

#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "feature/packages/PackagesInternal.h"
#include "game/WorldDb.h"

namespace lv::feature::packages {

namespace items = common::items;

namespace {

constexpr float kViewDistance = 64.f;

struct Shown {
    std::uint64_t          id  = 0;
    int                    dim = 0;
    std::string            what; ///< "id x count": a different item is a new entity
    ItemStack              stack;
    Vec3                   pos{0, 0, 0};
    bool                   moved = false;
    std::set<std::int64_t> viewers;
};

std::map<std::string, Shown>& shown() {
    static std::map<std::string, Shown> m;
    return m;
}

/// Runtime ids far above the server's own counter and the funnels' range ("LV" + 'P').
std::uint64_t nextId() {
    static std::uint64_t id = 0x4C56500000000000ull;
    return ++id;
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
    // actor flags without HasGravity: the client does not let the item fall
    pkt.mData->clear();
    pkt.mData->push_back(DataItem::create(ActorDataIDs::Reserved0, int64{0}));
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

void showVanillaItem(std::string const& key, int dim, ItemStack const& stack, std::array<double, 3> const& at) {
    if (items::isEmpty(stack)) {
        hideVanillaItem(key);
        return;
    }
    Vec3 pos{static_cast<float>(at[0]), static_cast<float>(at[1]), static_cast<float>(at[2])};
    auto what = items::idOf(stack) + " x" + std::to_string(items::countOf(stack));
    auto it   = shown().find(key);
    if (it != shown().end() && (it->second.what != what || it->second.dim != dim)) {
        removeFromViewers(it->second);
        shown().erase(it);
        it = shown().end();
    }
    if (it == shown().end()) {
        Shown s;
        s.id    = nextId();
        s.dim   = dim;
        s.what  = what;
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

void hideVanillaItem(std::string const& key) {
    auto it = shown().find(key);
    if (it == shown().end()) return;
    removeFromViewers(it->second);
    shown().erase(it);
}

void hideVanillaItems(std::string const& prefix, std::set<std::string> const& keep) {
    for (auto it = shown().lower_bound(prefix); it != shown().end() && it->first.starts_with(prefix);) {
        if (keep.contains(it->first)) {
            ++it;
            continue;
        }
        removeFromViewers(it->second);
        it = shown().erase(it);
    }
}

std::size_t vanillaItemCount(std::string const& prefix) {
    std::size_t n = 0;
    for (auto it = shown().lower_bound(prefix); it != shown().end() && it->first.starts_with(prefix); ++it) ++n;
    return n;
}

void flushVanillaItems() {
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

} // namespace lv::feature::packages
