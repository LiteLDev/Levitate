// Client mod part of the trains unit (U27): bogey wheels turn on moving carriages (Create's
// BogeyRenderer: the wheel angle follows the distance the carriage travels along its bogey axis,
// backwards when it reverses).
//
// A carried bogey is shown by a display entity (a falling block, U4). While it is drawn, its block
// is swapped for the animation step `levitate:anim` 0..3 (the brass wheel crosses turned in 22.5
// degree steps, tools/models/bogeys.ps1) and put back afterwards, as the actors unit does for the
// harvester and roller. Vanilla clients draw step 0 (still wheels).

#ifdef LV_CLIENT

#include <array>
#include <cmath>
#include <numbers>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/SynchedActorDataEntityWrapper.h"
#include "mc/world/actor/item/FallingBlockActor.h"
#include "mc/world/level/BlockPalette.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "client/contraption/Display.h"
#include "feature/contraption/ActorsExt.h"
#include "feature/contraption/Internal.h"
#include "feature/trains/Trains.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::trains {

namespace {

namespace ext = ::lv::feature::contraption::actors_ext;
namespace ft  = ::lv::feature::trains;

struct Wheel {
    Vec3   last{};
    double angle = 0; ///< degrees, about the axle
    bool   seen  = false;
};

struct State {
    std::unordered_map<std::int64_t, Wheel> wheels; ///< by display runtime id
    bool                                     logged = false;
};
State& state() {
    static State s;
    return s;
}

/// The block shown with animation step `step` (cached per base block).
Block const* stepped(Block const& base, int step) {
    static std::unordered_map<Block const*, std::array<Block const*, 4>> cache;
    auto [it, fresh] = cache.try_emplace(&base);
    if (fresh)
        for (int i = 0; i < 4; ++i) it->second[i] = ext::withStates(base, {{"levitate:anim", std::int64_t{i}}});
    return it->second[step & 3];
}

std::int64_t intState(Block const& block, char const* name) {
    auto s  = ext::statesOf(block);
    auto it = s.find(name);
    if (it == s.end()) return 0;
    auto const* v = std::get_if<std::int64_t>(&it->second);
    return v ? *v : 0;
}

/// Turns the wheels by the distance travelled along the bogey's axis since the last frame.
double advance(Actor& entity, Block const& base, bool large, Vec3 const& renderPos, Vec3 const& cameraTarget, double residualDegrees) {
    if (state().wheels.size() > 4096) state().wheels.clear();
    auto& w = state().wheels[static_cast<std::int64_t>(entity.getRuntimeID().rawID)];
    // where it is drawn (camera relative when far from the entity's own position)
    Vec3   own = entity.getPosition();
    Vec3   pos = renderPos;
    double far = std::abs(pos.x - own.x) + std::abs(pos.y - own.y) + std::abs(pos.z - own.z);
    if (far > 24) pos = Vec3{renderPos.x + cameraTarget.x, renderPos.y + cameraTarget.y, renderPos.z + cameraTarget.z};
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return w.angle;
    if (!w.seen) {
        w.seen = true;
        w.last = pos;
        return w.angle;
    }
    double dx = pos.x - w.last.x, dz = pos.z - w.last.z;
    w.last = pos;
    if (std::abs(dx) + std::abs(dz) > 4) return w.angle; // a teleport (a display spawned again), not travel
    // the bogey's travel axis: model x, turned by bogey_axis and the display's residual yaw
    double base0 = intState(base, "levitate:bogey_axis") == 1 ? 90.0 : 0.0;
    double yaw   = (base0 + residualDegrees) * std::numbers::pi / 180.0;
    double ax = std::cos(yaw), az = -std::sin(yaw); // (1, 0, 0) turned right-handed about +Y
    double along  = dx * ax + dz * az;
    double radius = (large ? 7.0 : 6.5) / 16.0;
    // rolling forwards along +x turns the wheel the negative way about its axle (z)
    w.angle = std::fmod(w.angle - along * 180.0 / (std::numbers::pi * radius), 360.0);
    if (w.angle < 0) w.angle += 360.0;
    return w.angle;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    BogeyWheelRenderHook,
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
    auto&        falling = static_cast<FallingBlockActor&>(entity);
    Block const* base    = nullptr;
    if (auto const& ser = falling.mFallingBlockSerId; ser) {
        if (auto b = Block::tryGetFromRegistry(*ser)) base = b.as_ptr();
    }
    if (!base) {
        auto variant = static_cast<uint>(entity.mEntityData->getInt(static_cast<ushort>(ActorDataIDs::Variant)));
        if (auto b = Block::tryGetFromRegistry(variant)) base = b.as_ptr();
        // a remote server's block network ids: resolved through the level's block palette
        if (!base || base->isAir()) base = &entity.getLevel().getBlockPalette().getBlock(variant);
    }
    if (!base) return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    auto const& id = base->getTypeName();
    bool        large = id == ft::kLargeBogey;
    if (!large && id != ft::kSmallBogey) return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    // the yaw the display adds to the quarter turns its block shows
    double                     residual = 0;
    contraption::DisplayPose   pose;
    if (contraption::displayPose(entity, context.mFrameAlpha, pose)) {
        if (pose.axis == 1) residual = pose.degrees;
    } else if (feature::contraption::displayAxis(entity.getRotation().x) == 1) {
        residual = entity.getRotation().y;
    }
    double angle = advance(entity, *base, large, pos, cameraTargetPos, residual);
    // the wheel crosses are 4-fold symmetric: step k shows them turned by k * 22.5 degrees
    int          step  = static_cast<int>(std::floor(std::fmod(angle, 90.0) / 22.5)) & 3;
    Block const* shown = stepped(*base, step);
    if (!state().logged) {
        state().logged = true;
        log().info("trains client: turning bogey wheels ({})", id);
    }
    if (!shown || shown == base) return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    falling.setFallingBlock(*shown, false);
    origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    falling.setFallingBlock(*base, false);
}

class BogeyRendererModule {
public:
    void install() {
        BogeyWheelRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            state().wheels.clear();
            state().logged = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        BogeyWheelRenderHook::unhook();
        state().wheels.clear();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::trains

LV_REGISTER_MODULE(lv::client::trains::BogeyRendererModule);

#endif // LV_CLIENT
