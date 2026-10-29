// Display items (U33): the server registry and its sync to companions (see DisplayItems.h).

#include "feature/itemrender/DisplayItems.h"

#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::itemrender {

namespace ci    = core::itemrender;
namespace items = common::items;
using Json      = nlohmann::json;

namespace {

/// Companions further away than this don't get an item.
constexpr double kRange = 64.0;
/// Every this many ticks every item's viewers are checked again (players moving around).
constexpr int kFullPassTicks = 10;
/// Every this many ticks provided items are checked against their block.
constexpr int kValidateTicks = 40;

struct Key {
    GlobalPos    owner;
    std::int64_t slot = 0;
    friend auto  operator<=>(Key const&, Key const&) = default;
};

struct Item {
    DisplayItem            d;
    Json                   json; ///< items::toJson(d.stack)
    bool                   provided  = false;
    bool                   dirtyItem = true;
    bool                   dirtyMove = false;
    std::set<std::int64_t> viewers;
};

struct ProviderEntry {
    std::string                                  blockId; ///< "" = use matches
    std::function<bool(std::string_view)>        matches;
    Provider                                     fn;
};

struct State {
    std::map<Key, Item>                                 items;
    std::map<GlobalPos, std::string>                    providedOwners; ///< owner -> block id the providers saw
    std::vector<std::pair<Key, std::set<std::int64_t>>> removed;        ///< deletions to send
    std::set<std::int64_t>                              greeted;        ///< companions that got items (log)
    std::uint64_t                                       tick = 0;
};
State& state() {
    static State s;
    return s;
}

std::shared_mutex& providerMutex() {
    static std::shared_mutex m;
    return m;
}
std::vector<ProviderEntry>& providers() {
    static std::vector<ProviderEntry> p;
    return p;
}

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    if (!d) return nullptr;
    return &d->getBlockSourceFromMainChunkSource();
}

Block const* loadedBlock(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    if (!r) return nullptr;
    BlockPos bp = game::blockPos(p);
    if (bp.y < r->getMinHeight() || bp.y >= r->getMaxHeight() || !r->hasChunksAt(bp, 0, false)) return nullptr;
    return &r->getBlock(bp);
}

void dropItem(std::map<Key, Item>::iterator it) {
    auto& s = state();
    if (!it->second.viewers.empty()) s.removed.emplace_back(it->first, std::move(it->second.viewers));
    s.items.erase(it);
}

void dropProvided(GlobalPos const& owner) {
    auto& s = state();
    for (auto it = s.items.lower_bound(Key{owner, INT64_MIN}); it != s.items.end() && it->first.owner == owner;) {
        auto next = std::next(it);
        if (it->second.provided) dropItem(it);
        it = next;
    }
}

void put(GlobalPos const& owner, std::int64_t slot, DisplayItem const& d, bool provided) {
    auto& s         = state();
    auto [it, made] = s.items.try_emplace(Key{owner, slot});
    auto& item      = it->second;
    item.provided   = provided;
    if (made || !items::sameItemSameData(item.d.stack, d.stack) || items::countOf(item.d.stack) != items::countOf(d.stack)
        || item.d.style != d.style || item.d.spin != d.spin || item.d.pile != d.pile || item.d.hideActor != d.hideActor) {
        item.d         = d;
        item.json      = items::toJson(d.stack);
        item.dirtyItem = true;
        return;
    }
    if (!ci::nearlySame(item.d.at, d.at)) {
        item.d.at        = d.at;
        item.d.lerpTicks = d.lerpTicks;
        item.dirtyMove   = true;
    }
}

ci::Entry entryOf(Key const& k, Item const& item) {
    ci::Entry e;
    e.owner = {k.owner.x, k.owner.y, k.owner.z};
    e.slot  = k.slot;
    e.item  = item.json;
    e.t     = item.d.at;
    e.style = item.d.style;
    e.spin  = item.d.spin;
    e.pile  = item.d.pile;
    e.lerp  = item.d.lerpTicks;
    e.hide  = item.d.hideActor;
    return e;
}

struct Viewer {
    Player*      player;
    std::int64_t uid;
    int          dim;
    Vec3         pos;
};

struct Batch {
    int  dim = 0;
    Json set = Json::array(), move = Json::array(), del = Json::array();
    [[nodiscard]] bool empty() const { return set.empty() && move.empty() && del.empty(); }
};

void validateProvided() {
    auto& s = state();
    for (auto it = s.providedOwners.begin(); it != s.providedOwners.end();) {
        auto const* block = loadedBlock(it->first);
        if (block && game::blockId(*block) == it->second) {
            ++it;
            continue;
        }
        dropProvided(it->first); // broken, replaced or unloaded (asked again when seen)
        it = s.providedOwners.erase(it);
    }
}

void flush() {
    auto& s = state();
    ++s.tick;
    if (s.tick % kValidateTicks == 0) validateProvided();
    auto level = ll::service::getLevel();
    if (!level) return;
    std::vector<Viewer> viewers;
    level->forEachPlayer([&](Player& p) {
        if (net::isCompanion(p)) viewers.push_back({&p, p.getOrCreateUniqueID().rawID, static_cast<int>(p.getDimensionId()), p.getPosition()});
        return true;
    });
    if (viewers.empty() && s.removed.empty()) {
        for (auto& [k, item] : s.items) item.viewers.clear();
        return;
    }
    std::map<std::int64_t, Batch> batches;
    for (auto const& v : viewers) batches[v.uid].dim = v.dim;

    for (auto& [key, gone] : s.removed) {
        for (auto uid : gone)
            if (auto b = batches.find(uid); b != batches.end() && b->second.dim == key.owner.dim)
                b->second.del.push_back(ci::encodeKey({key.owner.x, key.owner.y, key.owner.z}, key.slot));
    }
    s.removed.clear();

    bool full = s.tick % kFullPassTicks == 0;
    for (auto& [key, item] : s.items) {
        if (!full && !item.dirtyItem && !item.dirtyMove) continue;
        std::set<std::int64_t> inRange;
        for (auto const& v : viewers) {
            double dx = v.pos.x - item.d.at.x, dy = v.pos.y - item.d.at.y, dz = v.pos.z - item.d.at.z;
            if (v.dim != key.owner.dim || dx * dx + dy * dy + dz * dz > kRange * kRange) continue;
            inRange.insert(v.uid);
            auto& b = batches[v.uid];
            if (!item.viewers.contains(v.uid) || item.dirtyItem) b.set.push_back(ci::encode(entryOf(key, item), true));
            else if (item.dirtyMove) b.move.push_back(ci::encode(entryOf(key, item), false));
        }
        for (auto uid : item.viewers)
            if (!inRange.contains(uid))
                // a viewer now in another dimension already dropped it (and sent itemrender.reset)
                if (auto b = batches.find(uid); b != batches.end() && b->second.dim == key.owner.dim)
                    b->second.del.push_back(ci::encodeKey({key.owner.x, key.owner.y, key.owner.z}, key.slot));
        item.viewers   = std::move(inRange);
        item.dirtyItem = false;
        item.dirtyMove = false;
    }

    for (auto const& v : viewers) {
        auto& b = batches[v.uid];
        if (b.empty()) continue;
        if (!s.greeted.contains(v.uid) && !b.set.empty()) {
            s.greeted.insert(v.uid);
            log().info("itemrender: {} gets display items ({} now)", v.player->getRealName(), b.set.size());
        }
        net::send(*v.player, "itemrender.batch", {{"dim", b.dim}, {"set", std::move(b.set)}, {"move", std::move(b.move)}, {"del", std::move(b.del)}});
    }
}

/// A companion (re)joined: it has no items yet.
void forgetViewer(std::int64_t uid) {
    auto& s = state();
    for (auto& [k, item] : s.items) item.viewers.erase(uid);
    s.greeted.erase(uid);
}

void onQuery(Player& player, Json const& msg) {
    int dim = static_cast<int>(player.getDimensionId());
    if (msg.value("dim", dim) != dim) return;
    auto list = msg.value("pos", Json::array());
    if (!list.is_array()) return;
    auto const& at = player.getPosition();
    int         n  = 0;
    for (auto const& p : list) {
        if (++n > 512) break;
        if (!p.is_array() || p.size() != 3 || !p[0].is_number_integer() || !p[1].is_number_integer() || !p[2].is_number_integer()) continue;
        GlobalPos pos{dim, p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
        double    dx = at.x - pos.x, dy = at.y - pos.y, dz = at.z - pos.z;
        if (dx * dx + dy * dy + dz * dz > 2 * kRange * kRange) continue;
        if (state().providedOwners.contains(pos)) continue;
        refresh(pos);
    }
}

} // namespace

void set(GlobalPos const& owner, std::int64_t slot, DisplayItem const& item) {
    if (items::isEmpty(item.stack)) {
        remove(owner, slot);
        return;
    }
    put(owner, slot, item, false);
}

void remove(GlobalPos const& owner, std::int64_t slot) {
    auto& s  = state();
    auto  it = s.items.find(Key{owner, slot});
    if (it != s.items.end()) dropItem(it);
}

void clear(GlobalPos const& owner) {
    auto& s = state();
    for (auto it = s.items.lower_bound(Key{owner, INT64_MIN}); it != s.items.end() && it->first.owner == owner;) {
        auto next = std::next(it);
        dropItem(it);
        it = next;
    }
    s.providedOwners.erase(owner);
}

DisplayItem const* find(GlobalPos const& owner, std::int64_t slot) {
    auto it = state().items.find(Key{owner, slot});
    return it == state().items.end() ? nullptr : &it->second.d;
}

std::size_t count(GlobalPos const* owner) {
    auto& s = state();
    if (!owner) return s.items.size();
    std::size_t n = 0;
    for (auto it = s.items.lower_bound(Key{*owner, INT64_MIN}); it != s.items.end() && it->first.owner == *owner; ++it) ++n;
    return n;
}

std::vector<std::pair<std::int64_t, DisplayItem>> itemsOf(GlobalPos const& owner) {
    std::vector<std::pair<std::int64_t, DisplayItem>> out;
    auto& s = state();
    for (auto it = s.items.lower_bound(Key{owner, INT64_MIN}); it != s.items.end() && it->first.owner == owner; ++it)
        out.emplace_back(it->first.slot, it->second.d);
    return out;
}

void registerProvider(std::string const& blockId, Provider fn) {
    std::unique_lock lock(providerMutex());
    providers().push_back({blockId, nullptr, std::move(fn)});
}

void registerProvider(std::function<bool(std::string_view)> matches, Provider fn) {
    std::unique_lock lock(providerMutex());
    providers().push_back({"", std::move(matches), std::move(fn)});
}

bool hasProvider(std::string_view blockId) {
    std::shared_lock lock(providerMutex());
    for (auto const& p : providers())
        if (p.blockId.empty() ? p.matches && p.matches(blockId) : p.blockId == blockId) return true;
    return false;
}

void refresh(GlobalPos const& owner) {
    auto& s = state();
    dropProvided(owner);
    s.providedOwners.erase(owner);
    auto const* block = loadedBlock(owner);
    if (!block) return;
    auto id = game::blockId(*block);
    std::vector<Provider> fns;
    {
        std::shared_lock lock(providerMutex());
        for (auto const& p : providers())
            if (p.blockId.empty() ? p.matches && p.matches(id) : p.blockId == id) fns.push_back(p.fn);
    }
    if (fns.empty()) return;
    std::vector<std::pair<std::int64_t, DisplayItem>> out;
    for (auto const& fn : fns) fn(owner, *block, out);
    for (auto const& [slot, d] : out)
        if (!items::isEmpty(d.stack)) put(owner, slot, d, true);
    s.providedOwners[owner] = std::move(id);
}

Transform facingFace(int face) {
    Transform t;
    switch (face) {
    case 0: t.pitch = 90.f; break;
    case 1: t.pitch = -90.f; break;
    case 2: t.yaw = 180.f; break;
    case 4: t.yaw = -90.f; break;
    case 5: t.yaw = 90.f; break;
    default: break; // south: the canonical +Z
    }
    return t;
}

class DisplayItemsModule {
public:
    void install() {
        net::on("itemrender.query", onQuery);
        net::on("itemrender.reset", [](Player& p, Json const&) { forgetViewer(p.getOrCreateUniqueID().rawID); });
        net::onCompanionJoined([](Player& p) { forgetViewer(p.getOrCreateUniqueID().rawID); });
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { flush(); });
        game::db::onWorldClosed([] { state() = State{}; });
    }
    void uninstall() {
        net::off("itemrender.query");
        net::off("itemrender.reset");
        ll::event::EventBus::getInstance().removeListener(mTick);
        state() = State{};
    }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace lv::feature::itemrender

LV_REGISTER_MODULE(lv::feature::itemrender::DisplayItemsModule);
