// Package ports (SPEC §2.21, §3.6.25): the package frogport (Create's FrogportBlockEntity) and the
// postboxes (PostboxBlockEntity). Both hold 18 packages and an address filter; the frogport
// catches packages for its address from a chain conveyor and sends the others onto it, the
// postbox is where trains (U27) deliver and pick up packages at a station.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/form/CustomForm.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "core/common/Utf8.h"
#include "core/packages/Address.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/display/Clipboard.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/packages/AddressForm.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::packages {

namespace items     = common::items;
namespace blockdata = common::blockdata;
namespace screens   = common::screens;
using common::Access;

namespace {

using core::utf8Prefix;

constexpr char const* kPortScreen = "packages.port";

void setOpen(GlobalPos const& pos, bool open);

std::map<GlobalPos, PortState>& states() {
    static std::map<GlobalPos, PortState> m;
    return m;
}
/// Postboxes per targeted station (Create's GlobalStation.connectedPorts).
std::map<GlobalPos, std::set<GlobalPos>>& stationPorts() {
    static std::map<GlobalPos, std::set<GlobalPos>> m;
    return m;
}

/// Create's GlobalPackagePort: what a station knows of a postbox while its chunk is unloaded (its
/// address and an 18-slot offline buffer trains load from and deliver to). Saved in the world DB.
struct StationPort {
    GlobalPos              station;
    std::string            blockId;
    std::string            address;
    bool                   accepts = true;
    std::vector<ItemStack> buffer  = std::vector<ItemStack>(kPortSlots);
    bool                   primed  = false; ///< the buffer changed while unloaded: restore it on load
};
constexpr char const* kStationPortsKey = "lv:packages:station_ports";
struct StationPorts {
    std::map<GlobalPos, StationPort> ports; ///< by postbox position
    bool                             dirty  = false;
    bool                             loaded = false;
};
StationPorts& stationRecords() {
    static StationPorts s;
    return s;
}

Json posJsonOf(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }
std::optional<GlobalPos> posOf(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

void saveStationPorts() {
    auto& r = stationRecords();
    if (!r.dirty || !game::db::available()) return;
    Json list = Json::array();
    for (auto const& [pos, sp] : r.ports) {
        Json buffer = Json::array();
        for (auto const& s : sp.buffer) buffer.push_back(items::toJson(s));
        list.push_back({{"pos", posJsonOf(pos)}, {"station", posJsonOf(sp.station)}, {"block", sp.blockId}, {"address", sp.address}, {"accepts", sp.accepts}, {"buffer", buffer}, {"primed", sp.primed}});
    }
    game::db::save(kStationPortsKey, list);
    r.dirty = false;
}

void loadStationPorts() {
    auto& r = stationRecords();
    if (r.loaded || !game::db::available()) return;
    r.loaded = true;
    r.ports.clear();
    auto j = game::db::load(kStationPortsKey);
    if (!j || !j->is_array()) return;
    for (auto const& e : *j) {
        auto pos     = posOf(e.value("pos", Json()));
        auto station = posOf(e.value("station", Json()));
        if (!pos || !station) continue;
        StationPort sp;
        sp.station = *station;
        sp.blockId = e.value("block", std::string{});
        sp.address = e.value("address", std::string{});
        sp.accepts = e.value("accepts", true);
        sp.primed  = e.value("primed", false);
        auto buf   = e.value("buffer", Json::array());
        for (std::size_t i = 0; i < buf.size() && i < sp.buffer.size(); ++i) sp.buffer[i] = items::fromJson(buf[i]);
        stationPorts()[sp.station].insert(*pos);
        r.ports.emplace(*pos, std::move(sp));
    }
}

struct StationPortsLifecycle {
    StationPortsLifecycle() {
        game::db::onWorldReady([] {
            stationRecords().loaded = false;
            loadStationPorts();
        });
        game::db::onWorldSave([] { saveStationPorts(); });
        game::db::onWorldClosed([] { stationRecords() = {}; });
    }
} const gStationPortsLifecycle;

/// The record of an unloaded postbox (nullptr when the postbox is loaded or unknown).
StationPort* offlinePort(GlobalPos const& pos) {
    loadStationPorts();
    if (isLoaded(pos)) return nullptr;
    auto it = stationRecords().ports.find(pos);
    return it == stationRecords().ports.end() ? nullptr : &it->second;
}
std::unordered_map<std::int64_t, PendingTarget>& pending() {
    static std::unordered_map<std::int64_t, PendingTarget> m;
    return m;
}

Json offsetJson(cp::Offset o) { return Json::array({o.x, o.y, o.z}); }
std::optional<cp::Offset> offsetFrom(Json const& j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    return cp::Offset{j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

bool isFrog(GlobalPos const& pos) { return kindOf(idAt(pos)) == Kind::Frogport; }
bool isPort(GlobalPos const& pos) {
    auto k = kindOf(idAt(pos));
    return k == Kind::Frogport || k == Kind::Postbox;
}

void updateFlag(GlobalPos const& pos, PortState const& s) {
    if (kindOf(idAt(pos)) != Kind::Postbox) return;
    bool any = std::any_of(s.inventory.begin(), s.inventory.end(), [](ItemStack const& i) { return !items::isEmpty(i); });
    if (setStates(pos, {{kFlag, any ? 1 : 0}}) && any) playSound(pos.dim, centre(pos), "levitate.packages.postbox_flag", 1.f, 2.f);
}

/// Inserts a package into the first empty slot. Returns false when full.
bool insertIntoInventory(PortState& s, ItemStack const& box) {
    for (auto& slot : s.inventory) {
        if (!items::isEmpty(slot)) continue;
        slot = items::withCount(box, 1);
        return true;
    }
    return false;
}

/// Create's PackagePortTarget.export.
bool exportTo(GlobalPos const& portPos, PortState const& s, ItemStack const& box, bool simulate) {
    if (!s.target || s.target->type != PortTarget::Type::Chain) return false;
    auto conv = offset(portPos, s.target->relative);
    if (!hasConveyor(conv)) return false;
    auto& c = conveyor(conv);
    if (s.target->connection && !c.connections.contains(*s.target->connection)) return false;
    if (simulate) return kinetics::speedAt(conv) != 0 && canAcceptPackagesFor(conv, s.target->connection);
    ChainPackage p;
    p.chainPosition = s.target->chainPos;
    p.item          = items::withCount(box, 1);
    p.netId         = nextNetId();
    if (!s.target->connection) return addLoopingPackage(conv, p);
    return addTravellingPackage(conv, p, *s.target->connection);
}

void portDrop(GlobalPos const& pos, ItemStack const& box) { dropItem(pos.dim, centre(pos), box); }

/// Create's FrogportBlockEntity.tryPullingFrom: the first package not addressed to this port.
bool tryPullingFromOwn(GlobalPos const& pos, PortState& s) {
    auto filter = portFilter(s);
    for (auto& slot : s.inventory) {
        if (items::isEmpty(slot) || !isPackage(slot)) continue;
        if (filter && matchAddress(addressOf(slot), *filter)) continue;
        ItemStack box = slot;
        slot          = items::empty();
        savePort(pos);
        startFrogAnimation(pos, box, true);
        return true;
    }
    return false;
}

bool tryPullingFromBelow(GlobalPos const& pos, PortState& s) {
    auto below = offsetFace(pos, 0);
    if (!isLoaded(below) || isFrog(below)) return false;
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    bool fromPackager = isPackager(below);
    auto filter       = portFilter(s);
    auto got          = items::extract(
        *region,
        game::blockPos(below),
        1,
        [&](ItemStack const& st) {
            if (!isPackage(st)) return false;
            return !filter || fromPackager || !matchAddress(addressOf(st), *filter);
        },
        1,
        false
    );
    if (items::isEmpty(got)) return false;
    startFrogAnimation(pos, got, true);
    return true;
}

void tryPullingFromOwnAndAdjacent(GlobalPos const& pos, PortState& s) {
    if (s.animating) return;
    if (!s.target || !exportTo(pos, s, defaultBox(), true)) return;
    if (tryPullingFromOwn(pos, s)) return;
    tryPullingFromBelow(pos, s);
}

/// Create's tryPushingToAdjacentInventories: caught packages go into the inventory below.
void tryPushingDown(GlobalPos const& pos, PortState& s) {
    bool prev          = s.failedLastExport;
    s.failedLastExport = false;
    bool empty = std::all_of(s.inventory.begin(), s.inventory.end(), [](ItemStack const& i) { return items::isEmpty(i); });
    if (empty) return;
    auto below = offsetFace(pos, 0);
    if (!isLoaded(below) || isFrog(below)) return;
    auto* region = regionOf(pos.dim);
    if (!region || !items::has(*region, game::blockPos(below), 1)) return;
    auto filter = portFilter(s);
    bool changed = false;
    for (auto& slot : s.inventory) {
        // the automation view of the port: only packages addressed to it come out
        if (items::isEmpty(slot) || !isPackage(slot) || !filter || !matchAddress(addressOf(slot), *filter)) continue;
        auto rest = items::insert(*region, game::blockPos(below), 1, slot, false);
        if (items::isEmpty(rest)) {
            slot    = items::empty();
            changed = true;
        } else {
            s.failedLastExport = true;
        }
    }
    if (changed || prev != s.failedLastExport) savePort(pos);
}

} // namespace

// ---- state ---------------------------------------------------------------------------------------

namespace {
/// Create's GlobalPackagePort.restoreOfflineBuffer: what trains moved while the postbox was
/// unloaded becomes its inventory as soon as it is used again.
void restoreOfflineBuffer(GlobalPos const& pos, PortState& s) {
    auto& r = stationRecords();
    if (r.ports.empty()) return;
    auto it = r.ports.find(pos);
    if (it == r.ports.end() || !it->second.primed || !isLoaded(pos)) return;
    it->second.primed = false;
    s.inventory       = it->second.buffer;
    r.dirty           = true;
    savePort(pos);
    log().info("packages: postbox at {} restored its offline buffer", pos.key());
}

PortState& loadPort(GlobalPos const& pos) {
    auto& m  = states();
    auto  it = m.find(pos);
    if (it != m.end()) return it->second;
    PortState s;
    if (auto const* doc = blockdata::get(pos); doc && doc->contains("packages")) {
        auto const& j = (*doc)["packages"];
        auto        inv = j.value("inventory", Json::array());
        for (std::size_t i = 0; i < inv.size() && i < s.inventory.size(); ++i) s.inventory[i] = items::fromJson(inv[i]);
        s.address         = j.value("address", std::string{});
        s.acceptsPackages = j.value("accepts", true);
        if (auto t = j.find("target"); t != j.end() && t->is_object()) {
            PortTarget pt;
            pt.type       = t->value("type", std::string{"chain"}) == "station" ? PortTarget::Type::Station : PortTarget::Type::Chain;
            pt.relative   = offsetFrom(t->value("relative", Json())).value_or(cp::Offset{});
            pt.chainPos   = t->value("chain_pos", 0.f);
            pt.connection = offsetFrom(t->value("connection", Json()));
            pt.flipped    = t->value("flipped", false);
            s.target      = pt;
        }
        s.animatedPackage  = items::fromJson(j.value("animated", Json()));
        s.depositing       = j.value("deposit", false);
        s.animating        = !items::isEmpty(s.animatedPackage);
        s.animationTicks   = 0;
        s.failedLastExport = j.value("failed", false);
        s.passiveYaw       = j.value("yaw", 0.f);
    }
    return m.emplace(pos, std::move(s)).first->second;
}
} // namespace

PortState& port(GlobalPos const& pos) {
    auto& s = loadPort(pos);
    restoreOfflineBuffer(pos, s);
    return s;
}

void savePort(GlobalPos const& pos) {
    auto it = states().find(pos);
    if (it == states().end()) return;
    auto const& s   = it->second;
    Json        inv = Json::array();
    for (auto const& i : s.inventory) inv.push_back(items::toJson(i));
    Json j{
        {"inventory", inv              },
        {"address",   s.address        },
        {"accepts",   s.acceptsPackages},
        {"failed",    s.failedLastExport},
        {"yaw",       s.passiveYaw     },
    };
    if (s.target) {
        j["target"] = {
            {"type",       s.target->type == PortTarget::Type::Station ? "station" : "chain"},
            {"relative",   offsetJson(s.target->relative)},
            {"chain_pos",  s.target->chainPos},
            {"connection", s.target->connection ? offsetJson(*s.target->connection) : Json()},
            {"flipped",    s.target->flipped},
        };
    }
    if (s.animating && !items::isEmpty(s.animatedPackage)) {
        j["animated"] = items::toJson(s.animatedPackage);
        j["deposit"]  = s.depositing;
    }
    blockdata::edit(pos)["packages"] = j;
    updateFlag(pos, s);
    // Create's PostboxBlockEntity.saveOfflineBuffer: the station keeps a copy for when it unloads
    if (s.target && s.target->type == PortTarget::Type::Station) {
        auto& r = stationRecords();
        if (auto it = r.ports.find(pos); it != r.ports.end() && !it->second.primed) {
            it->second.buffer  = s.inventory;
            it->second.address = s.address;
            it->second.accepts = s.acceptsPackages;
            r.dirty            = true;
        }
    }
}

void forgetPort(GlobalPos const& pos) {
    deregisterFromTarget(pos);
    states().erase(pos);
}

void resetPorts() {
    states().clear();
    stationPorts().clear();
    pending().clear();
}

std::optional<std::string> portFilter(PortState const& s) {
    if (!s.acceptsPackages) return std::nullopt;
    return s.address;
}

bool isBackedUp(PortState const& s) {
    return std::none_of(s.inventory.begin(), s.inventory.end(), [](ItemStack const& i) { return items::isEmpty(i); });
}

std::optional<cp::V3> targetLocation(GlobalPos const& portPos, PortTarget const& t) {
    auto tp = offset(portPos, t.relative);
    if (t.type == PortTarget::Type::Station) return centre(tp);
    if (!hasConveyor(tp)) return std::nullopt;
    return packagePosition(tp, conveyor(tp), t.chainPos, t.connection);
}

// ---- registration with the target ---------------------------------------------------------------

void registerWithTarget(GlobalPos const& pos) {
    auto& s = port(pos);
    if (!s.target) return;
    if (s.target->type == PortTarget::Type::Station) {
        // Create's StationBlockEntity.attachPackagePort: a new GlobalPackagePort, or the offline
        // buffer the trains used while the postbox was unloaded goes back into its inventory
        auto station = offset(pos, s.target->relative);
        stationPorts()[station].insert(pos);
        loadStationPorts();
        auto& r  = stationRecords();
        auto  it = r.ports.find(pos);
        if (it == r.ports.end() || it->second.station != station) {
            StationPort sp;
            sp.station = station;
            sp.blockId = idAt(pos);
            sp.address = s.address;
            sp.accepts = s.acceptsPackages;
            sp.buffer  = s.inventory;
            r.ports[pos] = std::move(sp);
            r.dirty      = true;
        }
        return;
    }
    auto conv = offset(pos, s.target->relative);
    if (!hasConveyor(conv)) return;
    auto actual = conv;
    // jump to the opposite chain when the motion reversed
    if (s.target->connection && (kinetics::speedAt(conv) < 0) != s.target->flipped) {
        deregisterFromTarget(pos);
        auto other = offset(conv, *s.target->connection);
        if (!hasConveyor(other)) return;
        auto& c = conveyor(conv);
        prepareStats(conv, c);
        if (auto it = c.stats.find(*s.target->connection); it != c.stats.end()) s.target->chainPos = it->second.chainLength - s.target->chainPos;
        s.target->connection = s.target->connection->negated();
        s.target->flipped    = !s.target->flipped;
        s.target->relative   = diff(pos, other);
        actual               = other;
        savePort(pos);
    }
    auto& c = conveyor(actual);
    if (s.target->connection && !c.connections.contains(*s.target->connection)) return;
    auto filter = portFilter(s);
    if (!filter) return;
    c.routing.receivePortInfo(*filter, s.target->connection.value_or(cp::Offset{}));
    auto& map = s.target->connection ? c.travelPorts : c.loopPorts;
    map[diff(actual, pos)] = ConnectedPort{s.target->chainPos, s.target->connection, *filter};
}

void deregisterFromTarget(GlobalPos const& pos) {
    auto it = states().find(pos);
    if (it == states().end() || !it->second.target) return;
    auto const& s = it->second;
    if (s.target->type == PortTarget::Type::Station) {
        auto st = stationPorts().find(offset(pos, s.target->relative));
        if (st != stationPorts().end()) st->second.erase(pos);
        // Create's removePackagePort
        if (stationRecords().ports.erase(pos)) stationRecords().dirty = true;
        return;
    }
    auto conv = offset(pos, s.target->relative);
    if (!hasConveyor(conv)) return;
    auto& c = conveyor(conv);
    c.loopPorts.erase(diff(conv, pos));
    c.travelPorts.erase(diff(conv, pos));
    if (auto filter = portFilter(s)) c.routing.removePort(*filter);
}

// ---- ticking -------------------------------------------------------------------------------------

void tickPort(GlobalPos const& pos, Kind kind, bool lazy) {
    auto& s = port(pos);
    if (lazy) registerWithTarget(pos);
    if (kind == Kind::Postbox) {
        if (s.openTicks > 0 && --s.openTicks == 0) setOpen(pos, false);
        if (!s.registered) {
            s.registered = true;
            updateFlag(pos, s);
        }
        return;
    }
    if (lazy) updateFrogYaw(pos);
    if (lazy && !s.animating) {
        bool prevFail = s.failedLastExport;
        tryPushingDown(pos, s);
        tryPullingFromOwnAndAdjacent(pos, s);
        if (prevFail != s.failedLastExport) syncFrog(pos, s);
    }
    if (!s.animating) return;
    s.animationTicks++;
    float value = float(s.animationTicks) / kFrogAnimationTicks;
    if (s.depositing && value > 0.5f && !items::isEmpty(s.animatedPackage)) {
        auto box = s.animatedPackage;
        s.animatedPackage = items::empty();
        if (!s.target || !exportTo(pos, s, box, false)) {
            portDrop(pos, box);
            log().info("packages: frogport at {} dropped a package it could not send", pos.key());
        } else {
            log().info("packages: frogport at {} sent a package for '{}'", pos.key(), addressOf(box));
        }
        savePort(pos);
    }
    if (value < 1.f) return;
    s.animating      = false;
    s.animationTicks = 0;
    if (!s.depositing && !items::isEmpty(s.animatedPackage)) {
        auto box = s.animatedPackage;
        if (!insertIntoInventory(s, box)) portDrop(pos, box);
        else log().info("packages: frogport at {} caught a package for '{}'", pos.key(), addressOf(box));
    }
    s.animatedPackage = items::empty();
    savePort(pos);
}

void updateFrogYaw(GlobalPos const& pos) {
    // Create's FrogportBlockEntity.getYaw: towards the target, else the yaw it was placed with;
    // the block shows it in 22.5 degree steps (tools/models/packages.ps1)
    auto& s   = port(pos);
    float yaw = s.passiveYaw;
    if (s.target)
        if (auto at = targetLocation(pos, *s.target)) {
            auto c = centre(pos);
            yaw    = float(std::atan2(at->x - c.x, at->z - c.z) * 180.0 / 3.14159265358979) + 180.f;
        }
    int step = int(std::lround(yaw / 22.5f)) % 16;
    if (step < 0) step += 16;
    setStates(pos, {{kYaw, step}});
}

void startFrogAnimation(GlobalPos const& pos, ItemStack const& box, bool deposit) {
    if (!isPackage(box)) return;
    auto& s = port(pos);
    if (deposit && !s.target) return;
    s.animating       = true;
    s.animationTicks  = 0;
    s.animatedPackage = items::withCount(box, 1);
    s.depositing      = deposit;
    if (!deposit) advancements::awardOwner(pos, "frogport"); // Create's FrogportBlockEntity#startAnimation
    savePort(pos);
    syncFrog(pos, s);
    playSound(pos.dim, centre(pos), "levitate.packages.frogport_open", 1.f, 1.f);
    playSound(pos.dim, centre(pos), deposit ? "levitate.packages.frogport_deposit" : "levitate.packages.frogport_catch", 1.f, 1.f);
}

void anticipate(GlobalPos const& pos) {
    auto& s = port(pos);
    if (s.animating || isBackedUp(s)) return;
    broadcast(pos, "packages.frog_anticipate", {{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}});
}

// ---- placement -----------------------------------------------------------------------------------

void setPendingTarget(Player& player, PendingTarget t) {
    pending()[player.getOrCreateUniqueID().rawID] = t;
    actionBar(player, "§a" + trFor(player, "create.packager_link.set", "Target selected"));
    log().info("packages: {} selected a {} target at {}", player.getRealName(), t.postbox ? "station" : "chain", t.targetPos.key());
}

void onPortPlaced(Player* player, GlobalPos const& pos, Kind kind) {
    auto& s = port(pos);
    if (player) {
        if (kind == Kind::Frogport) {
            auto   feet = player->getFeetPos();
            cp::V3 d    = centre(pos) - cp::V3{feet.x, feet.y, feet.z};
            float  yaw  = float(std::atan2(d.x, d.z) * 180.0 / 3.14159265358979);
            s.passiveYaw = std::round(yaw / 11.25f) * 11.25f;
        }
        auto key = player->getOrCreateUniqueID().rawID;
        auto it  = pending().find(key);
        if (it == pending().end() || it->second.postbox != (kind == Kind::Postbox)) {
            actionBar(*player, trFor(*player, "create.gui.package_port.not_targeting_anything", "No target selected"));
        } else {
            auto   t      = it->second;
            cp::V3 source = corner(pos) + cp::V3{0.5, 0, 0.5};
            cp::V3 d      = t.exact - source;
            std::optional<std::string> error;
            if (t.targetPos.dim != pos.dim) error = "create.package_port.too_far";
            else if (d.y < 0 && !t.postbox) error = "create.package_port.cannot_reach_down";
            else if (d.length() > config().packagePortRange) error = "create.package_port.too_far";
            if (error) {
                actionBar(*player, "§c" + trFor(*player, *error, *error == "create.package_port.too_far" ? "Too far away" : "Cannot connect downward"));
            } else {
                PortTarget target = t.target;
                target.relative   = diff(pos, t.targetPos);
                if (target.type == PortTarget::Type::Chain) target.flipped = kinetics::speedAt(t.targetPos) < 0;
                s.target = target;
                actionBar(*player, "§a" + trFor(*player, "create.package_port.valid", "Can Connect ✔"));
                log().info("packages: {} at {} targets {}", kind == Kind::Postbox ? "postbox" : "frogport", pos.key(), t.targetPos.key());
                // Create's PackagePortPlacementPacket.handle ends with ppbe.use(player): the port's
                // screen opens (after the placement finished)
                auto uid = player->getOrCreateUniqueID().rawID;
                defer([uid, pos, kind] {
                    auto* p = playerById(uid);
                    if (p && kindOf(idAt(pos)) == kind) usePort(*p, pos, kind);
                });
            }
            pending().erase(it);
        }
    }
    savePort(pos);
    registerWithTarget(pos);
    if (kind == Kind::Frogport) updateFrogYaw(pos);
}

// ---- interaction / screens -------------------------------------------------------------------------

bool usePort(Player& player, GlobalPos const& pos, Kind kind) {
    auto const& held = player.getSelectedItem();
    if (kind == Kind::Frogport && items::idOf(held) == kGogglesId && !stateAt(pos, kGoggles).value_or(0)) {
        setStates(pos, {{kGoggles, 1}});
        playSound(pos.dim, centre(pos), "armor.equip_gold", 0.5f, 1.f);
        log().info("packages: {} put goggles on the frogport at {}", player.getRealName(), pos.key());
        return true;
    }
    // Create's PackagePortBlockEntity.use: a clipboard takes the port's address
    // (addAddressToClipboard) instead of opening the screen
    if (display::clipboard::isClipboard(held)) {
        auto const& address = port(pos).address;
        ItemStack   copy    = held;
        if (display::clipboard::addAddress(copy, address)) {
            player.setSelectedItem(copy);
            player.sendInventory(false);
            actionBar(player, trFor(player, "create.clipboard.address_added", "Added '{}' to clipboard", address));
            log().info("packages: {} added the address '{}' of the port at {} to a clipboard", player.getRealName(), address, pos.key());
        }
        return true;
    }
    openPortScreen(player, pos);
    return true;
}

namespace {

Json posJson(GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }
std::optional<GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    return GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

/// The port named by a screen context if the player may still use it.
std::optional<GlobalPos> contextPort(Player& player, Json const& ctx) {
    auto pos = posFrom(ctx.value("pos", Json()));
    if (!pos || int(player.getDimensionId()) != pos->dim || !isPort(*pos)) return std::nullopt;
    auto   feet = player.getFeetPos();
    cp::V3 d    = centre(*pos) - cp::V3{feet.x, feet.y + 1, feet.z};
    if (d.length() > 8.0) return std::nullopt;
    return pos;
}

void setOpen(GlobalPos const& pos, bool open) {
    if (kindOf(idAt(pos)) != Kind::Postbox) return;
    // native screens report no close: the lid shuts by itself after 3 s (re-opened while used)
    port(pos).openTicks = open ? 60 : 0;
    if (setStates(pos, {{kOpen, open ? 1 : 0}})) playSound(pos.dim, centre(pos), open ? "random.barrel_open" : "random.barrel_close");
}

void setAddressOf(GlobalPos const& pos, std::string const& typed, Player& by) {
    auto  address = utf8Prefix(typed, kAddressMaxLength); // Create's address box: 25 characters
    auto& s       = port(pos);
    if (s.address == address) return;
    deregisterFromTarget(pos);
    s.address = address;
    savePort(pos);
    registerWithTarget(pos);
    log().info("packages: port at {} now catches '{}' (set by {})", pos.key(), address, by.getRealName());
}

void setAccepts(GlobalPos const& pos, bool accepts, Player& by) {
    auto& s = port(pos);
    if (s.acceptsPackages == accepts) return;
    deregisterFromTarget(pos);
    s.acceptsPackages = accepts;
    savePort(pos);
    registerWithTarget(pos);
    log().info("packages: port at {} set to {} by {}", pos.key(), accepts ? "send and receive" : "send only", by.getRealName());
}

void takeSlot(Player& player, GlobalPos const& pos, int index) {
    auto& s = port(pos);
    if (index < 0 || index >= kPortSlots || items::isEmpty(s.inventory[std::size_t(index)])) return;
    auto box                          = s.inventory[std::size_t(index)];
    s.inventory[std::size_t(index)] = items::empty();
    savePort(pos);
    giveOrDrop(player, box);
    log().info("packages: {} took a package from the port at {}", player.getRealName(), pos.key());
}

void insertFromInventory(Player& player, GlobalPos const& pos, int invSlot) {
    auto& inv = player.getInventory();
    if (invSlot < 0 || invSlot >= inv.getContainerSize()) return;
    ItemStack st = inv.getItem(invSlot);
    if (!isPackage(st)) return;
    auto& s = port(pos);
    if (!insertIntoInventory(s, st)) return;
    inv.setItem(invSlot, items::withCount(st, items::countOf(st) - 1));
    player.sendInventory(false);
    savePort(pos);
    log().info("packages: {} put a package into the port at {}", player.getRealName(), pos.key());
}

std::optional<Json> portData(Player& player, Json const& ctx) {
    auto pos = contextPort(player, ctx);
    if (!pos) return std::nullopt;
    auto& s   = port(*pos);
    Json  inv = Json::array();
    for (auto const& i : s.inventory) inv.push_back(items::toJson(i));
    return Json{
        {"inventory", inv},
        {"address", s.address},
        {"accepts", s.acceptsPackages},
        {"postbox", kindOf(idAt(*pos)) == Kind::Postbox},
        {"target", s.target.has_value()},
        {"suggestions", addressSuggestions(player)},
    };
}

void addressForm(Player& player, GlobalPos const& pos);

void portForm(Player& player, Json const& ctx) {
    auto pos = contextPort(player, ctx);
    if (!pos) return;
    auto&                    s = port(*pos);
    ll::form::CustomForm     form(items::displayName(items::make(idAt(*pos)).value_or(ItemStack::EMPTY_ITEM())));
    form.appendLabel(trFor(player, "create.gui.package_port.catch_packages", "Catch packages addressed to..."));
    form.appendInput("address", trFor(player, "create.gui.package_port.catch_packages_wildcard", "Use * as a text wildcard"), "", s.address,
                     trFor(player, "create.gui.package_port.catch_packages_empty", "Leave empty to match non-addressed"));
    auto suggestions = addressSuggestions(player);
    appendAddressSuggestions(form, player, suggestions);
    form.appendToggle("accepts", trFor(player, "create.gui.package_port.send_and_receive", "Send and receive packages"), s.acceptsPackages);
    std::vector<std::string> options{trFor(player, "lv.packages.form.keep", "(none)")};
    std::vector<int>         slots{-1};
    std::vector<std::string> shown{""}; // what each option showed: the slot must still hold it
    for (int i = 0; i < kPortSlots; ++i) {
        auto const& b = s.inventory[std::size_t(i)];
        if (items::isEmpty(b)) continue;
        auto a = addressOf(b);
        options.push_back(std::to_string(i + 1) + ": " + items::displayName(b) + (a.empty() ? "" : " → " + a));
        slots.push_back(i);
        shown.push_back(items::toJson(b).dump());
    }
    form.appendDropdown("take", trFor(player, "lv.packages.form.take", "Take a package"), options, 0);
    form.appendToggle("insert", trFor(player, "lv.packages.form.insert", "Put the packages from my inventory in"), false);
    auto at = *pos;
    setOpen(at, true);
    form.sendTo(player, [at, slots, shown, suggestions](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        setOpen(at, false);
        if (!result || !contextPort(p, {{"pos", posJson(at)}})) return;
        if (auto it = result->find("address"); it != result->end())
            if (auto const* t = std::get_if<std::string>(&it->second)) setAddressOf(at, chosenSuggestion(result, suggestions).value_or(*t), p);
        if (auto it = result->find("accepts"); it != result->end()) {
            if (auto const* v = std::get_if<uint64>(&it->second)) setAccepts(at, *v != 0, p);
            else if (auto const* d = std::get_if<double>(&it->second)) setAccepts(at, *d != 0, p);
        }
        if (auto it = result->find("take"); it != result->end())
            if (auto const* v = std::get_if<uint64>(&it->second); v && *v > 0 && *v < slots.size()) {
                auto const& now = port(at).inventory[std::size_t(slots[*v])];
                if (items::toJson(now).dump() == shown[*v]) takeSlot(p, at, slots[*v]);
            }
        if (auto it = result->find("insert"); it != result->end()) {
            bool on = false;
            if (auto const* v = std::get_if<uint64>(&it->second)) on = *v != 0;
            if (on) {
                auto& inv = p.getInventory();
                for (int i = 0; i < inv.getContainerSize(); ++i)
                    while (isPackage(inv.getItem(i)) && !isBackedUp(port(at))) insertFromInventory(p, at, i);
            }
        }
    });
}

void addressForm(Player& player, GlobalPos const& pos) {
    auto&                s = port(pos);
    ll::form::CustomForm form(trFor(player, "create.gui.package_port.catch_packages", "Catch packages addressed to..."));
    form.appendLabel(trFor(player, "create.gui.package_port.catch_packages_wildcard", "Use * as a text wildcard"));
    form.appendLabel(trFor(player, "create.gui.package_port.catch_packages_empty", "Leave empty to match non-addressed"));
    form.appendInput("address", trFor(player, "lv.packages.form.address", "Address"), "", s.address);
    form.sendTo(player, [pos](Player& p, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result || !contextPort(p, {{"pos", posJson(pos)}})) return;
        if (auto it = result->find("address"); it != result->end())
            if (auto const* t = std::get_if<std::string>(&it->second)) setAddressOf(pos, *t, p);
        screens::open(p, kPortScreen, {{"pos", posJson(pos)}});
    });
}

void portAction(Player& player, Json const& ctx, Json const& action) {
    auto pos = contextPort(player, ctx);
    if (!pos) return;
    auto op = action.value("op", std::string{});
    if (op == "take") takeSlot(player, *pos, action.value("index", -1));
    else if (op == "insert") insertFromInventory(player, *pos, action.value("slot", -1));
    else if (op == "accepts") setAccepts(*pos, action.value("value", true), player);
    else if (op == "address") {
        if (action.contains("text")) setAddressOf(*pos, action.value("text", std::string{}), player);
        else {
            screens::close(player, kPortScreen);
            addressForm(player, *pos);
        }
    } else if (op == "close") setOpen(*pos, false);
}

} // namespace

void openPortScreen(Player& player, GlobalPos const& pos) {
    Json ctx{{"pos", posJson(pos)}};
    if (screens::open(player, kPortScreen, ctx) && net::isCompanion(player)) setOpen(pos, true);
}

void installPortScreens() {
    screens::registerType({
        .id     = kPortScreen,
        .data   = portData,
        .action = portAction,
        .form   = portForm,
    });
}

void installPortCapabilities() {
    common::items::Handler h;
    h.slots   = [](Access const&) { return kPortSlots; };
    h.getSlot = [](Access const& a, int slot) {
        auto& s = port(game::globalPos(a.region, a.pos));
        return slot >= 0 && slot < kPortSlots ? s.inventory[std::size_t(slot)] : items::empty();
    };
    // Create's PackagePortAutomationInventoryWrapper: packages for elsewhere go in, packages
    // addressed to the port come out; nothing else.
    h.insert = [](Access const& a, ItemStack const& stack, bool simulate) -> ItemStack {
        auto  pos = game::globalPos(a.region, a.pos);
        auto& s   = port(pos);
        if (!isPackage(stack)) return stack;
        auto filter = portFilter(s);
        if (filter && matchAddress(addressOf(stack), *filter)) return stack;
        for (auto& slot : s.inventory) {
            if (!items::isEmpty(slot)) continue;
            if (!simulate) {
                slot = items::withCount(stack, 1);
                savePort(pos);
            }
            return items::withCount(stack, items::countOf(stack) - 1);
        }
        return stack;
    };
    h.extract = [](Access const& a, common::items::ItemPredicate const& pred, int amount, bool simulate) -> ItemStack {
        auto  pos = game::globalPos(a.region, a.pos);
        auto& s   = port(pos);
        auto  filter = portFilter(s);
        if (!filter || amount <= 0) return items::empty();
        for (auto& slot : s.inventory) {
            if (items::isEmpty(slot) || !isPackage(slot) || !matchAddress(addressOf(slot), *filter)) continue;
            if (pred && !pred(slot)) continue;
            auto box = slot;
            if (!simulate) {
                slot = items::empty();
                savePort(pos);
            }
            return box;
        }
        return items::empty();
    };
    common::items::registerBlock(kFrogportId, h);
    for (auto const& id : postboxIds()) common::items::registerBlock(id, h);
}

// ---- contract ------------------------------------------------------------------------------------

std::optional<PortInfo> portAt(GlobalPos const& pos) {
    if (!isPort(pos)) return std::nullopt;
    auto&    s = port(pos);
    PortInfo info;
    info.pos             = pos;
    info.blockId         = idAt(pos);
    info.address         = s.address;
    info.acceptsPackages = s.acceptsPackages;
    if (s.target && s.target->type == PortTarget::Type::Station) info.station = offset(pos, s.target->relative);
    return info;
}

std::vector<PortInfo> postboxesForStation(GlobalPos const& station) {
    loadStationPorts();
    std::vector<PortInfo> out;
    auto                  it = stationPorts().find(station);
    if (it == stationPorts().end()) return out;
    for (auto const& p : std::vector<GlobalPos>(it->second.begin(), it->second.end())) {
        // Create's connectedPorts: an unloaded postbox still takes part through its offline buffer
        if (auto const* off = offlinePort(p); off && off->station == station) {
            PortInfo info;
            info.pos             = p;
            info.blockId         = off->blockId;
            info.address         = off->address;
            info.acceptsPackages = off->accepts;
            info.station         = station;
            out.push_back(info);
            continue;
        }
        auto info = portAt(p);
        if (!info || !info->station || *info->station != station) {
            it->second.erase(p);
            if (isLoaded(p) && stationRecords().ports.erase(p)) stationRecords().dirty = true;
            continue;
        }
        out.push_back(*info);
    }
    return out;
}

std::vector<ItemStack> portInventory(GlobalPos const& pos) {
    if (auto const* off = offlinePort(pos)) return off->buffer;
    if (!isPort(pos)) return {};
    return port(pos).inventory;
}

ItemStack insertIntoPort(GlobalPos const& pos, ItemStack const& box, bool simulate) {
    if (auto* off = offlinePort(pos); off && isPackage(box)) {
        // Create's runMailTransfer into the offline buffer (primed: restored when the postbox loads)
        for (auto& slot : off->buffer) {
            if (!items::isEmpty(slot)) continue;
            if (!simulate) {
                slot                    = items::withCount(box, 1);
                off->primed             = true;
                stationRecords().dirty = true;
            }
            return items::withCount(box, items::countOf(box) - 1);
        }
        return box;
    }
    if (!isPort(pos) || !isPackage(box)) return box;
    auto& s = port(pos);
    for (auto& slot : s.inventory) {
        if (!items::isEmpty(slot)) continue;
        if (!simulate) {
            slot = items::withCount(box, 1);
            savePort(pos);
        }
        return items::withCount(box, items::countOf(box) - 1);
    }
    return box;
}

ItemStack extractFromPort(GlobalPos const& pos, std::function<bool(ItemStack const&)> const& filter, bool simulate) {
    if (auto* off = offlinePort(pos)) {
        for (auto& slot : off->buffer) {
            if (items::isEmpty(slot) || (filter && !filter(slot))) continue;
            auto box = slot;
            if (!simulate) {
                slot                    = items::empty();
                off->primed             = true;
                stationRecords().dirty = true;
            }
            return box;
        }
        return items::empty();
    }
    if (!isPort(pos)) return items::empty();
    auto& s = port(pos);
    for (auto& slot : s.inventory) {
        if (items::isEmpty(slot) || (filter && !filter(slot))) continue;
        auto box = slot;
        if (!simulate) {
            slot = items::empty();
            savePort(pos);
        }
        return box;
    }
    return items::empty();
}

// Create's FrogportBlockEntity.addToTooltip: a frogport whose caught packages could not go into
// the inventory below shows the "Contains Packages" hint.
LV_REGISTER_GOGGLE_INFO(kFrogportId, {
    .hover = [](goggles::InfoContext const& c, goggles::Tooltip& t) {
        if (!port(c.pos).failedLastExport) return false;
        t.push_back(goggles::line(std::string(core::goggles::fmt::kGold) + c.tr("create.hint.blocked_frogport.title", "Contains Packages")));
        goggles::addHint(
            t,
            c.tr("create.hint.blocked_frogport", "A _Package_ caught by this _Frogport_ could not be transferred to the _inventory_ _below._ Could it be full?")
        );
        return true;
    },
});

std::vector<std::string> addressSuggestions(Player& player, std::string const& localAddress) {
    // Create's AddressEditBoxHelper.createSuggestions: the local address first, then the unchecked
    // "#address" entries of the clipboards in the inventory and of the clipboards placed within 32
    // blocks, each once
    // screens ask for their data several times per action: reuse a scan for a second
    struct Cached {
        std::uint64_t            tick = 0;
        std::string              local;
        std::vector<std::string> list;
    };
    static std::unordered_map<std::int64_t, Cached> cache;
    auto& c = cache[player.getOrCreateUniqueID().rawID];
    if (c.tick != 0 && c.local == localAddress && currentTick() - c.tick < 20) return c.list;
    std::vector<std::string> out;
    std::set<std::string>    seen;
    if (!localAddress.empty() && seen.insert(localAddress).second) out.push_back(localAddress);
    auto addFrom = [&](display::clipboard::Contents const& c) {
        for (auto const& page : c.pages) {
            for (auto const& e : page) {
                if (e.checked || e.text.size() < 2 || e.text[0] != '#') continue;
                auto address = e.text.substr(1);
                auto first   = address.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                auto last = address.find_last_not_of(" \t");
                address   = address.substr(first, last - first + 1);
                if (seen.insert(address).second) out.push_back(address);
            }
        }
    };
    auto& inv = player.getInventory();
    for (int i = 0; i < std::min(36, inv.getContainerSize()); ++i) {
        auto const& s = inv.getItem(i);
        if (display::clipboard::isClipboard(s)) addFrom(display::clipboard::ofItem(s));
    }
    auto const& at  = player.getPosition();
    int         dim = int(player.getDimensionId());
    int         cx  = int(std::floor(at.x)) >> 4, cz = int(std::floor(at.z)) >> 4;
    for (int dx = -2; dx <= 2; ++dx) {
        for (int dz = -2; dz <= 2; ++dz) {
            blockdata::forEachInChunk(dim, cx + dx, cz + dz, [&](GlobalPos const& p, Json const& doc) {
                auto it = doc.find("clipboard");
                if (it == doc.end() || idAt(p) != display::kClipboardId) return;
                double ddx = p.x + .5 - at.x, ddy = p.y + .5 - at.y, ddz = p.z + .5 - at.z;
                if (ddx * ddx + ddy * ddy + ddz * ddz > 32.0 * 32.0) return;
                addFrom(display::clipboard::fromJson(*it));
            });
        }
    }
    c = {std::max<std::uint64_t>(currentTick(), 1), localAddress, out};
    return out;
}

void addStationPortRecord(GlobalPos const& port, GlobalPos const& station, std::string const& address) {
    loadStationPorts();
    StationPort sp;
    sp.station                     = station;
    sp.blockId                     = postboxIds().front();
    sp.address                     = address;
    stationRecords().ports[port]   = std::move(sp);
    stationRecords().dirty         = true;
    stationPorts()[station].insert(port);
}

void notifyDelivered(GlobalPos const& pos) {
    if (!isPort(pos)) return;
    particle(pos.dim, centre(pos), "minecraft:crop_growth_emitter");
    updateFlag(pos, port(pos));
}

} // namespace lv::feature::packages
