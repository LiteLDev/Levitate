// Schematic and quill (Create's SchematicAndQuillHandler): two corners by right-click (mid-air
// at a distance with Ctrl, or sprinting for vanilla clients), face resizing by Ctrl-scroll (client
// mod), the name prompt (native screen / form) and saving, optionally converting the quill into a
// written schematic at once ("Save and Upload Immediately").

#include <cmath>
#include <map>

#include "ll/api/form/CustomForm.h"

#include "mc/deps/core/math/Vec3.h"
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

namespace {

struct QuillState {
    cs::Selection sel;
    bool          ctrl = false; ///< client mod: Ctrl held
};
std::map<std::int64_t, QuillState> gQuills;

QuillState& stateOf(Player const& p) { return gQuills[uidOf(p)]; }

constexpr char const* kNameScreen = "schematics.quill_name";

bool ctrlHeld(Player& p) {
    if (net::isCompanion(p)) return stateOf(p).ctrl;
    return p.isSprinting(); // vanilla clients: no Ctrl key reaches the server (SPEC §8)
}

void sync(Player& p) {
    if (!net::isCompanion(p)) return;
    auto const& s = stateOf(p).sel;
    Json        msg{{"range", s.range}};
    if (s.first) msg["first"] = {s.first->x, s.first->y, s.first->z};
    if (s.second) msg["second"] = {s.second->x, s.second->y, s.second->z};
    net::send(p, "schematics.quill", msg);
}

bool holdingQuill(Player& p) { return isItem(p.getSelectedItem(), kQuill); }

std::string sizeText(Player& p, cs::Pos size) {
    return trFor(p, "create.schematicAndQuill.dimensions", "Schematic Size: {0}x{1}x{2}", size.x, size.y, size.z);
}

void nameForm(Player& player, Json const&) {
    if (!holdingQuill(player) || !stateOf(player).sel.complete()) return;
    ll::form::CustomForm form(trFor(player, "create.schematicAndQuill.title", "Schematic Name:"));
    form.appendLabel(sizeText(player, stateOf(player).sel.size()));
    form.appendInput("name", trFor(player, "create.schematicAndQuill.title", "Schematic Name:"), trFor(player, "create.schematicAndQuill.fallbackName", "My Schematic"));
    form.appendToggle("convert", trFor(player, "create.schematicAndQuill.convert", "Save and Upload Immediately"), false);
    form.appendToggle("abort", trFor(player, "lv.schematics.quill.discard", "Discard the selection"), false);
    form.sendTo(player, [](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto get = [&](std::string const& k) -> ll::form::CustomFormElementResult const* {
            auto it = result->find(k);
            return it == result->end() ? nullptr : &it->second;
        };
        auto flag = [&](std::string const& k) {
            auto const* v = get(k);
            if (!v) return false;
            if (auto const* u = std::get_if<uint64>(v)) return *u != 0;
            return false;
        };
        if (flag("abort")) {
            quillDiscard(p);
            return;
        }
        std::string name;
        if (auto const* v = get("name"))
            if (auto const* s = std::get_if<std::string>(v)) name = *s;
        quillSave(p, name, flag("convert"));
    });
}

} // namespace

cs::Selection& selectionOf(Player const& player) { return stateOf(player).sel; }

void quillDiscard(Player& player) {
    auto& s  = stateOf(player).sel;
    s.first  = std::nullopt;
    s.second = std::nullopt;
    actionBar(player, trFor(player, "create.schematicAndQuill.abort", "Removed selection."));
    sync(player);
}

void quillUse(Player& player, std::optional<std::pair<cs::Pos, int>> hit) {
    auto& st = stateOf(player);
    auto& s  = st.sel;
    if (player.isSneaking()) {
        quillDiscard(player);
        return;
    }
    if (s.complete()) {
        screens::open(player, kNameScreen, Json::object());
        return;
    }
    std::optional<cs::Pos> selected;
    if (ctrlHeld(player)) {
        auto eye  = player.getEyePos();
        auto look = player.getViewVector(1.f);
        selected  = cs::Pos{
            static_cast<int>(std::floor(eye.x + look.x * s.range)),
            static_cast<int>(std::floor(eye.y + look.y * s.range)),
            static_cast<int>(std::floor(eye.z + look.z * s.range))
        };
    } else if (hit || (hit = lookTrace(player))) {
        auto [pos, face] = *hit;
        int dim          = static_cast<int>(player.getDimensionId());
        // Create: clicking the top / bottom of a solid block selects the block beyond that face
        if ((face == 0 || face == 1) && !replaceable(gp(dim, pos))) pos.y += face == 1 ? 1 : -1;
        selected = pos;
    }
    if (!selected) {
        actionBar(player, trFor(player, "create.schematicAndQuill.noTarget", "Hold [Ctrl] to select Air blocks."));
        return;
    }
    if (s.first) {
        s.second = selected;
        actionBar(player, trFor(player, "create.schematicAndQuill.secondPos", "Second position set."));
        auto size = s.size();
        log().info("schematic and quill: {} selected {}x{}x{}", player.getRealName(), size.x, size.y, size.z);
    } else {
        s.first = selected;
        actionBar(player, trFor(player, "create.schematicAndQuill.firstPos", "First position set."));
        log().info("schematic and quill: {} set the first corner at {} {} {}", player.getRealName(), selected->x, selected->y, selected->z);
    }
    sync(player);
}

std::optional<std::string> quillSave(Player& player, std::string name, bool convert) {
    auto& s = stateOf(player).sel;
    if (!s.complete()) return std::nullopt;
    auto [lo, hi] = s.box();
    int  dim      = static_cast<int>(player.getDimensionId());
    if (name.empty()) name = trFor(player, "create.schematicAndQuill.fallbackName", "My Schematic");
    auto dir   = playerDir(player.getRealName());
    auto& cfg  = config();
    // the server's folder of the player is Create's uploaded folder: same limit for every save
    deleteOldestIfFull(dir, cfg.maxSchematics);
    auto file = saveArea(dir, name, convert, dim, lo, hi);
    if (!file) {
        message(player, "§c" + trFor(player, "create.schematicAndQuill.failed", "Failed to save schematic, check logs for details"));
        return std::nullopt;
    }
    actionBar(player, trFor(player, "create.schematicAndQuill.saved", "Saved as {0}", *file));
    log().info("schematic and quill: {} saved {} ({}x{}x{})", player.getRealName(), *file, hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1);
    s.first  = std::nullopt;
    s.second = std::nullopt;
    sync(player);
    // the client mod keeps a copy in its own schematics folder (Create saves there)
    if (net::isCompanion(player)) {
        if (auto bytes = readFile(dir / upath(*file))) {
            constexpr std::size_t kChunk = 24 * 1024;
            std::size_t           count  = (bytes->size() + kChunk - 1) / kChunk;
            for (std::size_t i = 0; i < count; ++i) {
                net::send(player, "schematics.download", {{"name", *file}, {"index", i}, {"count", count}, {"data", cs::base64Encode(bytes->substr(i * kChunk, kChunk))}});
            }
        }
    }
    if (convert) {
        auto size = std::filesystem::file_size(dir / upath(*file));
        if (size > static_cast<std::uintmax_t>(cfg.maxTotalSchematicSize) * 1000) {
            message(player, "§c" + trFor(player, "create.schematics.uploadTooLarge", "Your schematic exceeds limitations specified by the server.") + " ("
                                + std::to_string(size / 1000) + " KB).");
            message(player, trFor(player, "create.schematics.maxAllowedSize", "The maximum allowed schematic file size is:") + " " + std::to_string(cfg.maxTotalSchematicSize) + " KB");
            return file;
        }
        if (!holdingQuill(player)) return file;
        auto item = makeSchematic(*file, player.getRealName());
        if (!item) {
            message(player, "§c" + trFor(player, "create.schematicAndQuill.instant_failed", "Schematic instant-upload failed, check logs for details"));
            return file;
        }
        player.setSelectedItem(*item);
        player.sendInventory(false);
        log().info("schematic and quill: {} converted {} into a schematic", player.getRealName(), *file);
    }
    return file;
}

void resetQuills() { gQuills.clear(); }

/// Particle outlines of the selection for vanilla players holding the quill (the client mod
/// draws Create's outline itself).
void tickQuillOutlines() {
    if (currentTick() % 10 != 0) return;
    for (auto& [uid, st] : gQuills) {
        auto* p = playerById(uid);
        if (!p || net::isCompanion(*p) || !holdingQuill(*p) || !st.sel.first) continue;
        auto [lo, hi] = st.sel.box();
        if (!st.sel.second) hi = lo;
        int    dim  = static_cast<int>(p->getDimensionId());
        double x0 = lo.x, y0 = lo.y, z0 = lo.z, x1 = hi.x + 1, y1 = hi.y + 1, z1 = hi.z + 1;
        auto   edge = [&](double ax, double ay, double az, double bx, double by, double bz) {
            double len = std::max({std::abs(bx - ax), std::abs(by - ay), std::abs(bz - az)});
            int    n   = std::max(1, static_cast<int>(len * 2));
            if (n > 400) n = 400;
            for (int i = 0; i <= n; ++i) {
                double t = static_cast<double>(i) / n;
                particle(dim, ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t, "minecraft:villager_happy");
            }
        };
        for (double y : {y0, y1}) {
            edge(x0, y, z0, x1, y, z0);
            edge(x0, y, z1, x1, y, z1);
            edge(x0, y, z0, x0, y, z1);
            edge(x1, y, z0, x1, y, z1);
        }
        edge(x0, y0, z0, x0, y1, z0);
        edge(x1, y0, z0, x1, y1, z0);
        edge(x0, y0, z1, x0, y1, z1);
        edge(x1, y0, z1, x1, y1, z1);
    }
}

namespace detail {

void registerQuill() {
    screens::registerType({
        .id   = kNameScreen,
        .data = [](Player& p, Json const&) -> std::optional<Json> {
            if (!holdingQuill(p) || !stateOf(p).sel.complete()) return std::nullopt;
            auto size = stateOf(p).sel.size();
            return Json{{"size", {size.x, size.y, size.z}}};
        },
        .action =
            [](Player& p, Json const&, Json const& action) {
                auto op = action.value("op", std::string{});
                if (op == "abort") quillDiscard(p);
                else if (op == "save") quillSave(p, action.value("name", std::string{}), action.value("convert", false));
            },
        .form = nameForm,
    });
    net::on("schematics.quill.ctrl", [](Player& p, Json const& msg) {
        auto& st = stateOf(p);
        st.ctrl  = msg.value("down", false);
        if (msg.contains("range")) st.sel.range = std::clamp(msg.value("range", 10), 1, 100);
    });
    net::on("schematics.quill.resize", [](Player& p, Json const& msg) {
        auto& s = stateOf(p).sel;
        if (!holdingQuill(p) || !s.complete()) return;
        s.resize(msg.value("face", -1), std::clamp(msg.value("delta", 0), -16, 16), msg.value("inside", false));
        actionBar(p, sizeText(p, s.size()));
        sync(p);
    });
}

} // namespace detail

} // namespace lv::feature::schematics
