// Toolboxes (Create's ToolboxBlock / ToolboxBlockEntity / ToolboxInventory / ToolboxHandler):
// 8 compartments of 4 stacks kept in BlockData, the toolbox screen (native + form), dyeing,
// picking up with contents, the item capability, and hotbar slots linked to a compartment
// ("equipped" through the radial menu of the client mod or the vanilla form).

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/platform/UUID.h"
#include "mc/world/PlayerUIContainer.h"
#include "mc/world/inventory/network/crafting/ItemStackRequestActionCraftHandler.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "core/equipment/Toolbox.h"
#include "core/equipment/ToolboxLid.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/equipment/Equipment.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Commands.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;
namespace bd    = common::blockdata;

namespace {

constexpr char kScreen[] = "equipment.toolbox";

// ---- stacks ---------------------------------------------------------------------------------------

/// Items that can't go inside container items (Create: canFitInsideContainerItems).
bool fitsInToolbox(std::string const& id) {
    return !(isToolbox(id) || id.find("shulker_box") != std::string::npos || id == "minecraft:bundle");
}

ce::TStack toT(ItemStack const& s) {
    if (items::isEmpty(s)) return {};
    auto one = items::withCount(s, 1);
    ce::TStack t;
    t.payload = items::toJson(one).dump();
    // damageable unstackable tools share a compartment whatever their damage (Create)
    t.match = (s.getMaxStackSize() <= 1 && s.isDamageableItem()) ? items::idOf(s) : t.payload;
    t.count = items::countOf(s);
    t.max   = std::max<int>(1, s.getMaxStackSize());
    return t;
}

ItemStack toItem(ce::TStack const& t) {
    if (t.empty()) return items::empty();
    auto j = Json::parse(t.payload, nullptr, false);
    if (j.is_discarded()) return items::empty();
    return items::withCount(items::fromJson(j), t.count);
}

Json const* docOf(GlobalPos const& pos) {
    auto const* d = bd::get(pos);
    if (!d || !d->contains("equipment")) return nullptr;
    auto const& e = (*d)["equipment"];
    return e.is_object() && e.contains("toolbox") ? &e["toolbox"] : nullptr;
}

ce::ToolboxInv load(GlobalPos const& pos) {
    auto const* d = docOf(pos);
    return d ? ce::ToolboxInv::fromJson(*d) : ce::ToolboxInv{};
}

void store(GlobalPos const& pos, ce::ToolboxInv const& inv) {
    auto& doc = bd::edit(pos);
    auto& e   = doc["equipment"];
    if (!e.is_object()) e = Json::object();
    e["toolbox"] = inv.toJson();
}

std::string colourOf(std::string_view id) {
    std::string s(id);
    if (!s.starts_with("levitate:") || !s.ends_with("_toolbox")) return {};
    return s.substr(9, s.size() - 9 - 8);
}

// ---- links (Create's ToolboxHandler: hotbar slot -> toolbox compartment) --------------------------

struct Link {
    GlobalPos box;
    int       compartment = 0;
};
/// player uuid -> hotbar slot -> link
std::map<std::string, std::map<int, Link>>& links() {
    static std::map<std::string, std::map<int, Link>> m;
    return m;
}
bool gLinksDirty = false;
constexpr char kLinksKey[] = "lv:equipment:toolbox_links";

std::string uuidOf(Player const& p) { return p.getUuid().asString(); }

double rangeSq() { return double(config().toolboxRange) * config().toolboxRange; }

bool inRange(Player const& p, GlobalPos const& box) {
    if (static_cast<int>(p.getDimensionId()) != box.dim) return false;
    auto const& v = p.getPosition();
    return ce::inToolboxRange(v.x, v.y, v.z, box.x, box.y, box.z, config().toolboxRange);
}

void saveLinks() {
    if (!gLinksDirty || !game::db::available()) return;
    Json j = Json::object();
    for (auto const& [uuid, slots] : links()) {
        Json arr = Json::array();
        for (auto const& [slot, l] : slots) arr.push_back({slot, l.box.dim, l.box.x, l.box.y, l.box.z, l.compartment});
        if (!arr.empty()) j[uuid] = arr;
    }
    game::db::save(kLinksKey, j);
    gLinksDirty = false;
}

void loadLinks() {
    links().clear();
    auto j = game::db::load(kLinksKey);
    if (!j || !j->is_object()) return;
    for (auto const& [uuid, arr] : j->items()) {
        if (!arr.is_array()) continue;
        for (auto const& e : arr) {
            if (!e.is_array() || e.size() != 6) continue;
            links()[uuid][e[0].get<int>()] = Link{{e[1].get<int>(), e[2].get<int>(), e[3].get<int>(), e[4].get<int>()}, e[5].get<int>()};
        }
    }
}

void unlinkBox(GlobalPos const& box) {
    for (auto& [uuid, slots] : links())
        for (auto it = slots.begin(); it != slots.end();) {
            if (it->second.box == box) {
                it          = slots.erase(it);
                gLinksDirty = true;
            } else ++it;
        }
}

// ---- lid (Create's ToolboxBlockEntity openTracker: the lid is open while anyone has the screen open) ----

/// Block state of the lid: 0 closed, 1 open (vanilla clients see the open model), 2 the body only
/// (meshed by the client mod, which draws lid and drawers animated; never set in the world).
constexpr char kLid[] = "levitate:toolbox_lid";

struct Openers {
    std::set<std::int64_t> players;
    std::uint64_t          holdUntil = 0; ///< `/create toolbox lid` (debug): open until this tick
    [[nodiscard]] bool     open(std::uint64_t now) const { return !players.empty() || holdUntil > now; }
};
std::map<GlobalPos, Openers>& openers() {
    static std::map<GlobalPos, Openers> m;
    return m;
}
/// Toolboxes whose lid is closing: the tick of Create's iron door close sound (the lid shut).
std::map<GlobalPos, std::uint64_t>& closing() {
    static std::map<GlobalPos, std::uint64_t> m;
    return m;
}

void setLid(GlobalPos const& pos, bool open) {
    if (!isToolbox(idAt(pos)) || stateAt(pos, kLid).value_or(0) == (open ? 1 : 0)) return;
    place(pos, idAt(pos), {{kFacing, stateAt(pos, kFacing).value_or(0)}, {kLid, open ? 1 : 0}});
}

void lidOpened(GlobalPos const& pos) {
    closing().erase(pos);
    setLid(pos, true);
    // Create: iron door + chest open sounds when the lid starts to open
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "open.iron_door", .25f, 1.2f + float(random01()) * .1f);
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.chestopen", .1f, 1.1f + float(random01()) * .1f);
    log().info("equipment: toolbox lid at {} {} {} opened", pos.x, pos.y, pos.z);
}

void lidClosed(GlobalPos const& pos) {
    setLid(pos, false);
    if (!isToolbox(idAt(pos))) return;
    // Create: the chest close sound as the lid starts to close, the iron door when it is shut (5 ticks)
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.chestclosed", .1f, 1.1f + float(random01()) * .1f);
    closing()[pos] = currentTick() + 5;
    log().info("equipment: toolbox lid at {} {} {} closed", pos.x, pos.y, pos.z);
}

void addOpener(GlobalPos const& pos, std::int64_t uid) {
    auto& o       = openers()[pos];
    bool  wasOpen = o.open(currentTick());
    o.players.insert(uid);
    if (!wasOpen) lidOpened(pos);
}

void removeOpener(GlobalPos const& pos, std::int64_t uid) {
    auto it = openers().find(pos);
    if (it == openers().end() || !it->second.players.erase(uid)) return;
    if (it->second.open(currentTick())) return;
    openers().erase(it);
    lidClosed(pos);
}

/// Every 10 ticks: players that left, went away (Create's menu: 8 blocks) or toolboxes that are gone.
void pruneOpeners() {
    auto now = currentTick();
    for (auto it = openers().begin(); it != openers().end();) {
        auto const& pos = it->first;
        auto&       o   = it->second;
        if (!isToolbox(idAt(pos))) {
            if (blockAt(pos)) { // loaded and gone
                it = openers().erase(it);
                continue;
            }
            ++it;
            continue;
        }
        for (auto p = o.players.begin(); p != o.players.end();) {
            auto* player = playerById(*p);
            bool  near   = false;
            if (player && static_cast<int>(player->getDimensionId()) == pos.dim) {
                auto const& v  = player->getPosition();
                double      dx = v.x - (pos.x + .5), dy = v.y - (pos.y + .5), dz = v.z - (pos.z + .5);
                near           = dx * dx + dy * dy + dz * dz <= 64.0;
            }
            p = near ? std::next(p) : o.players.erase(p);
        }
        if (!o.open(now)) {
            GlobalPos p = pos;
            it          = openers().erase(it);
            lidClosed(p);
            continue;
        }
        ++it;
    }
    for (auto it = closing().begin(); it != closing().end();) {
        if (it->second > now) {
            ++it;
            continue;
        }
        auto const& p = it->first;
        if (isToolbox(idAt(p))) sound(p.dim, p.x + .5, p.y + .5, p.z + .5, "close.iron_door", .25f, 1.2f + float(random01()) * .1f);
        it = closing().erase(it);
    }
}

// ---- screen ----------------------------------------------------------------------------------------

Json screenData(Player& player, GlobalPos const& pos) {
    auto inv = load(pos);
    Json comps = Json::array();
    for (int c = 0; c < ce::kCompartments; ++c) {
        auto first = inv.first(c);
        Json item  = nullptr;
        if (!first.empty()) item = items::toJson(toItem(first.withCount(1)));
        Json filter = nullptr;
        if (!inv.filters[static_cast<std::size_t>(c)].empty()) filter = items::toJson(toItem(inv.filters[static_cast<std::size_t>(c)]));
        comps.push_back({{"item", item}, {"count", inv.count(c)}, {"filter", filter}});
    }
    return {
        {"pos", {pos.dim, pos.x, pos.y, pos.z}},
        {"colour", colourOf(idAt(pos))},
        {"title", trFor(player, "block.levitate." + colourOf(idAt(pos)) + "_toolbox", "Toolbox")},
        {"compartments", comps},
    };
}

std::optional<GlobalPos> posFrom(Json const& ctx) {
    auto p = ctx.value("pos", Json::array());
    if (!p.is_array() || p.size() != 4) return std::nullopt;
    return GlobalPos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>(), p[3].get<int>()};
}

bool canUse(Player& player, GlobalPos const& pos) {
    if (!isToolbox(idAt(pos))) return false;
    auto const& v = player.getPosition();
    double      dx = v.x - (pos.x + .5), dy = v.y - (pos.y + .5), dz = v.z - (pos.z + .5);
    return static_cast<int>(player.getDimensionId()) == pos.dim && dx * dx + dy * dy + dz * dz <= 64.0;
}

/// Takes a stack (or half a stack) out of a compartment into the player's inventory.
void takeFrom(Player& player, GlobalPos const& pos, int c, bool half) {
    auto inv   = load(pos);
    auto first = inv.first(c);
    if (first.empty()) {
        // clicking an empty slot with an empty cursor clears the compartment's filter
        if (inv.clearFilter(c)) store(pos, inv);
        return;
    }
    int n   = half ? std::max(1, (std::min(first.count, first.max) + 1) / 2) : first.max;
    auto got = inv.extract(c, n, false);
    store(pos, inv);
    giveOrDrop(player, toItem(got));
}

/// Moves an inventory stack into the toolbox (shift-click).
void putFrom(Player& player, GlobalPos const& pos, int slot) {
    auto& container = player.getInventory();
    if (slot < 0 || slot >= container.getContainerSize()) return;
    ItemStack s = container.getItem(slot);
    if (items::isEmpty(s) || !fitsInToolbox(items::idOf(s))) return;
    auto inv  = load(pos);
    auto rest = inv.insert(toT(s), false, false);
    store(pos, inv);
    container.setItem(slot, rest.empty() ? items::empty() : items::withCount(s, rest.count));
    player.sendInventory(false);
}

void openForm(Player& player, GlobalPos const& pos);

void registerScreen() {
    common::screens::registerType({
        .id   = kScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            auto pos = posFrom(ctx);
            if (!pos || !canUse(p, *pos)) return std::nullopt;
            return screenData(p, *pos);
        },
        .action =
            [](Player& p, Json const& ctx, Json const& action) {
                auto pos = posFrom(ctx);
                if (!pos || !canUse(p, *pos)) return;
                auto op = action.value("op", std::string{});
                if (op == "take") takeFrom(p, *pos, action.value("c", -1), action.value("half", false));
                else if (op == "put") putFrom(p, *pos, action.value("slot", -1));
            },
        .form = [](Player& p, Json const& ctx) {
            if (auto pos = posFrom(ctx)) openForm(p, *pos);
        },
        .closed = [](Player& p, Json const& ctx) {
            if (auto pos = posFrom(ctx)) removeOpener(*pos, uidOf(p));
        },
    });
}

void openForm(Player& player, GlobalPos const& pos) {
    auto inv = load(pos);
    ll::form::SimpleForm form;
    form.setTitle(trFor(player, "block.levitate." + colourOf(idAt(pos)) + "_toolbox", "Toolbox"));
    form.setContent(trFor(player, "lv.equipment.toolbox.form", "Take a stack from a compartment, or put the held item in."));
    for (int c = 0; c < ce::kCompartments; ++c) {
        auto        first = inv.first(c);
        std::string text;
        if (!first.empty()) text = items::displayName(toItem(first)) + " x" + std::to_string(inv.count(c));
        else if (!inv.filters[static_cast<std::size_t>(c)].empty())
            text = items::displayName(toItem(inv.filters[static_cast<std::size_t>(c)])) + " x0";
        else text = trFor(player, "lv.equipment.toolbox.empty", "Empty compartment");
        form.appendButton(text);
    }
    form.appendButton(trFor(player, "lv.equipment.toolbox.put_held", "Put the held item in"));
    // one callback for the buttons and closing the form (the lid closes with it)
    form.sendTo(player, [pos](Player& p, int index, ll::form::FormCancelReason reason) {
        if (reason || index < 0 || !canUse(p, pos)) {
            removeOpener(pos, uidOf(p));
            return;
        }
        if (index < ce::kCompartments) takeFrom(p, pos, index, false);
        else putFrom(p, pos, p.getSelectedItemSlot());
        openForm(p, pos);
    });
}

// ---- radial menu protocol ---------------------------------------------------------------------------

Json radialData(Player& player) {
    Json boxes = Json::array();
    for (auto const& box : nearbyToolboxes(player)) {
        auto inv   = load(box);
        Json comps = Json::array();
        for (int c = 0; c < ce::kCompartments; ++c) {
            auto first = inv.first(c);
            comps.push_back({{"item", first.empty() ? Json(nullptr) : items::toJson(toItem(first.withCount(1)))}, {"count", inv.count(c)}});
        }
        boxes.push_back({{"pos", {box.dim, box.x, box.y, box.z}}, {"colour", colourOf(idAt(box))}, {"compartments", comps}});
    }
    int  slot     = player.getSelectedItemSlot();
    Json equipped = nullptr;
    auto lit      = links().find(uuidOf(player));
    if (lit != links().end()) {
        auto it = lit->second.find(slot);
        if (it != lit->second.end()) {
            equipped = {
                {"pos", {it->second.box.dim, it->second.box.x, it->second.box.y, it->second.box.z}},
                {"compartment", it->second.compartment},
                {"inRange", inRange(player, it->second.box)},
                {"item", items::toJson(player.getInventory().getItem(slot))},
            };
        }
    }
    Json linked = Json::array();
    if (lit != links().end())
        for (auto const& [s, l] : lit->second) linked.push_back({s, inRange(player, l.box)});
    return {{"boxes", boxes}, {"slot", slot}, {"equipped", equipped}, {"linked", linked}};
}

void sendLinked(Player& player) {
    if (!net::isCompanion(player)) return;
    Json linked = Json::array();
    auto lit    = links().find(uuidOf(player));
    if (lit != links().end())
        for (auto const& [s, l] : lit->second) linked.push_back({s, inRange(player, l.box)});
    net::send(player, "equipment.toolbox.linked", {{"linked", linked}});
}

void registerNet() {
    net::on("equipment.toolbox.radial", [](Player& p, Json const&) { net::send(p, "equipment.toolbox.radial", radialData(p)); });
    net::on("equipment.toolbox.select", [](Player& p, Json const& msg) {
        auto pos = posFrom(msg);
        if (!pos) return;
        equipFromToolbox(p, *pos, msg.value("compartment", -1), p.getSelectedItemSlot());
    });
    net::on("equipment.toolbox.unequip", [](Player& p, Json const& msg) { unequipSlot(p, p.getSelectedItemSlot(), msg.value("keep", false)); });
    net::on("equipment.toolbox.deposit", [](Player& p, Json const& msg) {
        std::vector<GlobalPos> boxes;
        if (auto pos = posFrom(msg)) {
            if (!isToolbox(idAt(*pos)) || !inRange(p, *pos)) return;
            boxes.push_back(*pos);
        } else boxes = nearbyToolboxes(p);
        depositAll(p, boxes);
    });
}

// ---- item capability --------------------------------------------------------------------------------

common::items::Handler handler() {
    common::items::Handler h;
    h.slots   = [](common::Access const&) { return ce::kToolboxSlots; };
    h.getSlot = [](common::Access const& a, int slot) {
        auto inv = load(game::globalPos(a.region, a.pos));
        if (slot < 0 || slot >= ce::kToolboxSlots) return items::empty();
        return toItem(inv.slots[static_cast<std::size_t>(slot)]);
    };
    h.insert = [](common::Access const& a, ItemStack const& stack, bool simulate) {
        if (items::isEmpty(stack) || !fitsInToolbox(items::idOf(stack))) return stack;
        auto p    = game::globalPos(a.region, a.pos);
        auto inv  = load(p);
        auto rest = inv.insert(toT(stack), false, simulate);
        if (!simulate) store(p, inv);
        return rest.empty() ? items::empty() : items::withCount(stack, rest.count);
    };
    h.extract = [](common::Access const& a, common::items::ItemPredicate const& filter, int amount, bool simulate) {
        auto p   = game::globalPos(a.region, a.pos);
        auto inv = load(p);
        for (int c = 0; c < ce::kCompartments; ++c) {
            auto first = inv.first(c);
            if (first.empty() || !filter(toItem(first))) continue;
            auto got = inv.extract(c, std::min(amount, first.max), simulate);
            if (!simulate) store(p, inv);
            return toItem(got);
        }
        return items::empty();
    };
    return h;
}

// ---- breaking -----------------------------------------------------------------------------------------

/// Positions picked up by a left-click (the item was given already).
std::map<GlobalPos, bool>& pickedUp() {
    static std::map<GlobalPos, bool> m;
    return m;
}

ItemStack toolboxItem(std::string const& id, ce::ToolboxInv const& inv) {
    auto item = items::make(id, 1);
    if (!item) return items::empty();
    bool any = false;
    for (int c = 0; c < ce::kCompartments; ++c) any = any || inv.count(c) > 0 || !inv.filters[static_cast<std::size_t>(c)].empty();
    if (any) items::setData(*item, kToolboxData, {{"inv", inv.toJson()}});
    return *item;
}

} // namespace

bool isToolbox(std::string_view id) {
    return id.starts_with("levitate:") && id.ends_with("_toolbox") && !colourOf(id).empty();
}

std::string toolboxId(std::string_view colour) { return "levitate:" + std::string(colour) + "_toolbox"; }

Json toolboxInventory(GlobalPos const& pos) {
    auto const* d = docOf(pos);
    return d ? *d : Json::object();
}

void setToolboxInventory(GlobalPos const& pos, Json const& inv) { store(pos, ce::ToolboxInv::fromJson(inv)); }

std::vector<GlobalPos> nearbyToolboxes(Player& player) {
    auto*                  region = regionOf(static_cast<int>(player.getDimensionId()));
    std::vector<GlobalPos> out;
    if (!region) return out;
    auto const& v   = player.getPosition();
    int         r   = config().toolboxRange;
    int         dim = static_cast<int>(player.getDimensionId());
    std::vector<std::pair<double, GlobalPos>> found;
    // every toolbox block in range (toolboxes placed by commands have no data until used)
    int bx = int(std::floor(v.x)), by = int(std::floor(v.y)), bz = int(std::floor(v.z));
    for (int y = by - r; y <= by + r; ++y) {
        if (y < region->getMinHeight() || y >= region->getMaxHeight()) continue;
        for (int x = bx - r; x <= bx + r; ++x)
            for (int z = bz - r; z <= bz + r; ++z) {
                GlobalPos p{dim, x, y, z};
                if (!inRange(player, p)) continue;
                BlockPos bp{x, y, z};
                if (!region->hasChunksAt(bp, 0, false)) continue;
                if (!isToolbox(region->getBlock(bp).getTypeName())) continue;
                double dx = v.x - (p.x + .5), dy = v.y - p.y, dz = v.z - (p.z + .5);
                found.emplace_back(dx * dx + dy * dy + dz * dz, p);
            }
    }
    std::sort(found.begin(), found.end());
    for (std::size_t i = 0; i < found.size() && i < 8; ++i) out.push_back(found[i].second);
    return out;
}

void openToolbox(Player& player, GlobalPos const& pos) {
    if (screenDebounced(player)) return;
    if (!common::screens::open(player, kScreen, {{"pos", {pos.dim, pos.x, pos.y, pos.z}}})) return;
    // the lid opens (with Create's sounds) while anyone has the screen open
    addOpener(pos, uidOf(player));
    log().info("equipment: toolbox at {} {} {} opened by {}", pos.x, pos.y, pos.z, player.getRealName());
}

bool useToolbox(Player& player, ItemStack& held, GlobalPos const& pos) {
    std::string id   = idAt(pos);
    std::string hid  = items::isEmpty(held) ? std::string() : items::idOf(held);
    // dye: recolour in place (Create does not use up the dye)
    if (hid.starts_with("minecraft:") && hid.ends_with("_dye")) {
        std::string colour = hid.substr(10, hid.size() - 10 - 4);
        if (std::find(kColours.begin(), kColours.end(), colour) != kColours.end() && toolboxId(colour) != id) {
            int  facing = stateAt(pos, kFacing).value_or(0);
            int  lid    = stateAt(pos, kLid).value_or(0) == 1 ? 1 : 0;
            auto doc    = bd::take(pos);
            if (place(pos, toolboxId(colour), {{kFacing, facing}, {kLid, lid}})) {
                if (doc) bd::put(pos, *doc);
                // links keep pointing at the same position
                log().info("equipment: toolbox at {} {} {} dyed {} by {}", pos.x, pos.y, pos.z, colour, player.getRealName());
            } else if (doc) bd::put(pos, *doc);
            return true;
        }
    }
    if (player.isSneaking()) return false; // Create: sneak passes through (placing against it)
    openToolbox(player, pos);
    return true;
}

bool attackToolbox(Player& player, GlobalPos const& pos) {
    std::string id = idAt(pos);
    if (!isToolbox(id) || player.isClientSide()) return false;
    auto inv        = load(pos);
    pickedUp()[pos] = true;
    if (!clearAt(pos)) {
        pickedUp().erase(pos);
        return true; // nothing changed: no item either
    }
    pickedUp().erase(pos);
    unlinkBox(pos);
    giveOrDrop(player, toolboxItem(id, inv));
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.pop", .2f, 1.4f);
    log().info("equipment: toolbox at {} {} {} picked up by {}", pos.x, pos.y, pos.z, player.getRealName());
    return true;
}

void toolboxPlaced(Player& player, GlobalPos const& pos, Json const& itemData) {
    int facing = game::opposite(game::playerFacing(player));
    place(pos, idAt(pos), {{kFacing, facing}});
    ce::ToolboxInv inv;
    if (itemData.is_object() && itemData.contains("inv")) inv = ce::ToolboxInv::fromJson(itemData["inv"]);
    store(pos, inv);
    int count = 0;
    for (int c = 0; c < ce::kCompartments; ++c) count += inv.count(c);
    log().info("equipment: toolbox placed at {} {} {} by {} ({} items)", pos.x, pos.y, pos.z, player.getRealName(), count);
}

// ---- equipping ------------------------------------------------------------------------------------------

bool unequipSlot(Player& player, int hotbarSlot, bool keepItems) {
    auto lit = links().find(uuidOf(player));
    if (lit == links().end()) return false;
    auto it = lit->second.find(hotbarSlot);
    if (it == lit->second.end()) return false;
    Link l = it->second;
    lit->second.erase(it);
    gLinksDirty = true;
    if (!keepItems && inRange(player, l.box) && isToolbox(idAt(l.box))) {
        auto& container = player.getInventory();
        ItemStack s     = container.getItem(hotbarSlot);
        if (!items::isEmpty(s)) {
            auto inv  = load(l.box);
            auto rest = inv.insert(toT(s), true, false);
            store(l.box, inv);
            container.setItem(hotbarSlot, rest.empty() ? items::empty() : items::withCount(s, rest.count));
            player.sendInventory(false);
        }
    }
    sendLinked(player);
    return true;
}

bool equipFromToolbox(Player& player, GlobalPos const& pos, int compartment, int hotbarSlot) {
    if (compartment < 0 || compartment >= ce::kCompartments || hotbarSlot < 0 || hotbarSlot > 8) return false;
    if (!isToolbox(idAt(pos)) || !inRange(player, pos)) return false;
    unequipSlot(player, hotbarSlot, false);
    auto& container = player.getInventory();
    auto  inv       = load(pos);
    if (inv.first(compartment).empty()) return false;
    // the hotbar item goes into the toolbox (matching compartments only), then the inventory
    ItemStack cur = container.getItem(hotbarSlot);
    if (!items::isEmpty(cur)) {
        auto rest = inv.insert(toT(cur), true, false);
        container.setItem(hotbarSlot, items::empty());
        if (!rest.empty()) {
            ItemStack r = items::withCount(cur, rest.count);
            for (int s = 9; s < 36 && !items::isEmpty(r); ++s) {
                if (!items::isEmpty(container.getItem(s))) continue;
                container.setItem(s, r);
                r = items::empty();
            }
            if (!items::isEmpty(r)) {
                container.setItem(hotbarSlot, r); // nowhere to put it: stays, nothing is equipped
                store(pos, inv);
                player.sendInventory(false);
                return false;
            }
        }
    }
    auto first = inv.first(compartment);
    auto got   = inv.extract(compartment, ce::hotbarTarget(first.max), false);
    store(pos, inv);
    container.setItem(hotbarSlot, toItem(got));
    player.sendInventory(false);
    links()[uuidOf(player)][hotbarSlot] = Link{pos, compartment};
    gLinksDirty                         = true;
    sendLinked(player);
    log().info("equipment: {} equipped {} from the toolbox at {} {} {}", player.getRealName(), items::idOf(toItem(got)), pos.x, pos.y, pos.z);
    return true;
}

int depositAll(Player& player, std::vector<GlobalPos> const& boxes) {
    auto& container = player.getInventory();
    int   moved     = 0;
    auto  lit       = links().find(uuidOf(player));
    for (int s = 0; s < 36; ++s) {
        if (lit != links().end()) {
            auto it = lit->second.find(s);
            if (it != lit->second.end() && std::any_of(boxes.begin(), boxes.end(), [&](GlobalPos const& b) { return b == it->second.box; })) {
                lit->second.erase(it);
                gLinksDirty = true;
            }
        }
        ItemStack st = container.getItem(s);
        if (items::isEmpty(st)) continue;
        int before = items::countOf(st);
        for (auto const& box : boxes) {
            if (items::isEmpty(st) || !isToolbox(idAt(box))) continue;
            auto inv  = load(box);
            auto rest = inv.insert(toT(st), true, false);
            store(box, inv);
            st = rest.empty() ? items::empty() : items::withCount(st, rest.count);
        }
        moved += before - (items::isEmpty(st) ? 0 : items::countOf(st));
        container.setItem(s, st);
    }
    player.sendInventory(false);
    sendLinked(player);
    log().info("equipment: {} returned {} items to {} toolbox(es)", player.getRealName(), moved, boxes.size());
    return moved;
}

void openToolboxForm(Player& player) {
    auto boxes = nearbyToolboxes(player);
    int  slot  = player.getSelectedItemSlot();
    ll::form::SimpleForm form;
    form.setTitle(trFor(player, "create.keyinfo.toolbelt", "Access Nearby Toolboxes"));
    auto lit = links().find(uuidOf(player));
    if (lit != links().end() && lit->second.contains(slot)) {
        auto const& l = lit->second.at(slot);
        if (!inRange(player, l.box)) {
            form.setContent(trFor(player, "create.toolbox.outOfRange", "Toolbox of held item not in Range"));
            form.appendButton(trFor(player, "create.toolbox.detach", "Stop tracking and keep item"), [slot](Player& p) { unequipSlot(p, slot, true); });
            form.sendTo(player);
            return;
        }
        form.appendButton(trFor(player, "create.toolbox.unequip", "Unequip: {}", items::displayName(player.getInventory().getItem(slot))),
                          [slot](Player& p) { unequipSlot(p, slot, false); });
    }
    if (boxes.empty()) form.setContent(trFor(player, "lv.equipment.toolbox.none", "No toolbox in range"));
    for (auto const& box : boxes) {
        auto inv = load(box);
        for (int c = 0; c < ce::kCompartments; ++c) {
            auto first = inv.first(c);
            if (first.empty()) continue;
            std::string label = items::displayName(toItem(first)) + " x" + std::to_string(inv.count(c));
            if (boxes.size() > 1) label += " (" + trFor(player, "block.levitate." + colourOf(idAt(box)) + "_toolbox", "Toolbox") + ")";
            form.appendButton(label, [box, c](Player& p) { equipFromToolbox(p, box, c, p.getSelectedItemSlot()); });
        }
    }
    if (!boxes.empty()) {
        auto key = boxes.size() > 1 ? "create.toolbox.depositAll" : "create.toolbox.depositBox";
        auto fb  = boxes.size() > 1 ? "Return items to nearby Toolboxes" : "Return items to Toolbox";
        form.appendButton(trFor(player, key, fb), [boxes](Player& p) { depositAll(p, boxes); });
    }
    form.sendTo(player);
}

void tickToolboxes() {
    auto tick = currentTick();
    for (auto& [uuid, slots] : links()) {
        if (slots.empty()) continue;
        Player* player = nullptr;
        if (auto level = ll::service::getLevel()) player = level->getPlayer(mce::UUID::fromString(uuid));
        if (!player) continue;
        auto& container = player->getInventory();
        bool  changed   = false;
        std::vector<std::pair<int, bool>> unequips; ///< (hotbar slot, keep items), after the loop
        for (auto it = slots.begin(); it != slots.end();) {
            int   slot = it->first;
            Link  l    = it->second;
            if (!isToolbox(idAt(l.box))) {
                if (blockAt(l.box)) { // loaded and gone
                    it          = slots.erase(it);
                    gLinksDirty = true;
                    continue;
                }
                ++it;
                continue;
            }
            ItemStack s = container.getItem(slot);
            if (tick % 10 != 0 && !items::isEmpty(s) && items::countOf(s) == ce::hotbarTarget(std::max<int>(1, s.getMaxStackSize()))) {
                ++it;
                continue; // nothing to do this tick (Create checks every tick; the result is the same)
            }
            auto inv    = load(l.box);
            auto filter = inv.filters[static_cast<std::size_t>(l.compartment)];
            // Create's tickPlayers: a cleared filter drops the link; otherwise nothing happens out of
            // range, and in range a slot that no longer holds the compartment's item drops it
            bool clear = filter.empty();
            if (!clear && !inRange(*player, l.box)) {
                ++it;
                continue;
            }
            if (clear || (!items::isEmpty(s) && toT(s).match != filter.match)) {
                it          = slots.erase(it);
                gLinksDirty = true;
                changed     = true;
                continue;
            }
            {
                int target = ce::hotbarTarget(filter.max);
                int have   = items::isEmpty(s) ? 0 : items::countOf(s);
                // Create: while the player has this toolbox's screen open, a refill or deposit
                // unequips the slot instead (refill: the stack goes back into the box)
                bool open = false;
                if (auto o = openers().find(l.box); o != openers().end()) open = o->second.players.contains(uidOf(*player));
                if (open && have < target && !inv.extract(l.compartment, target - have, true).empty()) {
                    unequips.emplace_back(slot, false);
                    ++it;
                    continue;
                }
                if (open && have > target) {
                    auto rest = inv.insert(toT(items::withCount(s, have - target)), true, true);
                    if ((have - target) - (rest.empty() ? 0 : rest.count) > 0) {
                        unequips.emplace_back(slot, true);
                        ++it;
                        continue;
                    }
                }
                if (have < target) {
                    auto got = inv.extract(l.compartment, target - have, false);
                    if (!got.empty()) {
                        container.setItem(slot, items::isEmpty(s) ? toItem(got) : items::withCount(s, have + got.count));
                        store(l.box, inv);
                        changed = true;
                    }
                } else if (have > target) {
                    auto rest = inv.insert(toT(items::withCount(s, have - target)), true, false);
                    int  back = (have - target) - (rest.empty() ? 0 : rest.count);
                    if (back > 0) {
                        container.setItem(slot, items::withCount(s, have - back));
                        store(l.box, inv);
                        changed = true;
                    }
                }
            }
            ++it;
        }
        for (auto [slot, keep] : unequips) changed = unequipSlot(*player, slot, keep) || changed;
        if (changed) player->sendInventory(false);
        if (tick % 20 == 0) sendLinked(*player);
    }
    if (tick % 600 == 0) saveLinks();
    if (!openers().empty() || !closing().empty()) {
        if (tick % 10 == 0 || !closing().empty()) pruneOpeners();
    }
}

void toolboxScreenClosed(Player& player, GlobalPos const& pos) { removeOpener(pos, uidOf(player)); }

int toolboxOpenCount(GlobalPos const& pos) {
    auto it = openers().find(pos);
    return it == openers().end() ? 0 : static_cast<int>(it->second.players.size());
}

bool holdToolboxOpen(GlobalPos const& pos, int ticks) {
    if (!isToolbox(idAt(pos))) return false;
    auto& o       = openers()[pos];
    bool  wasOpen = o.open(currentTick());
    o.holdUntil   = currentTick() + static_cast<std::uint64_t>(std::max(ticks, 0));
    if (!wasOpen && ticks > 0) lidOpened(pos);
    if (ticks <= 0) {
        if (o.players.empty()) {
            openers().erase(pos);
            if (wasOpen) lidClosed(pos);
        }
    }
    return true;
}

void resetToolboxes() {
    links().clear();
    pickedUp().clear();
    openers().clear();
    closing().clear();
    gLinksDirty = false;
}

void installToolboxes() {
    static bool done = false;
    if (done) return;
    done = true;
    registerScreen();
    registerNet();
    for (auto c : kColours) common::items::registerBlock(toolboxId(c), handler());
    game::db::onWorldReady([] { loadLinks(); });
    game::db::onWorldSave([] {
        gLinksDirty = true;
        saveLinks();
    });
    if (game::db::available()) loadLinks();
    // a broken toolbox drops itself with its contents (Create's loot copies the inventory)
    bd::onErase([](GlobalPos const& p, Json const& doc, std::string const& prevId) {
        if (prevId.empty() || !isToolbox(prevId)) return;
        if (!doc.contains("equipment") || !doc["equipment"].contains("toolbox")) return;
        unlinkBox(p);
        if (pickedUp().contains(p)) return; // picked up: the player got the item
        auto inv  = ce::ToolboxInv::fromJson(doc["equipment"]["toolbox"]);
        auto item = toolboxItem(prevId, inv);
        defer([p, item] { dropAt(p.dim, p.x + .5, p.y + .5, p.z + .5, item); });
    });
}

/// Called by the destroy hook before a player breaks a toolbox.
void noteToolboxBreak(GlobalPos const& p) {
    // a toolbox placed by a command has no data yet: create it so the erase listener drops the item
    if (!docOf(p)) store(p, ce::ToolboxInv{});
}

// ---- dyeing in the crafting grid (Create's ToolboxDyeingRecipe keeps the toolbox's data) ----------------

namespace {

/// The crafting grid slots of the player's UI container (3x3 crafting table, else the 2x2 grid).
std::vector<ItemStack const*> craftingGrid(Player& player, bool table) {
    std::vector<ItemStack const*> grid;
    auto&                         ui = player.mPlayerUIContainer.get();
    int                           from = table ? 32 : 28, to = table ? 40 : 31;
    for (int i = from; i <= to; ++i) grid.push_back(&ui.getItem(i));
    return grid;
}

/// The toolbox of a dyeing craft in the player's grid (nullptr: not a dyeing craft).
ItemStack const* dyedToolboxSource(Player& player) {
    for (bool table : {true, false}) {
        auto                     grid = craftingGrid(player, table);
        std::vector<std::string> ids;
        for (auto const* s : grid) ids.push_back(items::isEmpty(*s) ? std::string() : items::idOf(*s));
        if (auto slot = ce::dyeingToolboxSlot(ids)) return grid[static_cast<std::size_t>(*slot)];
    }
    return nullptr;
}

} // namespace

bool keepDyedToolboxData(Player& player, ItemStackBase& result) {
    if (result.isNull() || !isToolbox(result.getTypeName())) return false;
    auto const* source = dyedToolboxSource(player);
    if (!source || !source->mUserData) return false;
    // the result keeps the toolbox's data: its contents and any custom name
    result.setUserData(source->mUserData->clone());
    return true;
}

// The crafting results of an item stack request (the server's authoritative craft, also for
// shift-crafting): a dyed toolbox gets the data of the toolbox in the grid.
LL_TYPE_INSTANCE_HOOK(
    ToolboxDyeingCraftHook,
    ll::memory::HookPriority::Normal,
    ItemStackRequestActionCraftHandler,
    &ItemStackRequestActionCraftHandler::_initCraftResults,
    ItemStackNetResult,
    std::vector<ItemInstance> const& results,
    uchar                            numCrafts
) {
    try {
        Player& player = mPlayer;
        auto    any    = std::any_of(results.begin(), results.end(), [](ItemInstance const& r) { return !r.isNull() && isToolbox(r.getTypeName()); });
        if (!player.isClientSide() && any) {
            auto copy = results;
            bool dyed = false;
            for (auto& r : copy) dyed = keepDyedToolboxData(player, r) || dyed;
            if (dyed) {
                log().info("equipment: {} dyed a toolbox into {} in the crafting grid (contents kept)", player.getRealName(), copy.front().getTypeName());
                // the client predicted the result without the data: send the inventory again afterwards
                auto uid = uidOf(player);
                defer([uid] {
                    if (auto* p = playerById(uid)) p->sendInventory(false);
                });
                return origin(copy, numCrafts);
            }
        }
    } catch (std::exception const& e) {
        log().error("equipment: toolbox dyeing failed: {}", e.what());
    }
    return origin(results, numCrafts);
}

// `/create toolbox lid <pos> <ticks>` (Levitate debug, op): holds a toolbox's lid open as if a player
// had its screen open (e2e scenarios photograph the open lid; 0 releases it).
struct ToolboxLidParams {
    CommandPosition pos;
    int             ticks = 0;
};

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload<ToolboxLidParams>().text("toolbox").text("lid").required("pos").required("ticks").execute(
        [](CommandOrigin const& origin, CommandOutput& output, ToolboxLidParams const& params) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.contraption.need_op", "You need operator permission."));
            auto* dim = origin.getDimension();
            if (!dim) return output.error("No dimension.");
            auto      bp = game::blockPosOf(params.pos.getPosition(0x7FFFFFFF, origin, Vec3{0, 0, 0}));
            GlobalPos pos{static_cast<int>(dim->getDimensionId()), bp.x, bp.y, bp.z};
            if (!holdToolboxOpen(pos, params.ticks)) return output.error("not a toolbox: " + idAt(pos));
            output.success("toolbox lid held open for " + std::to_string(params.ticks) + " ticks");
        }
    );
});

namespace {
class ToolboxHooksModule {
public:
    void install() { ToolboxDyeingCraftHook::hook(); }
    void uninstall() { ToolboxDyeingCraftHook::unhook(); }
};
} // namespace

} // namespace lv::feature::equipment

LV_REGISTER_MODULE(lv::feature::equipment::ToolboxHooksModule);
