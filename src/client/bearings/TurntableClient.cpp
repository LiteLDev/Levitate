// Client mod: the view of the local player standing on a turning turntable turns with it
// (Create's TurntableHandler#gameRenderFrame: speed · 2/3 per tick, less away from the centre).
// The server carries the player around the centre (feature/bearings/Turntable.cpp); only the
// client can turn a player's own view.

#ifdef LV_CLIENT

#include <algorithm>
#include <cmath>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "client/Client.h"
#include "client/kinetics/KineticsClient.h"
#include "mod/Module.h"

namespace lv::client::bearings {

namespace {

void turnView() {
    auto* player = currentPlayer();
    if (!player || !player->isOnGround()) return;
    auto     feet = player->getFeetPos();
    BlockPos on{int(std::floor(feet.x)), int(std::floor(feet.y - 0.2f)), int(std::floor(feet.z))};
    auto&    region = player->getDimensionBlockSource();
    if (region.getBlock(on).getTypeName() != "levitate:turntable") return;
    auto synced = kinetics::syncedSpeed(static_cast<int>(player->getDimensionId()), on.x, on.y, on.z);
    if (!synced || synced->speed == 0) return;
    // speed · 2/3 per tick, scaled per frame by the partial tick in Create: about a third per tick
    float speed = synced->speed * (2 / 3.f) * 0.5f;
    float dx = feet.x - (on.x + .5f), dz = feet.z - (on.z + .5f);
    // (Create's half-height plate puts the feet at the centre's height: only the horizontal offset counts)
    float len = std::sqrt(dx * dx + dz * dz);
    if (len > 1 / 4.f) speed *= std::clamp((1 / 2.f - len) * 2, 0.f, 1.f);
    if (speed == 0) return;
    Vec2& rot = player->mBuiltInComponents->mActorRotationComponent->mRot.get();
    rot       = Vec2{rot.x, rot.y - speed};
    player->setYBodyRotation(player->getYBodyRotation() - speed);
}

class TurntableClientModule {
public:
    void install() {
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { turnView(); });
    }
    void uninstall() { ll::event::EventBus::getInstance().removeListener(mTick); }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace

} // namespace lv::client::bearings

LV_REGISTER_MODULE(lv::client::bearings::TurntableClientModule);

#endif // LV_CLIENT
