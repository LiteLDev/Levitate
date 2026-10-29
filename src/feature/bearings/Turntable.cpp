// Turntable (Create's TurntableBlock): a kinetic block turning what stands on it.

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/phys/AABB.h"

#include "feature/bearings/Bearings.h"
#include "feature/kinetics/Kinetics.h"

namespace lv::feature::bearings {

namespace k  = ::lv::feature::kinetics;
namespace ck = ::lv::core::kinetics;

namespace {

ck::KineticSpec turntableSpec() {
    ck::KineticSpec s;
    s.rotationAxis    = [](ck::BlockState const&) -> std::optional<ck::Axis> { return ck::Axis::Y; };
    s.hasShaftTowards = [](ck::BlockState const&, ck::Face f) { return f == ck::Face::Down; };
    s.rotorState      = "levitate:rotor";
    s.rotorModel      = "geometry.levitate.turntable";
    s.rotorTexture    = "levitate_turntable_model";
    return s;
}

LV_REGISTER_KINETIC_BLOCK("levitate:turntable", turntableSpec());

} // namespace

void tickTurntable(BlockSource& region, GP const& pos) {
    float speed = cb::angularSpeed(k::speedAt(pos));
    if (speed == 0) return;
    // entities standing on the plate (the block's top)
    double top = pos.y + 1.0;
    AABB   box{
        Vec3{float(pos.x), float(top - 0.01), float(pos.z)},
        Vec3{float(pos.x + 1), float(top + 0.5), float(pos.z + 1)}
    };
    std::vector<Actor*> found = region.getEntities(box, 0.0f);
    if (auto level = ll::service::getLevel()) {
        level->forEachPlayer([&](Player& p) {
            if (static_cast<int>(p.getDimensionId()) == pos.dim && box.contains(p.getFeetPos())) found.push_back(&p);
            return true;
        });
    }
    cc::Vec3d                         center{pos.x + .5, top, pos.z + .5};
    std::unordered_set<Actor const*> seen;
    for (auto* e : found) {
        if (!e || !e->isAlive() || !seen.insert(e).second) continue;
        if (e->getEntityTypeId() == ActorType::FallingBlock) continue; // contraption displays
        auto feet = e->getFeetPos();
        if (feet.y < top - 0.01 || feet.y > top + 0.5) continue;
        if (e->isPlayer()) {
            // Create (client side for players): the player is carried around the centre, his view
            // doesn't turn
            if (!e->isOnGround()) continue;
            double    step   = std::clamp(speed, -16.f, 16.f);
            cc::Vec3d offset = cc::Vec3d{feet.x, feet.y, feet.z} - center;
            offset.y         = 0;
            cc::Vec3d moved  = cc::rotate(offset, cc::Axis::Y, step);
            cc::Vec3d delta  = moved - offset;
            if (delta.lengthSqr() < 1e-8) continue;
            static_cast<Player*>(e)->teleport(feet + Vec3{float(delta.x), 0.f, float(delta.z)}, e->getDimensionId());
            continue;
        }
        // everything else turns on the spot
        auto rot = e->getRotation();
        float yaw = rot.y - speed;
        while (yaw < -180.f) yaw += 360.f;
        while (yaw >= 180.f) yaw -= 360.f;
        e->mBuiltInComponents->mActorRotationComponent->mRot = Vec2{rot.x, yaw};
        e->setYHeadRot(yaw);
    }
}

} // namespace lv::feature::bearings
