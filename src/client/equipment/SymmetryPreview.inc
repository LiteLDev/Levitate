// Create's SymmetryHandler#onRenderWorld in the client mod (U59): every enabled wand of symmetry
// in the hotbar shows its mirror at the mirror block, bobbing (sin(t / 16) / 5, 0.2 above the
// block): one vertical plane, two crossing planes (orthogonal or diagonal) or the four planes of
// the triple mirror. Drawn as framed planes with the outline lines (our own shape, not Create's
// model).

#ifdef LV_CLIENT

#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

#include "ll/api/event/EventBus.h"
// (ClientExitLevelEvent.h has no include guard: ShaperPreview.cpp includes it)
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/render/Outline.h"
#include "core/equipment/Symmetry.h"
#include "feature/common/Items.h"
#include "mod/Module.h"

namespace lv::client::equipment {

namespace {

namespace ce    = lv::core::equipment;
namespace items = lv::feature::common::items;

std::mutex& mirrorMutex() {
    static std::mutex m;
    return m;
}
/// The mirrors to draw (updated on the client tick).
std::vector<ce::Mirror>& shownMirrors() {
    static std::vector<ce::Mirror> v;
    return v;
}

void tickMirrors() {
    std::vector<ce::Mirror> next;
    if (auto* lp = currentPlayer(); lp && inLevel()) {
        auto& inv = lp->getInventory();
        int   dim = static_cast<int>(lp->getDimensionId());
        for (int s = 0; s < 9; ++s) {
            auto const& st = inv.getItem(s);
            if (items::isEmpty(st) || items::idOf(st) != "levitate:wand_of_symmetry") continue;
            auto m = ce::Mirror::fromJson(items::getData(st, "lv:symmetry"));
            if (m.placed && m.enabled && m.dim() == dim) next.push_back(m);
        }
    }
    std::lock_guard lock(mirrorMutex());
    shownMirrors() = std::move(next);
}

void renderMirrors(BaseActorRenderContext& ctx) {
    std::vector<ce::Mirror> mirrors;
    {
        std::lock_guard lock(mirrorMutex());
        mirrors = shownMirrors();
    }
    if (mirrors.empty()) return;
    // Create: yShift = sin(renderTime / 16) / 5 (render time in ticks)
    double ticks = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 20.0;
    double bob   = std::sin(ticks / 16.0) / 5.0 + .2;
    render::Lines lines(ctx);
    auto const    colour = render::Rgb::hex(0xa8e6ff);
    for (auto const& m : mirrors) {
        double cx = m.x + .5, cz = m.z + .5, y0 = m.y + bob, y1 = y0 + 1.0;
        // a vertical frame through the centre along (dx, dz) (half width 0.75)
        auto plane = [&](double dx, double dz) {
            double ax = cx - dx * .75, az = cz - dz * .75, bx = cx + dx * .75, bz = cz + dz * .75;
            lines.line({ax, y0, az}, {bx, y0, bz}, colour);
            lines.line({ax, y1, az}, {bx, y1, bz}, colour);
            lines.line({ax, y0, az}, {ax, y1, az}, colour);
            lines.line({bx, y0, bz}, {bx, y1, bz}, colour);
            lines.line({cx, y0, cz}, {cx, y1, cz}, colour, 1.0 / 64);
        };
        constexpr double d = 0.70710678;
        switch (m.type) {
        case ce::MirrorType::Plane: // orientation 0 flips x: the plane runs along z
            m.orientation == 0 ? plane(0, 1) : plane(1, 0);
            break;
        case ce::MirrorType::Cross:
            if (m.orientation == 0) plane(0, 1), plane(1, 0);
            else plane(d, d), plane(d, -d);
            break;
        case ce::MirrorType::Triple:
            plane(0, 1), plane(1, 0), plane(d, d), plane(d, -d);
            break;
        }
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    SymmetryMirrorRenderHook,
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
        renderMirrors(renderContext);
    } catch (...) {
    }
}

class SymmetryPreviewModule {
public:
    void install() {
        SymmetryMirrorRenderHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { tickMirrors(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(mirrorMutex());
            shownMirrors().clear();
        }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        SymmetryMirrorRenderHook::unhook();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::equipment

LV_REGISTER_MODULE(lv::client::equipment::SymmetryPreviewModule);

#endif // LV_CLIENT
