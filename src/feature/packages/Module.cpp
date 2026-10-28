// Packages unit (U25): block index, ticking, world lifecycle and the game hooks every player
// action goes through (GameMode: network players and SimulatedPlayers alike).

#include <algorithm>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("packages", lv::feature::packages::PackagesConfig);

namespace lv::feature::packages {

namespace items     = common::items;
namespace blockdata = common::blockdata;

PackagesConfig const& config() { return lv::config::get<PackagesConfig>(); }

Kind kindOf(std::string_view id) {
    if (id == kPackagerId) return Kind::Packager;
    if (id == kRepackagerId) return Kind::Repackager;
    if (id == kFrogportId) return Kind::Frogport;
    if (id == kChainConveyorId) return Kind::ChainConveyor;
    if (isPostboxId(id)) return Kind::Postbox;
    return Kind::None;
}

char const* kindName(Kind k) {
    switch (k) {
    case Kind::Packager: return "packager";
    case Kind::Repackager: return "repackager";
    case Kind::Frogport: return "frogport";
    case Kind::Postbox: return "postbox";
    case Kind::ChainConveyor: return "chain_conveyor";
    default: return "none";
    }
}

namespace {

constexpr char const* kIndexKey = "lv:packages:index";

struct ModuleState {
    std::map<GlobalPos, Kind>          index;
    bool                               indexDirty = false;
    bool                               loaded     = false;
    std::vector<std::function<void()>> deferred;
    std::uint64_t                      tick = 0;
    /// (dim, pos) broken by a creative player this tick (no chain drops, Create's playerWillDestroy)
    std::set<GlobalPos> creativeBroken;
    /// player -> tick of the start of a package use (throwing)
    std::unordered_map<std::int64_t, std::uint64_t> useStart;
    /// player -> (pos, tick) of the last handled click (Bedrock repeats use-on while held)
    std::unordered_map<std::int64_t, std::pair<GlobalPos, std::uint64_t>> lastClick;
};

ModuleState& S() {
    static ModuleState s;
    return s;
}

void loadIndex() {
    auto& s = S();
    if (s.loaded || !game::db::available()) return;
    s.loaded = true;
    s.index.clear();
    if (auto j = game::db::load(kIndexKey); j && j->is_array()) {
        for (auto const& e : *j) {
            if (!e.is_array() || e.size() != 5) continue;
            auto kind = static_cast<Kind>(e[0].get<int>());
            s.index[{e[1].get<int>(), e[2].get<int>(), e[3].get<int>(), e[4].get<int>()}] = kind;
        }
    }
    log().info("packages: world ready, {} logistics block(s) indexed", s.index.size());
}

void saveIndex() {
    auto& s = S();
    if (!s.indexDirty || !game::db::available()) return;
    nlohmann::json arr = nlohmann::json::array();
    for (auto const& [p, k] : s.index) arr.push_back({static_cast<int>(k), p.dim, p.x, p.y, p.z});
    game::db::save(kIndexKey, arr);
    s.indexDirty = false;
}

/// Bedrock repeats use-on packets while the button is held: one action per click.
bool debounced(Player& player, GlobalPos const& pos) {
    auto& s    = S();
    auto  key  = player.getOrCreateUniqueID().rawID;
    auto& last = s.lastClick[key];
    bool  skip = last.first == pos && s.tick - last.second < 5;
    if (!skip) last = {pos, s.tick};
    return skip;
}

void tickAll() {
    auto& s = S();
    ++s.tick;
    loadIndex();
    auto work = std::exchange(s.deferred, {});
    for (auto& fn : work) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().error("packages: deferred action failed: {}", e.what());
        }
    }
    std::vector<std::pair<GlobalPos, Kind>> list(s.index.begin(), s.index.end());
    for (auto const& [pos, kind] : list) {
        auto const* block = blockAt(pos);
        if (!block) continue; // not loaded
        if (kindOf(game::blockId(*block)) != kind) {
            untrack(pos);
            continue;
        }
        // Create's lazyTick (every 10 ticks), spread over the ticks by position
        bool lazy = ((s.tick + std::uint64_t(pos.x * 7 + pos.y * 13 + pos.z * 3)) % 10) == 0;
        try {
            switch (kind) {
            case Kind::Packager: tickPackager(pos, false, lazy); break;
            case Kind::Repackager: tickPackager(pos, true, lazy); break;
            case Kind::Frogport:
            case Kind::Postbox: tickPort(pos, kind, lazy); break;
            case Kind::ChainConveyor: tickConveyor(pos, lazy); break;
            default: break;
            }
        } catch (std::exception const& e) {
            log().error("packages: tick of {} at {} failed: {}", kindName(kind), pos.key(), e.what());
        }
    }
    tickRiders();
    if (s.tick % 4 == 0) syncConveyors();
    s.creativeBroken.clear();
}

void onBlockChanged(ll::event::BlockChangedEvent& ev) {
    if (ev.layer() != 0) return;
    auto& region = ev.blockSource();
    if (region.getLevel().isClientSide()) return;
    auto before = kindOf(game::blockId(ev.previousBlock()));
    auto after  = kindOf(game::blockId(ev.newBlock()));
    if (before == Kind::None && after == Kind::None) return;
    auto pos = game::globalPos(region, ev.pos());
    if (before != Kind::None && game::blockId(ev.previousBlock()) != game::blockId(ev.newBlock())) {
        untrack(pos);
        switch (before) {
        case Kind::Packager:
        case Kind::Repackager: forgetPackager(pos); break;
        case Kind::Frogport:
        case Kind::Postbox: forgetPort(pos); break;
        case Kind::ChainConveyor: break; // its connections are needed by the erase listener
        default: break;
        }
    }
    if (after != Kind::None && game::blockId(ev.previousBlock()) != game::blockId(ev.newBlock())) track(pos, after);
}

void onErase(GlobalPos const& pos, nlohmann::json const& doc, std::string const& previousBlockId) {
    auto kind = kindOf(previousBlockId);
    if (kind == Kind::None) return;
    auto it = doc.find("packages");
    if (it == doc.end()) return;
    nlohmann::json data     = *it;
    bool           creative = S().creativeBroken.contains(pos);
    // drops are spawned after the block change finished
    defer([pos, kind, data, creative] {
        auto drop = [&](nlohmann::json const& j, int count = 1) {
            auto stack = items::fromJson(j);
            for (int i = 0; i < count; ++i) dropItem(pos.dim, centre(pos), stack);
        };
        switch (kind) {
        case Kind::Packager:
        case Kind::Repackager:
            if (data.contains("held")) drop(data["held"]);
            for (auto const& q : data.value("queue", nlohmann::json::array())) drop(q.value("box", nlohmann::json()), q.value("count", 1));
            break;
        case Kind::Frogport:
        case Kind::Postbox:
            for (auto const& j : data.value("inventory", nlohmann::json::array())) drop(j);
            if (data.contains("animated")) drop(data["animated"]);
            break;
        case Kind::ChainConveyor: onConveyorRemoved(pos, data, creative); break;
        default: break;
        }
    });
}

// ---- hooks ---------------------------------------------------------------------------------------

bool isServerPlayer(Player const& p) { return !p.isClientSide(); }

InteractionResult consumed() {
    InteractionResult r{};
    r.mSuccess = true;
    r.mSwing   = true;
    return r;
}

/// Where a placement right-click on `at` / `face` puts a block.
GlobalPos placementPos(BlockSource& region, BlockPos const& at, uchar face) {
    auto p = game::globalPos(region, at);
    if (game::replaceable(region, at)) return p;
    return face < 6 ? offsetFace(p, face) : p;
}

/// Chain interactions (wrench, frogport, package, picking packages off a chain) under the
/// crosshair, when closer than `blockDistance`. True when handled.
bool chainInteraction(Player& player, ItemStack const& held, double blockDistance) {
    auto id       = items::idOf(held);
    bool wrench   = id == kWrenchId;
    bool frogport = id == kFrogportId;
    bool package  = isPackage(held);
    double range  = 6.0; // Create: block interaction range + 1 (survival reach 5)
    if (wrench || frogport || package) {
        if (auto pick = pickChain(player, wrench && player.isSneaking(), std::min(range, blockDistance))) {
            if (debounced(player, pick->conveyor)) return true;
            if (wrench) {
                if (!player.isSneaking()) startRiding(player, *pick);
                else if (pick->connection) disconnectConveyors(pick->conveyor, offset(pick->conveyor, *pick->connection), false, &player);
                return true;
            }
            if (frogport) {
                PendingTarget t;
                t.target.type       = PortTarget::Type::Chain;
                t.target.chainPos   = pick->chainPosition;
                t.target.connection = pick->connection;
                t.targetPos         = pick->conveyor;
                t.exact             = pick->exact;
                setPendingTarget(player, t);
                return true;
            }
            putPackageOnChain(player, *pick);
            return true;
        }
    }
    if (auto pick = pickPackage(player, std::min(range, blockDistance))) {
        if (debounced(player, pick->conveyor)) return true;
        takePackageFromChain(player, *pick);
        return true;
    }
    return false;
}

} // namespace

// ---- index ---------------------------------------------------------------------------------------

void track(GlobalPos const& pos, Kind kind) {
    auto& s = S();
    auto  it = s.index.find(pos);
    if (it != s.index.end() && it->second == kind) return;
    s.index[pos]  = kind;
    s.indexDirty  = true;
}

void untrack(GlobalPos const& pos) {
    auto& s = S();
    if (s.index.erase(pos)) s.indexDirty = true;
}

std::vector<GlobalPos> tracked(Kind kind) {
    std::vector<GlobalPos> out;
    for (auto const& [p, k] : S().index)
        if (k == kind) out.push_back(p);
    return out;
}

void          defer(std::function<void()> fn) { S().deferred.push_back(std::move(fn)); }
std::uint64_t currentTick() { return S().tick; }

LL_TYPE_INSTANCE_HOOK(
    PackagesUseItemOnHook,
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
    if (!isServerPlayer(player)) return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
    auto&        region  = player.getDimensionBlockSource();
    Block const* clicked = targetBlock ? targetBlock : &region.getBlock(at);
    auto         target  = game::globalPos(region, at);
    auto         kind    = kindOf(game::blockId(*clicked));
    auto         heldId  = items::idOf(item);
    bool         sneak   = player.isSneaking();

    try {
        // chains in front of the clicked block
        cp::V3 hitAbs{at.x + double(hit.x), at.y + double(hit.y), at.z + double(hit.z)};
        if (hit.x >= -0.01 && hit.x <= 1.01 && hit.y >= -0.01 && hit.y <= 1.01 && hit.z >= -0.01 && hit.z <= 1.01) {
        } else {
            hitAbs = {hit.x, hit.y, hit.z};
        }
        double blockDistance = eyePos(player).distanceTo(hitAbs);
        if (chainInteraction(player, item, blockDistance)) return consumed();

        // the logistics blocks themselves
        if (kind == Kind::Packager || kind == Kind::Repackager) {
            bool pass = heldId == kWrenchId || heldId == "levitate:factory_gauge" || heldId == kFrogportId
                     || (heldId == "levitate:stock_link" && !stateAt(target, kLinked).value_or(0)) || (sneak && !item.isNull());
            if (!pass) {
                if (!debounced(player, target)) usePackager(player, target);
                return consumed();
            }
        } else if (kind == Kind::Frogport || kind == Kind::Postbox) {
            if (!sneak) {
                if (!debounced(player, target)) usePort(player, target, kind);
                return consumed();
            }
        } else if (kind == Kind::ChainConveyor) {
            if (isChainItem(item)) {
                if (!debounced(player, target)) useConveyorWithChain(player, target);
                return consumed();
            }
            if (heldId == kFrogportId) {
                // Create: the frogport item selects a chain target instead of being placed on the conveyor
                if (auto pick = pickChain(player, false, 8.0); pick && !debounced(player, target)) {
                    PendingTarget t;
                    t.target.type       = PortTarget::Type::Chain;
                    t.target.chainPos   = pick->chainPosition;
                    t.target.connection = pick->connection;
                    t.targetPos         = pick->conveyor;
                    t.exact             = pick->exact;
                    setPendingTarget(player, t);
                }
                return consumed();
            }
            if (heldId == kWrenchId && sneak) refundChainsOnWrench(player, target);
        }
        if (isChainItem(item) && kind != Kind::ChainConveyor) clearChainSelection(player, true);

        // postboxes select a train station before being placed
        if (isPostboxId(heldId) && game::blockId(*clicked) == kStationId) {
            if (!debounced(player, target)) {
                PendingTarget t;
                t.target.type = PortTarget::Type::Station;
                t.targetPos   = target;
                t.exact       = centre(target);
                t.postbox     = true;
                setPendingTarget(player, t);
            }
            return consumed();
        }

        // package items: sneak opens, otherwise it is put down on the block
        if (isPackage(item)) {
            if (debounced(player, target)) return consumed();
            if (sneak) {
                openPackage(player);
            } else {
                cp::V3 at3 = hitAbs;
                if (face == 0) at3.y -= 1.0;
                dropItem(player.getDimensionId(), at3 + cp::V3{0, 0.1, 0}, items::withCount(item, 1), cp::V3{});
                if (!player.isCreative()) {
                    player.setSelectedItem(items::withCount(item, items::countOf(item) - 1));
                    player.sendInventory(false);
                }
            }
            return consumed();
        }

        // placing one of our blocks
        auto placedKind = kindOf(heldId);
        if (placedKind != Kind::None) {
            auto place = placementPos(region, at, face);
            if (placedKind == Kind::ChainConveyor && conveyorPlacementBlocked(place)) return InteractionResult{};
            std::optional<int> facing;
            if (placedKind == Kind::Packager || placedKind == Kind::Repackager) {
                facing = packagerPlacementFacing(player, place, placedKind == Kind::Repackager);
                if (!facing) return InteractionResult{};
            }
            auto before = idAt(place);
            auto result = origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
            if (idAt(place) == heldId && before != heldId) {
                track(place, placedKind);
                switch (placedKind) {
                case Kind::Packager:
                case Kind::Repackager: onPackagerPlaced(place, *facing); break;
                case Kind::Frogport:
                case Kind::Postbox: onPortPlaced(&player, place, placedKind); break;
                default: break;
                }
                if (placedKind == Kind::Postbox) {
                    setStates(place, {{kDirection, game::opposite(game::playerFacing(player))}});
                }
                log().info("packages: {} placed at {} by {}", kindName(placedKind), place.key(), player.getRealName());
            }
            return result;
        }
    } catch (std::exception const& e) {
        log().error("packages: use on block failed: {}", e.what());
    }
    return origin(item, at, face, hit, handSlot, targetBlock, isFirstEvent);
}

LL_TYPE_INSTANCE_HOOK(PackagesUseItemHook, ll::memory::HookPriority::Normal, GameMode, &GameMode::$useItem, bool, ItemStack& item, HandSlot handSlot) {
    Player& player = mPlayer;
    if (!isServerPlayer(player)) return origin(item, handSlot);
    try {
        if (chainInteraction(player, item, 1e9)) return true;
        if (isChainItem(item) && player.isSneaking()) clearChainSelection(player, true);
        if (isPackage(item)) {
            if (player.isSneaking()) {
                if (!debounced(player, GlobalPos{-1, 0, 0, 0})) openPackage(player);
                return true;
            }
            // Create: hold to charge, release to throw
            S().useStart[player.getOrCreateUniqueID().rawID] = S().tick;
            return origin(item, handSlot);
        }
        if (isPackageFilter(item)) {
            if (!debounced(player, GlobalPos{-2, 0, 0, 0})) openPackageFilterForm(player);
            return true;
        }
    } catch (std::exception const& e) {
        log().error("packages: use item failed: {}", e.what());
    }
    return origin(item, handSlot);
}

LL_TYPE_INSTANCE_HOOK(PackagesReleaseHook, ll::memory::HookPriority::Normal, Player, &Player::releaseUsingItem, void) {
    if (!isClientSide()) {
        auto key = getOrCreateUniqueID().rawID;
        auto it  = S().useStart.find(key);
        if (it != S().useStart.end()) {
            int held = int(S().tick - it->second);
            S().useStart.erase(it);
            if (isPackage(getSelectedItem())) {
                try {
                    throwPackage(*this, held);
                } catch (std::exception const& e) {
                    log().error("packages: throw failed: {}", e.what());
                }
            }
        }
    }
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    PackagesDestroyBlockHook,
    ll::memory::HookPriority::Normal,
    GameMode,
    &GameMode::$destroyBlock,
    bool,
    BlockPos const& pos,
    uchar           face
) {
    Player& player = mPlayer;
    if (isServerPlayer(player) && player.isCreative()) S().creativeBroken.insert(game::globalPos(player.getDimensionBlockSource(), pos));
    return origin(pos, face);
}

class PackagesModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tickAll(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            try {
                onBlockChanged(ev);
            } catch (...) {
            }
        }));
        PackagesUseItemOnHook::hook();
        PackagesUseItemHook::hook();
        PackagesReleaseHook::hook();
        PackagesDestroyBlockHook::hook();
        installPackagerCapabilities();
        installPortCapabilities();
        installPortScreens();
        installItemScreens();
        installConveyorKinetics();
        static bool eraseListener = false;
        if (!std::exchange(eraseListener, true)) blockdata::onErase(onErase);
        if (game::db::available()) loadIndex();
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        PackagesDestroyBlockHook::unhook();
        PackagesReleaseHook::unhook();
        PackagesUseItemHook::unhook();
        PackagesUseItemOnHook::unhook();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

namespace {
struct Lifecycle {
    Lifecycle() {
        game::db::onWorldReady([] {
            S().loaded = false;
            loadIndex();
        });
        game::db::onWorldSave([] {
            for (auto const& p : tracked(Kind::ChainConveyor))
                if (isLoaded(p)) saveConveyor(p);
            saveIndex();
        });
        game::db::onWorldClosed([] {
            auto& s = S();
            s.index.clear();
            s.loaded     = false;
            s.indexDirty = false;
            s.deferred.clear();
            s.useStart.clear();
            s.lastClick.clear();
            resetState();
        });
    }
};
Lifecycle gLifecycle;
} // namespace

} // namespace lv::feature::packages

LV_REGISTER_MODULE(lv::feature::packages::PackagesModule);
