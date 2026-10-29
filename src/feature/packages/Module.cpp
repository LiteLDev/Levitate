// Packages unit (U25): block index, ticking, world lifecycle and the player interactions
// (common/Interactions.h: network players and SimulatedPlayers alike; network players' block-item
// placements also arrive as PlayerPlacingBlockEvent / PlayerPlacedBlockEvent).

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
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
    /// positions placed (and set up) through the useItemOn hook this tick
    std::set<GlobalPos> placedByHook;
    /// positions set up by PlayerPlacedBlockEvent this tick (it fires inside the hook's origin call)
    std::set<GlobalPos> placedByEvent;
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
    s.placedByHook.clear();
    s.placedByEvent.clear();
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

// ---- interactions (common/Interactions.h) --------------------------------------------------------

namespace ia = common::interactions;

namespace {

/// Packager facing worked out before the placement, by placement position.
std::map<GlobalPos, std::optional<int>>& pendingFacing() {
    static std::map<GlobalPos, std::optional<int>> m;
    return m;
}

cp::V3 absoluteHit(ia::UseOnContext const& c) {
    return {c.pos.x + c.hit[0], c.pos.y + c.hit[1], c.pos.z + c.hit[2]};
}

ia::Result packagesUseOn(ia::UseOnContext const& c) {
    auto&       player = c.player;
    auto&       item   = ia::mutableItem(c.item);
    auto&       region = player.getDimensionBlockSource();
    BlockPos    at     = game::blockPos(c.pos);
    auto const& target = c.pos;
    auto        kind   = kindOf(game::blockId(c.block));
    auto const& heldId = c.itemId;
    bool        sneak  = c.sneaking;
    int         face   = c.face;
    // the caller writes `item` back into the hand slot: keep it in sync with what we changed
    auto done = [&] {
        item = player.getSelectedItem();
        return ia::Result::Consume;
    };

    try {
        // chains in front of the clicked block
        cp::V3 hitAbs        = absoluteHit(c);
        double blockDistance = eyePos(player).distanceTo(hitAbs);
        if (chainInteraction(player, item, blockDistance)) return done();

        // the logistics blocks themselves
        if (kind == Kind::Packager || kind == Kind::Repackager) {
            bool pass = heldId == kWrenchId || heldId == "levitate:factory_gauge" || heldId == kFrogportId
                     || (heldId == "levitate:stock_link" && !stateAt(target, kLinked).value_or(0)) || (sneak && !item.isNull());
            if (!pass) {
                if (!debounced(player, target)) usePackager(player, target);
                return done();
            }
        } else if (kind == Kind::Frogport || kind == Kind::Postbox) {
            if (!sneak) {
                if (!debounced(player, target)) usePort(player, target, kind);
                return done();
            }
        } else if (kind == Kind::ChainConveyor) {
            if (isChainItem(item)) {
                if (!debounced(player, target)) useConveyorWithChain(player, target);
                return done();
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
                return done();
            }
            if (heldId == kWrenchId && sneak) refundChainsOnWrench(player, target);
        }
        if (isChainItem(item) && kind != Kind::ChainConveyor) clearChainSelection(player, true);

        // postboxes select a train station before being placed
        if (isPostboxId(heldId) && game::blockId(c.block) == kStationId) {
            if (!debounced(player, target)) {
                PendingTarget t;
                t.target.type = PortTarget::Type::Station;
                t.targetPos   = target;
                t.exact       = centre(target);
                t.postbox     = true;
                setPendingTarget(player, t);
            }
            return done();
        }

        // package items: sneak opens, otherwise it is put down on the block (onUnusedUseOn below)
        if (isPackage(item)) {
            if (sneak) {
                // Java skips the block's use while sneaking with an item: the package opens
                if (!debounced(player, target)) openPackage(player);
                return done();
            }
            return ia::Result::Pass;
        }

        // placing one of our blocks (oriented in packagesPlaced)
        auto placedKind = kindOf(heldId);
        if (placedKind != Kind::None) {
            auto place = placementPos(region, at, static_cast<uchar>(face));
            if (placedKind == Kind::ChainConveyor && conveyorPlacementBlocked(place)) return ia::Result::Deny;
            std::optional<int> facing;
            if (placedKind == Kind::Packager || placedKind == Kind::Repackager) {
                facing = packagerPlacementFacing(player, place, placedKind == Kind::Repackager);
                if (!facing) return ia::Result::Deny;
            }
            pendingFacing()[place] = facing;
        }
    } catch (std::exception const& e) {
        log().error("packages: use on block failed: {}", e.what());
    }
    return ia::Result::Pass;
}

/// Create's PackageItem.useOn: when the clicked block passes, the package is put down on it.
ia::Result packagesPutDown(ia::UseOnContext const& c) {
    auto& player = c.player;
    auto& item   = ia::mutableItem(c.item);
    if (c.sneaking || !isPackage(item) || debounced(player, c.pos)) return ia::Result::Pass;
    try {
        cp::V3 at3 = absoluteHit(c);
        if (c.face == 0) at3.y -= 1.0;
        dropItem(player.getDimensionId(), at3 + cp::V3{0, 0.1, 0}, items::withCount(item, 1), cp::V3{});
        if (!player.isCreative()) {
            player.setSelectedItem(items::withCount(item, items::countOf(item) - 1));
            player.sendInventory(false);
        }
        item = player.getSelectedItem();
        return ia::Result::Consume;
    } catch (std::exception const& e) {
        log().error("packages: use on block failed: {}", e.what());
    }
    return ia::Result::Pass;
}

/// After the game placed one of our blocks from a click (network players' placements arrive
/// through PlayerPlacedBlockEvent instead, see onPlaced).
void packagesPlaced(ia::PlaceContext const& c) {
    auto& pending = pendingFacing();
    auto  it      = pending.find(c.pos);
    std::optional<int> facing;
    if (it != pending.end()) {
        facing = it->second;
        pending.erase(it);
    }
    auto placedKind = kindOf(c.itemId);
    if (placedKind == Kind::None || c.blockId != c.itemId || S().placedByEvent.contains(c.pos)) return;
    try {
        auto& player = c.player;
        auto  place  = c.pos;
        S().placedByHook.insert(place);
        track(place, placedKind);
        switch (placedKind) {
        case Kind::Packager:
        case Kind::Repackager:
            if (facing) onPackagerPlaced(place, *facing);
            break;
        case Kind::Frogport:
        case Kind::Postbox: onPortPlaced(&player, place, placedKind); break;
        default: break;
        }
        if (placedKind == Kind::Postbox) setStates(place, {{kDirection, game::opposite(game::playerFacing(player))}});
        log().info("packages: {} placed at {} by {}", kindName(placedKind), place.key(), player.getRealName());
    } catch (std::exception const& e) {
        log().error("packages: placement failed: {}", e.what());
    }
}

ia::Result packagesUse(ia::UseContext const& c) {
    if (c.onBlock) return ia::Result::Pass; // block clicks are the use-on's
    auto& player = c.player;
    auto& item   = ia::mutableItem(c.item);
    try {
        if (chainInteraction(player, item, 1e9)) {
            item = player.getSelectedItem();
            return ia::Result::Consume;
        }
        if (isChainItem(item) && c.sneaking) clearChainSelection(player, true);
        if (isPackage(item)) {
            if (c.sneaking) {
                if (!debounced(player, GlobalPos{-1, 0, 0, 0})) openPackage(player);
                item = player.getSelectedItem();
                return ia::Result::Consume;
            }
            // Create: hold to charge, release to throw
            S().useStart[player.getOrCreateUniqueID().rawID] = S().tick;
            return ia::Result::Pass;
        }
        if (isPackageFilter(item)) {
            if (!debounced(player, GlobalPos{-2, 0, 0, 0})) openPackageFilterScreen(player);
            return ia::Result::Consume;
        }
    } catch (std::exception const& e) {
        log().error("packages: use item failed: {}", e.what());
    }
    return ia::Result::Pass;
}

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

void installInteractions() {
    auto& ids = handlerIds();
    ids.push_back(ia::onUseOn({}, packagesUseOn));
    ids.push_back(ia::onUnusedUseOn({}, packagesPutDown));
    ids.push_back(ia::onPlace({}, packagesPlaced));
    ids.push_back(ia::onUse({}, packagesUse));
    ids.push_back(ia::onDestroy({}, [](ia::DestroyContext const& c) {
        if (c.player.isCreative()) S().creativeBroken.insert(c.pos);
        return ia::Result::Pass;
    }));
    static bool registered = false;
    if (!registered) {
        registered = true;
        game::db::onWorldClosed([] { pendingFacing().clear(); }); // the client loads many worlds
    }
}

void uninstallInteractions() {
    for (auto id : handlerIds()) ia::remove(id);
    handlerIds().clear();
    pendingFacing().clear();
}

} // namespace

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


namespace {

/// Network players' block-item clicks (chains, frogports, postboxes) reach the server as block
/// placements, not through GameMode::useItemOn: handle the clicks on our blocks here.
void onPlacing(ll::event::PlayerPlacingBlockEvent& ev) {
    Player& player = ev.self();
    if (!isServerPlayer(player)) return;
    auto const& held   = player.getSelectedItem();
    auto        heldId = items::idOf(held);
    auto&       region = player.getDimensionBlockSource();
    auto        placed = game::globalPos(region, ev.pos());
    int         face   = std::clamp<int>(ev.face(), 0, 5);
    // the clicked block: the placement position itself (replaceable target) or the one behind it
    GlobalPos clicked = kindOf(idAt(placed)) != Kind::None ? placed : offsetFace(placed, oppositeFace(face));
    auto      kind    = kindOf(idAt(clicked));
    if (kind == Kind::ChainConveyor && isChainItem(held)) {
        ev.cancel();
        if (!debounced(player, clicked)) {
            auto uid = player.getOrCreateUniqueID().rawID;
            defer([uid, clicked] {
                if (auto* p = playerById(uid)) useConveyorWithChain(*p, clicked);
            });
        }
        return;
    }
    if (kind == Kind::ChainConveyor && heldId == kFrogportId) {
        ev.cancel();
        if (auto pick = pickChain(player, false, 8.0); pick && !debounced(player, clicked)) {
            PendingTarget t;
            t.target.type       = PortTarget::Type::Chain;
            t.target.chainPos   = pick->chainPosition;
            t.target.connection = pick->connection;
            t.targetPos         = pick->conveyor;
            t.exact             = pick->exact;
            setPendingTarget(player, t);
        }
        return;
    }
    if (isPostboxId(heldId) && idAt(clicked) == kStationId) {
        ev.cancel();
        if (!debounced(player, clicked)) {
            PendingTarget t;
            t.target.type = PortTarget::Type::Station;
            t.targetPos   = clicked;
            t.exact       = centre(clicked);
            t.postbox     = true;
            setPendingTarget(player, t);
        }
        return;
    }
    if (kind == Kind::Packager || kind == Kind::Repackager || ((kind == Kind::Frogport || kind == Kind::Postbox) && !player.isSneaking())) {
        // clicking our machines with a block uses them (Create), sneaking places
        if (kind == Kind::Packager || kind == Kind::Repackager) {
            if (player.isSneaking() || heldId == kFrogportId) return;
            ev.cancel();
            if (!debounced(player, clicked)) {
                auto uid = player.getOrCreateUniqueID().rawID;
                defer([uid, clicked] {
                    if (auto* p = playerById(uid); p && isPackager(clicked)) usePackager(*p, clicked);
                });
            }
        } else {
            ev.cancel();
            if (!debounced(player, clicked)) {
                auto uid = player.getOrCreateUniqueID().rawID;
                defer([uid, clicked, kind] {
                    if (auto* p = playerById(uid); p && kindOf(idAt(clicked)) == kind) usePort(*p, clicked, kind);
                });
            }
        }
        return;
    }
    auto placedKind = kindOf(heldId);
    if (placedKind == Kind::ChainConveyor && conveyorPlacementBlocked(placed)) ev.cancel();
    if ((placedKind == Kind::Packager || placedKind == Kind::Repackager) && !packagerPlacementFacing(player, placed, false)) ev.cancel();
}

void onPlaced(ll::event::PlayerPlacedBlockEvent& ev) {
    Player& player = ev.self();
    if (!isServerPlayer(player)) return;
    auto kind = kindOf(game::blockId(ev.placedBlock()));
    if (kind == Kind::None) return;
    auto pos = game::globalPos(player.getDimensionBlockSource(), ev.pos());
    if (S().placedByHook.contains(pos)) return; // already set up by the useItemOn hook
    S().placedByEvent.insert(pos);
    track(pos, kind);
    switch (kind) {
    case Kind::Packager:
    case Kind::Repackager:
        if (auto f = packagerPlacementFacing(player, pos, kind == Kind::Repackager)) onPackagerPlaced(pos, *f);
        break;
    case Kind::Frogport: onPortPlaced(&player, pos, kind); break;
    case Kind::Postbox:
        onPortPlaced(&player, pos, kind);
        setStates(pos, {{kDirection, game::opposite(game::playerFacing(player))}});
        break;
    default: break;
    }
    log().info("packages: {} placed at {} by {}", kindName(kind), pos.key(), player.getRealName());
}

} // namespace

class PackagesModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerPlacingBlockEvent>([](ll::event::PlayerPlacingBlockEvent& ev) {
            try {
                onPlacing(ev);
            } catch (std::exception const& e) {
                log().error("packages: placing failed: {}", e.what());
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerPlacedBlockEvent>([](ll::event::PlayerPlacedBlockEvent& ev) {
            try {
                onPlaced(ev);
            } catch (std::exception const& e) {
                log().error("packages: placed failed: {}", e.what());
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tickAll(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            try {
                onBlockChanged(ev);
            } catch (...) {
            }
        }));
        installInteractions();
        PackagesReleaseHook::hook();
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
        PackagesReleaseHook::unhook();
        uninstallInteractions();
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
