// Schematics unit: player interaction (GameMode hooks, so network players and SimulatedPlayers
// behave alike), block removal, ticking, world lifecycle and `/create schematic ...`.

#include <set>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/schematics/Schematics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::schematics {

struct SchematicGiveParams {
    std::string file;
};

namespace detail {
void runDeferred();
void clearDeferred();
void registerQuill();
void registerTable();
void registerPlacement();
void registerCannon();
void registerBlueprint();
} // namespace detail

namespace {

namespace items = common::items;
namespace ia    = common::interactions;

std::map<std::int64_t, std::uint64_t> gLastUse;
std::set<GlobalPos>                   gNoDropAt; ///< blueprints a creative player is breaking right now

/// One tool action per 4 ticks per player (Bedrock repeats use while the button is held and may
/// send a click on air after a click on a block).
bool debounce(Player& p) {
    auto  uid  = uidOf(p);
    auto& last = gLastUse[uid];
    if (last && currentTick() - last < 5) return false;
    last = currentTick();
    return true;
}

bool isOurItem(std::string const& id) { return id == kQuill || id == kSchematic || id == kBlueprintItem; }

} // namespace

} // namespace lv::feature::schematics

using namespace lv::feature::schematics;

// ---- interactions (U36: on the shared dispatch) --------------------------------------------------

namespace {

/// Right-clicks on the unit's blocks and with its items.
ia::Result schematicsUseOn(ia::UseOnContext const& c) {
    Player&            player        = c.player;
    std::string const& tid           = c.blockId;
    std::string const& held          = c.itemId;
    GlobalPos          p             = c.pos;
    bool               sneakWithItem = c.sneaking && !c.item.isNull();
    try {
        if (tid == kTable && !sneakWithItem) {
            if (debounce(player)) openTable(player, p);
            return ia::Result::Consume;
        }
        if (tid == kCannon && !sneakWithItem) {
            if (debounce(player)) openCannon(player, p);
            return ia::Result::Consume;
        }
        if (tid == kBlueprintBlock && held != kBlueprintItem) {
            if (debounce(player)) blueprintUse(player, p, held == kWrench);
            return ia::Result::Consume;
        }
        if (isOurItem(held)) {
            if (!debounce(player)) return ia::Result::Consume;
            std::pair<cs::Pos, int> h{cp(p), c.face};
            if (held == kQuill) quillUse(player, h);
            else if (held == kSchematic) schematicUse(player, h);
            else if (held == kBlueprintItem) {
                if (placeBlueprint(player, p, c.face) && !player.isCreative()) {
                    // the held stack is the player's own (non-const) inventory slot
                    const_cast<ItemStack&>(c.item).remove(1);
                    player.sendInventory(false);
                }
            }
            return ia::Result::Consume;
        }
    } catch (std::exception const& e) {
        lv::log().error("schematics: use failed: {}", e.what());
        return ia::Result::Consume;
    }
    return ia::Result::Pass;
}

/// The table faces the player (Create: horizontal facing opposite to the player's).
void schematicsPlaced(ia::PlaceContext const& c) {
    if (c.itemId == kCannon && c.blockId == kCannon) {
        cannonPlaced(c.pos, c.player.getRotation().y, c.sneaking, c.itemBefore);
        return;
    }
    if (c.itemId != kTable) return;
    placeId(c.pos, kTable, {{kFacing, lv::game::opposite(lv::game::playerFacing(c.player))}});
}

/// Quill and schematic used in the air.
ia::Result schematicsUse(ia::UseContext const& c) {
    std::string const& held = c.itemId;
    if (held != kQuill && held != kSchematic) return ia::Result::Pass;
    if (!debounce(c.player)) return ia::Result::Consume;
    try {
        if (held == kQuill) quillUse(c.player, std::nullopt);
        else schematicUse(c.player, std::nullopt);
    } catch (std::exception const& e) {
        lv::log().error("schematics: use failed: {}", e.what());
    }
    return ia::Result::Consume;
}

/// Breaking a blueprint: hitting a configured section clears it instead (Create), creative
/// players break it without drops.
ia::Result schematicsDestroy(ia::DestroyContext const& c) {
    GlobalPos p = c.pos;
    if (c.blockId == kBlueprintBlock && blueprintAttack(c.player, p)) {
        if (auto const* b = blockAt(p)) place(p, *b, 3);
        return ia::Result::Deny;
    }
    if (c.player.isCreative()) {
        gNoDropAt.insert(p); // read by the erase listener while vanilla breaks the block
        ia::nextTick([p] { gNoDropAt.erase(p); });
    }
    return ia::Result::Pass;
}

} // namespace

namespace lv::feature::schematics {

namespace {

class SchematicsModule {
public:
    void install() {
        static bool registered = false;
        if (!registered) {
            registered = true;
            detail::registerQuill();
            detail::registerTable();
            detail::registerPlacement();
            detail::registerCannon();
            detail::registerBlueprint();
            game::db::onWorldReady([] { loadCannons(); });
            game::db::onWorldClosed([] {
                detail::clearDeferred();
                resetCannons();
                resetQuills();
                resetUploads();
                clearSchematicCache();
                clearBlockCaches();
                gLastUse.clear();
            });
            common::blockdata::onErase([](GlobalPos const& p, Json const& doc, std::string const& prev) {
                if (prev.empty() || !doc.contains("schematics")) return;
                Json d = doc["schematics"];
                if (prev == kTable) tableRemoved(p, d);
                else if (prev == kCannon) {
                    cannonRemoved(p, d);
                    defer([p, o = cannonOptions(d)] { tagDroppedCannon(p, o); });
                }
                else if (prev == kBlueprintBlock && !blueprintSilent()) {
                    bool drop = gNoDropAt.erase(p) == 0;
                    defer([p, d, drop] { blueprintRemoved(p, d, drop); });
                }
            });
        }
        mHandlers.push_back(ia::onUseOn({}, schematicsUseOn));
        mHandlers.push_back(ia::onPlace({std::string(kTable), std::string(kTable)}, schematicsPlaced));
        mHandlers.push_back(ia::onPlace({std::string(kCannon), std::string(kCannon)}, schematicsPlaced));
        mHandlers.push_back(ia::onUse({}, schematicsUse));
        mHandlers.push_back(ia::onDestroy({std::string(kBlueprintBlock)}, schematicsDestroy));

        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([](ll::event::PlayerDisconnectEvent& ev) {
            gLastUse.erase(uidOf(ev.self()));
        }));
        if (game::db::available()) loadCannons();
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
    }

private:
    static void tick() {
        try {
            detail::runDeferred();
            tickCannons();
            tickUploads();
            tickQuillOutlines();
            tickPlacementOutlines();
        } catch (std::exception const& e) {
            log().error("schematics: tick failed: {}", e.what());
        }
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    std::vector<ia::HandlerId>          mHandlers;
};

} // namespace

} // namespace lv::feature::schematics

LV_REGISTER_MODULE(lv::feature::schematics::SchematicsModule);

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    using namespace lv::feature::schematics;
    using Origin = CommandOrigin const;
    // `/create schematic list`: the caller's stored schematics
    cmd.overload().text("schematic").text("list").execute([](Origin& origin, CommandOutput& output) {
        auto* p = lv::commands::playerOf(origin);
        if (!p) return output.error(lv::tr("lv.schematics.need_player", "Only players can use this."));
        auto files = listFiles(playerDir(p->getRealName()));
        if (files.empty()) return output.success(lv::trFor(*p, "create.gui.schematicTable.noSchematics", "No Schematics Saved"));
        std::string list;
        for (auto const& f : files) list += (list.empty() ? "" : ", ") + f;
        output.success(lv::trFor(*p, "create.gui.schematicTable.availableSchematics", "Available Schematics") + ": " + list);
    });
    // `/create schematic give <file>` (op): a written schematic of one of the caller's files
    cmd.overload<SchematicGiveParams>().text("schematic").text("give").required("file").execute([](Origin& origin, CommandOutput& output, SchematicGiveParams const& params) {
        auto* p = lv::commands::playerOf(origin);
        if (!p) return output.error(lv::tr("lv.schematics.need_player", "Only players can use this."));
        if (!lv::commands::isAdmin(origin)) return output.error(lv::tr("lv.contraption.need_op", "You need operator permission."));
        std::string err;
        if (!loadSchematic(p->getRealName(), params.file, &err)) return output.error(lv::trFor(*p, "create.schematic.error", "Schematic failed to Load - Check Game Logs") + " (" + err + ")");
        auto item = makeSchematic(params.file, p->getRealName());
        if (!item) return output.error("?");
        give(*p, *item);
        reportUnmapped(*p, p->getRealName(), params.file);
        lv::log().info("schematics: gave {} a schematic of {}", p->getRealName(), params.file);
        output.success(lv::trFor(*p, "lv.schematics.given", "Gave a schematic of {0}", params.file));
    });
});
