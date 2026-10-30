// Chassis (linear, secondary linear, radial) and stickers: placement orientation, sticky faces
// (slime ball / super glue / sneaking empty hand), chassis ranges (U3 BlockData
// `data["chassis"]["range"]`, edited with the wrench on a value box, carried with contraptions
// by the BlockData carrier), sticker power; plus the player interactions of the unit (super glue
// item, chassis use; through common/Interactions.h) and the deferred world edits.

#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/common/FacingID.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/Redstone.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Internal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::contraption {

namespace {

namespace bd = common::blockdata;
namespace ia = common::interactions;
namespace vb = common::valuebox;

/// Wave-1 storage of the ranges (one array for the world), migrated into BlockData on load.
constexpr char kDbChassis[]  = "lv:contraption:chassis";
constexpr char kDbStickers[] = "lv:contraption:stickers";
/// Wave-1 data carrier of the ranges: contraptions saved with it still restore it.
constexpr char kRangeCarrier[] = "contraption:chassis_range";
/// BlockData key of a chassis (`{"range": n}`).
constexpr char kDataKey[] = "chassis";
std::set<std::pair<int, cc::Pos>>& stickers() {
    static std::set<std::pair<int, cc::Pos>> s;
    return s;
}
/// The redstone watch of each tracked sticker (Create: neighborChanged updates the sticker).
std::map<std::pair<int, cc::Pos>, common::redstone::WatchId>& stickerWatches() {
    static std::map<std::pair<int, cc::Pos>, common::redstone::WatchId> w;
    return w;
}
void watchSticker(int dim, cc::Pos const& pos) {
    auto key = std::make_pair(dim, pos);
    if (stickerWatches().contains(key)) return;
    stickerWatches()[key] = common::redstone::watch({dim, pos.x, pos.y, pos.z}, [](core::GlobalPos const& at, int) {
        if (auto* region = detail::regionOf(at.dim)) detail::updateSticker(*region, at.dim, {at.x, at.y, at.z});
    });
}
std::vector<std::function<void()>>& deferred() {
    static std::vector<std::function<void()>> d;
    return d;
}

void storeRange(core::GlobalPos const& pos, int range) { bd::edit(pos)[kDataKey]["range"] = range; }
void persistStickers() {
    if (!game::db::available()) return;
    nlohmann::json j = nlohmann::json::array();
    for (auto const& [dim, p] : stickers()) j.push_back({dim, p.x, p.y, p.z});
    game::db::save(kDbStickers, j);
}

int maxRange() { return std::max(1, config::get<Config>().maxChassisRange); }
int defaultRange() { return std::max(1, config::get<Config>().maxChassisRange / 2); }
int clampRange(int r) { return std::clamp(r, 1, std::max(1, config::get<Config>().maxChassisRange)); }

std::string blockIdAt(BlockSource& region, cc::Pos const& p) {
    auto bp = detail::toBlockPos(p);
    if (!region.hasChunksAt(bp, 0, false)) return {};
    return region.getBlock(bp).getTypeName();
}

bool setStates(BlockSource& region, cc::Pos const& p, cc::States const& states) {
    auto        bp    = detail::toBlockPos(p);
    auto const& block = region.getBlock(bp);
    auto const* next  = detail::withStates(block, states);
    if (!next || next == &block) return false;
    return game::setBlock(region, bp, *next);
}

/// The glueable state of a chassis face (nullptr if the face isn't glueable).
char const* glueableState(cc::ChassisKind kind, cc::Axis axis, cc::Dir face) {
    if (kind == cc::ChassisKind::Radial) {
        if (cc::axisOf(face) == axis) return nullptr;
        static constexpr char const* keys[4]{"levitate:sticky_north", "levitate:sticky_south", "levitate:sticky_east", "levitate:sticky_west"};
        auto sides = cc::radialSides(axis);
        for (int i = 0; i < 4; ++i)
            if (sides[i] == face) return keys[i];
        return nullptr;
    }
    if (cc::axisOf(face) != axis) return nullptr;
    return cc::positive(face) ? "levitate:sticky_top" : "levitate:sticky_bottom";
}

cc::Axis chassisAxisAt(BlockSource& region, cc::Pos const& p) {
    auto states = detail::statesOf(region.getBlock(detail::toBlockPos(p)));
    auto it     = states.find("levitate:axis");
    auto v      = it == states.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
    return static_cast<cc::Axis>(std::clamp<std::int64_t>(v ? *v : 1, 0, 2));
}

cc::Dir nearestLooking(Player const& player) {
    // Bedrock rotation: x = pitch (down positive), y = yaw (0 = south, 90 = west)
    auto   rot   = player.getRotation();
    double pitch = rot.x * std::numbers::pi / 180.0, yaw = rot.y * std::numbers::pi / 180.0;
    cc::Vec3d look{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    cc::Dir   best  = cc::Dir::Up;
    double    score = -2;
    for (auto d : cc::kAllDirs) {
        auto   o = cc::offset(d);
        double s = o.x * look.x + o.y * look.y + o.z * look.z;
        if (s > score) {
            score = s;
            best  = d;
        }
    }
    return best;
}

struct ChassisLifecycle {
    ChassisLifecycle() {
        game::db::onWorldReady([] {
            stickers().clear();
            if (auto j = game::db::load(kDbChassis); j && j->is_array()) {
                // one-time migration of the wave-1 range list into BlockData
                int count = 0;
                for (auto const& e : *j) {
                    if (!e.is_array() || e.size() != 5) continue;
                    storeRange({e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}, e[4].get<int>());
                    ++count;
                }
                bd::flush();
                game::db::erase(kDbChassis);
                log().info("contraption: moved {} chassis ranges into block data", count);
            }
            stickerWatches().clear();
            if (auto j = game::db::load(kDbStickers); j && j->is_array()) {
                for (auto const& e : *j)
                    if (e.is_array() && e.size() == 4) stickers().insert({e[0].get<int>(), {e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}});
            }
            for (auto const& [dim, p] : stickers()) watchSticker(dim, p);
        });
        game::db::onWorldClosed([] {
            stickers().clear();
            stickerWatches().clear(); // (the redstone watches are dropped with the world)
            deferred().clear();
        });
        // chassis ranges travel with the blocks in BlockData (Create keeps them in the chassis
        // block entity); contraptions saved by wave 1 still carry them under the old carrier
        registerDataCarrier({
            kRangeCarrier,
            nullptr,
            [](BlockSource&, core::GlobalPos const& gp, nlohmann::json const& data) {
                storeRange(gp, clampRange(data.value("range", defaultRange())));
            },
            nullptr,
        });
    }
};
ChassisLifecycle const gChassisLifecycle;

} // namespace

namespace detail {

void defer(std::function<void()> fn) { deferred().push_back(std::move(fn)); }

cc::ChassisKind chassisKind(std::string_view id) {
    if (id == kLinearChassis) return cc::ChassisKind::Linear;
    if (id == kSecondaryChassis) return cc::ChassisKind::SecondaryLinear;
    if (id == kRadialChassis) return cc::ChassisKind::Radial;
    return cc::ChassisKind::None;
}

int storedChassisRange(int dim, cc::Pos const& p) {
    auto const* doc = bd::get({dim, p.x, p.y, p.z});
    if (!doc) return defaultRange();
    auto c = doc->find(kDataKey);
    if (c == doc->end() || !c->is_object()) return defaultRange();
    auto r = c->find("range");
    return r == c->end() || !r->is_number_integer() ? defaultRange() : clampRange(r->get<int>());
}

bool chassisUse(Player& player, int dim, cc::Pos const& pos, cc::Dir face, std::string const& heldId, bool sneaking) {
    auto* region = regionOf(dim);
    if (!region) return false;
    auto id   = blockIdAt(*region, pos);
    auto kind = chassisKind(id);
    if (kind == cc::ChassisKind::None) return false;
    auto const& block  = region->getBlock(toBlockPos(pos));
    auto        states = statesOf(block);
    auto        axis   = static_cast<cc::Axis>(std::clamp<std::int64_t>(std::get<std::int64_t>(states["levitate:axis"]), 0, 2));
    bool        isSlime = heldId == "minecraft:slime_ball" || heldId == kSuperGlue;
    auto const* side    = glueableState(kind, axis, face);
    if (!side) return false;
    auto stickyOf = [&](char const* key) { return std::get<std::int64_t>(states[key]) != 0; };
    if (isSlime && stickyOf(side)) {
        // slime on a sticky face: make every glueable face sticky
        bool changed = false;
        for (auto d : cc::kAllDirs) {
            if (auto const* s = glueableState(kind, axis, d); s && !stickyOf(s)) {
                states[s] = std::int64_t{1};
                changed   = true;
            }
        }
        if (!changed) return false;
    } else {
        if (!heldId.empty() && !isSlime) return false;
        if (heldId.empty() && !sneaking) return false;
        if (stickyOf(side) == isSlime) return false;
        states[side] = std::int64_t{isSlime ? 1 : 0};
    }
    setStates(*region, pos, states);
    playSound(dim, cc::Vec3d::center(pos), isSlime ? "mob.slime.big" : "mob.slime.small", 0.5f, 1.f);
    log().info("chassis at {} {} {}: face {} {} by {}", pos.x, pos.y, pos.z, cc::dirName(face), isSlime ? "made sticky" : "cleared", player.getRealName());
    return true;
}

void orientPlaced(BlockSource& region, cc::Pos const& pos, cc::Dir clickedFace, Player& player) {
    auto id = blockIdAt(region, pos);
    if (chassisKind(id) != cc::ChassisKind::None) {
        // Create: the chassis axis follows the clicked face (pillar placement)
        cc::States s{{"levitate:axis", std::int64_t(cc::axisOf(clickedFace))}};
        setStates(region, pos, s);
        return;
    }
    if (id == kSticker) {
        cc::Dir look   = nearestLooking(player);
        cc::Dir facing = player.isSneaking() ? look : cc::opposite(look);
        int     dim    = static_cast<int>(region.getDimensionId());
        cc::States s{
            {"levitate:facing",   std::int64_t(facing)                   },
            {"levitate:extended", std::int64_t(powered(region, pos) ? 1 : 0)}
        };
        setStates(region, pos, s);
        trackSticker(dim, pos);
    }
}

void trackSticker(int dim, cc::Pos const& pos) {
    if (stickers().insert({dim, pos}).second) persistStickers();
    watchSticker(dim, pos);
}

void updateSticker(BlockSource& region, int dim, cc::Pos const& pos) {
    if (blockIdAt(region, pos) != kSticker) {
        if (stickers().erase({dim, pos})) persistStickers();
        if (auto it = stickerWatches().find({dim, pos}); it != stickerWatches().end()) {
            common::redstone::unwatch(it->second);
            stickerWatches().erase(it);
        }
        return;
    }
    auto states   = statesOf(region.getBlock(toBlockPos(pos)));
    bool extended = std::get<std::int64_t>(states["levitate:extended"]) != 0;
    bool power    = powered(region, pos);
    if (extended == power) return;
    states["levitate:extended"] = std::int64_t{power ? 1 : 0};
    setStates(region, pos, states);
    playSound(dim, cc::Vec3d::center(pos), power ? "tile.piston.out" : "tile.piston.in", 0.35f, power ? 0.75f : 0.6f);
    log().info("sticker at {} {} {} {}", pos.x, pos.y, pos.z, power ? "extended" : "retracted");
}

} // namespace detail

int chassisRange(core::GlobalPos const& pos) { return detail::storedChassisRange(pos.dim, {pos.x, pos.y, pos.z}); }

void setChassisRange(core::GlobalPos const& pos, int range) {
    auto* region = detail::regionOf(pos.dim);
    if (!region) return;
    range = clampRange(range);
    // the whole connected group takes the value (Create's BulkScrollValueBehaviour)
    std::vector<cc::Pos>                     frontier{{pos.x, pos.y, pos.z}};
    std::unordered_set<cc::Pos, cc::PosHash> seen;
    auto query = [&](cc::Pos const& p) { return detail::describe(*region, pos.dim, p); };
    while (!frontier.empty() && seen.size() < 4096) {
        auto p = frontier.back();
        frontier.pop_back();
        if (!seen.insert(p).second) continue;
        auto info = query(p);
        if (info.chassis == cc::ChassisKind::None) continue;
        storeRange({pos.dim, p.x, p.y, p.z}, range);
        for (auto const& n : cc::attachedChassis(p, info, query)) frontier.push_back(n);
    }
    log().info("chassis range at {} {} {} set to {}", pos.x, pos.y, pos.z, range);
}

// ---- events of the unit -----------------------------------------------------------------------

class SupportModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        // chassis faces first (slime ball and super glue both make faces sticky), then super glue
        mHandlers.push_back(ia::onUseOn({}, [this](ia::UseOnContext const& c) { return onUseOn(c); }, ia::priority::High));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerPlacingBlockEvent>([this](ll::event::PlayerPlacingBlockEvent& e) {
            mLastFace[e.self().getRealName()] = static_cast<cc::Dir>(std::clamp<int>(e.face(), 0, 5));
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerPlacedBlockEvent>([this](ll::event::PlayerPlacedBlockEvent& e) {
            auto id = e.placedBlock().getTypeName();
            if (detail::chassisKind(id) == cc::ChassisKind::None && id != kSticker) return;
            auto    name = e.self().getRealName();
            auto    face = mLastFace.contains(name) ? mLastFace[name] : cc::Dir::Up;
            cc::Pos p    = detail::toPos(e.pos());
            int     dim  = static_cast<int>(e.self().getDimensionId());
            auto    uid  = e.self().getOrCreateUniqueID().rawID;
            detail::defer([=] {
                auto* player = playerById(uid);
                auto* region = detail::regionOf(dim);
                if (player && region) detail::orientPlaced(*region, p, face, *player);
            });
        }));
        // hitting a block with super glue removes its glue regions (the block stays)
        mHandlers.push_back(ia::onDestroy({.item = std::string(kSuperGlue)}, [](ia::DestroyContext const& c) {
            return detail::glueAttack(c.player, {c.pos.x, c.pos.y, c.pos.z}) ? ia::Result::Consume : ia::Result::Pass;
        }));
        registerRangeBoxes();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { tick(); }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
        for (auto id : {kLinearChassis, kSecondaryChassis, kRadialChassis}) vb::unregisterBox(std::string(id), "range");
    }

private:
    /// Create's chassis range: a bulk scroll value on every face (CenteredSideValueBoxTransform),
    /// wrench only, 1..maxChassisRange, shared by the connected chassis group.
    static void registerRangeBoxes() {
        for (auto id : {kLinearChassis, kSecondaryChassis, kRadialChassis}) {
            bool radial = id == kRadialChassis;
            vb::registerBox(std::string(id), {
                .id            = "range",
                .label         = radial ? "create.contraptions.chassis.radius" : "create.contraptions.chassis.range",
                .labelFallback = radial ? "Radius when Sticky" : "Range of Sticky Sides",
                .board         = [](Block const&) {
                    return core::common::plainBoard(maxRange(), 10, tr("create.generic.range", "Range"));
                },
                .centre = [](Block const&, int face) -> std::optional<core::common::Vec> {
                    return core::common::faceLocalPoint(face, 8, 8, 0.5);
                },
                .scale = 0.5,
                .get   = [](core::GlobalPos const& p) {
                    return core::common::plainToSettings(chassisRange(p));
                },
                .set = [](core::GlobalPos const& p, core::common::ValueSettings v, Player&) {
                    setChassisRange(p, core::common::plainFromSettings(v, 1, maxRange()));
                },
                .format         = {},
                .requiresWrench = true,
            });
        }
    }

    static Player* playerById(std::int64_t uid) {
        auto level = ll::service::getLevel();
        if (!level) return nullptr;
        ActorUniqueID id;
        id.rawID = uid;
        return level->getPlayer(id);
    }

    ia::Result onUseOn(ia::UseOnContext const& c) {
        auto&              player = c.player;
        std::string const& held   = c.itemId;
        cc::Pos            pos{c.pos.x, c.pos.y, c.pos.z};
        int                dim   = c.pos.dim;
        auto               uid   = player.getOrCreateUniqueID().rawID;
        auto               face  = static_cast<cc::Dir>(std::clamp(c.face, 0, 5));
        bool               sneak = c.sneaking;
        auto*              region = detail::regionOf(dim);
        if (!region) return ia::Result::Pass;

        // chassis faces first (slime ball and super glue both make faces sticky)
        auto kind = detail::chassisKind(c.blockId);
        if (kind != cc::ChassisKind::None && (held == "minecraft:slime_ball" || held == kSuperGlue || (held.empty() && sneak))
            && glueableState(kind, chassisAxisAt(*region, pos), face)) {
            if (cooldown(uid)) return ia::Result::Consume;
            detail::chassisUse(player, dim, pos, face, held, sneak);
            return ia::Result::Consume;
        }
        if (held == kSuperGlue) {
            if (!cooldown(uid)) detail::glueClick(player, pos, sneak);
            return ia::Result::Consume;
        }
        return ia::Result::Pass;
    }

    /// Bedrock repeats the interaction while the button is held: one action per 5 ticks.
    bool cooldown(std::int64_t uid) {
        auto& last = mLastUse[uid];
        bool  busy = mTicks - last < 5;
        if (!busy) last = mTicks;
        return busy;
    }

    void tick() {
        ++mTicks;
        auto work = std::move(deferred());
        deferred().clear();
        for (auto& fn : work) {
            try {
                fn();
            } catch (std::exception const& ex) {
                log().error("contraption: deferred action failed: {}", ex.what());
            }
        }
        // stickers that were mined or carried away (their redstone watch only reports power changes)
        if (mTicks % 200 == 0) {
            for (auto [dim, pos] : std::vector<std::pair<int, cc::Pos>>(stickers().begin(), stickers().end())) {
                auto* region = detail::regionOf(dim);
                if (region && region->hasChunksAt(detail::toBlockPos(pos), 0, false) && blockIdAt(*region, pos) != kSticker)
                    detail::updateSticker(*region, dim, pos); // forgets it
            }
        }
        if (mTicks % 10 == 0 && config::get<Config>().glueOutlines) {
            auto level = ll::service::getLevel();
            if (!level) return;
            level->forEachPlayer([](Player& p) {
                auto const& item = p.getSelectedItem();
                if (!item.isNull() && item.getTypeName() == kSuperGlue) detail::glueOutlines(p);
                return true;
            });
        }
    }

    std::vector<ll::event::ListenerPtr>             mListeners;
    std::vector<ia::HandlerId>                      mHandlers;
    std::unordered_map<std::string, cc::Dir>        mLastFace;
    std::unordered_map<std::int64_t, std::uint64_t> mLastUse;
    std::uint64_t                                   mTicks = 0;
};

} // namespace lv::feature::contraption

LV_REGISTER_MODULE(lv::feature::contraption::SupportModule);
