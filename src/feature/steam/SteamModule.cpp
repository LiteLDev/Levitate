// Steam unit: module (events, world lifecycle, game hooks), the powered shaft's kinetic spec,
// the client sync of tank contents / engines, the query API and `/create steam <pos>`.

#include <climits>

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
    if (gTankIndexDirty && currentTick() % 20 == 0) saveTankIndex();
}

void sendFullSync(Player& player) {
    std::map<int, Json> sets;
    for (auto const& [p, j] : lastSent())
        if (j.is_object()) sets[p.dim].push_back(j);
    net::send(player, "steam.tanks", Json{{"reset", true}, {"dim", 0}, {"set", Json::array()}, {"del", Json::array()}});
    for (auto const& [d, list] : sets) net::send(player, "steam.tanks", Json{{"dim", d}, {"set", list}, {"del", Json::array()}});
    net::send(player, "steam.engines", enginesPayload());
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
                    bool container = !common::fluids::fluidInItem(item).empty() || heldId == "minecraft:bucket"
                                  || heldId == "minecraft:glass_bottle";
                    if (container && (busy(player) || useItemOnTank(player, pos, item))) {
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
            if (useOnWhistle(player, pos, item, sneak)) {
                player.sendInventory(false);
                return ia::Result::Consume;
            }
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
