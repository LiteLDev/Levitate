#include "feature/common/BlockData.h"

#include <map>
#include <set>
#include <tuple>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"

#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/ILevel.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include "core/common/Transfer.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::common::blockdata {

namespace {
using core::common::blockDataChunkKey;
using core::common::chunkOf;

using ChunkId = std::tuple<int, int, int>;
struct Chunk {
    std::map<GlobalPos, Json> entries;
    bool                      dirty = false;
};

struct Store {
    std::set<ChunkId>          index; ///< chunks that have a document in the DB or in memory
    std::map<ChunkId, Chunk>   loaded;
    std::vector<EraseListener> listeners;
    bool                       indexed = false;
};
Store& store() {
    static Store s;
    return s;
}

ChunkId chunkIdOf(GlobalPos const& p) { return {p.dim, chunkOf(p.x), chunkOf(p.z)}; }

void buildIndex() {
    auto& s = store();
    if (s.indexed || !game::db::available()) return;
    s.indexed = true;
    game::db::forEachKey("lv:common:bd:", [&s](std::string_view suffix) {
        int dim, cx, cz;
        if (core::common::parseChunkSuffix(suffix, dim, cx, cz)) s.index.insert({dim, cx, cz});
    });
}

Chunk& chunk(ChunkId const& id) {
    auto& s = store();
    buildIndex();
    if (auto it = s.loaded.find(id); it != s.loaded.end()) return it->second;
    Chunk c;
    if (s.index.contains(id)) {
        auto [dim, cx, cz] = id;
        if (auto doc = game::db::load(blockDataChunkKey(dim, cx, cz)); doc && doc->is_object()) {
            for (auto& [k, v] : doc->items()) {
                int x, y, z;
                if (core::common::parsePosKey(k, x, y, z)) c.entries[GlobalPos{dim, x, y, z}] = v;
            }
        }
    }
    return s.loaded.emplace(id, std::move(c)).first->second;
}

/// Chunk if it may have data (no DB access for chunks that never had any).
Chunk* chunkIfAny(ChunkId const& id) {
    auto& s = store();
    buildIndex();
    if (auto it = s.loaded.find(id); it != s.loaded.end()) return &it->second;
    if (!s.index.contains(id)) return nullptr;
    return &chunk(id);
}

void markDirty(ChunkId const& id, Chunk& c) {
    c.dirty = true;
    store().index.insert(id);
}

void runListeners(GlobalPos const& pos, Json const& doc, std::string const& prev) {
    for (auto const& fn : store().listeners) {
        try {
            fn(pos, doc, prev);
        } catch (std::exception const& e) {
            log().error("block data erase listener failed at {}: {}", pos.key(), e.what());
        }
    }
}

bool eraseImpl(GlobalPos const& pos, std::string const& prev, bool notify) {
    auto id = chunkIdOf(pos);
    auto* c = chunkIfAny(id);
    if (!c) return false;
    auto it = c->entries.find(pos);
    if (it == c->entries.end()) return false;
    Json doc = std::move(it->second);
    c->entries.erase(it);
    markDirty(id, *c);
    if (notify) runListeners(pos, doc, prev);
    return true;
}

void saveDirty() {
    auto& s = store();
    for (auto& [id, c] : s.loaded) {
        if (!c.dirty) continue;
        auto [dim, cx, cz] = id;
        auto key           = blockDataChunkKey(dim, cx, cz);
        if (c.entries.empty()) {
            game::db::erase(key);
            s.index.erase(id);
        } else {
            Json doc = Json::object();
            for (auto const& [p, v] : c.entries) doc[core::common::posKey(p.x, p.y, p.z)] = v;
            game::db::save(key, doc);
        }
        c.dirty = false;
    }
}

void reset() {
    auto& s = store();
    s.index.clear();
    s.loaded.clear();
    s.indexed = false;
}
} // namespace

Json const* get(GlobalPos const& pos) {
    auto* c = chunkIfAny(chunkIdOf(pos));
    if (!c) return nullptr;
    auto it = c->entries.find(pos);
    return it == c->entries.end() ? nullptr : &it->second;
}

Json& edit(GlobalPos const& pos) {
    auto  id = chunkIdOf(pos);
    auto& c  = chunk(id);
    markDirty(id, c);
    auto& v = c.entries[pos];
    if (v.is_null()) v = Json::object();
    return v;
}

void set(GlobalPos const& pos, Json value) {
    if (value.is_null()) {
        eraseImpl(pos, {}, false);
        return;
    }
    auto  id = chunkIdOf(pos);
    auto& c  = chunk(id);
    c.entries[pos] = std::move(value);
    markDirty(id, c);
}

bool erase(GlobalPos const& pos) { return eraseImpl(pos, {}, true); }

bool has(GlobalPos const& pos) { return get(pos) != nullptr; }

std::optional<Json> take(GlobalPos const& pos) {
    auto id = chunkIdOf(pos);
    auto* c = chunkIfAny(id);
    if (!c) return std::nullopt;
    auto it = c->entries.find(pos);
    if (it == c->entries.end()) return std::nullopt;
    Json doc = std::move(it->second);
    c->entries.erase(it);
    markDirty(id, *c);
    return doc;
}

void put(GlobalPos const& pos, Json value) { set(pos, std::move(value)); }

void forEachInChunk(int dim, int cx, int cz, std::function<void(GlobalPos const&, Json const&)> const& fn) {
    auto* c = chunkIfAny({dim, cx, cz});
    if (!c) return;
    auto copy = c->entries; // callbacks may edit
    for (auto const& [p, v] : copy) fn(p, v);
}

void forEach(std::function<void(GlobalPos const&, Json const&)> const& fn) {
    buildIndex();
    auto ids = store().index;
    for (auto const& [dim, cx, cz] : ids) forEachInChunk(dim, cx, cz, fn);
}

void onErase(EraseListener fn) { store().listeners.push_back(std::move(fn)); }

void flush() { saveDirty(); }

class BlockDataModule {
public:
    void install() {
        game::db::onWorldSave([] { saveDirty(); });
        game::db::onWorldClosed([] { reset(); });
        game::db::onWorldReady([] { reset(); });
        mListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::BlockChangedEvent>(
            [](ll::event::BlockChangedEvent& ev) {
                if (ev.layer() != 0) return;
                auto& region = ev.blockSource();
                if (region.getILevel().isClientSide()) return;
                auto const& prev = ev.previousBlock();
                auto const& next = ev.newBlock();
                if (&prev.getBlockType() == &next.getBlockType()) return; // state change only
                auto const& p   = ev.pos();
                GlobalPos   pos{static_cast<int>(region.getDimensionId()), p.x, p.y, p.z};
                auto*       c = chunkIfAny(chunkIdOf(pos));
                if (!c || !c->entries.contains(pos)) return;
                eraseImpl(pos, prev.getTypeName(), true);
            }
        );
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mListener);
        if (game::db::available()) saveDirty();
    }

private:
    ll::event::ListenerPtr mListener;
};

} // namespace lv::feature::common::blockdata

LV_REGISTER_MODULE_ORDERED(lv::feature::common::blockdata::BlockDataModule, lv::ModuleOrder::Services);
