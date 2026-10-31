// Bearings unit: the module (index of our blocks, tick, world lifecycle, config) and the game
// handlers for using items on blocks / actors (the shared dispatch, common/Interactions.h).

#include <algorithm>
#include <map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerPlaceBlockEvent.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/bearings/Bearings.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("bearings", lv::feature::bearings::Config);

namespace lv::feature::bearings {

namespace ia = common::interactions;

bool fitsInsideContainerItems(std::string_view itemId) {
    if (itemId != kMinecartContraption && itemId != kChestMinecartContraption) return true;
    return config::get<Config>().minecartContraptionInContainers;
}

namespace {

constexpr char kIndexKey[] = "lv:bearings:index";

struct ModuleState {
    std::map<GP, Kind>                  index;
    std::vector<std::function<void()>>  deferred;
    std::uint64_t                       tick  = 0;
    bool                                dirty = false;
    std::unordered_map<std::int64_t, std::uint64_t> lastUse; // player -> tick (Bedrock repeats use packets)
};

ModuleState& ms() {
    static ModuleState s;
    return s;
}

void loadIndex() {
    ms().index.clear();
    auto j = game::db::load(kIndexKey);
    if (!j || !j->is_array()) return;
    for (auto const& e : *j) {
        if (!e.is_array() || e.size() != 5) continue;
        GP p{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()};
        ms().index[p] = static_cast<Kind>(e[4].get<int>());
    }
}

void saveIndex() {
    if (!game::db::available()) return;
    Json j = Json::array();
    for (auto const& [p, k] : ms().index) j.push_back({p.dim, p.x, p.y, p.z, static_cast<int>(k)});
    game::db::save(kIndexKey, j);
    ms().dirty = false;
}

/// One action per click: Bedrock repeats use packets while the button is held.
bool debounce(Player& player) {
    auto& last = ms().lastUse[uidOf(player)];
    bool  busy = ms().tick - last < 5 && last != 0;
    if (!busy) last = ms().tick;
    return busy;
}

void tick() {
    auto& s = ms();
    ++s.tick;
    auto work = std::exchange(s.deferred, {});
    for (auto& fn : work) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().error("bearings: deferred action failed: {}", e.what());
        }
    }
    std::vector<std::pair<GP, Kind>> list(s.index.begin(), s.index.end());
    for (auto const& [pos, kind] : list) {
        auto* region = regionOf(pos.dim);
        if (!region || !loaded(*region, pos)) continue;
        auto now = kindOf(idAt(*region, pos));
        if (!now || *now != kind) {
            if (kind == Kind::CartAssembler || kind == Kind::ControllerRail) railRemoved(pos, kind);
            else bearingRemoved(pos);
            s.index.erase(pos);
            s.dirty = true;
            if (now) s.index[pos] = *now;
            continue;
        }
        try {
            switch (kind) {
            case Kind::Mechanical:
            case Kind::Windmill:
            case Kind::Clockwork:
                tickBearing(*region, pos, kind);
                break;
            case Kind::Turntable:
                tickTurntable(*region, pos);
                break;
            case Kind::CartAssembler:
            case Kind::ControllerRail:
                tickRail(*region, pos, kind);
                break;
            }
        } catch (std::exception const& e) {
            log().error("bearings: tick of {} failed: {}", pos.key(), e.what());
        }
    }
    try {
        tickCarts();
    } catch (std::exception const& e) {
        log().error("bearings: cart tick failed: {}", e.what());
    }
    if (s.dirty && s.tick % 100 == 0) saveIndex();
}

} // namespace

void defer(std::function<void()> fn) { ms().deferred.push_back(std::move(fn)); }
std::uint64_t currentTick() { return ms().tick; }

void track(GP const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return;
    auto kind = kindOf(idAt(*region, pos));
    if (!kind) return;
    auto [it, fresh] = ms().index.insert_or_assign(pos, *kind);
    if (fresh) ms().dirty = true;
}

std::vector<GP> tracked() {
    std::vector<GP> out;
    for (auto const& [p, k] : ms().index) out.push_back(p);
    return out;
}

// ---- player interactions (common/Interactions.h) ------------------------------------------------

namespace {

ia::Result handled(Player& player) {
    player.sendInventory(false); // undo the client's predicted use
    return ia::Result::Consume;
}

ia::Result onUseOn(ia::UseOnContext const& c) {
    Player&            player  = c.player;
    Block const*       clicked = &c.block;
    std::string const& target  = c.blockId;
    std::string const& held    = c.itemId;
    GP                 pos     = c.pos;
    auto               uid     = uidOf(player);
    cc::Vec3d          local{c.hit[0], c.hit[1], c.hit[2]};
    int                face  = c.face;
    auto               kind  = kindOf(target);
    bool               build = player.canUseAbility(AbilitiesIndex::Build);

    // bearings: an empty hand assembles / disassembles (value boxes on the sides go to U3)
    if (kind && (*kind == Kind::Mechanical || *kind == Kind::Windmill || *kind == Kind::Clockwork) && held.empty()
        && !c.sneaking && build && !common::valuebox::boxAt(*clicked, face, {local.x, local.y, local.z})) {
        if (!debounce(player)) defer([uid, pos] {
            if (auto* p = playerById(uid)) bearingUse(*p, pos);
        });
        return handled(player);
    }
    // sails: placement helper, dye, shears
    if (build && isSail(target) && !held.empty() && (isSail(held) || held == "minecraft:shears" || dyeColourOf(held))) {
        if ((isSail(held) && !c.sneaking) || held == "minecraft:shears" || (!isSailFrame(target) && dyeColourOf(held))) {
            if (!debounce(player)) {
                int face_ = face;
                defer([uid, pos, face_, local, held] {
                    auto* p = playerById(uid);
                    if (p && common::items::idOf(p->getSelectedItem()) == held) sailUse(*p, pos, face_, local, p->getSelectedItem());
                });
            }
            return handled(player);
        }
    }
    if (build && held == kCartAssembler) {
        if (!debounce(player)) defer([uid, pos] {
            if (auto* p = playerById(uid)) placeAssembler(*p, pos);
        });
        return handled(player);
    }
    if (build && (held == kMinecartContraption || held == kChestMinecartContraption)) {
        if (!debounce(player)) defer([uid, pos] {
            if (auto* p = playerById(uid)) placeMinecartContraption(*p, pos);
        });
        return handled(player);
    }
    if (build && (target == kCartAssembler || target == kControllerRail) && held.ends_with("minecart")) {
        if (!debounce(player)) defer([uid, pos, held] {
            if (auto* p = playerById(uid)) placeMinecartOn(*p, pos, held);
        });
        return handled(player);
    }
    return ia::Result::Pass;
}

/// Minecart coupling / wrench on a minecart.
ia::Result onInteract(ia::EntityContext const& c) {
    if (c.itemId != kMinecartCoupling && c.itemId != kWrench) return ia::Result::Pass;
    if (!cartInteract(c.player, c.entity)) return ia::Result::Pass;
    c.player.sendInventory(false);
    return ia::Result::Consume;
}

ia::Result onDestroy(ia::DestroyContext const& c) {
    if (c.player.isCreative()) markCreativeBreak(c.pos);
    return ia::Result::Pass;
}

} // namespace

// ---- module ---------------------------------------------------------------------------------------

class BearingsModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            if (game::db::available()) tick();
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0) return;
            auto& region = ev.blockSource();
            if (region.getLevel().isClientSide()) return;
            if (kindOf(ev.newBlock().getTypeName())) {
                GP p = game::globalPos(region, ev.pos());
                auto k = *kindOf(ev.newBlock().getTypeName());
                auto [it, fresh] = ms().index.insert_or_assign(p, k);
                if (fresh) ms().dirty = true;
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerPlacingBlockEvent>([this](ll::event::PlayerPlacingBlockEvent& e) {
            mLastFace[uidOf(e.self())] = static_cast<int>(e.face());
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerPlacedBlockEvent>([this](ll::event::PlayerPlacedBlockEvent& e) {
            std::string id = e.placedBlock().getTypeName();
            auto        k  = kindOf(id);
            bool        bearing = k && (*k == Kind::Mechanical || *k == Kind::Windmill || *k == Kind::Clockwork);
            if (!bearing && !isSail(id) && id != kControllerRail) return;
            auto uid  = uidOf(e.self());
            int  face = mLastFace.contains(uid) ? mLastFace[uid] : 1;
            GP   pos  = game::globalPos(static_cast<int>(e.self().getDimensionId()), e.pos());
            defer([=] {
                auto* p      = playerById(uid);
                auto* region = regionOf(pos.dim);
                if (!p || !region || idAt(*region, pos) != id) return;
                if (bearing) orientBearing(*region, pos, *p);
                else if (id == kControllerRail) orientControllerRail(*region, pos, *p);
                else orientSail(*region, pos, *p, face);
            });
        }));
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldReady([] {
                loadIndex();
                loadCarts();
                forgetBearings();
            });
            game::db::onWorldSave([] {
                saveIndex();
                saveCarts();
            });
            game::db::onWorldClosed([] {
                ms().index.clear();
                ms().deferred.clear();
                forgetCarts();
                forgetBearings();
            });
        }
        registerBearingBoxes();
        registerCartBoxes();
        mHandlers = {
            ia::onUseOn({}, onUseOn),
            ia::onInteract({}, onInteract),
            ia::onDestroy({.block = std::string(kCartAssembler)}, onDestroy),
        };
        if (game::db::available()) {
            loadIndex();
            loadCarts();
        }
    }

    void uninstall() {
        for (auto id : mHandlers) ia::remove(id);
        mHandlers.clear();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        for (auto id : {kMechanicalBearing, kWindmillBearing, kClockworkBearing})
            for (auto box : {"mode", "direction", "hands"}) common::valuebox::unregisterBox(std::string(id), box);
        common::valuebox::unregisterBox(std::string(kCartAssembler), "mode");
        if (game::db::available()) {
            saveIndex();
            saveCarts();
        }
    }

private:
    std::vector<ll::event::ListenerPtr>         mListeners;
    std::unordered_map<std::int64_t, int>       mLastFace;
    std::vector<ia::HandlerId>                  mHandlers;
};

} // namespace lv::feature::bearings

LV_REGISTER_MODULE(lv::feature::bearings::BearingsModule);
