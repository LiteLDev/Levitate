// Client mod: contraption blocks turn smoothly.
//
// The server shows each contraption block as a display entity (a falling block entity placed at
// the block's rotated center, showing the block rotated by the nearest quarter turn). Vanilla
// clients draw the block axis-aligned; here the block is turned by the remaining angle about its
// center, so the structure is drawn at its exact transform. The angle comes from the
// contraption's motion at this frame's partial tick (ContraptionClient.cpp, which also moves the
// display entities); a display the client doesn't know yet falls back to the angle the server
// encodes in the entity's own rotation (see feature/contraption/Internal.h).

#ifdef LV_CLIENT

#include <cmath>

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"

#include "client/contraption/Display.h"
#include "feature/contraption/Internal.h"
#include "mod/Module.h"

namespace lv::client::contraption {

namespace {

bool turnedDisplay(Actor& actor, int& axis, float& degrees) {
    if (actor.getEntityTypeId() != ActorType::FallingBlock) return false;
    auto rot = actor.getRotation();
    axis     = feature::contraption::displayAxis(rot.x);
    if (axis < 0) return false;
    degrees = rot.y;
    while (degrees > 180.f) degrees -= 360.f;
    while (degrees < -180.f) degrees += 360.f;
    return std::abs(degrees) > 0.01f;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    ContraptionDisplayRenderHook,
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
    if (entity.getEntityTypeId() != ActorType::FallingBlock) return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    int         axis    = 0;
    float       degrees = 0;
    Vec3        at      = pos;
    DisplayPose pose;
    if (displayPose(entity, context.mFrameAlpha, pose)) {
        // from the straight line between its places onto the contraption's arc
        at      = Vec3{pos.x + float(pose.shift.x), pos.y + float(pose.shift.y), pos.z + float(pose.shift.z)};
        axis    = pose.axis;
        degrees = pose.degrees;
        if (std::abs(degrees) <= 0.01f) return origin(context, entity, cameraTargetPos, at, rot, ignoreLighting);
    } else if (!turnedDisplay(entity, axis, degrees)) {
        return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    }
    ScreenContext& screen = context.mScreenContext;
    mce::Camera&   camera = screen.camera;
    MatrixStack&   stack  = camera.worldMatrixStack;
    auto           ref    = stack.push(false);
    Matrix*        mat    = ref.mat;
    glm::mat4&     m      = mat->_m;
    glm::vec3 pivot(at.x, at.y + 0.5f, at.z);
    glm::vec3 dir(axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f);
    m = glm::translate(m, pivot);
    m = glm::rotate(m, glm::radians(degrees), dir);
    m = glm::translate(m, -pivot);
    origin(context, entity, cameraTargetPos, at, rot, ignoreLighting);
}

class RenderModule {
public:
    void install() { ContraptionDisplayRenderHook::hook(); }
    void uninstall() { ContraptionDisplayRenderHook::unhook(); }
};

} // namespace lv::client::contraption

LV_REGISTER_MODULE(lv::client::contraption::RenderModule);

#endif // LV_CLIENT
