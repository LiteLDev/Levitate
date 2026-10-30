// Client trace lines for the e2e harness (client build only): one log line when the top screen
// changes (`client screen: chat_screen`, `client screen: hud_screen`, ...) and one when the server
// teleports the local player (`client teleported to x y z`). tools/e2e/client/lib.ps1 waits for
// them instead of fixed sleeps: the chat really opened before a command is typed, a `/tp` arrived
// before the next click. Both are user-visible actions, never per tick.

#ifdef LV_CLIENT

#include <string>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/entity/components/PlayerPositionModeComponent.h"
#include "mc/network/packet/MovePlayerPacket.h"

#include "client/Client.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::trace {

namespace {
std::string& lastScreen() {
    static std::string s;
    return s;
}

void pollScreen() {
    auto* c = currentClient();
    if (!c) return;
    auto name = c->getScreenName();
    if (name == lastScreen()) return;
    lastScreen() = name;
    log().info("client screen: {}", name);
}
} // namespace

LL_TYPE_INSTANCE_HOOK(
    ClientTeleportTraceHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const& source,
    MovePlayerPacket const&  packet
) {
    origin(source, packet);
    if (packet.mResetPosition != PlayerPositionModeComponent::PositionMode::Teleport) return;
    auto* player = currentPlayer();
    if (!player || player->getRuntimeID() != packet.mPlayerID) return;
    Vec3 const& p = packet.mPos;
    Vec2 const& r = packet.mRot;
    log().info("client teleported to {:.2f} {:.2f} {:.2f} ({:.1f} {:.1f})", p.x, p.y, p.z, r.y, r.x);
}

class ClientTraceModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { pollScreen(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { lastScreen().clear(); }));
        ClientTeleportTraceHook::hook();
    }
    void uninstall() {
        ClientTeleportTraceHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::trace

LV_REGISTER_MODULE(lv::client::trace::ClientTraceModule);

#endif // LV_CLIENT
