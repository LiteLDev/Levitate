// The assembly unit (U15): deployer, mechanical arm, item application and the sequenced assembly
// engine. Registers its blocks with the kinetic network, capabilities, value boxes, filter slots,
// contraption actors and the game hooks; ticks the machines.

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/assembly/SequencedAssembly.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::assembly {

namespace {
std::uint64_t                       gTick = 0;
std::vector<std::function<void()>>& deferred() {
    static std::vector<std::function<void()>> d;
    return d;
}
} // namespace

namespace detail {
std::uint64_t currentTick() { return gTick; }
void          defer(std::function<void()> fn) { deferred().push_back(std::move(fn)); }
} // namespace detail

namespace {
struct Lifecycle {
    Lifecycle() {
        game::db::onWorldReady([] {
            deployer::onWorldReady();
            arm::onWorldReady();
        });
        game::db::onWorldSave([] {
            deployer::onWorldSave();
            arm::onWorldSave();
        });
        game::db::onWorldClosed([] {
            deployer::onWorldClosed();
            arm::onWorldClosed();
            fakeplayer::forget();
            deferred().clear();
            sequenced::reload();
        });
    }
};
Lifecycle const gLifecycle;
} // namespace

class AssemblyModule {
public:
    void install() {
        deployer::install();
        arm::install();
        sync::install();
        installHooks();
        fakeplayer::install();
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            ++gTick;
            auto work = std::exchange(deferred(), {});
            for (auto& fn : work) {
                try {
                    fn();
                } catch (std::exception const& e) {
                    log().error("assembly: deferred action failed: {}", e.what());
                }
            }
            deployer::tick();
            arm::tick();
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mTick);
        uninstallHooks();
        fakeplayer::uninstall();
        sync::uninstall();
        arm::uninstall();
        deployer::uninstall();
        fakeplayer::removeAll();
    }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace lv::feature::assembly

LV_REGISTER_MODULE(lv::feature::assembly::AssemblyModule);
