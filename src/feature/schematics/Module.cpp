// Schematics unit: player interaction (GameMode hooks, so network players and SimulatedPlayers
// behave alike), block removal, ticking, world lifecycle and `/create schematic ...`.

#include "ll/api/command/CommandHandle.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

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

std::map<std::int64_t, std::uint64_t> gLastUse;
bool                                  gNoDrop = false;

/// One tool action per 4 ticks per player (Bedrock repeats use while the button is held and may
/// send a click on air after a click on a block).
bool debounce(Player& p) {
    auto  uid  = uidOf(p);
    auto& last = gLastUse[uid];
    if (last && currentTick() - last < 5) return false;
    last = currentTick();
    return true;
}

InteractionResult used() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

bool isOurItem(std::string const& id) { return id == kQuill || id == kSchematic || id == kBlueprintItem; }

} // namespace

} // namespace lv::feature::schematics

using namespace lv::feature::schematics;

LL_TYPE_INSTANCE_HOOK(
    SchematicsUseItemOnHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$useItemOn,
    InteractionResult,
    ItemStack&      item,
    BlockPos const& at,
    uchar           face,
    Vec3 const&     hit,
    HandSlot        handSlot,
    Block const*    targetBlock,
    bool            isFirstEvent
) {
    Player& player = mPlayer;
    if (player.isClientSide()) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto&        region = player.getDimensionBlockSource();
    Block const& target = targetBlock ? *targetBlock : region.getBlock(at);
    std::string  tid    = target.getTypeName();
    std::string  held   = item.isNull() ? std::string() : std::string(item.getTypeName());
    GlobalPos    p      = lv::game::globalPos(region, at);
    bool         sneakWithItem = player.isSneaking() && !item.isNull();
    try {
        if (tid == kTable && !sneakWithItem) {
            if (debounce(player)) openTable(player, p);
            return used();
        }
        if (tid == kCannon && !sneakWithItem) {
            if (debounce(player)) openCannon(player, p);
            return used();
        }
        if (tid == kBlueprintBlock && held != kBlueprintItem) {
            if (debounce(player)) blueprintUse(player, p, held == kWrench);
            return used();
        }
        if (isOurItem(held)) {
            if (!debounce(player)) return used();
            std::pair<cs::Pos, int> h{cp(p), static_cast<int>(face)};
            if (held == kQuill) quillUse(player, h);
            else if (held == kSchematic) schematicUse(player, h);
            else if (held == kBlueprintItem) {
                if (placeBlueprint(player, p, static_cast<int>(face)) && !player.isCreative()) {
                    item.remove(1);
                    player.sendInventory(false);
                }
            }
            return used();
        }
    } catch (std::exception const& e) {
        lv::log().error("schematics: use failed: {}", e.what());
        return used();
    }
    if (held != kTable) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    // the table faces the player (Create: horizontal facing opposite to the player's)
    static constexpr std::array<lv::core::schematics::Pos, 6> kN{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};
    GlobalPos placed = lv::game::replaceable(region, at) ? p : gp(p.dim, cp(p) + kN[std::min<int>(face, 5)]);
    std::string before = idAt(placed);
    auto        result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    if (before != kTable && idAt(placed) == kTable) placeId(placed, kTable, {{kFacing, lv::game::opposite(lv::game::playerFacing(player))}});
    return result;
}

LL_TYPE_INSTANCE_HOOK(SchematicsUseItemHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$useItem, bool, ItemStack& item, HandSlot handSlot) {
    Player& player = mPlayer;
    if (player.isClientSide() || item.isNull()) return origin(item, handSlot);
    std::string held = item.getTypeName();
    if (held != kQuill && held != kSchematic) return origin(item, handSlot);
    if (!debounce(player)) return true;
    try {
        if (held == kQuill) quillUse(player, std::nullopt);
        else schematicUse(player, std::nullopt);
    } catch (std::exception const& e) {
        lv::log().error("schematics: use failed: {}", e.what());
    }
    return true;
}

LL_TYPE_INSTANCE_HOOK(SchematicsDestroyBlockHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$destroyBlock, bool, BlockPos const& pos, uchar face) {
    Player& player = mPlayer;
    if (player.isClientSide()) return origin(pos, face);
    auto&     region = player.getDimensionBlockSource();
    GlobalPos p      = lv::game::globalPos(region, pos);
    if (region.getBlock(pos).getTypeName() != kBlueprintBlock) return origin(pos, face);
    // Create: hitting a configured blueprint section clears it instead of breaking the blueprint
    if (blueprintAttack(player, p)) {
        if (auto const* b = blockAt(p)) place(p, *b, 3);
        return false;
    }
    gNoDrop = player.isCreative();
    bool ok = origin(pos, face);
    gNoDrop = false;
    return ok;
}

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
                else if (prev == kCannon) cannonRemoved(p, d);
                else if (prev == kBlueprintBlock && !blueprintSilent()) {
                    bool drop = !gNoDrop;
                    defer([p, d, drop] { blueprintRemoved(p, d, drop); });
                }
            });
        }
        SchematicsUseItemOnHook::hook();
        SchematicsUseItemHook::hook();
        SchematicsDestroyBlockHook::hook();
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
        SchematicsDestroyBlockHook::unhook();
        SchematicsUseItemHook::unhook();
        SchematicsUseItemOnHook::unhook();
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
