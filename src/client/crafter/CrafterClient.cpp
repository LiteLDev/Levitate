#ifdef LV_CLIENT

// Client mod: the items of mechanical crafters (Create's MechanicalCrafterRenderer).
//
// The server sends each crafter's state on `crafter.state` (phase, countdown, speed, slot item,
// grouped items, facing, arrow) whenever it changes (Create's sendData). Every frame the items are
// drawn on the crafter's front: the slot item while idle, the grouped grid while assembling,
// sliding towards the next crafter while exporting, the result growing while crafting. The
// countdown is advanced locally between updates at the crafter's speed, as Create's client does.
// The saw blade and the drill head turn through the kinetic renderer (their KineticSpec rotor).

#include <cmath>
#include <map>
#include <mutex>

#include "glm/gtc/matrix_transform.hpp"
#include "nlohmann/json.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ItemRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"

#include "client/Client.h"
#include "core/crafter/Crafter.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::crafter {

namespace {

namespace cr = core::crafter;
namespace k  = core::kinetics;
using Json   = nlohmann::json;

struct GridItem {
    int       x = 0, y = 0;
    ItemStack stack;
};

struct Crafter {
    cr::Phase             phase     = cr::Phase::Idle;
    int                   countDown = 0;
    float                 speed     = 0;
    k::Face               facing    = k::Face::South;
    cr::Pointing          pointing  = cr::Pointing::Up;
    bool                  covered   = false;
    ItemStack             slot;
    std::vector<GridItem> grid;
    double                receivedAt = 0; ///< level tick (+ partial) of the update
};

struct State {
    std::mutex                                      mutex;
    std::map<std::tuple<int, int, int, int>, Crafter> crafters; ///< (dim, x, y, z)
    bool                                            loggedDraw = false;
};
State& state() {
    static State s;
    return s;
}

double gNow = 0; // last frame time (ticks)

ItemStack stackOf(std::string const& id) {
    if (id.empty()) return feature::common::items::empty();
    auto s = feature::common::items::make(id);
    return s ? *s : feature::common::items::empty();
}

void onState(Json const& msg) {
    auto const& p = msg.value("pos", Json::array());
    if (!p.is_array() || p.size() != 4) return;
    Crafter c;
    c.phase      = cr::phaseFromName(msg.value("phase", std::string("IDLE")));
    c.countDown  = msg.value("countDown", 0);
    c.speed      = msg.value("speed", 0.f);
    c.facing     = static_cast<k::Face>(std::clamp(msg.value("facing", 3), 2, 5));
    c.pointing   = static_cast<cr::Pointing>(std::clamp(msg.value("pointing", 0), 0, 3));
    c.covered    = msg.value("covered", false);
    c.slot       = stackOf(msg.value("slot", std::string()));
    c.receivedAt = gNow;
    for (auto const& e : msg.value("grid", Json::array())) {
        if (!e.is_array() || e.size() != 3) continue;
        c.grid.push_back({e[0].get<int>(), e[1].get<int>(), stackOf(e[2].get<std::string>())});
    }
    std::lock_guard lock(state().mutex);
    state().crafters[{p[0].get<int>(), p[1].get<int>(), p[2].get<int>(), p[3].get<int>()}] = std::move(c);
}

glm::vec3 vec(k::Offset o) { return {float(o.x), float(o.y), float(o.z)}; }

/// Draws one item icon centred at `centre` on the plane spanned by `right` / `up` (facing `normal`).
void drawItem(BaseActorRenderContext& ctx, ItemRenderer& renderer, ItemStack const& stack, glm::vec3 centre, glm::vec3 right, glm::vec3 up,
              glm::vec3 normal, float size) {
    if (feature::common::items::isEmpty(stack) || size <= 0.01f) return;
    auto& world = ctx.mScreenContext.camera.worldMatrixStack.get();
    auto  ref   = world.push(false);
    glm::mat4 basis{1.f};
    basis[0] = glm::vec4(right, 0.f);
    basis[1] = glm::vec4(-up, 0.f); // GUI y grows downwards
    basis[2] = glm::vec4(-normal, 0.f);
    glm::mat4 m = glm::translate(glm::mat4{1.f}, centre) * basis * glm::scale(glm::mat4{1.f}, glm::vec3{size / 16.f, size / 16.f, size / 16.f});
    ref.mat->_m.get() = ref.mat->_m.get() * m;
    renderer.renderGuiItemNew(ctx, stack, 0, -8.f, -8.f, false, 1.f, 1.f, 1.f, 0);
}

void renderCrafterItems(BaseActorRenderContext& ctx) {
    auto* ci = lv::client::currentClient();
    if (!ci) return;
    auto* level    = ci->getLevel();
    auto* player   = ci->getLocalPlayer();
    auto* renderer = ci->getItemRenderer();
    if (!level || !player || !renderer) return;
    double now = static_cast<double>(level->getCurrentTick().tickID) + ctx.mFrameAlpha;
    gNow       = now;
    int dim    = static_cast<int>(player->getDimensionId());

    std::vector<std::pair<std::tuple<int, int, int, int>, Crafter>> todo;
    {
        std::lock_guard lock(state().mutex);
        for (auto const& [pos, c] : state().crafters) {
            if (std::get<0>(pos) != dim) continue;
            if (c.phase == cr::Phase::Idle && feature::common::items::isEmpty(c.slot)) continue;
            if (c.phase == cr::Phase::Waiting) continue;
            todo.emplace_back(pos, c);
        }
    }
    if (todo.empty()) return;
    Vec3 const cam = ctx.mImpl.get()->mCameraTargetPosition;
    for (auto const& [pos, c] : todo) {
        auto [d, x, y, z] = pos;
        glm::vec3 normal  = vec(k::offsetOf(c.facing));
        glm::vec3 up{0.f, 1.f, 0.f};
        glm::vec3 right  = glm::cross(up, normal);
        glm::vec3 centre = glm::vec3{x - float(cam.x) + .5f, y - float(cam.y) + .5f, z - float(cam.z) + .5f} + normal * .51f;
        // the countdown runs on between updates (Create's client ticks it too)
        float elapsed   = static_cast<float>(std::max(0.0, now - c.receivedAt));
        float countDown = static_cast<float>(c.countDown) - static_cast<float>(cr::countDownSpeed(c.speed)) * elapsed;

        if (c.phase == cr::Phase::Idle || c.phase == cr::Phase::Accepting) {
            drawItem(ctx, *renderer, c.slot, centre, right, up, normal, .5f);
            for (auto const& g : c.grid) drawItem(ctx, *renderer, g.stack, centre, right, up, normal, .5f);
            continue;
        }
        if (c.phase == cr::Phase::Crafting || c.phase == cr::Phase::Inserting) {
            float progress = std::clamp((2000.f - countDown) / 2000.f, 0.f, 1.f);
            float size     = c.phase == cr::Phase::Inserting ? .5f : .25f + .25f * progress;
            for (auto const& g : c.grid) {
                glm::vec3 at = centre + right * (float(g.x) * .5f) + up * (float(g.y) * .5f);
                drawItem(ctx, *renderer, g.stack, at, right, up, normal, size);
            }
            continue;
        }
        // assembling / exporting: the grouped grid spread over the face (half a block apart),
        // sliding one block towards the next crafter while exporting
        glm::vec3 shift{0.f};
        if (c.phase == cr::Phase::Exporting) {
            float progress = std::clamp((1000.f - countDown) / 1000.f, 0.f, 1.f);
            shift          = vec(k::offsetOf(cr::targetDirection(c.facing, c.pointing))) * progress;
        }
        for (auto const& g : c.grid) {
            glm::vec3 at = centre + shift + right * (float(g.x) * .5f) + up * (float(g.y) * .5f);
            drawItem(ctx, *renderer, g.stack, at, right, up, normal, .4f);
        }
    }
    if (!state().loggedDraw) {
        state().loggedDraw = true;
        log().info("crafter client: drawing items of {} crafters", todo.size());
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    CrafterItemsRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);
    if (renderAlphaLayer) return;
    try {
        renderCrafterItems(renderContext);
    } catch (...) {
    }
}

class CrafterClientModule {
public:
    void install() {
        CrafterItemsRenderHook::hook();
        feature::net::client::on("crafter.state", [](Json const& msg) { onState(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(state().mutex);
            state().crafters.clear();
            state().loggedDraw = false;
        });
    }
    void uninstall() {
        CrafterItemsRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::crafter

LV_REGISTER_MODULE(lv::client::crafter::CrafterClientModule);

#endif // LV_CLIENT
