// Transmission unit (U6) module: hooks, listeners and the world lifecycle.

#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "feature/transmission/TransmissionInternal.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::transmission {

class TransmissionModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        static bool once = false;
        if (!once) {
            once = true;
            registerSequencerScreen();
            registerSpeedControllerBox();
            game::db::onWorldReady([] { loadTracked(); });
            game::db::onWorldSave([] { saveTracked(); });
            game::db::onWorldClosed([] {
                clearTracked();
                clearGauges();
                clearSequencers();
                clearSpeedControllers();
            });
            net::onCompanionJoined([](Player& p) { gaugeSyncTo(p); });
        }
        if (game::db::available()) loadTracked(); // BDS: the world exists before mods are enabled
        mListeners.push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([this](ll::event::BlockChangedEvent& ev) {
            if (ev.layer() != 0) return;
            auto& region = ev.blockSource();
            if (region.getLevel().isClientSide()) return;
            auto const& before = ev.previousBlock().getTypeName();
            auto const& after  = ev.newBlock().getTypeName();
            if (before.rfind("levitate:", 0) != 0 && after.rfind("levitate:", 0) != 0) return;
            onBlockChanged(region, game::globalPos(region, ev.pos()), before, after, mLater);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { tick(); }));
        mSpeedListener = kinetics::onSpeedChanged(kSequencedGearshift, [](kinetics::SpeedChange const& c) {
            if (!c.removed) sequencerSpeedChanged(c.pos, c.prevTheoretical, c.theoretical);
        });
        installPlacementHooks();
        installComparatorOutput();
    }

    void uninstall() {
        uninstallComparatorOutput();
        uninstallPlacementHooks();
        kinetics::removeListener(mSpeedListener);
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        saveTracked();
    }

private:
    void tick() {
        ++mTicks;
        auto later = std::move(mLater);
        mLater.clear();
        for (auto& fn : later) {
            try {
                fn();
            } catch (std::exception const& e) {
                log().error("transmission: deferred update failed: {}", e.what());
            }
        }
        tickBlocks();
        // companions changing dimension get the gauges of the new one
        if (mTicks % 20 == 0) {
            auto level = ll::service::getLevel();
            if (!level) return;
            level->forEachPlayer([this](Player& p) {
                if (!net::isCompanion(p)) return true;
                int  dim = static_cast<int>(p.getDimensionId());
                auto key = p.getOrCreateUniqueID().rawID;
                auto it  = mPlayerDims.find(key);
                if (it != mPlayerDims.end() && it->second != dim) gaugeSyncTo(p);
                mPlayerDims[key] = dim;
                return true;
            });
        }
    }

    std::vector<ll::event::ListenerPtr>        mListeners;
    std::vector<std::function<void()>>         mLater;
    std::unordered_map<std::int64_t, int>      mPlayerDims;
    kinetics::ListenerId                       mSpeedListener = 0;
    std::uint64_t                              mTicks         = 0;
};

} // namespace lv::feature::transmission

LV_REGISTER_MODULE(lv::feature::transmission::TransmissionModule);
