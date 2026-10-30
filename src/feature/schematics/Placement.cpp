// Written schematic (Create's SchematicItem + SchematicHandler, server side): positioning sent by
// the client mod's tools (Create's SchematicSyncPacket), the creative Print tool, the coordinates
// screen on sneak right-click (Create's SchematicEditScreen; form for vanilla players), the
// vanilla fallback (right-click a block to deploy there, particle outline of the bounds) and the
// block data the client mod needs to draw the ghost preview.

#include <algorithm>
#include <cmath>
#include <set>

#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/level/Level.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
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

constexpr char const* kEditScreen = "schematics.edit";

std::optional<SchematicData> held(Player& p) { return schematicData(p.getSelectedItem()); }

void store(Player& p, SchematicData const& d) {
    ItemStack s = p.getSelectedItem();
    setSchematicData(s, d);
    p.setSelectedItem(s);
    p.sendInventory(false);
}

std::array<cs::Pos, 2> worldBounds(SchematicData const& d) {
    auto b = cs::bounds(d.transform, d.size);
    return {d.anchor + b[0], d.anchor + b[1]};
}

std::string rotationName(Player& p, int r) {
    static constexpr std::array<std::pair<char const*, char const*>, 4> k{{
        {"create.schematic.rotation.none", "None"},
        {"create.schematic.rotation.cw90", "Clockwise 90"},
        {"create.schematic.rotation.cw180", "Clockwise 180"},
        {"create.schematic.rotation.cw270", "Clockwise 270"},
    }};
    auto const& e = k[static_cast<std::size_t>(std::clamp(r, 0, 3))];
    return trFor(p, e.first, e.second);
}

std::string mirrorName(Player& p, int m) {
    static constexpr std::array<std::pair<char const*, char const*>, 3> k{{
        {"create.schematic.mirror.none", "None"},
        {"create.schematic.mirror.leftRight", "Left-Right"},
        {"create.schematic.mirror.frontBack", "Front-Back"},
    }};
    auto const& e = k[static_cast<std::size_t>(std::clamp(m, 0, 2))];
    return trFor(p, e.first, e.second);
}

void editForm(Player& player, Json const&) {
    auto d = held(player);
    if (!d) return;
    ll::form::CustomForm form(d->file);
    form.appendLabel(trFor(player, "create.schematic.position", "Position"));
    form.appendInput("x", "X", "", std::to_string(d->anchor.x));
    form.appendInput("y", "Y", "", std::to_string(d->anchor.y));
    form.appendInput("z", "Z", "", std::to_string(d->anchor.z));
    std::vector<std::string> rot, mir;
    for (int i = 0; i < 4; ++i) rot.push_back(rotationName(player, i));
    for (int i = 0; i < 3; ++i) mir.push_back(mirrorName(player, i));
    form.appendDropdown("rotation", trFor(player, "create.schematic.rotation", "Rotation"), rot, d->transform.rotation);
    form.appendDropdown("mirror", trFor(player, "create.schematic.mirror", "Mirror"), mir, static_cast<int>(d->transform.mirror));
    if (player.isCreative())
        form.appendToggle("print", trFor(player, "create.schematic.tool.print", "Print") + " - " + trFor(player, "create.schematic.tool.print.description.0", "Instantly places the structure in the world."), false);
    form.sendTo(player, [](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto d = held(p);
        if (!d) return;
        auto num = [&](std::string const& k, int fallback) {
            auto it = result->find(k);
            if (it == result->end()) return fallback;
            if (auto const* s = std::get_if<std::string>(&it->second)) {
                try {
                    return std::stoi(*s);
                } catch (...) {
                    return fallback;
                }
            }
            if (auto const* u = std::get_if<uint64>(&it->second)) return static_cast<int>(*u);
            return fallback;
        };
        d->anchor             = {num("x", d->anchor.x), num("y", d->anchor.y), num("z", d->anchor.z)};
        d->transform.rotation = std::clamp(num("rotation", 0), 0, 3);
        d->transform.mirror   = static_cast<cs::Mirror>(std::clamp(num("mirror", 0), 0, 2));
        d->deployed           = true;
        store(p, *d);
        log().info("schematic: {} placed {} at {} {} {} (form)", p.getRealName(), d->file, d->anchor.x, d->anchor.y, d->anchor.z);
        if (num("print", 0) && p.isCreative()) printInstantly(&p, static_cast<int>(p.getDimensionId()), *d, config().creativePrintIncludesAir);
    });
}

std::set<std::string> gReported; ///< "<player>|<owner>/<file>"

} // namespace

void reportUnmapped(Player& player, std::string const& owner, std::string const& file) {
    auto loaded = loadSchematic(owner, file);
    if (!loaded || loaded->unmapped.empty()) return;
    auto key = player.getRealName() + "|" + owner + "/" + file;
    if (!gReported.insert(key).second) return;
    std::string list;
    for (std::size_t i = 0; i < loaded->unmapped.size() && i < 12; ++i) list += (i ? ", " : "") + loaded->unmapped[i];
    if (loaded->unmapped.size() > 12) list += ", ...";
    message(player, "§e" + trFor(player, "lv.schematics.unmapped", "{0} block type(s) of {1} have no Bedrock equivalent and will be skipped: {2}", loaded->unmapped.size(), file, list));
}

void schematicUse(Player& player, std::optional<std::pair<cs::Pos, int>> hit) {
    auto d = held(player);
    if (!d) {
        actionBar(player, "§c" + trFor(player, "create.schematic.invalid", "[!] Invalid Item - Use the Schematic Table instead"));
        return;
    }
    if (player.isSneaking()) {
        screens::open(player, kEditScreen, Json::object());
        return;
    }
    if (net::isCompanion(player)) return; // the client mod's tools position the schematic
    if (!hit) hit = lookTrace(player); // Create's tools trace up to 75 blocks
    if (!hit) return;
    // Create's DeployTool: the selected block becomes the bottom centre of the footprint
    auto [pos, face] = *hit;
    static constexpr std::array<cs::Pos, 6> kN{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
    if (face >= 0 && face < 6) pos = pos + kN[static_cast<std::size_t>(face)];
    auto b       = cs::bounds(d->transform, d->size);
    cs::Pos span = b[1] - b[0];
    cs::Pos min{pos.x - span.x / 2, pos.y, pos.z - span.z / 2};
    d->anchor   = min - b[0];
    d->deployed = true;
    store(player, *d);
    reportUnmapped(player, d->owner, d->file);
    actionBar(player, trFor(player, "create.schematic.position", "Position") + ": " + std::to_string(d->anchor.x) + " " + std::to_string(d->anchor.y) + " " + std::to_string(d->anchor.z));
    log().info("schematic: {} placed {} at {} {} {}", player.getRealName(), d->file, d->anchor.x, d->anchor.y, d->anchor.z);
}

int printInstantly(Player* player, int dim, SchematicData const& d, bool includeAir) {
    auto sch = loadSchematic(d.owner, d.file);
    if (!sch) return 0;
    auto const& s     = sch->schematic;
    int         count = 0;
    for (std::size_t i = 0; i < s.blocks.size(); ++i) {
        auto idx = s.blocks[i];
        if (idx < 0) continue;
        auto const& st = s.palette[static_cast<std::size_t>(idx)];
        if (st.isAir() && !includeAir) continue;
        auto state = st;
        cs::transformStates(state, d.transform);
        auto const* block = resolve(state);
        if (!block) continue;
        auto      local = s.posOf(i);
        GlobalPos at    = gp(dim, d.anchor + cs::apply(d.transform, local));
        auto const* cur = blockAt(at);
        if (!cur || unbreakable(*cur)) continue;
        place(at, *block, 2);
        if (auto it = s.levitateData.find(i); it != s.levitateData.end()) {
            auto doc = Json::parse(it->second, nullptr, false);
            if (!doc.is_discarded()) common::blockdata::set(at, doc);
        }
        if (!st.isAir()) ++count;
    }
    // Create's creative print places the template with its block entity NBT and its entities
    int  data = 0, spawned = 0;
    bool bedrock = s.format == "mcstructure"; // Java files carry Java NBT: blocks only
    if (bedrock) {
        for (auto const& [i, tag] : s.blockEntities)
            if (s.blocks[i] >= 0 && placeBlockEntity(gp(dim, d.anchor + cs::apply(d.transform, s.posOf(i))), tag, s.worldOrigin, d.transform, d.anchor)) ++data;
        for (auto const& e : s.entityTags)
            if (placeEntity(dim, e, s.worldOrigin, d.transform, d.anchor)) ++spawned;
    }
    if (player)
        log().info("schematic: {} printed {} ({} blocks, {} block entities, {} entities)", player->getRealName(), d.file, count, data, spawned);
    return count;
}

void tickPlacementOutlines() {
    if (currentTick() % 10 != 5) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([](Player& p) {
        if (net::isCompanion(p)) return true;
        auto d = held(p);
        if (!d || !d->deployed) return true;
        auto [lo, hi] = worldBounds(*d);
        int    dim    = static_cast<int>(p.getDimensionId());
        double x0 = lo.x, y0 = lo.y, z0 = lo.z, x1 = hi.x + 1, y1 = hi.y + 1, z1 = hi.z + 1;
        auto   edge   = [&](double ax, double ay, double az, double bx, double by, double bz) {
            double len = std::max({std::abs(bx - ax), std::abs(by - ay), std::abs(bz - az)});
            int    n   = std::clamp(static_cast<int>(len), 1, 200);
            for (int i = 0; i <= n; ++i) {
                double t = static_cast<double>(i) / n;
                particle(dim, ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t, "minecraft:blue_flame_particle");
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
        return true;
    });
}

namespace detail {

void registerPlacement() {
    screens::registerType({
        .id   = kEditScreen,
        .data = [](Player& p, Json const&) -> std::optional<Json> {
            auto d = held(p);
            if (!d) return std::nullopt;
            return Json{
                {"file",     d->file                                    },
                {"anchor",   {d->anchor.x, d->anchor.y, d->anchor.z}    },
                {"rotation", d->transform.rotation                      },
                {"mirror",   static_cast<int>(d->transform.mirror)      },
                {"deployed", d->deployed                                },
            };
        },
        .action =
            [](Player& p, Json const&, Json const& action) {
                auto d = held(p);
                if (!d || action.value("op", std::string{}) != "set") return;
                auto a = action.value("anchor", Json::array());
                if (a.is_array() && a.size() == 3) d->anchor = {a[0].get<int>(), a[1].get<int>(), a[2].get<int>()};
                d->transform.rotation = std::clamp(action.value("rotation", d->transform.rotation), 0, 3);
                d->transform.mirror   = static_cast<cs::Mirror>(std::clamp(action.value("mirror", static_cast<int>(d->transform.mirror)), 0, 2));
                d->deployed           = true;
                store(p, *d);
                screens::close(p, kEditScreen);
                log().info("schematic: {} placed {} at {} {} {}", p.getRealName(), d->file, d->anchor.x, d->anchor.y, d->anchor.z);
            },
        .form = editForm,
    });

    // Create's SchematicSyncPacket: the client tools moved / rotated / mirrored / deployed
    net::on("schematics.sync", [](Player& p, Json const& msg) {
        if (msg.value("slot", -1) != p.getSelectedItemSlot()) return;
        auto d = held(p);
        if (!d) return;
        auto a = msg.value("anchor", Json::array());
        if (!a.is_array() || a.size() != 3) return;
        d->anchor             = {a[0].get<int>(), a[1].get<int>(), a[2].get<int>()};
        d->transform.rotation = std::clamp(msg.value("rotation", 0), 0, 3);
        d->transform.mirror   = static_cast<cs::Mirror>(std::clamp(msg.value("mirror", 0), 0, 2));
        bool wasDeployed      = d->deployed;
        d->deployed           = msg.value("deployed", true);
        store(p, *d);
        if (!wasDeployed && d->deployed) {
            reportUnmapped(p, d->owner, d->file);
            log().info("schematic: {} placed {} at {} {} {}", p.getRealName(), d->file, d->anchor.x, d->anchor.y, d->anchor.z);
        }
    });
    // Create's PlaceTool (creative only)
    net::on("schematics.print", [](Player& p, Json const& msg) {
        if (!p.isCreative() || msg.value("slot", -1) != p.getSelectedItemSlot()) return;
        auto d = held(p);
        if (!d || !d->deployed) return;
        printInstantly(&p, static_cast<int>(p.getDimensionId()), *d, config().creativePrintIncludesAir);
    });
    // the ghost preview: palette (block SNBT, "" = unmapped) + run-length block indices
    net::on("schematics.preview.request", [](Player& p, Json const& msg) {
        // any written schematic of that file in the inventory (the hand may have changed meanwhile)
        std::optional<SchematicData> d;
        auto&                        inv = p.getInventory();
        for (int i = 0; i < inv.getContainerSize() && !d; ++i) {
            auto sd = schematicData(inv.getItem(i));
            if (sd && sd->file == msg.value("file", std::string{}) && sd->owner == msg.value("owner", std::string{})) d = sd;
        }
        if (!d) return;
        std::string err;
        auto        loaded = loadSchematic(d->owner, d->file, &err);
        if (!loaded) {
            actionBar(p, "§c" + trFor(p, "create.schematic.error", "Schematic failed to Load - Check Game Logs"));
            return;
        }
        auto const& s       = loaded->schematic;
        Json        palette = Json::array();
        for (std::size_t i = 0; i < s.palette.size(); ++i) {
            auto const* b = loaded->blocks[i];
            palette.push_back(b && !s.palette[i].isAir() ? Json{{"snbt", blockSnbt(*b)}, {"name", s.palette[i].name}} : Json{{"snbt", ""}, {"name", s.palette[i].name}});
        }
        Json         runs = Json::array();
        std::int32_t last = -2;
        int          n    = 0;
        for (auto v : s.blocks) {
            if (v == last) {
                ++n;
                continue;
            }
            if (n) runs.push_back({last, n});
            last = v;
            n    = 1;
        }
        if (n) runs.push_back({last, n});
        net::send(p, "schematics.preview", {{"owner", d->owner}, {"file", d->file}, {"size", {s.size.x, s.size.y, s.size.z}}, {"palette", palette}, {"runs", runs}});
        reportUnmapped(p, d->owner, d->file);
    });
}

} // namespace detail

} // namespace lv::feature::schematics
