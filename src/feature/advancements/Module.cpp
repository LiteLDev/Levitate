// The module of the advancements unit (U45): world lifecycle, the tick of the triggers.

#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "feature/advancements/Internal.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::advancements {

namespace {

class AdvancementsModule {
public:
    void install() {
        static bool registered = false;
        if (!registered) {
            registered = true;
            game::db::onWorldSave([] { flush(); });
            game::db::onWorldClosed([] {
                resetTriggers();
                resetState();
            });
        }
        installProgress();
        installTriggers();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            try {
                tickTriggers();
            } catch (std::exception const& e) {
                log().error("advancements: tick failed: {}", e.what());
            }
        }));
    }

    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        uninstallTriggers();
        flush();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace

} // namespace lv::feature::advancements

LV_REGISTER_MODULE(lv::feature::advancements::AdvancementsModule);
