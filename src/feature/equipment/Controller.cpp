// Linked controller (Create's LinkedControllerItem / LinkedControllerServerHandler /
// LecternControllerBlock): six buttons bound to redstone link frequencies, transmitted while
// held (client mod: the movement keys; vanilla: the player's movement input) or used on a lectern.

#include <algorithm>
#include <map>

#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/entity/components/MoveInputComponent.h"
#include "mc/input/MoveInputState.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/actor/LecternBlockActor.h"

#include "core/equipment/Gear.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/equipment/Equipment.h"
#include "feature/redstone/RedstoneLink.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;
namespace bd    = common::blockdata;
namespace rl    = feature::redstone::link;

namespace {

constexpr char kScreen[]      = "equipment.controller";
constexpr int  kKeepAlive     = 30; ///< Create: a pressed frequency stays on for 30 ticks without a refresh

struct User {
    bool                     active = false;      ///< the controller in hand is in use
    std::optional<GlobalPos> lectern;             ///< using a lectern controller
    std::array<bool, 6>      pressed{};
    std::array<std::uint64_t, 6> lastSeen{};
    std::optional<GlobalPos> bindLink;            ///< bind mode: the link to copy
    bool                     fromClient = false;  ///< presses come from the client mod
    std::array<rl::LinkId, 6>  nodes{};           ///< U23 link transmitters of the pressed buttons (0: none)
    std::array<rl::LinkKey, 6> keys{};
    std::array<GlobalPos, 6>   nodePos{};
};

void releaseButton(User& u, int b) {
    auto& id = u.nodes[static_cast<std::size_t>(b)];
    if (!id) return;
    rl::remove(id);
    id = 0;
}
void releaseAll(User& u) {
    for (int b = 0; b < 6; ++b) releaseButton(u, b);
}
std::map<std::int64_t, User>& users() {
    static std::map<std::int64_t, User> m;
    return m;
}
/// lectern position -> user
std::map<GlobalPos, std::int64_t>& lecternUsers() {
    static std::map<GlobalPos, std::int64_t> m;
    return m;
}

} // namespace

bool lecternInRange(Player const& player, GlobalPos const& pos) {
    // Create's playerInRange: eye to the block centre < 0.4 x the block interaction range (4.5 in
    // survival, 5 in creative: 1.8 / 2 blocks)
    if (static_cast<int>(player.getDimensionId()) != pos.dim) return false;
    double reach = 0.4 * (player.isCreative() ? 5.0 : 4.5);
    auto   e     = player.getEyePos();
    double dx = e.x - (pos.x + .5), dy = e.y - (pos.y + .5), dz = e.z - (pos.z + .5);
    return dx * dx + dy * dy + dz * dz < reach * reach;
}

namespace {

/// A frequency slot: an item (Create's Frequency), stored as item JSON (or an item id).
using Freq = std::pair<ItemStack, ItemStack>;

ItemStack slotItem(Json const& j) {
    if (j.is_string()) {
        auto id = j.get<std::string>();
        if (id.empty()) return items::empty();
        auto s = items::make(id, 1);
        return s ? *s : items::empty();
    }
    if (j.is_object()) return items::withCount(items::fromJson(j), 1);
    return items::empty();
}
Json slotJson(ItemStack const& s) { return items::isEmpty(s) ? Json(nullptr) : items::toJson(items::withCount(s, 1)); }

std::array<Freq, 6> frequencies(ItemStack const& controller) {
    std::array<Freq, 6> out{};
    for (auto& f : out) f = {items::empty(), items::empty()};
    auto j = items::getData(controller, kControllerData);
    auto f = j.is_object() ? j.value("freq", Json::array()) : Json::array();
    for (std::size_t i = 0; i < 6 && i < f.size(); ++i) {
        if (!f[i].is_array() || f[i].size() != 2) continue;
        out[i] = {slotItem(f[i][0]), slotItem(f[i][1])};
    }
    return out;
}

void setFrequency(ItemStack& controller, int button, Freq const& freq) {
    auto f = frequencies(controller);
    f[static_cast<std::size_t>(button)] = freq;
    Json arr = Json::array();
    for (auto const& [a, b] : f) arr.push_back(Json::array({slotJson(a), slotJson(b)}));
    items::setData(controller, kControllerData, {{"freq", arr}});
}

/// The frequency of a redstone link block (U23 keeps its two slots in BlockData `data["redstone"]`).
std::optional<Freq> linkFrequency(GlobalPos const& pos) {
    auto const* d = bd::get(pos);
    if (!d || !d->contains("redstone")) return Freq{items::empty(), items::empty()};
    auto const& r = (*d)["redstone"];
    return Freq{slotItem(r.value("first", Json())), slotItem(r.value("last", Json()))};
}

std::string freqName(Freq const& f) {
    auto name = [](ItemStack const& s) { return items::isEmpty(s) ? std::string("-") : items::idOf(s); };
    return name(f.first) + " + " + name(f.second);
}

std::string keyName(Player const& p, int b) {
    static char const* keys[]      = {"key.forward", "key.back", "key.left", "key.right", "key.jump", "key.sneak"};
    static char const* fallbacks[] = {"W", "S", "A", "D", "Space", "Shift"};
    return trFor(p, std::string("lv.equipment.controller.") + keys[b], fallbacks[b]);
}

ItemStack const* heldController(Player& p) {
    if (items::idOf(p.getSelectedItem()) == kLinkedController) return &p.getSelectedItem();
    if (items::idOf(p.getOffhandSlot()) == kLinkedController) return &p.getOffhandSlot();
    return nullptr;
}

std::optional<ItemStack> lecternController(GlobalPos const& pos) {
    auto const* d = bd::get(pos);
    if (!d || !d->contains("equipment") || !(*d)["equipment"].contains("controller")) return std::nullopt;
    return items::fromJson((*d)["equipment"]["controller"]);
}

void setActive(Player& p, bool on, std::optional<GlobalPos> lectern) {
    auto& u   = users()[uidOf(p)];
    u.active  = on;
    u.lectern = on ? lectern : std::nullopt;
    if (!on) {
        u.pressed.fill(false);
        releaseAll(u);
        for (auto it = lecternUsers().begin(); it != lecternUsers().end();)
            it = it->second == uidOf(p) ? lecternUsers().erase(it) : std::next(it);
    } else if (lectern) lecternUsers()[*lectern] = uidOf(p);
    if (net::isCompanion(p)) {
        Json msg = {{"active", on}};
        if (lectern) msg["lectern"] = {lectern->dim, lectern->x, lectern->y, lectern->z};
        net::send(p, "equipment.controller.active", msg);
    }
    auto pos = p.getPosition();
    sound(static_cast<int>(p.getDimensionId()), pos.x, pos.y, pos.z, "random.click", .5f, on ? .75f : .5f);
    log().info("equipment: {} {} the linked controller", p.getRealName(), on ? "started using" : "stopped using");
}

void openForm(Player& player, int slot) {
    auto& inv   = player.getInventory();
    auto  freqs = frequencies(inv.getItem(slot));
    std::vector<std::string> ids{""};
    for (int s = 0; s < inv.getContainerSize(); ++s) {
        auto id = items::idOf(inv.getItem(s));
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    for (auto const& [a, b] : freqs) {
        for (auto const& id : {items::idOf(a), items::idOf(b)})
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    std::vector<std::string> names;
    for (auto const& id : ids) {
        if (id.empty()) names.push_back(trFor(player, "lv.common.filter.empty", "(empty)"));
        else if (auto it = items::make(id, 1)) names.push_back(items::displayName(*it));
        else names.push_back(id);
    }
    ll::form::CustomForm form(trFor(player, "item.levitate:linked_controller.name", "Linked Controller"));
    for (int b = 0; b < 6; ++b)
        for (int k = 0; k < 2; ++k) {
            auto        cur = items::idOf(k == 0 ? freqs[static_cast<std::size_t>(b)].first : freqs[static_cast<std::size_t>(b)].second);
            auto        idx = std::find(ids.begin(), ids.end(), cur) - ids.begin();
            auto        key = k == 0 ? "create.linked_controller.frequency_slot_1" : "create.linked_controller.frequency_slot_2";
            auto        fb  = k == 0 ? "Keybind: {}, Freq. #1" : "Keybind: {}, Freq. #2";
            form.appendDropdown("f" + std::to_string(b * 2 + k), trFor(player, key, fb, keyName(player, b)), names, static_cast<std::size_t>(idx));
        }
    form.sendTo(player, [slot, ids](Player& p, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r) return;
        auto& inv = p.getInventory();
        auto  s   = inv.getItem(slot);
        if (items::idOf(s) != kLinkedController) return;
        for (int b = 0; b < 6; ++b) {
            Freq f{items::empty(), items::empty()};
            for (int k = 0; k < 2; ++k) {
                auto it = r->find("f" + std::to_string(b * 2 + k));
                std::size_t idx = 0;
                if (it != r->end()) {
                    if (auto const* v = std::get_if<uint64>(&it->second)) idx = static_cast<std::size_t>(*v);
                }
                auto id = idx < ids.size() ? ids[idx] : std::string();
                (k == 0 ? f.first : f.second) = slotItem(Json(id));
            }
            setFrequency(s, b, f);
        }
        inv.setItem(slot, s);
        p.sendInventory(false);
        log().info("equipment: {} configured the linked controller", p.getRealName());
    });
}

void registerScreen() {
    common::screens::registerType({
        .id   = kScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            int slot = ctx.value("slot", -1);
            if (slot < 0 || items::idOf(p.getInventory().getItem(slot)) != kLinkedController) return std::nullopt;
            auto freqs = frequencies(p.getInventory().getItem(slot));
            Json rows  = Json::array();
            for (int b = 0; b < 6; ++b) {
                Json row = Json::array();
                row.push_back(slotJson(freqs[static_cast<std::size_t>(b)].first));
                row.push_back(slotJson(freqs[static_cast<std::size_t>(b)].second));
                rows.push_back({{"key", keyName(p, b)}, {"items", row}});
            }
            return Json{{"title", trFor(p, "item.levitate:linked_controller.name", "Linked Controller")}, {"rows", rows}};
        },
        .action =
            [](Player& p, Json const& ctx, Json const& action) {
                int slot = ctx.value("slot", -1);
                auto& inv = p.getInventory();
                if (slot < 0) return;
                auto s = inv.getItem(slot);
                if (items::idOf(s) != kLinkedController) return;
                int b = action.value("button", -1), k = action.value("slot", 0);
                if (b < 0 || b > 5) return;
                auto f     = frequencies(s)[static_cast<std::size_t>(b)];
                auto item = action.contains("item") ? slotItem(action["item"]) : items::empty();
                (k == 0 ? f.first : f.second) = item;
                setFrequency(s, b, f);
                inv.setItem(slot, s);
                p.sendInventory(false);
            },
        .form = [](Player& p, Json const& ctx) { openForm(p, ctx.value("slot", -1)); },
    });
}

/// Polls the movement input of a player without the client mod (Bedrock's move input).
std::array<bool, 6> polledInput(Player& p) {
    std::array<bool, 6> out{};
    auto comp = p.getEntityContext().tryGetComponent<MoveInputComponent>();
    if (comp) {
        auto const& flags = comp->mInputState->mFlagValues;
        using F           = MoveInputState::Flag;
        out[0]            = flags->test(static_cast<std::size_t>(F::Up));
        out[1]            = flags->test(static_cast<std::size_t>(F::Down));
        out[2]            = flags->test(static_cast<std::size_t>(F::Left));
        out[3]            = flags->test(static_cast<std::size_t>(F::Right));
        out[4]            = flags->test(static_cast<std::size_t>(F::JumpDown));
        out[5]            = flags->test(static_cast<std::size_t>(F::SneakDown));
    }
    out[4] = out[4] || p.isJumping();
    out[5] = out[5] || p.isSneaking();
    return out;
}

void registerNet() {
    net::on("equipment.controller.input", [](Player& p, Json const& msg) {
        auto pressed = msg.value("pressed", Json::array());
        std::array<bool, 6> st{};
        for (std::size_t i = 0; i < 6 && i < pressed.size(); ++i) st[i] = pressed[i].get<bool>();
        auto& u      = users()[uidOf(p)];
        u.fromClient = true;
        if (!u.active) return; // a press still in flight after the controller was put away
        controllerButtons(p, st, u.lectern);
    });
    net::on("equipment.controller.stop", [](Player& p, Json const&) {
        if (controllerActive(p)) setActive(p, false, std::nullopt);
    });
}

} // namespace

bool controllerActive(Player const& player) {
    auto it = users().find(uidOf(player));
    return it != users().end() && it->second.active;
}

void controllerButtons(Player& player, std::array<bool, 6> const& pressed, std::optional<GlobalPos> lectern) {
    auto  uid = uidOf(player);
    auto& u   = users()[uid];
    std::optional<ItemStack> controller;
    if (lectern) controller = lecternController(*lectern);
    else if (auto const* h = heldController(player)) controller = *h;
    if (!controller) return;
    auto freqs = frequencies(*controller);
    auto pos   = lectern.value_or(GlobalPos{static_cast<int>(player.getDimensionId()), int(std::floor(player.getPosition().x)),
                                          int(std::floor(player.getPosition().y)), int(std::floor(player.getPosition().z))});
    for (int b = 0; b < 6; ++b) {
        bool now = pressed[static_cast<std::size_t>(b)];
        bool was = u.pressed[static_cast<std::size_t>(b)];
        if (now) u.lastSeen[static_cast<std::size_t>(b)] = currentTick();
        if (now && !was) {
            // bind mode: the first pressed control takes the link's frequency
            if (u.bindLink && !lectern) {
                auto f = idAt(*u.bindLink) == kRedstoneLink ? linkFrequency(*u.bindLink) : std::nullopt;
                if (f) {
                    int  slot = items::idOf(player.getSelectedItem()) == kLinkedController ? player.getSelectedItemSlot() : -1;
                    if (slot >= 0) {
                        auto s = player.getInventory().getItem(slot);
                        setFrequency(s, b, *f);
                        player.getInventory().setItem(slot, s);
                        player.sendInventory(false);
                    } else {
                        auto s = player.getOffhandSlot();
                        setFrequency(s, b, *f);
                        player.setOffhandSlot(s);
                    }
                    actionBar(player, trFor(player, "create.linked_controller.key_bound", "Frequency bound to {}", keyName(player, b)));
                    log().info("equipment: {} bound controller button {} to {}", player.getRealName(), ce::controllerButtonName(b), freqName(*f));
                }
                u.bindLink.reset();
                u.pressed[static_cast<std::size_t>(b)] = now;
                continue;
            }
            auto pos2 = player.getPosition();
            sound(pos.dim, pos2.x, pos2.y, pos2.z, "random.click", .3f, .75f);
            log().info("equipment: {} pressed controller button {}", player.getRealName(), ce::controllerButtonName(b));
        }
        auto const& f   = freqs[static_cast<std::size_t>(b)];
        auto        key = rl::keyOf(f.first, f.second);
        auto        i   = static_cast<std::size_t>(b);
        if (now && !(key.first.empty() && key.last.empty())) {
            // a transmitter on the button's frequency at the player (Create's ManualFrequencyEntry)
            if (!u.nodes[i]) {
                u.nodes[i]   = rl::add({.pos = pos, .key = key, .listening = false, .transmit = 15});
                u.keys[i]    = key;
                u.nodePos[i] = pos;
                // Create's LinkedControllerServerHandler#receivePressed: a link listens on it
                if (rl::hasListeningLinks(pos.dim, key)) advancements::award(player, "linked_controller");
            } else {
                if (!(u.keys[i] == key)) rl::setKey(u.nodes[i], u.keys[i] = key);
                if (u.nodePos[i] != pos) rl::move(u.nodes[i], u.nodePos[i] = pos);
            }
        }
        if (!now && was) releaseButton(u, b);
        u.pressed[static_cast<std::size_t>(b)] = now;
    }
}

bool controllerUse(Player& player, ItemStack& held) {
    (void)held;
    int slot = player.getSelectedItemSlot();
    if (player.isSneaking() && items::idOf(player.getSelectedItem()) == kLinkedController) {
        if (!screenDebounced(player)) common::screens::open(player, kScreen, {{"slot", slot}});
        return true;
    }
    setActive(player, !controllerActive(player), std::nullopt);
    return true;
}

bool controllerUseOn(Player& player, ItemStack& held, GlobalPos const& pos) {
    std::string id = idAt(pos);
    if (id == kRedstoneLink && !player.isSneaking()) {
        auto& u    = users()[uidOf(player)];
        u.bindLink = pos;
        if (!u.active) setActive(player, true, std::nullopt);
        actionBar(player, trFor(player, "create.linked_controller.bind_mode", "Bind mode active"));
        log().info("equipment: {} entered bind mode on the link at {} {} {}", player.getRealName(), pos.x, pos.y, pos.z);
        return true;
    }
    if (id == "minecraft:lectern" || id == kLecternController) return lecternUse(player, held, pos);
    return controllerUse(player, held);
}

bool lecternUse(Player& player, ItemStack& held, GlobalPos const& pos) {
    std::string id = idAt(pos);
    if (id == "minecraft:lectern") {
        if (items::idOf(held) != kLinkedController || player.isSneaking()) return false;
        if (common::blockdata::has(pos)) return false;
        // Create: only a lectern without a book takes the controller
        if (auto* region = regionOf(pos.dim)) {
            auto* actor = region->getBlockEntity(BlockPos{pos.x, pos.y, pos.z});
            if (actor && !static_cast<LecternBlockActor*>(actor)->mBook->isNull()) return false;
        }
        int dir = stateAt(pos, "direction").value_or(0);
        ItemStack controller = items::withCount(held, 1);
        if (!place(pos, kLecternController, {{kFacing, std::clamp(dir, 0, 3)}})) return false;
        auto& doc = bd::edit(pos);
        doc["equipment"]["controller"] = items::toJson(controller);
        if (!player.isCreative()) {
            held.remove(1);
            player.setSelectedItem(held);
            player.sendInventory(false);
        }
        sound(pos.dim, pos.x + .5, pos.y + 1, pos.z + .5, "random.click", 1.f, 1.f);
        log().info("equipment: {} put a linked controller on the lectern at {} {} {}", player.getRealName(), pos.x, pos.y, pos.z);
        return true;
    }
    if (id != kLecternController) return false;
    if (!player.isSneaking()) {
        auto it = lecternUsers().find(pos);
        if (it != lecternUsers().end() && it->second != uidOf(player)) return true; // one user at a time
        bool stop = controllerActive(player) && users()[uidOf(player)].lectern == pos;
        if (!stop && !lecternInRange(player, pos)) return true; // Create's tryStartUsing: too far away
        setActive(player, !stop, pos);
        return true;
    }
    auto current = lecternController(pos);
    if (items::idOf(held) == kLinkedController) {
        // swap controllers
        auto& doc = bd::edit(pos);
        doc["equipment"]["controller"] = items::toJson(items::withCount(held, 1));
        if (current) {
            player.setSelectedItem(*current);
            player.sendInventory(false);
        }
        return true;
    }
    // take it off: back to a lectern, the controller drops on top
    int  facing = stateAt(pos, kFacing).value_or(0);
    bd::erase(pos);
    place(pos, "minecraft:lectern", {{"direction", facing}});
    if (current) dropAt(pos.dim, pos.x + .5, pos.y + 1.1, pos.z + .5, *current);
    sound(pos.dim, pos.x + .5, pos.y + 1, pos.z + .5, "random.pop", 1.f, 1.f);
    log().info("equipment: {} took the controller off the lectern at {} {} {}", player.getRealName(), pos.x, pos.y, pos.z);
    return true;
}

void tickControllers() {
    auto now = currentTick();
    for (auto it = users().begin(); it != users().end();) {
        auto* p = playerById(it->first);
        auto& u = it->second;
        if (!p) {
            releaseAll(u);
            std::erase_if(lecternUsers(), [&](auto const& e) { return e.second == it->first; });
            it = users().erase(it);
            continue;
        }
        if (u.active) {
            bool keep = true;
            if (u.lectern) {
                // Create: the user must stay near the lectern
                keep = idAt(*u.lectern) == kLecternController && lecternInRange(*p, *u.lectern);
            } else keep = heldController(*p) != nullptr;
            if (!keep) {
                setActive(*p, false, std::nullopt);
                ++it;
                continue;
            }
            if (!net::isCompanion(*p) || !u.fromClient) controllerButtons(*p, polledInput(*p), u.lectern);
            // expire presses without refresh (client mod: press / release / keep-alive messages)
            for (int b = 0; b < 6; ++b) {
                if (u.pressed[static_cast<std::size_t>(b)] && now - u.lastSeen[static_cast<std::size_t>(b)] > kKeepAlive) {
                    u.pressed[static_cast<std::size_t>(b)] = false;
                    releaseButton(u, b);
                }
            }
        }
        ++it;
    }
}

void resetControllers() {
    users().clear(); // U23 drops every link node when the world closes
    lecternUsers().clear();
}

void installControllers() {
    static bool done = false;
    if (done) return;
    done = true;
    registerScreen();
    registerNet();
    // a broken lectern controller drops the lectern and the controller (Create's loot)
    bd::onErase([](GlobalPos const& p, Json const& doc, std::string const& prevId) {
        if (prevId != kLecternController || !doc.contains("equipment") || !doc["equipment"].contains("controller")) return;
        auto item = items::fromJson(doc["equipment"]["controller"]);
        defer([p, item] { dropAt(p.dim, p.x + .5, p.y + .5, p.z + .5, item); });
    });
}

void uninstallControllers() {}

} // namespace lv::feature::equipment
