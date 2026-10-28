// Items moving through chutes, shown to every client (SPEC §3.6 U12, §8).
//
// Create draws the item inside a chute with its block entity renderer. Bedrock has no block
// entity renderers for custom blocks, so the server shows each moving stack as a *display item*:
// an item entity that exists only on the clients (AddItemActor / MoveActorAbsolute /
// RemoveActor packets with runtime ids of our own range, never a server entity, so nobody can
// pick it up). Clients interpolate its movement; vanilla clients and the client mod see the same.

#include <unordered_map>

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/NetworkItemStackDescriptor.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/common/Items.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/WorldDb.h"

namespace lv::feature::funnels::detail {

namespace items = common::items;

namespace {

struct Shown {
    std::uint64_t id = 0;
    std::string   item; ///< what is shown ("id x count")
    Vec3          pos{0, 0, 0};
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

BlockPos around(Vec3 const& p) { return BlockPos{int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z))}; }

void sendRemove(int dim, Shown const& s) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::RemoveActor);
    if (!packet) return;
    auto& pkt     = static_cast<RemoveActorPacket&>(*packet);
    pkt.mEntityId = ActorUniqueID{static_cast<int64>(s.id)};
    pkt.sendTo(around(s.pos), DimensionType(dim));
}

void sendAdd(int dim, Shown const& s, ItemStack const& stack) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::AddItemActor);
    if (!packet) return;
    auto& pkt      = static_cast<AddItemActorPacket&>(*packet);
    pkt.mId        = ActorUniqueID{static_cast<int64>(s.id)};
    pkt.mRuntimeId = ActorRuntimeID{s.id};
    // NetworkItemStackDescriptor has no usable assignment: rebuild it in place
    std::destroy_at(&pkt.mItem.get());
    std::construct_at(&pkt.mItem.get(), stack);
    pkt.mPos       = s.pos;
    pkt.mVelocity  = Vec3{0, 0, 0};
    pkt.sendTo(around(s.pos), DimensionType(dim));
}

void sendMove(int dim, Shown const& s) {
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
    pkt.sendTo(around(s.pos), DimensionType(dim));
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
    auto  key  = owner.key();
    auto  what = items::idOf(stack) + " x" + std::to_string(items::countOf(stack));
    auto  it   = shown().find(key);
    if (it != shown().end() && it->second.item != what) {
        sendRemove(owner.dim, it->second);
        shown().erase(it);
        it = shown().end();
    }
    if (it == shown().end()) {
        Shown s{nextId(), what, pos};
        sendAdd(owner.dim, s, stack);
        shown().emplace(key, s);
        return;
    }
    auto& s = it->second;
    if (std::abs(s.pos.x - pos.x) + std::abs(s.pos.y - pos.y) + std::abs(s.pos.z - pos.z) < 1e-3f) return;
    s.pos = pos;
    sendMove(owner.dim, s);
}

void hideItem(GlobalPos const& owner) {
    auto it = shown().find(owner.key());
    if (it == shown().end()) return;
    sendRemove(owner.dim, it->second);
    shown().erase(it);
}

std::size_t shownItems() { return shown().size(); }

} // namespace lv::feature::funnels::detail
