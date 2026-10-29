// Bulk unit module: tracks the unit's blocks (world DB index), ticks them, orients placed blocks
// as Create's getStateForPlacement, the millstone's right-click and the cleanup on removal.
//
// Player actions come from the shared interaction dispatch (common/Interactions.h);
// placements and removals of any cause come from the BlockChangedEvent.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/bulk/Bulk.h"
#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::bulk {

namespace k = core::kinetics;

namespace {

constexpr char kDbIndex[] = "lv:bulk:index";

/// How the player placed a block (Create's BlockPlaceContext).
struct PlaceIntent {
    cb::Vec3d     look;
    bool          sneaking = false;
    int           face     = 1; ///< clicked face
    std::uint64_t tick     = 0;
};

struct State {
    std::map<GlobalPos, Kind>                    tracked;
    bool                                         dirty = false;
    std::vector<std::function<void()>>           deferred;
    std::map<GlobalPos, PlaceIntent>             intents;
    std::unordered_map<std::int64_t, std::uint64_t> lastUse;
    std::uint64_t                                tick = 0;
};
State& state() {
    static State s;
    return s;
}

void saveIndex() {
    auto& s = state();
    if (!s.dirty || !game::db::available()) return;
    Json j = Json::array();
    for (auto const& [p, kind] : s.tracked) j.push_back({int(kind), p.dim, p.x, p.y, p.z});
    game::db::save(kDbIndex, j);
    s.dirty = false;
}

void loadIndex() {
    auto& s = state();
    s.tracked.clear();
    s.dirty = false;
    if (auto j = game::db::load(kDbIndex); j && j->is_array()) {
        for (auto const& e : *j) {
            if (!e.is_array() || e.size() != 5) continue;
            s.tracked[GlobalPos{e[1].get<int>(), e[2].get<int>(), e[3].get<int>(), e[4].get<int>()}] = static_cast<Kind>(e[0].get<int>());
        }
    }
    log().info("bulk: tracking {} machine block(s)", s.tracked.size());
}

void resetWorld() {
    auto& s = state();
    s.tracked.clear();
    s.deferred.clear();
    s.intents.clear();
    s.lastUse.clear();
    s.dirty = false;
    clearFans();
    clearCrushers();
}

/// Create's nearest looking direction.
cb::Face nearestLooking(cb::Vec3d const& look) {
    cb::Face best  = cb::Face::North;
    double   score = -2;
    for (auto f : k::kFaces) {
        auto   n = cb::normalOf(f);
        double s = n.x * look.x + n.y * look.y + n.z * look.z;
        if (s > score) {
            score = s;
            best  = f;
        }
    }
    return best;
}

/// The side whose neighbour has a shaft towards `p` (Create's getPreferredFacing; nullopt when
/// none or ambiguous across axes).
std::optional<cb::Face> preferredFacing(BlockSource& region, int dim, BlockPos const& p) {
    std::optional<cb::Face> preferred;
    for (auto side : k::kFaces) {
        auto n    = offset(p, side);
        auto node = kinetics::nodeAt(toGlobal(dim, n));
        if (!node) continue;
        auto const* spec = kinetics::specOf(node->state.id);
        if (!spec || !spec->shaftTowards(node->state, k::opposite(side))) continue;
        if (preferred && k::axisOf(*preferred) != k::axisOf(side)) return std::nullopt;
        preferred = side;
    }
    return preferred;
}

/// Create's getStateForPlacement of the fan, nozzle and crushing wheel, then their survival checks.
void onPlaced(int dim, BlockPos p, Kind kind) {
    auto* region = regionOf(dim);
    if (!region || !loaded(*region, p)) return;
    auto gp     = toGlobal(dim, p);
    auto intent = state().intents.find(gp);
    bool fresh  = intent != state().intents.end() && state().tick - intent->second.tick < 10;
    switch (kind) {
    case Kind::Fan: {
        if (!fresh) break;
        auto const& in      = intent->second;
        auto        face    = faceOf(in.face);
        auto        placedOn = idAt(*region, offset(p, k::opposite(face)));
        auto        opposite = idAt(*region, offset(p, face));
        auto        isChute  = [](std::string const& id) { return id == "levitate:chute" || id == "levitate:smart_chute"; };
        cb::Face    facing;
        if (isChute(placedOn)) facing = k::opposite(face);
        else if (isChute(opposite)) facing = face;
        else {
            auto pref = preferredFacing(*region, dim, p);
            auto dir  = pref ? *pref : nearestLooking(in.look);
            facing    = in.sneaking ? dir : k::opposite(dir);
        }
        setStates(*region, p, {{"levitate:facing", int(facing)}});
        markFanDirty(gp);
        break;
    }
    case Kind::Nozzle: {
        if (fresh) setStates(*region, p, {{"levitate:facing", intent->second.face}});
        if (!nozzleCanSurvive(*region, p)) {
            log().info("nozzle at {} {} {}: needs an encased fan facing the same way behind it, dropped", p.x, p.y, p.z);
            breakBlock(*region, p);
        }
        break;
    }
    case Kind::Wheel: {
        if (fresh) {
            auto const&           in = intent->second;
            std::optional<cb::Axis> preferred;
            bool                  conflict = false;
            for (auto side : k::kFaces) {
                auto node = kinetics::nodeAt(toGlobal(dim, offset(p, side)));
                if (!node) continue;
                auto const* spec = kinetics::specOf(node->state.id);
                if (!spec || !spec->shaftTowards(node->state, k::opposite(side))) continue;
                if (preferred && *preferred != k::axisOf(side)) conflict = true;
                preferred = k::axisOf(side);
            }
            if (conflict) preferred.reset();
            cb::Axis axis;
            if (preferred && !in.sneaking) axis = *preferred;
            else axis = preferred && in.sneaking ? k::axisOf(faceOf(in.face)) : k::axisOf(nearestLooking(in.look));
            setStates(*region, p, {{"levitate:axis", int(axis)}});
        }
        // Create checks this when a player places the wheel (commands bypass it)
        if (fresh && !wheelCanSurvive(*region, p)) {
            log().info("crushing wheel at {} {} {}: no room next to another wheel / controller, dropped", p.x, p.y, p.z);
            breakBlock(*region, p);
            break;
        }
        fixControllers(*region, gp);
        break;
    }
    default:
        break;
    }
    if (intent != state().intents.end()) state().intents.erase(intent);
}

void onRemoved(int dim, BlockPos p, Kind kind) {
    auto* region = regionOf(dim);
    auto  gp     = toGlobal(dim, p);
    switch (kind) {
    case Kind::Fan:
        forgetFan(gp);
        // a nozzle in front loses its fan
        break;
    case Kind::Nozzle:
        forgetFan(gp);
        break;
    case Kind::Wheel:
        if (region) removeControllersAround(*region, gp, -1);
        break;
    default:
        break;
    }
}

/// Bedrock repeats the use while the button is held: one action per 5 ticks.
bool cooldown(std::int64_t uid) {
    auto& s    = state();
    auto& last = s.lastUse[uid];
    bool  busy = s.tick - last < 5;
    if (!busy) last = s.tick;
    return busy;
}

} // namespace

void track(GlobalPos const& pos, Kind kind) {
    auto& s = state();
    auto  it = s.tracked.find(pos);
    if (it != s.tracked.end() && it->second == kind) return;
    s.tracked[pos] = kind;
    s.dirty        = true;
}

void untrack(GlobalPos const& pos) {
    if (state().tracked.erase(pos)) state().dirty = true;
}

void defer(std::function<void()> fn) { state().deferred.push_back(std::move(fn)); }

std::uint64_t currentTick() { return state().tick; }

/// Records how a player places one of our blocks (called from the useItemOn hook and selftests).
void recordPlacement(Player& player, BlockPos const& clicked, int face) {
    auto&    region = player.getDimensionBlockSource();
    BlockPos at     = clicked;
    if (!game::replaceable(region, clicked)) at = offset(clicked, faceOf(face));
    auto look = player.getViewVector();
    state().intents[toGlobal(int(player.getDimensionId()), at)] =
        PlaceIntent{{look.x, look.y, look.z}, player.isSneaking(), std::clamp(face, 0, 5), state().tick};
}

bool useOnMillstone(Player& player, BlockPos const& at) {
    if (cooldown(player.getOrCreateUniqueID().rawID)) return true;
    return millstoneTakeOut(player, toGlobal(int(player.getDimensionId()), at));
}

} // namespace lv::feature::bulk

namespace {

using namespace lv::feature::bulk;
namespace ia = lv::feature::common::interactions;

ia::Result onUseOn(ia::UseOnContext const& c) {
    BlockPos at{c.pos.x, c.pos.y, c.pos.z};
    bool     empty = c.item.isNull() || c.item.mCount == 0;
    if (empty && c.blockId == kMillstone) {
        useOnMillstone(c.player, at);
        return ia::Result::Consume;
    }
    if (!empty) {
        auto kind = kindOf(c.itemId);
        if (kind && (*kind == Kind::Fan || *kind == Kind::Nozzle || *kind == Kind::Wheel)) recordPlacement(c.player, at, c.face);
    }
    return ia::Result::Pass;
}

class BulkModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0) return;
            auto& region = ev.blockSource();
            if (region.getLevel().isClientSide()) return;
            auto const& now  = ev.newBlock().getTypeName();
            auto const& prev = ev.previousBlock().getTypeName();
            int         dim  = static_cast<int>(region.getDimensionId());
            BlockPos    p    = ev.pos();
            auto        gp   = toGlobal(dim, p);
            if (now != prev) {
                if (auto kind = kindOf(prev)) {
                    untrack(gp);
                    defer([dim, p, kind] { onRemoved(dim, p, *kind); });
                }
                if (auto kind = kindOf(now)) {
                    track(gp, *kind);
                    defer([dim, p, kind] { onPlaced(dim, p, *kind); });
                }
            }
            // Create: a fan rebuilds its current when a neighbour changes
            for (auto f : k::kFaces) markFanDirty(toGlobal(dim, offset(p, f)));
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { tick(); }));
        mWheelListener = lv::kinetics::onSpeedChanged(std::string(kWheel), [](lv::kinetics::SpeedChange const& c) {
            if (c.removed) return;
            auto pos = c.pos;
            defer([pos] {
                if (auto* region = regionOf(pos.dim)) fixControllers(*region, pos);
            });
        });
        mFanListener = lv::kinetics::onSpeedChanged(std::string(kFan), [](lv::kinetics::SpeedChange const& c) { markFanDirty(c.pos); });
        static bool once = [] {
            lv::game::db::onWorldReady([] {
                resetWorld();
                loadIndex();
            });
            lv::game::db::onWorldSave([] { saveIndex(); });
            lv::game::db::onWorldClosed([] {
                saveIndex();
                resetWorld();
            });
            // Create's destroy(): a broken millstone / controller drops its contents
            lv::feature::common::blockdata::onErase([](GlobalPos const& pos, nlohmann::json const& doc, std::string const&) {
                auto it = doc.find("bulk");
                if (it == doc.end() || !it->is_object()) return;
                std::vector<ItemStack> stacks;
                auto add = [&](nlohmann::json const& j) {
                    auto s = stackOf(j);
                    if (!s.isNull() && s.mCount > 0) stacks.push_back(s);
                };
                if (it->contains("in")) add((*it)["in"]);
                for (auto const& key : {"out", "inv"})
                    if (auto a = it->find(key); a != it->end() && a->is_array())
                        for (auto const& j : *a) add(j);
                if (stacks.empty()) return;
                if (auto* region = regionOf(pos.dim)) dropStacks(*region, toBlockPos(pos), stacks);
            });
            return true;
        }();
        (void)once;
        registerCapabilities();
        mUseOn = ia::onUseOn({}, onUseOn);
        if (lv::game::db::available()) {
            resetWorld();
            loadIndex();
        }
    }

    void uninstall() {
        ia::remove(mUseOn);
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        lv::kinetics::removeListener(mWheelListener);
        lv::kinetics::removeListener(mFanListener);
        saveIndex();
    }

private:
    void tick() {
        auto& s = state();
        ++s.tick;
        // world edits queued by events
        for (int round = 0; round < 4 && !s.deferred.empty(); ++round) {
            auto work = std::move(s.deferred);
            s.deferred.clear();
            for (auto& fn : work) {
                try {
                    fn();
                } catch (std::exception const& e) {
                    lv::log().error("bulk: deferred action failed: {}", e.what());
                }
            }
        }
        std::vector<std::pair<GlobalPos, Kind>> list(s.tracked.begin(), s.tracked.end());
        for (auto const& [pos, kind] : list) {
            auto* region = regionOf(pos.dim);
            if (!region) continue;
            auto bp = toBlockPos(pos);
            if (!loaded(*region, bp)) continue;
            auto actual = kindOf(idAt(*region, bp));
            if (!actual || *actual != kind) {
                // changed without an event (e.g. while unloaded)
                untrack(pos);
                onRemoved(pos.dim, bp, kind);
                continue;
            }
            try {
                switch (kind) {
                case Kind::Millstone:
                    tickMillstone(*region, pos);
                    break;
                case Kind::Wheel:
                    // Create's lazyTick (every 20 ticks)
                    if ((s.tick + static_cast<std::uint64_t>(pos.x * 7 + pos.z * 13 + pos.y)) % 20 == 0) fixControllers(*region, pos);
                    break;
                case Kind::Controller:
                    tickController(*region, pos);
                    break;
                case Kind::Fan:
                    tickFan(*region, pos);
                    break;
                case Kind::Nozzle:
                    tickNozzle(*region, pos);
                    break;
                }
            } catch (std::exception const& e) {
                lv::log().error("bulk: tick at {} {} {} failed: {}", pos.x, pos.y, pos.z, e.what());
            }
        }
        if (s.tick % 100 == 0) {
            pruneBypass();
            pruneFanState();
            std::erase_if(s.intents, [&](auto const& kv) { return s.tick - kv.second.tick > 100; });
        }
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    ia::HandlerId                       mUseOn = 0;
    lv::kinetics::ListenerId                mWheelListener = 0;
    lv::kinetics::ListenerId                mFanListener   = 0;
};

} // namespace

LV_REGISTER_MODULE(BulkModule);
