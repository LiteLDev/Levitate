// Client mod: contraption blocks turn smoothly.
//
// The server shows each contraption block as a display entity (a falling block entity placed at
// the block's rotated center, showing the block rotated by the nearest quarter turn). It encodes
// the rotation axis and the remaining angle (-45..45 degrees) in the entity's own rotation
// (see feature/contraption/Internal.h). Vanilla clients draw the
// block axis-aligned; here the block is turned by that remaining angle about its center, so the
// structure is drawn at its exact transform.

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
    int   axis    = 0;
    float degrees = 0;
    if (!turnedDisplay(entity, axis, degrees)) return origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
    ScreenContext& screen = context.mScreenContext;
    mce::Camera&   camera = screen.camera;
    MatrixStack&   stack  = camera.worldMatrixStack;
    auto           ref    = stack.push(false);
    Matrix*        mat    = ref.mat;
    glm::mat4&     m      = mat->_m;
    glm::vec3 pivot(pos.x, pos.y + 0.5f, pos.z);
    glm::vec3 dir(axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f);
    m = glm::translate(m, pivot);
    m = glm::rotate(m, glm::radians(degrees), dir);
    m = glm::translate(m, -pivot);
    origin(context, entity, cameraTargetPos, pos, rot, ignoreLighting);
}

class RenderModule {
public:
    void install() { ContraptionDisplayRenderHook::hook(); }
    void uninstall() { ContraptionDisplayRenderHook::unhook(); }
};

} // namespace lv::client::contraption

LV_REGISTER_MODULE(lv::client::contraption::RenderModule);

#endif // LV_CLIENT
