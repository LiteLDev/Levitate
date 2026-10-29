// Building unit (U10): module wiring. World changes of any cause go through BlockChangedEvent
// (connections, tracked copper / doors, door halves); the tick runs deferred edits, connection
// updates, copper weathering, door / trapdoor redstone and placard pulses.

#include <map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/building/Building.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::building {

namespace detail {
void processDirty();
void loadCopper();
void saveCopper();
void clearCopper();
void loadRedstone();
void saveRedstone();
void clearRedstone();
void clearPulses();
void registerDrops();
void advanceClock();
void doorHalfChanged(GlobalPos const& pos, Block const& previous, Block const& now, bool creativeBreak);
} // namespace detail

namespace {

std::vector<std::function<void()>>& deferred() {
    static std::vector<std::function<void()>> d;
    return d;
}
std::map<GlobalPos, bool>& playerBreaks() {
    static std::map<GlobalPos, bool> m;
    return m;
}
std::uint64_t& ticks() {
    static std::uint64_t t = 0;
    return t;
}

bool mine(Block const& b) { return infoOf(b) != nullptr; }

// the encased girder turns like a shaft along its axis (x or z); the beam is the static part
LV_REGISTER_KINETIC_BLOCK(
    kGirderShaftId,
    lv::kinetics::specs::withModel(lv::kinetics::specs::shaft("levitate:axis"), "geometry.levitate.girder_shaft_rotor", "levitate_girder_shaft")
);

struct Lifecycle {
    Lifecycle() {
        game::db::onWorldReady([] { loadWorld(); });
        game::db::onWorldSave([] { saveWorld(); });
        game::db::onWorldClosed([] { closeWorld(); });
        detail::registerDrops();
        // girders: a quarter turn about Y swaps the X and Z beams
        contraption::registerRotationHandler(kGirderId, [](contraption::cc::States& s, contraption::cc::Axis axis, int quarters) {
            if (axis != contraption::cc::Axis::Y || (quarters & 1) == 0) return;
            auto x = s.find("levitate:x"), z = s.find("levitate:z");
            if (x != s.end() && z != s.end()) std::swap(x->second, z->second);
        });
    }
} const gLifecycle;

} // namespace

void defer(std::function<void()> fn) { deferred().push_back(std::move(fn)); }

void notePlayerBreak(GlobalPos const& pos, bool creative) { playerBreaks()[pos] = creative; }

void onBlockChanged(GlobalPos const& pos, Block const& previous, Block const& now) {
    bool wasMine = mine(previous), isMine = mine(now);
    if (wasMine || isMine) markDirty(pos);
    if (isMine) {
        auto const* info = infoOf(now);
        if (!info->copperShape.empty() && !info->waxed && info->age < 3) trackCopper(pos);
        if ((info->kind == Kind::Door && !stateOf(now, "levitate:upper")) || info->kind == Kind::Trapdoor) trackRedstone(pos);
    }
    if (wasMine) {
        auto it       = playerBreaks().find(pos);
        bool creative = it != playerBreaks().end() && it->second;
        detail::doorHalfChanged(pos, previous, now, creative);
    }
}

void tick() {
    ++ticks();
    detail::advanceClock();
    auto work = std::move(deferred());
    deferred().clear();
    for (auto& fn : work) {
        try {
            fn();
        } catch (std::exception const& e) {
            log().error("building: deferred action failed: {}", e.what());
        }
    }
    detail::processDirty();
    copperTick();
    if (ticks() % 2 == 0) redstoneTick();
    placardTick();
    playerBreaks().clear();
}

void loadWorld() {
    detail::loadCopper();
    detail::loadRedstone();
    applyBlockProperties();
}

void saveWorld() {
    detail::saveCopper();
    detail::saveRedstone();
}

void closeWorld() {
    detail::clearCopper();
    detail::clearRedstone();
    detail::clearPulses();
    deferred().clear();
    playerBreaks().clear();
}

class BuildingModule {
public:
    void install() {
        registerPlacardSignal();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0) return;
            auto const& prev = ev.previousBlock();
            auto const& next = ev.newBlock();
            auto const& bp   = ev.pos();
            GlobalPos   pos{static_cast<int>(ev.blockSource().getDimensionId()), bp.x, bp.y, bp.z};
            if (mine(prev) || mine(next)) {
                onBlockChanged(pos, prev, next);
                return;
            }
            // a neighbour of one of our connected blocks changed
            auto& region = ev.blockSource();
            for (int d = 0; d < 6; ++d) {
                BlockPos n{bp.x + kDx[d], bp.y + kDy[d], bp.z + kDz[d]};
                if (n.y < region.getMinHeight() || n.y >= region.getMaxHeight() || !region.hasChunksAt(n, 0, false)) continue;
                if (mine(region.getBlock(n))) markDirty({pos.dim, n.x, n.y, n.z});
            }
        }));
        installShapes();
        installHooks();
        if (game::db::available()) loadWorld(); // BDS: the world exists before mods are enabled
    }

    void uninstall() {
        uninstallHooks();
        uninstallShapes();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        if (game::db::available()) saveWorld();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::feature::building

LV_REGISTER_MODULE(lv::feature::building::BuildingModule);
