// Elevator columns and contacts (Create's ElevatorColumn, ElevatorContactBlock and its block
// entity): floors, calls by redstone, arrival pulses, floor names and the contact settings.
//
// Contact data (BlockData `data["pistons"]`): short (floor identifier, 4 chars), long (floor
// description, 30 chars), door (door control 0 all, 1 north, 2 east, 3 south, 4 west, 5 none:
// Create's DoorControl order), floor (the last reported current floor, for display links),
// activate (Create's activateBlock).

#include <algorithm>
#include <map>
#include <set>

#include "ll/api/form/CustomForm.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"

#include "feature/contraption/PistonsExt.h"
#include "feature/pistons/Pistons.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::pistons {

namespace {

constexpr char kDbColumns[]  = "lv:pistons:columns";
constexpr char kDbContacts[] = "lv:pistons:contacts";

struct Column {
    int  target          = 0;
    bool targetAvailable = false;
    bool active          = false;
    int  namesVersion    = 0;
};

struct ElevatorState {
    std::map<ColumnCoords, Column> columns;
    std::set<GlobalPos>            contacts;
    std::set<GlobalPos>            scheduled; ///< activations next tick
    std::set<ColumnCoords>         nameInit;  ///< columns whose blank names get numbers
    bool                           dirty = false;
};
ElevatorState& st() {
    static ElevatorState s;
    return s;
}

void persist() {
    if (!game::db::available()) return;
    Json cols = Json::array();
    for (auto const& [c, col] : st().columns)
        cols.push_back({c.dim, c.x, c.z, int(c.side), col.target, col.targetAvailable, col.active});
    game::db::save(kDbColumns, cols);
    Json contacts = Json::array();
    for (auto const& p : st().contacts) contacts.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbContacts, contacts);
    st().dirty = false;
}

Column& column(ColumnCoords const& c) { return st().columns[c]; }

/// Create's RedstoneContactBlock.hasValidContact: a contact in front faces back.
bool hasValidContact(GlobalPos const& p, cc::Dir facing) {
    auto front = rel(p, facing);
    auto id    = idAt(front);
    if (id != kRedstoneContact && id != kElevatorContact) return false;
    return facingAt(front) == cc::opposite(facing);
}

std::string str(Json const* d, char const* key) { return d ? d->value(key, std::string()) : std::string(); }

void initNames(ColumnCoords const& c) {
    auto floors = columnFloors(c);
    auto before = floors;
    cp::initFloorNames(floors);
    for (std::size_t i = 0; i < floors.size(); ++i) {
        if (floors[i].shortName == before[i].shortName) continue;
        GlobalPos p{c.dim, c.x, floors[i].y, c.z};
        data(p)["short"] = floors[i].shortName;
    }
    column(c).namesVersion++;
}

} // namespace

std::optional<ColumnCoords> columnOf(GlobalPos const& contact) {
    auto id = idAt(contact);
    if (id != kElevatorContact && id != kRedstoneContact) return std::nullopt;
    return ColumnCoords{contact.dim, contact.x, contact.z, facingAt(contact)};
}

std::vector<int> columnContacts(ColumnCoords const& c) {
    std::vector<int> out;
    auto*            r = regionOf(c.dim);
    if (!r) return out;
    for (int y = r->getMinHeight(); y < r->getMaxHeight(); ++y) {
        GlobalPos p{c.dim, c.x, y, c.z};
        if (idAt(p) != kElevatorContact) continue;
        if (facingAt(p) != c.side) continue;
        out.push_back(y);
    }
    return out;
}

void gatherColumn(ColumnCoords const& c) {
    auto* r = regionOf(c.dim);
    if (!r) return;
    int converted = 0;
    for (int y = r->getMinHeight(); y < r->getMaxHeight(); ++y) {
        GlobalPos p{c.dim, c.x, y, c.z};
        auto      col = columnOf(p);
        if (!col || !(*col == c)) continue;
        if (idAt(p) == kRedstoneContact) {
            EditGuard g;
            place(p, kElevatorContact, {{kFacing, int(c.side)}, {kPowered, stateOr(p, kPowered, 0)}}, 3);
            ++converted;
        }
        trackContact(p);
        if (str(dataIf(p), "short").empty()) st().nameInit.insert(c);
    }
    column(c).namesVersion++;
    st().dirty = true;
    if (converted) log().info("elevator column at {} {}: {} redstone contact(s) became elevator contacts", c.x, c.z, converted);
}

void columnTarget(ColumnCoords const& c, int y) {
    auto& col           = column(c);
    col.target          = y;
    col.targetAvailable = true;
    st().dirty          = true;
}

std::optional<int> columnTargetOf(ColumnCoords const& c) {
    auto it = st().columns.find(c);
    if (it == st().columns.end() || !it->second.targetAvailable) return std::nullopt;
    return it->second.target;
}

void columnSetActive(ColumnCoords const& c, bool active) {
    column(c).active = active;
    st().dirty       = true;
}

bool columnActive(ColumnCoords const& c) {
    auto it = st().columns.find(c);
    return it != st().columns.end() && it->second.active;
}

std::vector<cp::Floor> columnFloors(ColumnCoords const& c) {
    std::vector<cp::Floor> out;
    for (int y : columnContacts(c)) {
        auto const* d = dataIf({c.dim, c.x, y, c.z});
        out.push_back({y, str(d, "short"), str(d, "long")});
    }
    return out;
}

int columnNamesVersion(ColumnCoords const& c) {
    auto it = st().columns.find(c);
    return it == st().columns.end() ? 0 : it->second.namesVersion;
}

void floorReached(ColumnCoords const& c, int contactY) {
    std::string name = str(dataIf({c.dim, c.x, contactY, c.z}), "short");
    for (int y : columnContacts(c)) {
        GlobalPos p{c.dim, c.x, y, c.z};
        if (str(dataIf(p), "floor") == name) continue;
        data(p)["floor"] = name; // Create: updateDisplayedFloor (display links read it)
    }
}

void triggerContact(ColumnCoords const& c, int contactY) {
    GlobalPos p{c.dim, c.x, contactY, c.z};
    if (!loaded(p) || idAt(p) != kElevatorContact) return;
    if (stateOr(p, kPowering, 0)) return;
    data(p)["activate"] = true;
    st().scheduled.insert(p);
}

void callToContact(GlobalPos const& p, bool powered) {
    auto c = columnOf(p);
    if (!c || idAt(p) != kElevatorContact) return;
    for (int y : columnContacts(*c)) {
        GlobalPos other{p.dim, p.x, y, p.z};
        if (other == p) continue;
        EditGuard g;
        setStates(other, {{kCalling, 0}}, 2);
        st().scheduled.insert(other);
    }
    {
        EditGuard g;
        std::vector<std::pair<std::string_view, int>> s{{kCalling, 1}};
        if (powered) s.emplace_back(kPowered, 1);
        setStates(p, s, 3);
    }
    columnTarget(*c, p.y);
    log().info("elevator column at {} {} called to contact y {}", c->x, c->z, p.y);
}

void setContactNames(GlobalPos const& p, std::string shortName, std::string longName, int doorMode) {
    if (shortName.size() > 4) shortName.resize(4);
    if (longName.size() > 30) longName.resize(30);
    auto& d    = data(p);
    d["short"] = shortName;
    d["long"]  = longName;
    d["door"]  = std::clamp(doorMode, 0, 5);
    if (auto c = columnOf(p)) column(*c).namesVersion++;
    log().info("elevator contact at {} {} {} named '{}' ({})", p.x, p.y, p.z, shortName, longName);
}

void openContactScreen(Player& player, GlobalPos const& p) {
    auto const* d = dataIf(p);
    std::vector<std::string> doors{
        trFor(player, "create.contraption.door_control.all", "Open All Doors"),
        trFor(player, "create.contraption.door_control.north", "North Side Only"),
        trFor(player, "create.contraption.door_control.east", "East Side Only"),
        trFor(player, "create.contraption.door_control.south", "South Side Only"),
        trFor(player, "create.contraption.door_control.west", "West Side Only"),
        trFor(player, "create.contraption.door_control.none", "Keep Doors Closed"),
    };
    ll::form::CustomForm form(trFor(player, "create.elevator_contact.title", "Elevator Contact"));
    form.appendInput("short", trFor(player, "create.elevator_contact.floor_identifier", "Floor Identifier"), "", str(d, "short"));
    form.appendInput("long", trFor(player, "create.elevator_contact.floor_description", "Floor Description"), trFor(player, "create.crafting_blueprint.optional", "Optional"), str(d, "long"));
    form.appendDropdown("door", trFor(player, "create.contraption.door_control", "Onboard Door Control"), doors, static_cast<std::size_t>(d ? d->value("door", 0) : 0));
    form.sendTo(player, [p](Player& pl, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) return;
        auto text = [&](char const* k) {
            auto it = result->find(k);
            if (it == result->end()) return std::string();
            auto const* s = std::get_if<std::string>(&it->second);
            return s ? *s : std::string();
        };
        int  door = 0;
        if (auto it = result->find("door"); it != result->end())
            if (auto const* v = std::get_if<uint64>(&it->second)) door = static_cast<int>(*v);
        if (idAt(p) != kElevatorContact) return;
        // reach check (as the server validates Create's edit packet)
        auto pos = pl.getFeetPos();
        double dx = pos.x - (p.x + 0.5), dy = pos.y - (p.y + 0.5), dz = pos.z - (p.z + 0.5);
        if (dx * dx + dy * dy + dz * dz > 20 * 20) return;
        setContactNames(p, text("short"), text("long"), door);
    });
}

void trackContact(GlobalPos const& p) {
    if (st().contacts.insert(p).second) st().dirty = true;
}

void tickElevators() {
    auto& s = st();
    // scheduled activations (Create's ElevatorContactBlock.tick)
    auto scheduled = std::move(s.scheduled);
    s.scheduled.clear();
    for (auto const& p : scheduled) {
        if (!loaded(p) || idAt(p) != kElevatorContact) continue;
        bool was      = stateOr(p, kPowering, 0) != 0;
        bool activate = false;
        if (auto const* d = dataIf(p); d && d->value("activate", false)) {
            activate = true;
            data(p)["activate"] = false;
        }
        bool should = activate || hasValidContact(p, facingAt(p));
        if (was || should) {
            EditGuard g;
            setStates(p, {{kPowering, should ? 1 : 0}}, 3);
        }
    }
    // blank floor names get numbers (Create's deferred name generator)
    auto names = std::move(s.nameInit);
    s.nameInit.clear();
    for (auto const& c : names) initNames(c);
    // redstone calls: polled (custom blocks get no neighbour updates, SPEC §8)
    if (currentTick() % 2 == 0) {
        for (auto const& p : std::vector<GlobalPos>(s.contacts.begin(), s.contacts.end())) {
            if (!loaded(p)) continue;
            if (idAt(p) != kElevatorContact) {
                s.contacts.erase(p);
                s.dirty = true;
                continue;
            }
            bool isPowered = stateOr(p, kPowered, 0) != 0;
            bool signal    = contraption::ext::powered(p.dim, cpos(p));
            if (isPowered == signal) continue;
            {
                EditGuard g;
                setStates(p, {{kPowered, signal ? 1 : 0}}, 2);
            }
            if (isPowered) continue; // falling edge
            if (stateOr(p, kCalling, 0)) continue;
            callToContact(p, true);
        }
    }
    if (s.dirty) persist();
}

void resetElevators() { st() = ElevatorState{}; }

void loadElevators() {
    st() = ElevatorState{};
    if (auto j = game::db::load(kDbColumns); j && j->is_array()) {
        for (auto const& e : *j) {
            if (!e.is_array() || e.size() < 7) continue;
            ColumnCoords c{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), static_cast<cc::Dir>(std::clamp(e[3].get<int>(), 0, 5))};
            auto&        col = st().columns[c];
            col.target          = e[4].get<int>();
            col.targetAvailable = e[5].get<bool>();
            col.active          = e[6].get<bool>();
        }
    }
    if (auto j = game::db::load(kDbContacts); j && j->is_array()) {
        for (auto const& e : *j)
            if (e.is_array() && e.size() == 4) st().contacts.insert({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()});
    }
}

} // namespace lv::feature::pistons
