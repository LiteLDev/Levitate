#include "feature/common/VanillaItems.h"

#include <cmath>
#include <map>
#include <set>
#include <string>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/NetworkItemStackDescriptor.h"
#include "mc/world/level/Level.h"

#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "game/WorldDb.h"
#include "mod/Module.h"

namespace lv::feature::common::vanillaitems {

namespace {

constexpr float kViewDistance = 64.f;
constexpr int   kInterval     = 4;

struct Entity {
    std::uint64_t          id = 0;
    int                    dim = 0;
    std::string            what; ///< "id xN" + data: a different stack is a new entity
    ItemStack              stack;
    Vec3                   pos{0, 0, 0};
    bool                   moved = false;
    std::set<std::int64_t> viewers;
};

struct State {
    std::vector<Source>                                     sources;
    std::map<std::pair<std::string, int>, Entity>           entities; ///< (owner key, slot)
};
State& st() {
    static State s;
    return s;
}

/// Runtime ids far above the server's own counter, apart from the chute's range (U12).
std::uint64_t nextId() {
    static std::uint64_t id = 0x4C56490000000000ull; // "LVI"
    return ++id;
}

Player* playerById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->getPlayer(id);
}

void sendRemove(Player& to, Entity const& e) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::RemoveActor);
    if (!packet) return;
    auto& pkt     = static_cast<RemoveActorPacket&>(*packet);
    pkt.mEntityId = ActorUniqueID{static_cast<int64>(e.id)};
    pkt.sendTo(to);
}

void sendAdd(Player& to, Entity const& e) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::AddItemActor);
    if (!packet) return;
    auto& pkt      = static_cast<AddItemActorPacket&>(*packet);
    pkt.mId        = ActorUniqueID{static_cast<int64>(e.id)};
    pkt.mRuntimeId = ActorRuntimeID{e.id};
    // NetworkItemStackDescriptor has no usable assignment: rebuild it in place
    std::destroy_at(&pkt.mItem.get());
    std::construct_at(&pkt.mItem.get(), e.stack);
    pkt.mPos      = e.pos;
    pkt.mVelocity = Vec3{0, 0, 0};
    pkt.sendTo(to);
}

void sendMove(Player& to, Entity const& e) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::MoveAbsoluteActor);
    if (!packet) return;
    auto& pkt                    = static_cast<MoveActorAbsolutePacket&>(*packet);
    pkt.mMoveData->mRuntimeId    = ActorRuntimeID{e.id};
    pkt.mMoveData->mHeader->mRaw = 0;
    pkt.mMoveData->mPos          = e.pos;
    pkt.mMoveData->mRotX         = 0;
    pkt.mMoveData->mRotY         = 0;
    pkt.mMoveData->mRotYHead     = 0;
    pkt.mMoveData->mRotYBody     = 0;
    pkt.sendTo(to);
}

void removeAll(Entity const& e) {
    for (auto uid : e.viewers)
        if (auto* p = playerById(uid)) sendRemove(*p, e);
}

std::string describe(ItemStack const& s) {
    return items::idOf(s) + " x" + std::to_string(items::countOf(s)) + " " + items::toJson(s).dump();
}

struct Viewer {
    Player*      player;
    std::int64_t uid;
    int          dim;
    Vec3         pos;
};

void flush() {
    auto& s = st();
    if (s.sources.empty()) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    std::vector<Viewer> viewers;
    level->forEachPlayer([&](Player& p) {
        // players with the client mod draw the display items instead
        if (!p.isSimulatedPlayer() && !net::isCompanion(p))
            viewers.push_back({&p, p.getOrCreateUniqueID().rawID, static_cast<int>(p.getDimensionId()), p.getPosition()});
        return true;
    });
    std::vector<Shown> wanted;
    if (!viewers.empty()) wanted = collect();
    // match the entities to what is wanted
    std::set<std::pair<std::string, int>> keep;
    for (auto const& w : wanted) {
        if (items::isEmpty(w.stack)) continue;
        auto key  = std::make_pair(w.owner.key(), w.slot);
        auto what = describe(w.stack);
        keep.insert(key);
        auto it = s.entities.find(key);
        if (it != s.entities.end() && it->second.what != what) {
            removeAll(it->second);
            s.entities.erase(it);
            it = s.entities.end();
        }
        if (it == s.entities.end()) {
            Entity e;
            e.id    = nextId();
            e.dim   = w.owner.dim;
            e.what  = what;
            e.stack = w.stack;
            e.pos   = w.pos;
            s.entities.emplace(key, std::move(e));
            continue;
        }
        auto& e = it->second;
        if (std::abs(e.pos.x - w.pos.x) + std::abs(e.pos.y - w.pos.y) + std::abs(e.pos.z - w.pos.z) > 1e-3f) {
            e.pos   = w.pos;
            e.moved = true;
        }
    }
    for (auto it = s.entities.begin(); it != s.entities.end();) {
        if (keep.contains(it->first)) {
            ++it;
            continue;
        }
        removeAll(it->second);
        it = s.entities.erase(it);
    }
    // send them to the vanilla players around them
    for (auto& [key, e] : s.entities) {
        std::set<std::int64_t> inRange;
        for (auto const& v : viewers) {
            float dx = v.pos.x - e.pos.x, dy = v.pos.y - e.pos.y, dz = v.pos.z - e.pos.z;
            if (v.dim != e.dim || dx * dx + dy * dy + dz * dz > kViewDistance * kViewDistance) continue;
            inRange.insert(v.uid);
            if (!e.viewers.contains(v.uid)) sendAdd(*v.player, e);
            else if (e.moved) sendMove(*v.player, e);
        }
        for (auto uid : e.viewers)
            if (!inRange.contains(uid))
                if (auto* p = playerById(uid)) sendRemove(*p, e);
        e.viewers = std::move(inRange);
        e.moved   = false;
    }
}

} // namespace

void registerSource(Source source) { st().sources.push_back(std::move(source)); }

std::vector<Shown> collect() {
    std::vector<Shown> out;
    for (auto const& src : st().sources) src(out);
    return out;
}

std::size_t sentCount() {
    std::size_t n = 0;
    for (auto const& [key, e] : st().entities) n += e.viewers.empty() ? 0 : 1;
    return n;
}

class VanillaItemsModule {
public:
    void install() {
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) {
            if (++mTicks % kInterval == 0) flush();
        });
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldClosed([] { st().entities.clear(); });
        }
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mTick);
        for (auto const& [key, e] : st().entities) removeAll(e);
        st().entities.clear();
    }

private:
    ll::event::ListenerPtr mTick;
    std::uint64_t          mTicks = 0;
};

} // namespace lv::feature::common::vanillaitems

LV_REGISTER_MODULE_ORDERED(lv::feature::common::vanillaitems::VanillaItemsModule, lv::ModuleOrder::Services);
