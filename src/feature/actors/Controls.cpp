// Contraption controls (Create's ContraptionControlsBlock / BlockEntity / Movement /
// MovingInteraction).
//
// On a contraption: right-click toggles the actors targeted by its filter (empty filter: all
// actors) — Create's disabled-actor list per contraption ("All Actors: Off", "<item> Actors:
// On"). A disabled actor does not act (its behaviour's isActive is false), which works for the
// actors of every unit: each actor block id found on a contraption is wrapped once.
// Stationary: right-click toggles it, a redstone signal change sets disabled = powered; when it
// is assembled while disabled, its targeted actors start disabled; when the contraption stops,
// the block remembers whether its target is disabled.

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/actors/Actors.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/ActorsExt.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::actors {

namespace ext = ::lv::feature::contraption::actors_ext;
namespace bd  = ::lv::feature::common::blockdata;
namespace fs  = ::lv::feature::common::filterslot;
namespace it  = ::lv::feature::common::items;

namespace {

constexpr char kDbKey[]    = "lv:actors:controls";
constexpr char kDbPosKey[] = "lv:actors:controls_pos";

/// Disabled actor types per contraption: item ids, "" = every actor (Create's disabledActors).
std::unordered_map<con::Id, std::vector<std::string>>& disabled() {
    static std::unordered_map<con::Id, std::vector<std::string>> d;
    return d;
}
/// Contraptions already scanned for initially disabled controls.
std::unordered_set<con::Id>& seen() {
    static std::unordered_set<con::Id> s;
    return s;
}
/// Stationary controls (redstone polling).
std::set<GlobalPos>& stationaryControls() {
    static std::set<GlobalPos> s;
    return s;
}
bool& dirty() {
    static bool d = false;
    return d;
}

/// Marker type of the wrapper (so an actor is wrapped only once).
struct ControlsGate {
    std::string                                      blockId;
    std::function<bool(con::MovementContext const&)> inner;
    bool operator()(con::MovementContext const& ctx) const {
        if (actorDisabled(ctx.contraption.id(), blockId)) return false;
        return inner ? inner(ctx) : true;
    }
};

/// Create's canBeDisabledVia: the actor's block item; controls themselves can't be disabled.
bool controllable(std::string const& blockId) { return blockId != kControls; }

void wrap(std::string const& blockId) {
    if (!controllable(blockId)) return;
    auto const* b = ext::actorBehaviour(blockId);
    if (!b || (b->isActive && b->isActive.target<ControlsGate>())) return;
    con::MovementBehaviour copy = *b;
    copy.isActive               = ControlsGate{blockId, b->isActive};
    con::registerActor(blockId, std::move(copy));
}

std::string filterIdOf(Json const* doc) {
    if (!doc || !doc->is_object() || !doc->contains("filter") || !(*doc)["filter"].contains("filter")) return {};
    return it::idOf(it::fromJson((*doc)["filter"]["filter"]));
}

bool typeDisabled(con::Id id, std::string const& filter) {
    auto i = disabled().find(id);
    if (i == disabled().end()) return false;
    return std::find(i->second.begin(), i->second.end(), filter) != i->second.end();
}

std::string displayName(std::string const& itemId) {
    auto s = it::make(itemId);
    return s ? it::displayName(*s) : itemId;
}

/// Create's ContraptionControlsBlockEntity.sendStatus.
void sendStatus(Player& player, std::string const& filter, bool enabled) {
    std::string state = enabled ? "§a" + trFor(player, "create.contraption.controls.actor_toggle.on", "On")
                                : "§6" + trFor(player, "create.contraption.controls.actor_toggle.off", "Off");
    std::string text  = filter.empty() ? trFor(player, "create.contraption.controls.all_actor_toggle", "All Actors: {0}", state)
                                       : trFor(player, "create.contraption.controls.specific_actor_toggle", "{0} Actors: {1}", displayName(filter), state);
    ext::actionBar(player, text);
}

void click(int dim, cc::Vec3d const& at, bool enable) {
    ext::playSound(dim, at, "random.click", 1.f, enable ? 1.5f : .8f);
}

/// Create's isDisabledInitially: controls assembled while disabled disable their target.
void scanNew(con::Contraption const& c) {
    if (!seen().insert(c.id()).second) return;
    dirty() = true; // remembered: a reload must not apply the assembly state again
    dirty() = true; // remembered: a reload must not apply the assembly state again
    bool any = false;
    for (auto const& b : c.blocks()) {
        if (b.id != kControls) continue;
        any            = true;
        auto const* doc = ext::carriedBlockData(c, b.local);
        bool        off = doc && doc->contains("actors") && (*doc)["actors"].value("disabled", false);
        if (!off) continue;
        auto  f    = filterIdOf(doc);
        auto& list = disabled()[c.id()];
        if (std::find(list.begin(), list.end(), f) == list.end()) list.push_back(f);
        dirty() = true;
    }
    if (any) wrapActorsOf(c);
}

/// Create's stopMoving: the carried controls remember whether their target is disabled (also
/// right after a toggle, so a contraption disassembled while standing still keeps it).
void rememberDisabled(con::Contraption& c) {
    std::vector<cc::Pos> controls;
    for (auto const& b : c.blocks())
        if (b.id == kControls) controls.push_back(b.local);
    for (auto const& local : controls) {
        // the carried data the engine persists with the contraption (U32's writable accessor)
        auto* carried = c.blockData(local);
        if (!carried) continue;
        auto& data = *carried;
        auto& doc  = data[ext::kBlockDataCarrier];
        if (!doc.is_object()) doc = Json::object();
        if (!doc["actors"].is_object()) doc["actors"] = Json::object();
        std::string f   = filterIdOf(&doc);
        bool        off = typeDisabled(c.id(), f) || typeDisabled(c.id(), "");
        doc["actors"]["disabled"] = off;
        // its indicator: set on the placed block at disassembly (the "actors" carrier, Seats.cpp)
        if (data.contains("actors") && data["actors"].is_object()) data["actors"]["disabled"] = off;
    }
}

} // namespace

bool actorDisabled(con::Id id, std::string const& blockId) {
    auto i = disabled().find(id);
    if (i == disabled().end() || i->second.empty()) return false;
    for (auto const& f : i->second)
        if (f.empty() || f == blockId) return true;
    return false;
}

void registerOwnActor(std::string const& blockId, con::MovementBehaviour behaviour) {
    if (controllable(blockId)) behaviour.isActive = ControlsGate{blockId, behaviour.isActive};
    con::registerActor(blockId, std::move(behaviour));
}

void wrapActorsOf(con::Contraption const& c) {
    std::unordered_set<std::string> ids;
    for (auto const& b : c.blocks()) ids.insert(b.id);
    for (auto const& id : ids) wrap(id);
}

void controlsClickOnContraption(Player& player, con::Contraption& c, cc::Pos const& local) {
    wrapActorsOf(c);
    std::string filter = filterIdOf(ext::carriedBlockData(c, local));
    auto&       list   = disabled()[c.id()];
    bool        disable = true, invert = false;
    for (auto i = list.begin(); i != list.end(); ++i) {
        if (*i == filter) {
            list.erase(i);
            disable = false;
            break;
        }
        if (i->empty()) {
            list.erase(i);
            disable = false;
            invert  = true;
            break;
        }
    }
    if (invert) {
        // everything was off: switch every other actor type off one by one
        for (auto const& b : c.blocks()) {
            if (!ext::actorBehaviour(b.id) || !controllable(b.id) || b.id == filter) continue;
            if (std::find(list.begin(), list.end(), b.id) == list.end()) list.push_back(b.id);
        }
    }
    if (filter.empty()) list.clear();
    if (disable) list.push_back(filter);
    dirty() = true;
    rememberDisabled(c);
    sendStatus(player, filter, !disable);
    click(c.dimension(), c.toWorld(cc::Vec3d::center(local)), !disable);
    logAction(
        "contraption #" + std::to_string(c.id()) + " controls: " + (filter.empty() ? std::string("all actors") : filter) + " "
        + (disable ? "off" : "on")
    );
}

void controlsClickStationary(Player& player, GlobalPos const& pos) {
    auto& doc = bd::edit(pos);
    if (!doc["actors"].is_object()) doc["actors"] = Json::object();
    bool nowDisabled = !doc["actors"].value("disabled", false);
    doc["actors"]["disabled"] = nowDisabled;
    stationaryControls().insert(pos);
    dirty() = true;
    if (auto* region = regionOf(pos.dim)) setIntState(*region, toBlockPos(pos), "levitate:disabled", nowDisabled ? 1 : 0);
    sendStatus(player, filterIdOf(bd::get(pos)), !nowDisabled);
    click(pos.dim, cc::Vec3d::center(cc::Pos{pos.x, pos.y, pos.z}), !nowDisabled);
    logAction(
        "controls at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z) + " "
        + (nowDisabled ? "disabled" : "enabled")
    );
}

void controlsTick(std::uint64_t tick) {
    for (auto id : con::all())
        if (auto* c = con::find(id)) scanNew(*c);
    // forget contraptions that are gone
    if (tick % 100 == 0) {
        auto live = con::all();
        std::unordered_set<con::Id> alive(live.begin(), live.end());
        std::erase_if(seen(), [&](con::Id id) { return !alive.contains(id); });
        for (auto i = disabled().begin(); i != disabled().end();) {
            if (!alive.contains(i->first)) {
                i       = disabled().erase(i);
                dirty() = true;
            } else {
                ++i;
            }
        }
    }
    // stationary: Create's updatePoweredState (a change of power sets disabled = powered)
    if (tick % 2 == 0) {
        for (auto i = stationaryControls().begin(); i != stationaryControls().end();) {
            auto const& pos    = *i;
            auto*       region = regionOf(pos.dim);
            auto        bp     = toBlockPos(pos);
            if (!region || !region->hasChunksAt(bp, 0, false)) {
                ++i;
                continue;
            }
            if (region->getBlock(bp).getTypeName() != kControls) {
                i       = stationaryControls().erase(i);
                dirty() = true;
                continue;
            }
            bool  powered = ext::powered(*region, {pos.x, pos.y, pos.z});
            auto* doc     = bd::get(pos);
            bool  was     = doc && doc->contains("actors") && (*doc)["actors"].value("powered", false);
            if (powered != was) {
                auto& d                 = bd::edit(pos);
                d["actors"]["powered"]  = powered;
                d["actors"]["disabled"] = powered;
                setIntState(*region, bp, "levitate:disabled", powered ? 1 : 0);
                logAction(
                    "controls at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z) + " "
                    + (powered ? "powered" : "unpowered")
                );
            }
            ++i;
        }
    }
}

void controlsStopMoving(con::MovementContext& ctx) { rememberDisabled(ctx.contraption); }

void trackStationaryControls(GlobalPos const& pos) {
    stationaryControls().insert(pos);
    dirty() = true;
}

void controlsLoad() {
    disabled().clear();
    seen().clear();
    stationaryControls().clear();
    if (auto j = game::db::load(kDbKey); j && j->is_object()) {
        for (auto const& id : j->value("scanned", Json::array())) seen().insert(id.get<con::Id>());
        for (auto const& [k, v] : j->value("disabled", Json::object()).items()) {
            auto& list = disabled()[std::stoull(k)];
            for (auto const& f : v) list.push_back(f.get<std::string>());
        }
    }
    if (auto j = game::db::load(kDbPosKey); j && j->is_array()) {
        for (auto const& p : *j) stationaryControls().insert(GlobalPos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>(), p[3].get<int>()});
    }
    dirty() = false;
}

void controlsSave() {
    if (!dirty()) return;
    Json j = {{"disabled", Json::object()}, {"scanned", Json::array()}};
    for (auto const& [id, list] : disabled())
        if (!list.empty()) j["disabled"][std::to_string(id)] = list;
    for (auto id : seen()) j["scanned"].push_back(id);
    game::db::save(kDbKey, j);
    Json p = Json::array();
    for (auto const& g : stationaryControls()) p.push_back(Json::array({g.dim, g.x, g.y, g.z}));
    game::db::save(kDbPosKey, p);
    dirty() = false;
}

void controlsInstall() {
    // Create's ControlsSlot: on the panel, towards the front (voxel (8, 14, 5.5) facing north)
    fs::registerSlot(
        std::string(kControls),
        fs::SlotSpec{
            .id     = "filter",
            .centre = [](Block const& b, int face) -> std::optional<core::common::Vec> {
                if (face != 1) return std::nullopt;
                auto f     = static_cast<cc::Dir>(std::clamp(game::intState(b, kFacing).value_or(2), 0, 5));
                auto front = cc::offset(f);
                return core::common::Vec{.5 + front.x * 2.5 / 16, 14 / 16.0, .5 + front.z * 2.5 / 16};
            },
            .scale     = 0.5,
            .onChanged = [](GlobalPos const& p) {
                trackStationaryControls(p);
                logAction(
                    "controls at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + " target set to "
                    + it::idOf(fs::get(p, "filter"))
                );
            },
            // (U55) Create's withPredicate(AllItemTags.CONTRAPTION_CONTROLLED::matches): only the
            // items of blocks controls can switch ("Cannot use this item here." otherwise)
            .accepts = [](GlobalPos const&, ItemStack const& s) {
                static constexpr std::string_view kControlled[] = {
                    "levitate:portable_fluid_interface", "levitate:mechanical_drill", "levitate:mechanical_saw", "levitate:deployer",
                    "levitate:portable_storage_interface", "levitate:redstone_contact", "levitate:mechanical_harvester",
                    "levitate:mechanical_plough", "levitate:mechanical_roller", "levitate:andesite_funnel", "levitate:brass_funnel",
                    "levitate:peculiar_bell", "levitate:haunted_bell", "levitate:andesite_door", "levitate:brass_door",
                    "levitate:copper_door", "levitate:train_door", "levitate:framed_glass_door", "minecraft:bell", "minecraft:campfire",
                    "minecraft:soul_campfire", "minecraft:dispenser", "minecraft:dropper",
                };
                auto id = it::idOf(s);
                return std::find(std::begin(kControlled), std::end(kControlled), id) != std::end(kControlled);
            },
        }
    );
}

void controlsClear() {
    disabled().clear();
    seen().clear();
    stationaryControls().clear();
    dirty() = false;
}

namespace {

struct Registrar {
    Registrar() {
        con::MovementBehaviour b;
        b.stopMoving = controlsStopMoving;
        con::registerActor(std::string(kControls), std::move(b));
    }
};
Registrar const gRegistrar;

} // namespace

} // namespace lv::feature::actors
