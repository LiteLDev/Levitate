#pragma once

// BlockData (U3, SPEC §1.2 / §3.2): the block-entity replacement.
//
// Custom Bedrock blocks can't have block entities, so per-position state (inventories, tanks,
// kinetic speed, settings, filters, ...) lives here: one JSON object per (dimension, position),
// owned by the block's unit. Server thread only.
//
// - Persistence: grouped per chunk in the world DB (key "lv:common:bd:<dim>:<cx>:<cz>"), loaded
//   lazily on first access, written on `onWorldSave` (autosave / stop) when dirty.
// - Lifetime: an entry is erased automatically when the block at its position changes to a
//   different block type (broken, replaced, exploded, moved by a vanilla piston); block *state*
//   changes keep it. Erase listeners let owners drop items / fluids first.
// - Namespacing: units share a position's object by top-level key (use your unit name or the
//   block's path, e.g. `data["kinetics"]`, `data["filter"]`), so a block that takes part in
//   several systems keeps one document.
// - Contraptions (U4) move data with `take()` (before removing the block, so the automatic erase
//   doesn't drop it) and `put()` (after placing the block again).
//
//     auto& d = blockdata::edit(pos);     // creates {} if missing, marks the chunk dirty
//     d["speed"] = 64;
//     if (auto const* d = blockdata::get(pos)) speed = d->value("speed", 0);

#include <functional>
#include <optional>

#include "nlohmann/json.hpp"

#include "core/GlobalPos.h"

namespace lv::feature::common::blockdata {

using Json = nlohmann::json;
using core::GlobalPos;

/// The document at `pos`, or nullptr. The pointer is valid until the next BlockData call.
[[nodiscard]] Json const* get(GlobalPos const& pos);
/// The document at `pos` (created as {} when missing) for writing; marks the chunk dirty.
/// The reference is valid until the next BlockData call.
[[nodiscard]] Json& edit(GlobalPos const& pos);
/// Replaces the whole document (`null` erases it).
void set(GlobalPos const& pos, Json value);
/// Erases the document (runs erase listeners). Returns true if something was erased.
bool erase(GlobalPos const& pos);
/// Whether a document exists at `pos`.
[[nodiscard]] bool has(GlobalPos const& pos);

/// Removes the document and returns it without running erase listeners (contraption assembly).
[[nodiscard]] std::optional<Json> take(GlobalPos const& pos);
/// Stores a document taken with `take()` at a (new) position (contraption disassembly).
void put(GlobalPos const& pos, Json value);

/// Calls `fn(pos, doc)` for every document of chunk (dim, cx, cz) (loads it if needed).
void forEachInChunk(int dim, int cx, int cz, std::function<void(GlobalPos const&, Json const&)> const& fn);
/// Calls `fn(pos, doc)` for every document of every chunk that has data (loads them all;
/// commands and selftests only).
void forEach(std::function<void(GlobalPos const&, Json const&)> const& fn);

/// Listener run before a document is erased because its block changed or `erase()` was called:
/// `fn(pos, doc, previousBlockId)` (previousBlockId is "" for explicit erases). Use it to drop
/// the contents of an inventory.
using EraseListener = std::function<void(GlobalPos const&, Json const&, std::string const& previousBlockId)>;
void onErase(EraseListener fn);

/// Writes dirty chunks now (also done on every world save).
void flush();

/// Owners that keep their part of a document cached elsewhere (e.g. the kinetic network keeps
/// `data["kinetics"]` in its solver) write it back here: `fn(nullptr)` runs before every save /
/// `flush()`, `fn(&pos)` before `take(pos)` (so a contraption carries the current data).
using WriteBackFn = std::function<void(GlobalPos const* pos)>;
void onWriteBack(WriteBackFn fn);
/// Listener run after `put()` stored a moved document (contraption disassembly): owners with a
/// cache adopt the data at its new position.
using PutListener = std::function<void(GlobalPos const& pos, Json const& doc)>;
void onPut(PutListener fn);

} // namespace lv::feature::common::blockdata
