// Schematic table (Create's SchematicTableBlockEntity + ServerSchematicLoader): an empty schematic
// in the input slot, a written schematic in the output slot. Players with the client mod upload a
// file from their local schematics folder in chunks (companion protocol, Create's size limits);
// vanilla players pick one of their files stored on the server (form).

#include <algorithm>
#include <map>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "core/schematics/Inflate.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/net/Net.h"
#include "feature/schematics/Schematics.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::schematics {

namespace items   = common::items;
namespace screens = common::screens;
namespace fs      = std::filesystem;

namespace {

constexpr char const* kScreen = "schematics.table";

ItemStack slotOf(GlobalPos const& p, int slot) {
    auto const* d = dataIf(p);
    if (!d || !d->contains("inv")) return items::empty();
    auto const& inv = (*d)["inv"];
    if (!inv.is_array() || slot >= static_cast<int>(inv.size())) return items::empty();
    return items::fromJson(inv[static_cast<std::size_t>(slot)]);
}

void setSlot(GlobalPos const& p, int slot, ItemStack const& s) {
    auto& d = data(p);
    if (!d["inv"].is_array()) d["inv"] = Json::array({nullptr, nullptr});
    while (d["inv"].size() < 2) d["inv"].push_back(nullptr);
    d["inv"][static_cast<std::size_t>(slot)] = items::toJson(s);
}

bool isTable(GlobalPos const& p) { return idAt(p) == kTable; }

std::optional<GlobalPos> tableOf(Player& player, Json const& ctx) {
    auto p = posFrom(ctx.value("pos", Json()));
    if (!p || !isTable(*p) || !inReach(player, *p)) return std::nullopt;
    return p;
}

struct Upload {
    std::string  name;
    GlobalPos    table;
    std::size_t  expected = 0;
    std::string  bytes;
    int          idle     = 0;
};
std::map<std::int64_t, Upload> gUploads;

void setUploading(GlobalPos const& p, std::string const& name, float progress) {
    auto& d = data(p);
    if (name.empty()) {
        d.erase("uploading");
        d.erase("progress");
    } else {
        d["uploading"] = name;
        d["progress"]  = progress;
    }
}

void refreshViewer(std::int64_t uid, GlobalPos const& table) {
    if (auto* p = playerById(uid)) screens::refresh(*p, kScreen, {{"pos", posJson(table)}});
}

void cancelUpload(std::int64_t uid, std::string const& why) {
    auto it = gUploads.find(uid);
    if (it == gUploads.end()) return;
    log().warn("schematics: cancelled schematic upload {} ({})", it->second.name, why);
    auto table = it->second.table;
    auto name  = it->second.name;
    gUploads.erase(it);
    if (auto* p = playerById(uid)) net::send(*p, "schematics.upload.status", {{"name", name}, {"ok", false}, {"message", why}});
    if (isTable(table)) setUploading(table, "", 0);
    refreshViewer(uid, table);
}

Json tableData(Player& player, GlobalPos const& p) {
    auto const* d     = dataIf(p);
    Json        files = Json::array();
    for (auto const& f : listFiles(playerDir(player.getRealName()))) files.push_back(f);
    return Json{
        {"input",     items::toJson(slotOf(p, 0))                                },
        {"output",    items::toJson(slotOf(p, 1))                                },
        {"uploading", d ? d->value("uploading", std::string{}) : std::string{}  },
        {"progress",  d ? d->value("progress", 0.0) : 0.0                         },
        {"maxSize",   config().maxTotalSchematicSize                             },
        {"packet",    config().maxSchematicPacketSize                            },
        {"files",     files                                                      },
        {"pos",       posJson(p)                                                 },
    };
}

void tableAction(Player& player, Json const& ctx, Json const& action) {
    auto p = tableOf(player, ctx);
    if (!p) return;
    auto op = action.value("op", std::string{});
    if (op == "insert") {
        int  slot = action.value("slot", -1);
        auto& inv = player.getInventory();
        if (slot < 0 || slot >= inv.getContainerSize()) return;
        if (!isItem(inv.getItem(slot), kEmptySchematic) || !items::isEmpty(slotOf(*p, 0))) return;
        setSlot(*p, 0, takeFromInventory(player, slot, 1));
        log().info("schematic table at {} {} {}: {} put an empty schematic in", p->x, p->y, p->z, player.getRealName());
    } else if (op == "take") {
        int slot = std::clamp(action.value("slot", 0), 0, 1);
        auto s   = slotOf(*p, slot);
        if (items::isEmpty(s)) return;
        setSlot(*p, slot, items::empty());
        give(player, s);
    } else if (op == "write") {
        std::string reason;
        if (!writeOnTable(player, *p, action.value("file", std::string{}), &reason) && !reason.empty()) actionBar(player, reason);
    }
}

void tableForm(Player& player, Json const& ctx) {
    auto p = tableOf(player, ctx);
    if (!p) return;
    auto files = listFiles(playerDir(player.getRealName()));
    auto in    = slotOf(*p, 0);
    auto out   = slotOf(*p, 1);

    ll::form::SimpleForm form(trFor(player, "create.gui.schematicTable.title", "Schematic Table"));
    std::string content;
    content += trFor(player, "lv.schematics.table.input", "Input: {0}", items::isEmpty(in) ? trFor(player, "lv.common.form.empty", "(empty)") : items::displayName(in)) + "\n";
    content += trFor(player, "lv.schematics.table.output", "Output: {0}", items::isEmpty(out) ? trFor(player, "lv.common.form.empty", "(empty)") : items::displayName(out)) + "\n\n";
    content += trFor(player, "create.gui.schematicTable.availableSchematics", "Available Schematics");
    if (files.empty()) content += "\n" + trFor(player, "create.gui.schematicTable.noSchematics", "No Schematics Saved");
    form.setContent(content);
    auto pos = *p;
    if (items::isEmpty(in)) {
        form.appendButton(trFor(player, "lv.schematics.table.insert", "Put in an Empty Schematic"), [pos](Player& pl) {
            auto& inv = pl.getInventory();
            for (int i = 0; i < inv.getContainerSize(); ++i) {
                if (!isItem(inv.getItem(i), kEmptySchematic)) continue;
                tableAction(pl, {{"pos", posJson(pos)}}, {{"op", "insert"}, {"slot", i}});
                break;
            }
            tableForm(pl, {{"pos", posJson(pos)}});
        });
    }
    if (!items::isEmpty(out)) {
        form.appendButton(trFor(player, "lv.schematics.table.take", "Take the Written Schematic"), [pos](Player& pl) {
            tableAction(pl, {{"pos", posJson(pos)}}, {{"op", "take"}, {"slot", 1}});
        });
    }
    for (auto const& f : files) {
        form.appendButton(f, [pos, f](Player& pl) { tableAction(pl, {{"pos", posJson(pos)}}, {{"op", "write"}, {"file", f}}); });
    }
    form.sendTo(player);
}

} // namespace

bool writeOnTable(Player& player, GlobalPos const& table, std::string const& file, std::string* reason) {
    auto fail = [&](std::string r) {
        if (reason) *reason = std::move(r);
        return false;
    };
    if (!isTable(table)) return fail("");
    if (!cs::isSchematicFileName(file)) return fail(trFor(player, "create.schematic.error", "Schematic failed to Load - Check Game Logs"));
    if (!isItem(slotOf(table, 0), kEmptySchematic)) return fail(trFor(player, "lv.schematics.table.need_empty", "Put an Empty Schematic on the table first"));
    std::string err;
    if (!loadSchematic(player.getRealName(), file, &err)) {
        log().warn("schematics: {} could not write {}: {}", player.getRealName(), file, err);
        return fail(trFor(player, "create.schematic.error", "Schematic failed to Load - Check Game Logs"));
    }
    auto item = makeSchematic(file, player.getRealName());
    if (!item) return fail("");
    auto old = slotOf(table, 1);
    setSlot(table, 0, items::empty());
    setSlot(table, 1, *item);
    if (!items::isEmpty(old)) give(player, old);
    log().info("schematic table at {} {} {}: wrote {} for {}", table.x, table.y, table.z, file, player.getRealName());
    screens::refresh(player, kScreen, {{"pos", posJson(table)}});
    return true;
}

void openTable(Player& player, GlobalPos const& pos) { screens::open(player, kScreen, {{"pos", posJson(pos)}}); }

void tableRemoved(GlobalPos const& p, Json const& doc) {
    if (!doc.contains("inv")) return;
    for (auto const& s : doc["inv"]) dropAt(p, items::fromJson(s));
}

void tickUploads() {
    int timeout = config().schematicIdleTimeout;
    std::vector<std::int64_t> dead;
    for (auto& [uid, u] : gUploads)
        if (u.idle++ > timeout) dead.push_back(uid);
    for (auto uid : dead) cancelUpload(uid, "timed out");
}

void resetUploads() { gUploads.clear(); }

namespace detail {

void registerTable() {
    screens::registerType({
        .id   = kScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            auto pos = tableOf(p, ctx);
            if (!pos) return std::nullopt;
            return tableData(p, *pos);
        },
        .action = tableAction,
        .form   = tableForm,
    });

    // ---- uploads (Create's SchematicUploadPacket: BEGIN / WRITE / FINISH) ----
    net::on("schematics.upload.begin", [](Player& player, Json const& msg) {
        auto        uid  = uidOf(player);
        std::string name = msg.value("name", std::string{});
        auto        size = msg.value("size", std::int64_t{0});
        auto        pos  = posFrom(msg.value("pos", Json()));
        auto        deny = [&](std::string why) {
            net::send(player, "schematics.upload.status", {{"name", name}, {"ok", false}, {"message", why}});
        };
        if (!cs::isSchematicFileName(name)) {
            log().warn("schematics: {} attempted an upload with an unsupported name {}", player.getRealName(), name);
            return deny("");
        }
        auto maxKb = config().maxTotalSchematicSize;
        if (size <= 0 || size > static_cast<std::int64_t>(maxKb) * 1000) {
            message(player, trFor(player, "create.schematics.uploadTooLarge", "Your schematic exceeds limitations specified by the server.") + " (" + std::to_string(size / 1000) + " KB).");
            message(player, trFor(player, "create.schematics.maxAllowedSize", "The maximum allowed schematic file size is:") + " " + std::to_string(maxKb) + " KB");
            return deny("too large");
        }
        if (gUploads.contains(uid)) return deny("busy");
        if (!pos || !isTable(*pos) || !inReach(player, *pos)) return deny("no table");
        if (!isItem(slotOf(*pos, 0), kEmptySchematic)) return deny("no empty schematic");
        Upload u;
        u.name     = name;
        u.table    = *pos;
        u.expected = static_cast<std::size_t>(size);
        gUploads[uid] = std::move(u);
        // Create's startUpload: the empty schematic is used up now
        setSlot(*pos, 0, items::empty());
        setUploading(*pos, name, 0);
        log().info("schematics: {} started uploading {} ({} bytes)", player.getRealName(), name, size);
        net::send(player, "schematics.upload.status", {{"name", name}, {"ok", true}});
        refreshViewer(uid, *pos);
    });
    net::on("schematics.upload.chunk", [](Player& player, Json const& msg) {
        auto uid = uidOf(player);
        auto it  = gUploads.find(uid);
        if (it == gUploads.end() || it->second.name != msg.value("name", std::string{})) return;
        auto bytes = cs::base64Decode(msg.value("data", std::string{}));
        if (!bytes) return cancelUpload(uid, "bad chunk");
        auto& u = it->second;
        if (static_cast<int>(bytes->size()) > config().maxSchematicPacketSize) return cancelUpload(uid, "oversized upload packet");
        if (u.bytes.size() + bytes->size() > u.expected) return cancelUpload(uid, "received more data than expected");
        u.bytes += *bytes;
        u.idle = 0;
        if (isTable(u.table)) setUploading(u.table, u.name, static_cast<float>(double(u.bytes.size()) / double(std::max<std::size_t>(1, u.expected))));
        refreshViewer(uid, u.table);
    });
    net::on("schematics.upload.end", [](Player& player, Json const& msg) {
        auto uid = uidOf(player);
        auto it  = gUploads.find(uid);
        if (it == gUploads.end() || it->second.name != msg.value("name", std::string{})) return;
        Upload u = std::move(it->second);
        gUploads.erase(it);
        if (isTable(u.table)) setUploading(u.table, "", 0);
        std::string err;
        if (u.bytes.size() != u.expected || !cs::load(u.name, u.bytes, &err)) {
            log().warn("schematics: upload {} from {} is broken: {}", u.name, player.getRealName(), err);
            message(player, "§c" + trFor(player, "create.schematics.wrongFormat", "The schematic you are trying to upload is either corrupt or encoded wrongly! It should be encoded with GZIP compatible format."));
            refreshViewer(uid, u.table);
            return;
        }
        auto dir  = playerDir(player.getRealName());
        auto path = dir / upath(u.name);
        std::error_code ec;
        fs::remove(path, ec);
        deleteOldestIfFull(dir, config().maxSchematics);
        if (!writeFile(path, u.bytes)) {
            log().error("schematics: exception thrown when finishing upload {}/{}", player.getRealName(), u.name);
            return;
        }
        log().info("schematics: new schematic uploaded: {}/{}", player.getRealName(), u.name);
        if (!isTable(u.table)) return;
        auto item = makeSchematic(u.name, player.getRealName());
        if (item) {
            auto old = slotOf(u.table, 1);
            setSlot(u.table, 1, *item);
            if (!items::isEmpty(old)) dropAt(u.table, old);
        }
        net::send(player, "schematics.upload.status", {{"name", u.name}, {"ok", true}, {"finished", true}});
        refreshViewer(uid, u.table);
    });
}

} // namespace detail

} // namespace lv::feature::schematics
