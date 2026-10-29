// Client mod part of the actors unit (U22):
// - actor animations on moving contraptions: the harvester's blades and the roller's drum turn
//   with the distance the block travels (Create: angle = animation speed x time). A carried
//   block is shown by a display entity (a falling block, U4); while it is drawn, its block is
//   swapped for the animation step (`levitate:anim` 0..3, a quarter turn of the 90-degree
//   symmetric part in 22.5-degree steps) and put back afterwards.
// - empty-hand right-click into the air: the server can't see it, so it is sent as
//   `actors.use` and the server casts the player's look against contraption controls / seats.

#ifdef LV_CLIENT

#include <atomic>
#include <cmath>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/SynchedActorDataEntityWrapper.h"
#include "mc/world/actor/item/FallingBlockActor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "feature/actors/Actors.h"
#include "feature/contraption/ActorsExt.h"
#include "feature/contraption/Internal.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::actors {

namespace {

namespace fa  = ::lv::feature::actors;
namespace ext = ::lv::feature::contraption::actors_ext;
namespace net = ::lv::feature::net;

struct Spin {
    Vec3   last{};
    double angle = 0; ///< degrees
    bool   seen  = false;
    int    frames = 0;
};

struct State {
    std::unordered_map<std::int64_t, Spin> spins;
    std::atomic<bool>                      useRequested{false};
    bool                                   loggedAnim = false;
    bool                                   loggedSeen = false;
    bool                                   loggedTrack = false;
};
State& state() {
    static State s;
    return s;
}

bool isAnimated(std::string const& id) { return id == fa::kHarvester || id == fa::kRoller; }

/// The block shown with the animation step `step` (cached per base block).
Block const* animated(Block const& base, int step) {
    static std::unordered_map<Block const*, std::array<Block const*, 4>> cache;
    auto [it, fresh] = cache.try_emplace(&base);
    if (fresh) {
        for (int i = 0; i < 4; ++i) it->second[i] = ext::withStates(base, {{"levitate:anim", std::int64_t{i}}});
    }
    return it->second[step & 3];
}

/// Create's harvester / roller speed: the block's movement (degrees per block travelled).
double advance(Actor& entity, std::string const& id, Vec3 const& renderPos, Vec3 const& cameraTarget) {
    auto& spin = state().spins[static_cast<std::int64_t>(entity.getRuntimeID().rawID)];
    // where it is drawn (the display entity's own position lags behind the drawn one): the render
    // position, camera relative when it is far from the entity
    Vec3 base = entity.getPosition();
    Vec3 pos  = renderPos;
    double far = std::abs(pos.x - base.x) + std::abs(pos.y - base.y) + std::abs(pos.z - base.z);
    if (far > 24) pos = Vec3{renderPos.x + cameraTarget.x, renderPos.y + cameraTarget.y, renderPos.z + cameraTarget.z};
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) pos = base;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return spin.angle;
    if (++spin.frames == 300 || spin.frames == 3000) {
        log().info("actors client: {} at {:.2f} {:.2f} {:.2f}, turned {:.1f} degrees", id, pos.x, pos.y, pos.z, spin.angle);
    }
    if (!spin.seen) {
        spin.seen = true;
        spin.last = pos;
        return spin.angle;
    }
    double dx = pos.x - spin.last.x, dy = pos.y - spin.last.y, dz = pos.z - spin.last.z;
    double d  = std::sqrt(dx * dx + dy * dy + dz * dz);
    spin.last = pos;
    if (d > 4) return spin.angle; // a teleport, not movement
    // harvester: Create turns its blades by the animation speed (1000 per block per tick) / 20
    // per tick, i.e. 50 degrees per block; the roller's drum rolls on the ground (radius 4 px)
    double perBlock = id == fa::kRoller ? 360.0 / (2 * 3.14159265 * 0.25) : 50.0;
    spin.angle      = std::fmod(spin.angle + d * perBlock, 360.0);
    return spin.angle;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    ActorsAnimationRenderHook,
    ll::memory::HookPriority::Normal,
    ActorRenderDispatcher,
    &ActorRenderDispatcher::render,
    void,
    BaseActorRenderContext& context,
    Actor&                  entity,
    Vec3 const&             cameraTargetPos,
    Vec3 const&             pos,
    Vec2 const&             rot,
    bool                    ignoreLighting
) {
    if (entity.getEntityTypeId() != ActorType::FallingBlock || feature::contraption::displayAxis(entity.getRotation().x) < 0) {
        return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    }
    auto& falling = static_cast<FallingBlockActor&>(entity);
    Block const* base = nullptr;
    // the block the entity shows: its serialized block (client copy), else the synced variant
    if (auto const& ser = falling.mFallingBlockSerId; ser) {
        if (auto b = Block::tryGetFromRegistry(*ser)) base = b.as_ptr();
    }
    if (!base) {
        auto variant = static_cast<uint>(entity.mEntityData->getInt(static_cast<ushort>(ActorDataIDs::Variant)));
        if (auto b = Block::tryGetFromRegistry(variant)) base = b.as_ptr();
    }
    if (!state().loggedSeen) {
        state().loggedSeen = true;
        log().info("actors client: display entity shows {}", base ? base->getTypeName() : std::string("?"));
    }
    if (!base || !isAnimated(base->getTypeName())) return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    double angle = advance(entity, base->getTypeName(), pos, cameraTargetPos);
    if (!state().loggedTrack) {
        state().loggedTrack = true;
        auto const* probe   = animated(*base, 1);
        log().info("actors client: tracking {} (step 1 block {})", base->getTypeName(), probe == base ? "unchanged" : "found");
    }
    int    step  = static_cast<int>(std::floor(angle / 22.5)) & 3;
    auto const* shown = animated(*base, step);
    if (!shown || shown == base) return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    auto& s = state();
    if (!s.loggedAnim) {
        s.loggedAnim = true;
        log().info("actors client: animating {}", base->getTypeName());
    }
    falling.setFallingBlock(*shown, false);
    origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    falling.setFallingBlock(*base, false);
}

class ActorsClientModule {
public:
    // Client input listeners are added at mod load, before the game's threads run (hooking them
    // while the client is running can deadlock the game).
    void load() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::MouseInputEvent>([](ll::event::MouseInputEvent& ev) {
            if (ev.actionButtonId() == 2 && ev.buttonData() == 1 && !lv::client::screen::current()) state().useRequested = true;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { onTick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { state().spins.clear(); }));
    }
    void install() { ActorsAnimationRenderHook::hook(); }
    void uninstall() { ActorsAnimationRenderHook::unhook(); }

private:
    static void onTick() {
        auto& s = state();
        if (s.spins.size() > 4096) s.spins.clear();
        if (!s.useRequested.exchange(false)) return;
        auto* player = currentPlayer();
        if (!player || !net::client::connected()) return;
        // only empty-hand clicks into the air: block clicks and item uses reach the server anyway
        if (!player->getSelectedItem().isNull()) return;
        if (player->getLevel().getHitResult().mType == HitResultType::Tile) return;
        net::client::send("actors.use", net::Json::object());
    }

    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::actors

LV_REGISTER_MODULE(lv::client::actors::ActorsClientModule);

#endif // LV_CLIENT
