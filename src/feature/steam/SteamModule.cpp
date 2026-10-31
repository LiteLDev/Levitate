// Steam unit: module (events, world lifecycle, game hooks), the powered shaft's kinetic spec,
// the client sync of tank contents / engines, the query API and `/create steam <pos>`.

#include <algorithm>
#include <climits>
#include <cmath>
#include <numbers>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPosition.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/fluids/ItemFluids.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/steam/Steam.h"
#include "feature/steam/SteamInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::steam {

namespace ia = common::interactions;

// The powered shaft is a generator shaft; its speed and capacity come from its engine.
LV_REGISTER_KINETIC_BLOCK(
    kPoweredShaft,
    kinetics::specs::withModel(kinetics::specs::generatorShaft(0, "levitate:axis"), "geometry.levitate.powered_shaft", "levitate_powered_shaft")
);

namespace {

constexpr char kDbTanks[] = "lv:steam:tanks";

bool relevant(std::string_view id) {
    return isTankId(id) || id == kEngine || id == kWhistle || id == kExtension || id == kPoweredShaft;
}

// ---- client sync ---------------------------------------------------------------------------------------

std::set<GlobalPos>& syncDirty() {
    static std::set<GlobalPos> s;
    return s;
}
std::map<GlobalPos, Json>& lastSent() {
    static std::map<GlobalPos, Json> m;
    return m;
}
Json& lastEngines() {
    static Json j;
    return j;
}
bool gTankIndexDirty = false;

Json posJson(GlobalPos const& p) { return Json::array({p.x, p.y, p.z}); }

std::optional<Json> tankEntry(GlobalPos const& ctrl) {
    auto info = tankAt(ctrl);
    if (!info || !(info->controller == ctrl)) return std::nullopt;
    bool boiler = boilerActive(info->controller);
    Json entry{
        {"p",      posJson(ctrl)  },
        {"w",      info->width    },
        {"h",      info->height   },
        {"f",      info->fluid    },
        {"a",      info->amount   },
        {"c",      info->capacity },
        {"win",    info->window   },
        {"boiler", boiler         },
    };
    if (boiler) {
        // the boiler gauges: dial target and the sides hidden by a block in front
        auto const& it = boilers().find(info->controller);
        if (it != boilers().end()) {
            entry["g"]   = it->second.gauge;
            entry["occ"] = it->second.occluded;
        }
    }
    return entry;
}

void broadcast(std::string_view channel, Json const& payload) {
    auto level = ll::service::getLevel();
    if (!level) return;
    static std::set<std::string> logged;
    level->forEachPlayer([&](Player& p) {
        if (net::send(p, channel, payload) && logged.insert(std::string(channel)).second)
            log().info("steam: first {} message sent to {}", channel, p.getRealName());
        return true;
    });
}

Json enginesPayload() {
    Json list = Json::array();
    for (auto const& e : index().engines) {
        auto* region = regionOf(e.dim);
        if (!region || !loaded(*region, e) || idAt(*region, e) != kEngine) continue;
        auto  shape    = engineShape(region->getBlock(game::blockPos(e)));
        auto  shaftPos = offset(e, shape.facing, 2);
        float speed    = idAt(*region, shaftPos) == kPoweredShaft ? kinetics::speedAt(shaftPos) : 0.f;
        int   axis     = idAt(*region, shaftPos) == kPoweredShaft ? intState(region->getBlock(game::blockPos(shaftPos)), "levitate:axis").value_or(0) : 0;
        list.push_back({
            {"d",  e.dim                              },
            {"p",  posJson(e)                         },
            {"f",  static_cast<int>(shape.facing)     },
            {"hz", static_cast<int>(shape.horizontal)},
            {"s",  speed                              },
            {"ax", axis                               },
        });
    }
    return Json{{"engines", list}};
}

/// The sounding whistles (Create's WhistleRenderer animates their mouth while they blow).
Json whistlesPayload() {
    Json list = Json::array();
    for (auto const& w : index().whistles)
        if (whistleSounding(w)) list.push_back({{"d", w.dim}, {"p", posJson(w)}});
    return Json{{"whistles", list}};
}

Json& lastWhistles() {
    static Json j;
    return j;
}

void saveTankIndex() {
    if (!game::db::available()) return;
    Json a = Json::array();
    for (auto const& [p, j] : lastSent()) a.push_back({p.dim, p.x, p.y, p.z});
    game::db::save(kDbTanks, a);
    gTankIndexDirty = false;
}

void loadTankIndex() {
    lastSent().clear();
    auto j = game::db::load(kDbTanks);
    if (!j || !j->is_array()) return;
    for (auto const& e : *j) {
        if (!e.is_array() || e.size() != 4) continue;
        GlobalPos p{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()};
        lastSent()[p] = Json(); // unknown until its chunk is loaded
        syncDirty().insert(p);
    }
}

} // namespace

void markTankForSync(GlobalPos const& controller) { syncDirty().insert(controller); }

void flushSync() {
    if (currentTick() % 100 == 0)
        for (auto const& [p, j] : lastSent()) syncDirty().insert(p);
    std::map<int, Json> sets, dels;
    for (auto const& ctrl : syncDirty()) {
        auto* region = regionOf(ctrl.dim);
        if (!region || !loaded(*region, ctrl)) continue;
        auto entry = tankEntry(ctrl);
        auto it    = lastSent().find(ctrl);
        if (!entry) {
            if (it != lastSent().end()) {
                lastSent().erase(it);
                dels[ctrl.dim].push_back(posJson(ctrl));
                gTankIndexDirty = true;
            }
            continue;
        }
        if (it != lastSent().end() && it->second == *entry) continue;
        if (it == lastSent().end()) gTankIndexDirty = true;
        lastSent()[ctrl] = *entry;
        sets[ctrl.dim].push_back(*entry);
    }
    syncDirty().clear();
    std::set<int> dims;
    for (auto const& [d, j] : sets) dims.insert(d);
    for (auto const& [d, j] : dels) dims.insert(d);
    for (int d : dims) {
        Json msg{{"dim", d}, {"set", sets.contains(d) ? sets[d] : Json::array()}, {"del", dels.contains(d) ? dels[d] : Json::array()}};
        broadcast("steam.tanks", msg);
    }
    if (currentTick() % 5 == 0) {
        auto engines = enginesPayload();
        if (engines != lastEngines()) {
            lastEngines() = engines;
            broadcast("steam.engines", engines);
        }
    }
    auto whistles = whistlesPayload();
    if (whistles != lastWhistles()) {
        lastWhistles() = whistles;
        broadcast("steam.whistles", whistles);
    }
    if (gTankIndexDirty && currentTick() % 20 == 0) saveTankIndex();
}

void sendFullSync(Player& player) {
    std::map<int, Json> sets;
    for (auto const& [p, j] : lastSent())
        if (j.is_object()) sets[p.dim].push_back(j);
    net::send(player, "steam.tanks", Json{{"reset", true}, {"dim", 0}, {"set", Json::array()}, {"del", Json::array()}});
    for (auto const& [d, list] : sets) net::send(player, "steam.tanks", Json{{"dim", d}, {"set", list}, {"del", Json::array()}});
    net::send(player, "steam.engines", enginesPayload());
    net::send(player, "steam.whistles", whistlesPayload());
}

// ---- query API -------------------------------------------------------------------------------------------

std::optional<TankView> tank(GlobalPos const& pos) {
    auto info = tankAt(pos);
    if (!info) return std::nullopt;
    TankView v;
    v.controller = info->controller;
    v.width      = info->width;
    v.height     = info->height;
    v.fluid      = info->fluid;
    v.amount     = info->amount;
    v.capacity   = info->capacity;
    v.window     = info->window;
    v.creative   = info->creative;
    auto* rt     = boilerOf(info->controller);
    v.boiler     = rt->boiler.isActive();
    int size     = info->width * info->width * info->height;
    v.boilerLevel = rt->boiler.actualHeat(size);
    v.passive     = rt->boiler.isPassive(size);
    return v;
}

std::vector<std::string> goggleLines(GlobalPos const& pos, Player const* viewer) { return describe(pos, viewer); }

// ---- goggles (U8's overlay: Create's addToGoggleTooltip of the unit's block entities) ----------------

namespace {

namespace goggles = feature::goggles;

/// Command lines (`describe`: two spaces per indent, "" between groups) as goggle lines.
void addAsGoggleLines(std::vector<std::string> const& lines, goggles::Tooltip& t) {
    for (auto const& l : lines) {
        if (l.empty()) t.emplace_back();
        else if (l.starts_with("  ")) t.push_back(goggles::line(l.substr(2), 1));
        else t.push_back(goggles::line(l));
    }
}

// FluidTankBlockEntity.addToGoggleTooltip: the boiler status of a boiler, else the fluid container
LV_REGISTER_GOGGLE_INFO(
    kTank,
    {
        .goggle =
            [](goggles::InfoContext const& c, goggles::Tooltip& t) {
                auto info = tankAt(c.pos);
                if (!info) return false;
                if (boilerActive(info->controller)) {
                    addAsGoggleLines(describe(c.pos, &c.player), t);
                    return true;
                }
                return goggles::fluidTooltip(c, t);
            },
        .replaceDefaults = true,
    }
);

// CreativeFluidTankBlockEntity.addToGoggleTooltip: nothing
LV_REGISTER_GOGGLE_INFO(kCreativeTank, {.replaceDefaults = true});

// SteamEngineBlockEntity.addToGoggleTooltip: the generator stats of its powered shaft
// (PoweredShaftBlockEntity.addToEngineTooltip)
LV_REGISTER_GOGGLE_INFO(
    kEngine,
    {
        .goggle =
            [](goggles::InfoContext const& c, goggles::Tooltip& t) {
                auto shaftPos = offset(c.pos, engineShape(c.block).facing, 2);
                if (idAt(c.region, shaftPos) != kPoweredShaft) return false;
                goggles::InfoContext shaft{c.player, c.region, shaftPos, c.region.getBlock(game::blockPos(shaftPos)), c.sneaking, c.wearingGoggles};
                return goggles::kineticGoggleTooltip(shaft, t);
            },
        .replaceDefaults = true,
    }
);

// PoweredShaftBlockEntity.addToGoggleTooltip: nothing (the engine shows it); the kinetic hints stay
LV_REGISTER_GOGGLE_INFO(kPoweredShaft, {.hover = goggles::kineticHoverTooltip, .replaceDefaults = true});

// WhistleBlockEntity.addToGoggleTooltip: "Pitch: <note>"
LV_REGISTER_GOGGLE_INFO(
    kWhistle,
    {
        .goggle =
            [](goggles::InfoContext const& c, goggles::Tooltip& t) {
                auto notes = c.tr("create.generic.notes", "F#;F;E;D#;D;C#;C;B;A#;A;G#;G");
                std::vector<std::string> names;
                for (std::size_t start = 0;;) {
                    auto semi = notes.find(';', start);
                    names.push_back(notes.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
                    if (semi == std::string::npos) break;
                    start = semi + 1;
                }
                auto pitch = static_cast<std::size_t>(std::max(0, whistlePitchAt(c.pos)));
                t.push_back(goggles::line(c.tr("create.generic.pitch", "Pitch: {0}", names[pitch % names.size()])));
                return true;
            },
        .replaceDefaults = true,
    }
);

} // namespace

int comparatorLevel(GlobalPos const& pos) {
    auto info = tankAt(pos);
    return info ? cs::comparatorLevel(info->amount, info->capacity) : 0;
}

int whistlePitch(GlobalPos const& pos) { return whistlePitchAt(pos); }

// ---- game hooks (GameMode sees network players and SimulatedPlayers alike) -------------------------------

namespace {

std::map<std::int64_t, std::uint64_t>& lastUse() {
    static std::map<std::int64_t, std::uint64_t> m;
    return m;
}

/// Bedrock repeats item use while the button is held: one action per 4 ticks per player.
bool busy(Player& player) {
    auto  id   = player.getOrCreateUniqueID().rawID;
    auto& last = lastUse()[id];
    if (last != 0 && currentTick() - last < 4) return true;
    last = currentTick() == 0 ? 1 : currentTick();
    return false;
}

} // namespace

// ---- interactions (U36: on the shared dispatch) --------------------------------------------------

namespace {

/// Create's SteamEngineBlock.PlacementHelper: a shaft used on an engine goes to the engine's shaft
/// position (two in front of it), along the first axis of the player's look order that is not the
/// engine's facing axis; the server places the powered shaft directly. False when the spot is taken.
bool placeEngineShaft(Player& player, GlobalPos const& engine, ItemStack& item) {
    auto* region = regionOf(engine.dim);
    if (!region) return false;
    auto      shape    = engineShape(region->getBlock(game::blockPos(engine)));
    GlobalPos shaftPos = offset(engine, shape.facing, 2);
    if (!loaded(*region, shaftPos) || !game::replaceable(*region, game::blockPos(shaftPos))) return false;
    // Direction.orderedByNearest(player): the six directions by how closely the player looks along them
    auto   rot   = player.getRotation();
    double pitch = rot.x * std::numbers::pi / 180.0, yaw = rot.y * std::numbers::pi / 180.0;
    double look[3]{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    std::vector<cs::Dir> order(std::begin(kDirs), std::end(kDirs));
    auto score = [&](cs::Dir d) {
        auto n = offset(GlobalPos{0, 0, 0, 0}, d);
        return n.x * look[0] + n.y * look[1] + n.z * look[2];
    };
    std::stable_sort(order.begin(), order.end(), [&](cs::Dir a, cs::Dir b) { return score(a) > score(b); });
    int axis = static_cast<int>(cs::axisOf(order.front()));
    for (auto d : order) {
        if (cs::axisOf(d) == cs::axisOf(shape.facing)) continue;
        axis = static_cast<int>(cs::axisOf(d));
        break;
    }
    auto const* block = game::blockWith(kPoweredShaft, {{"levitate:axis", axis}});
    if (!block || !game::setBlock(*region, game::blockPos(shaftPos), *block)) return false;
    playSound(shaftPos.dim, shaftPos.x + .5, shaftPos.y + .5, shaftPos.z + .5, "use.stone", 1.f, .8f);
    if (!player.isCreative()) {
        item.remove(1);
        player.setSelectedItem(item);
    }
    player.sendInventory(false);
    log().info("steam: {} placed a shaft at {} {} {} for the steam engine at {} {} {}", player.getRealName(), shaftPos.x, shaftPos.y, shaftPos.z,
               engine.x, engine.y, engine.z);
    return true;
}

/// Right-clicks on tanks and whistles, and the placement rules of engines and whistles.
ia::Result steamUseOn(ia::UseOnContext const& c) {
    Player&            player = c.player;
    std::string const& id     = c.blockId;
    std::string const& heldId = c.itemId;
    ItemStack const&   item   = c.item;
    auto               pos    = c.pos;
    bool               sneak  = c.sneaking;
    int                face   = c.face;
    try {
        if (isTankId(id)) {
            if (heldId == kWrench && !sneak) {
                if (!busy(player)) {
                    toggleWindows(pos);
                    playSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "block.itemframe.rotate_item", 1.f, 1.f);
                }
                return ia::Result::Consume;
            }
            if (!heldId.empty() && !sneak) {
                if (isTankId(heldId) && tryMultiPlace(player, pos, face, item)) return ia::Result::Consume;
                if (!isTankId(heldId) && heldId != kEngine && heldId != kWhistle) {
                    // Create's FluidTankBlock.useItemOn: creative players, or anyone on a creative tank
                    bool container = feature::fluids::canBeEmptied(item) || feature::fluids::canBeFilled(item, false);
                    bool allowed   = player.isCreative() || isCreativeTankId(id);
                    if (container && allowed && (busy(player) || useItemOnTank(player, pos, item))) {
                        player.sendInventory(false);
                        return ia::Result::Consume;
                    }
                }
            }
        }
        // whistle item: grow the column; wrench: next size; sneaking wrench on an extension: shorten
        // (a sneaking wrench on the whistle itself is the wrench's dismantle, U8)
        bool whistleUse = (id == kWhistle || id == kExtension)
                       && ((heldId == kWhistle && !sneak) || (heldId == kWrench && (!sneak || id == kExtension)));
        if (whistleUse) {
            if (busy(player)) return ia::Result::Consume;
            if (useOnWhistle(player, pos, item, sneak, c.hit[1])) {
                player.sendInventory(false);
                return ia::Result::Consume;
            }
        }
        // Create's SteamEngineBlock.useItemOn: the shaft placement helper
        if (id == kEngine && heldId == kShaft && !sneak) {
            if (busy(player)) return ia::Result::Consume;
            if (placeEngineShaft(player, pos, ia::mutableItem(item))) return ia::Result::Consume;
        }
        // Create: engines only go on tanks, whistles on tanks (side or top)
        if (heldId == kEngine && !isTankId(id)) {
            player.sendInventory(false);
            return ia::Result::Deny;
        }
        if (heldId == kWhistle && (!isTankId(id) || face == 0) && id != kWhistle && id != kExtension) {
            player.sendInventory(false);
            return ia::Result::Deny;
        }
    } catch (std::exception const& ex) {
        log().error("steam: interaction failed: {}", ex.what());
    }
    return ia::Result::Pass;
}

/// Create's powered shaft drops a shaft when a survival player breaks it.
void steamDestroyed(ia::DestroyContext const& c, bool destroyed) {
    if (!destroyed || c.player.isCreative()) return;
    GlobalPos pos    = c.pos;
    auto*     region = regionOf(pos.dim);
    auto      level  = ll::service::getLevel();
    if (!region || !level) return;
    BlockPos bp{pos.x, pos.y, pos.z};
    if (!region->hasChunksAt(bp, 0, false) || region->getBlock(bp).getTypeName() == kPoweredShaft) return;
    if (auto stack = common::items::make(kShaft)) {
        level->getSpawner().spawnItem(*region, *stack, nullptr, Vec3{float(pos.x) + .5f, float(pos.y) + .5f, float(pos.z) + .5f}, 0);
    }
}

} // namespace

class SteamModule {
public:
    void install() {
        static bool once = false;
        if (!once) {
            once = true;
            registerTankCapabilities();
            registerEngineValueBox();
            common::heat::registerBlock(kTestHeater, [](BlockSource&, BlockPos const&, Block const& block) {
                return static_cast<common::heat::HeatLevel>(std::clamp(intState(block, "levitate:heat").value_or(0), 0, 4));
            });
            common::blockdata::onErase([](GlobalPos const& pos, Json const& doc, std::string const& prevId) {
                if (isTankId(prevId)) stashErasedDoc(pos, doc);
            });
            game::db::onWorldReady([] { worldReady(); });
            game::db::onWorldSave([] {
                saveIndex();
                saveTankIndex();
            });
            game::db::onWorldClosed([] {
                resetRuntime();
                resetPendingChanges();
                lastSent().clear();
                syncDirty().clear();
                lastEngines() = Json();
                lastWhistles() = Json();
            });
            net::onCompanionJoined([](Player& p) { sendFullSync(p); });
        }
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0) return;
            auto& region = ev.blockSource();
            if (region.getILevel().isClientSide()) return;
            auto const& prev = ev.previousBlock();
            auto const& next = ev.newBlock();
            if (&prev.getBlockType() == &next.getBlockType()) return;
            std::string const& prevId = prev.getTypeName();
            std::string const& newId  = next.getTypeName();
            if (!relevant(prevId) && !relevant(newId)) return;
            queueChange(game::globalPos(region, ev.pos()), prevId, newId);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            try {
                processChanges();
                tickSteam();
                flushSync();
            } catch (std::exception const& ex) {
                log().error("steam: tick failed: {}", ex.what());
            }
        }));
        mHandlers.push_back(ia::onUseOn({}, steamUseOn));
        mHandlers.push_back(ia::onDestroyed({std::string(kPoweredShaft)}, steamDestroyed));
        if (game::db::available()) worldReady(); // BDS: the world exists before mods are enabled
    }

    void uninstall() {
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        if (game::db::available()) {
            saveIndex();
            saveTankIndex();
        }
    }

private:
    static void worldReady() {
        resetRuntime();
        resetPendingChanges();
        loadIndex();
        loadTankIndex();
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    std::vector<ia::HandlerId>          mHandlers;
};

// ---- command ---------------------------------------------------------------------------------------------

struct SteamCommandParams {
    CommandPosition pos{};
};
struct SteamFillParams {
    CommandPosition pos{};
    std::string     fluid;
    int             amount = 0;
};

namespace {
GlobalPos commandPos(CommandOrigin const& origin, CommandPosition const& p) {
    Vec3  v   = p.getPosition(INT_MAX, origin, Vec3{0.f, 0.f, 0.f});
    auto* dim = origin.getDimension();
    return game::globalPos(dim ? static_cast<int>(dim->getDimensionId()) : 0, game::blockPosOf(v));
}
} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload<SteamCommandParams>().text("steam").required("pos").execute(
        [](CommandOrigin const& origin, CommandOutput& output, SteamCommandParams const& params) {
            if (!commands::isAdmin(origin)) return output.error("You need operator permission.");
            auto  pos    = commandPos(origin, params.pos);
            auto* viewer = commands::playerOf(origin);
            auto  lines  = describe(pos, viewer);
            if (lines.empty()) return output.error(tr("lv.steam.cmd.none", "No fluid tank, steam engine or steam whistle at {0} {1} {2}.", pos.x, pos.y, pos.z));
            for (auto const& line : lines) output.success(line);
            if (!viewer)
                for (auto const& line : lines) log().info("steam: {}", line);
        }
    );
    // Levitate debug: pours fluid into a tank through its fluid capability (as a pump would)
    cmd.overload<SteamFillParams>().text("steam").required("pos").text("fill").required("fluid").required("amount").execute(
        [](CommandOrigin const& origin, CommandOutput& output, SteamFillParams const& params) {
            if (!commands::isAdmin(origin)) return output.error("You need operator permission.");
            auto  pos    = commandPos(origin, params.pos);
            auto* region = regionOf(pos.dim);
            if (!region || !tankAt(pos)) return output.error(tr("lv.steam.cmd.none", "No fluid tank, steam engine or steam whistle at {0} {1} {2}.", pos.x, pos.y, pos.z));
            std::string fluid = params.fluid.find(':') == std::string::npos ? "minecraft:" + params.fluid : params.fluid;
            int accepted = params.amount >= 0 ? common::fluids::fill(*region, game::blockPos(pos), -1, {fluid, params.amount}, false)
                                              : common::fluids::drain(*region, game::blockPos(pos), -1, {fluid, -params.amount}, false).amount;
            auto line = tr("lv.steam.cmd.filled", "{0} mB of {1} moved at {2} {3} {4}.", accepted, fluid, pos.x, pos.y, pos.z);
            log().info("steam: {}", line);
            output.success(line);
        }
    );
});

} // namespace lv::feature::steam

LV_REGISTER_MODULE(lv::feature::steam::SteamModule);
