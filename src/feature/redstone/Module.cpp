// Redstone unit (U23): the module. Tracks every loaded block of the unit, ticks them, follows
// world changes (BlockChangedEvent), handles players' clicks and placements (GameMode hooks:
// they see network players and SimulatedPlayers alike), answers the client mod's hover queries
// and registers the value boxes, filter slots, the threshold screen and the contact actor.

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/gamemode/GameMode.h"
#include "mc/world/gamemode/InteractionResult.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"

#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/net/Net.h"
#include "feature/redstone/Internal.h"
#include "feature/redstone/RedstoneLink.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("redstone", lv::feature::redstone::RedstoneConfig);

namespace lv::feature::redstone {

namespace ia = common::interactions;

namespace {

constexpr char kDbBlocks[] = "lv:redstone:blocks";

struct UnitState {
    std::map<GP, Runtime> tracked;
    bool                  dirty  = false; ///< tracked set changed since the last save
    bool                  ready  = false; ///< world loaded
    std::vector<GP>       added;          ///< from BlockChangedEvent, handled next tick
    std::vector<GP>       removed;
    std::set<GP>          changed;        ///< positions whose neighbours must look again
    std::map<std::int64_t, std::pair<GP, std::uint64_t>> lastUse; ///< player -> last used block, tick
};
UnitState& unit() {
    static UnitState s;
    return s;
}

std::string posText(GP const& p) { return std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z); }

void save() {
    auto& u = unit();
    if (!u.ready || !game::db::available()) return;
    for (auto const& [p, rt] : u.tracked) persistCounter(p, rt);
    common::blockdata::flush(); // BlockData may have saved before us this round
    if (!u.dirty) return;
    Json j = Json::array();
    for (auto const& [p, rt] : u.tracked) j.push_back({p.dim, p.x, p.y, p.z, static_cast<int>(rt.kind)});
    game::db::save(kDbBlocks, j);
    u.dirty = false;
}

void load() {
    auto& u = unit();
    u.tracked.clear();
    resetLinks();
    if (auto j = game::db::load(kDbBlocks); j && j->is_array()) {
        for (auto const& e : *j) {
            if (!e.is_array() || e.size() != 5) continue;
            Runtime rt;
            rt.kind = static_cast<Kind>(std::clamp(e[4].get<int>(), 0, static_cast<int>(kLastKind)));
            u.tracked.emplace(GP{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}, rt);
        }
    }
    u.ready = true;
    log().info("redstone: {} blocks tracked", u.tracked.size());
}

bool wearsGoggles(Player& player) {
    auto const& head = player.getArmor(SharedTypes::Legacy::ArmorSlot::Head);
    return !head.isNull() && head.getTypeName() == kGoggles;
}

// ---- the contact actor (Create's ContactMovementBehaviour) -------------------------------------

namespace cc = core::contraption;

std::optional<cc::Pos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    return cc::Pos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

void deactivateLastContact(contraption::MovementContext& m) {
    auto last = posFrom(m.data.value("lastContact", Json()));
    m.data.erase("lastContact");
    if (!last) return;
    GP gp{m.worldPos.dim, last->x, last->y, last->z};
    if (auto* rt = runtimeAt(gp); rt && rt->kind == Kind::Contact) rt->blockTick = 1; // re-check next tick
}

void contactTick(contraption::MovementContext& m) {
    auto it     = m.states.find(sFacing);
    int  facing = kUp;
    if (it != m.states.end()) {
        if (auto const* v = std::get_if<std::int64_t>(&it->second)) facing = static_cast<int>(*v);
    }
    auto  o      = offsetOf(facing);
    auto  centre = cc::Vec3d::center(m.localPos);
    auto  a      = m.contraption.toWorld(centre);
    auto  b      = m.contraption.toWorld(centre + cc::Vec3d{double(o[0]), double(o[1]), double(o[2])});
    auto  front  = b - a;
    // the rotated facing, snapped to the nearest direction
    int    dir  = kUp;
    double best = -2;
    for (int d = 0; d < 6; ++d) {
        auto   od = offsetOf(d);
        double s  = od[0] * front.x + od[1] * front.y + od[2] * front.z;
        if (s > best) best = s, dir = d;
    }
    auto    active = a + front * 0.65;
    cc::Pos visited{int(std::floor(active.x)), int(std::floor(active.y)), int(std::floor(active.z))};
    auto    prev = posFrom(m.data.value("visit", Json()));
    if (prev && *prev == visited) return;
    m.data["visit"] = Json::array({visited.x, visited.y, visited.z});
    if (!prev) return; // Create: nothing on the first movement
    deactivateLastContact(m);
    auto* region = regionOf(m.worldPos.dim);
    if (!region) return;
    BlockPos bp{visited.x, visited.y, visited.z};
    if (!loadedAt(*region, bp)) return;
    auto const& block = region->getBlock(bp);
    auto const& id    = block.getTypeName();
    if (id != kContact && id != kElevatorContact) return;
    if (state(block, sFacing, kUp) != opposite(dir)) return;
    if (id == kContact) {
        GP gp{m.worldPos.dim, visited.x, visited.y, visited.z};
        if (auto* rt = runtimeAt(gp)) {
            rt->contraptionContact = true;
            rt->blockTick          = -1;
        }
        if (setStates(*region, bp, {{sPowered, 1}})) log().info("redstone contact at {} powered by a contraption", posText(gp));
    }
    m.data["lastContact"] = Json::array({visited.x, visited.y, visited.z});
}

} // namespace

// ---- tracked blocks ----------------------------------------------------------------------------

std::map<GP, Runtime>& tracked() { return unit().tracked; }

Runtime* runtimeAt(GP const& pos) {
    auto it = unit().tracked.find(pos);
    return it == unit().tracked.end() ? nullptr : &it->second;
}

void track(GP const& pos, Kind kind) {
    auto& u = unit();
    auto  it = u.tracked.find(pos);
    if (it != u.tracked.end() && it->second.kind == kind) return;
    if (it != u.tracked.end()) {
        onRemoved(pos, it->second);
        u.tracked.erase(it);
    }
    Runtime rt;
    rt.kind = kind;
    u.tracked.emplace(pos, rt);
    u.dirty = true;
}

void untrack(GP const& pos) {
    auto& u  = unit();
    auto  it = u.tracked.find(pos);
    if (it == u.tracked.end()) return;
    onRemoved(pos, it->second);
    u.tracked.erase(it);
    u.dirty = true;
}

void touch(GP const& pos) {
    if (auto* rt = runtimeAt(pos)) rt->neighbour = true;
}

// ---- hover -------------------------------------------------------------------------------------

Json hoverLines(Player& player, GP const& pos, int face, core::common::Vec const& hit) {
    (void)face;
    Json out{
        {"lines", Json::array()},
        {"items", Json::array()}
    };
    if (static_cast<int>(player.getDimensionId()) != pos.dim) return out;
    auto& region = player.getDimensionBlockSource();
    auto  bp     = game::blockPos(pos);
    if (!loadedAt(region, bp)) return out;
    auto const& block = region.getBlock(bp);
    auto        kind  = kindOf(block.getTypeName());
    if (!kind) return out;
    if (*kind == Kind::Link) {
        auto [a, b] = frequencies(pos);
        auto name   = [&](ItemStack const& s) {
            return common::items::isEmpty(s) ? trFor(player, "lv.common.filter.empty", "(empty)") : common::items::displayName(s);
        };
        auto slot   = frequencySlotAt(block, hit);
        out["lines"].push_back(
            state(block, sReceiver) ? trFor(player, "lv.redstone.receiver", "Receiver") : trFor(player, "lv.redstone.transmitter", "Transmitter")
        );
        out["lines"].push_back(std::string(slot && *slot ? "> " : "") + trFor(player, "create.logistics.firstFrequency", "Frequency #1") + ": " + name(a));
        out["lines"].push_back(std::string(slot && !*slot ? "> " : "") + trFor(player, "create.logistics.secondFrequency", "Frequency #2") + ": " + name(b));
        out["items"].push_back(common::items::isEmpty(a) ? Json() : common::items::toJson(a));
        out["items"].push_back(common::items::isEmpty(b) ? Json() : common::items::toJson(b));
    } else if (*kind == Kind::Lever && wearsGoggles(player)) {
        out["lines"].push_back(trFor(player, "create.tooltip.analogStrength", "Analog Strength: {0}/15", state(block, sLeverState)));
    }
    return out;
}

// ---- hooks -------------------------------------------------------------------------------------

// ---- interactions (U36: on the shared dispatch) --------------------------------------------------

/// Right-clicks on the unit's blocks.
ia::Result redstoneUseOn(ia::UseOnContext const& c) {
    if (!unit().ready) return ia::Result::Pass;
    auto kind = kindOf(c.blockId);
    if (!kind) return ia::Result::Pass;
    Player& player = c.player;
    // value boxes and filter slots of U3 come first (they handle the click themselves)
    bool mayEdit   = !c.sneaking && !player.isSpectator() && player.canUseAbility(AbilitiesIndex::Build);
    bool tool      = c.itemId.empty() || c.itemId == kWrench;
    bool forCommon = mayEdit
                  && (common::filterslot::slotAt(c.block, c.face, c.hit) || (tool && common::valuebox::boxAt(c.block, c.face, c.hit)));
    if (forCommon) return ia::Result::Pass;
    // Bedrock repeats the use while the button is held (isFirstEvent false): drop those repeats,
    // never a new click
    invalidateCircuitCache();
    auto& last  = unit().lastUse[player.getOrCreateUniqueID().rawID];
    auto  since = gameTickCounter() - last.second;
    if (last.first == c.pos && (since < 2 || (!c.firstEvent && since < 5))) return ia::Result::Consume;
    Click click{player, c.pos, c.face, c.hit, c.item, c.sneaking};
    if (!use(click, c.block, *kind)) return ia::Result::Pass;
    last = {c.pos, gameTickCounter()};
    if (!c.item.isNull()) player.sendInventory(false); // undo a predicted placement
    return ia::Result::Consume;
}

/// A placed block of the unit: orient it as Create does (or refuse it when it cannot stay).
void redstonePlaced(ia::PlaceContext const& c) {
    if (!unit().ready) return;
    auto heldKind = kindOf(c.itemId);
    if (!heldKind || c.blockId != idOf(*heldKind)) return;
    Player&  player = c.player;
    auto&    region = player.getDimensionBlockSource();
    BlockPos target{c.pos.x, c.pos.y, c.pos.z};
    auto     rot = player.getRotation();
    Placement p{c.face, rot.x, rot.y, c.sneaking};
    auto const& placed   = region.getBlock(target);
    auto const* oriented = placementState(region, target, placed, *heldKind, p);
    if (oriented && oriented != &placed) game::setBlock(region, target, *oriented);
    auto const& now = region.getBlock(target);
    GP          gp  = c.pos;
    if (!canSurvive(region, target, now, *heldKind)) {
        // Create refuses the placement: give the block back
        if (auto level = ll::service::getLevel()) {
            BlockChangeContext ctx{};
            level->destroyBlock(region, target, !player.isCreative(), ctx);
        }
        return;
    }
    track(gp, *heldKind);
    if (auto* rt = runtimeAt(gp)) onAdded(region, gp, now, *rt);
    log().info("{} placed at {} by {}", c.blockId, posText(gp), player.getRealName());
}

// ---- module ------------------------------------------------------------------------------------

class RedstoneModule {
public:
    void install() {
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldReady([] { load(); });
            game::db::onWorldSave([] { save(); });
            game::db::onWorldClosed([] {
                save();
                unit() = UnitState{};
                resetLinks();
            });
            registerValueBoxes();
            registerFilterSlots();
            registerThresholdScreen();
            registerBellAndLamp();
            // our signal sources power the custom blocks of other units too (shared redstone input)
            for (auto k : {Kind::Link, Kind::Lever, Kind::PulseRepeater, Kind::PulseExtender, Kind::PulseTimer, Kind::PoweredLatch,
                           Kind::ToggleLatch, Kind::SmartObserver, Kind::ThresholdSwitch, Kind::Contact, Kind::DeskBell}) {
                common::redstone::registerEmitter(idOf(k), [](BlockSource&, BlockPos const&, Block const& block, int towards) {
                    return ourEmission(block, towards).value_or(0);
                });
            }
            contraption::registerActor(
                kContact,
                contraption::MovementBehaviour{
                    .startMoving      = nullptr,
                    .tick             = contactTick,
                    .visitNewPosition = nullptr,
                    .stopMoving       = deactivateLastContact,
                    .isActive         = nullptr,
                }
            );
        }
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0 || !unit().ready) return;
            auto& region = ev.blockSource();
            if (region.getLevel().isClientSide()) return;
            auto now  = kindOf(ev.newBlock().getTypeName());
            auto prev = kindOf(ev.previousBlock().getTypeName());
            GP   pos  = game::globalPos(region, ev.pos());
            auto& u   = unit();
            if (now && now != prev) u.added.push_back(pos);
            else if (prev && !now) u.removed.push_back(pos);
            // only changes next to (or of) our blocks matter
            bool near = now || prev;
            for (int d = 0; d < 6 && !near; ++d) near = u.tracked.contains(step(pos, d));
            if (near) u.changed.insert(pos);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { tick(); }));
        net::on("redstone.hover", [](Player& player, Json const& msg) {
            auto p   = msg.value("pos", Json());
            auto hit = msg.value("hit", Json::array());
            if (!p.is_array() || p.size() != 4 || !hit.is_array() || hit.size() != 3) return;
            for (auto const& v : p)
                if (!v.is_number_integer()) return;
            for (auto const& v : hit)
                if (!v.is_number()) return;
            GP pos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>(), p[3].get<int>()};
            // only blocks within reach (as the value boxes): no remote frequency snooping
            auto const& at = player.getPosition();
            double dx = at.x - (pos.x + 0.5), dy = at.y - (pos.y + 0.5), dz = at.z - (pos.z + 0.5);
            if (dx * dx + dy * dy + dz * dz > 64.0) return;
            core::common::Vec h{hit[0].get<double>(), hit[1].get<double>(), hit[2].get<double>()};
            auto              out = hoverLines(player, pos, msg.value("face", 0), h);
            out["pos"]            = p;
            net::send(player, "redstone.hover", out);
        });
        mHandlers.push_back(ia::onUseOn({}, redstoneUseOn));
        mHandlers.push_back(ia::onPlace({}, redstonePlaced));
        if (game::db::available() && !unit().ready) load(); // BDS: the world exists before mods are enabled
    }

    void uninstall() {
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        net::off("redstone.hover");
        save();
    }

private:
    void tick() {
        ++gameTickCounter();
        auto& u = unit();
        if (!u.ready) return;
        // world changes since the last tick
        for (auto const& pos : std::exchange(u.removed, {})) untrack(pos);
        for (auto const& pos : std::exchange(u.added, {})) {
            auto* region = regionOf(pos.dim);
            if (!region || !loadedAt(*region, game::blockPos(pos))) continue;
            auto const& block = region->getBlock(game::blockPos(pos));
            auto        kind  = kindOf(block.getTypeName());
            if (!kind) continue;
            bool fresh = !runtimeAt(pos) || runtimeAt(pos)->kind != *kind;
            track(pos, *kind);
            if (fresh) {
                if (auto* rt = runtimeAt(pos)) onAdded(*region, pos, block, *rt);
            }
        }
        for (auto const& pos : std::exchange(u.changed, {})) {
            for (int d = 0; d < 6; ++d) touch(step(pos, d));
            touch(pos);
        }

        invalidateCircuitCache();
        std::vector<GP>                          gone;
        std::vector<std::pair<GP, BlockSource*>> pop;
        std::vector<std::tuple<GP, Runtime*, BlockSource*>> active;
        std::map<int, BlockSource*>              regions;
        auto regionFor = [&](int dim) {
            auto it = regions.find(dim);
            if (it == regions.end()) it = regions.emplace(dim, regionOf(dim)).first;
            return it->second;
        };
        // pass 1: what is loaded, what must go, and every input before anything changes (so
        // the order blocks tick in can't add a tick of delay in one direction)
        for (auto& [pos, rt] : u.tracked) {
            auto* region = regionFor(pos.dim);
            auto  bp     = game::blockPos(pos);
            if (!region || !loadedAt(*region, bp)) {
                if (rt.loaded) onUnloaded(pos, rt);
                continue;
            }
            auto const& block = region->getBlock(bp);
            auto        kind  = kindOf(block.getTypeName());
            if (!kind || *kind != rt.kind) {
                gone.push_back(pos);
                continue;
            }
            if (!rt.loaded) {
                rt.loaded = true;
                rt.neighbour = true;
                if (rt.kind == Kind::Link) linkLoaded(*region, pos, block, rt);
            }
            if (rt.neighbour && !canSurvive(*region, bp, block, rt.kind)) {
                pop.emplace_back(pos, region);
                continue;
            }
            readInputs(*region, pos, block, rt);
            active.emplace_back(pos, &rt, region);
        }
        // pass 2: Create's block entity ticks
        for (auto& [pos, rt, region] : active) {
            auto const& block = region->getBlock(game::blockPos(pos));
            if (kindOf(block.getTypeName()) != rt->kind) continue;
            try {
                tickBlock(*region, pos, block, *rt);
            } catch (std::exception const& e) {
                log().error("redstone: tick of {} at {} failed: {}", idOf(rt->kind), posText(pos), e.what());
            }
        }
        for (auto const& pos : gone) untrack(pos);
        if (!pop.empty()) {
            auto level = ll::service::getLevel();
            for (auto const& [pos, region] : pop) {
                log().info("{} at {} lost its support and popped off", idOf(runtimeAt(pos)->kind), posText(pos));
                untrack(pos);
                if (level) {
                    BlockChangeContext ctx{};
                    level->destroyBlock(*region, game::blockPos(pos), true, ctx);
                }
            }
        }
    }

    std::vector<ll::event::ListenerPtr> mListeners;
    std::vector<ia::HandlerId>          mHandlers;
};

} // namespace lv::feature::redstone

LV_REGISTER_MODULE(lv::feature::redstone::RedstoneModule);
