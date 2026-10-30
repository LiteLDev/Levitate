// Client mod: Create's ClipboardValueSettingsHandler while a clipboard is held. Looking at a block
// whose settings the clipboard can copy or paste draws a white outline around it and the
// "Clipboard Actions" tip next to the crosshair ("Right-Click to Copy settings", "Left-Click to
// Paste settings", or "Right-Click to Copy entries from this Clipboard" on a placed clipboard).
// What is possible is asked from the server ("display.clipboard_query", answered on
// "display.clipboard_tip" by src/feature/display/Clipboard.cpp), which simulates the copy and the
// paste like Create's ClipboardCloneable behaviours do.

#ifdef LV_CLIENT

#include <mutex>
#include <optional>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::display {

namespace net = feature::net;
using Json    = nlohmann::json;

namespace {

constexpr char kClipboardId[] = "levitate:clipboard";

struct Target {
    int      dim = 0;
    BlockPos pos;
    bool operator==(Target const& o) const { return dim == o.dim && pos == o.pos; }
};

struct State {
    std::mutex            mutex;
    std::optional<Target> target;  ///< the block looked at while holding a clipboard
    Json                  answer;  ///< the server's last answer for `target`
    int                   cooldown = 0;
    bool                  logged   = false;
};
State& st() {
    static State s;
    return s;
}

Json posJson(Target const& t) { return Json::array({t.dim, t.pos.x, t.pos.y, t.pos.z}); }

bool holdingClipboard(LocalPlayer& p) {
    auto const& held = p.getSelectedItem();
    return !held.isNull() && held.getTypeName() == kClipboardId;
}

/// Client tick: follows the crosshair and asks the server when the target changes (and every 10
/// ticks, the clipboard's contents may change).
void tick() {
    auto* player = currentPlayer();
    auto& s      = st();
    std::optional<Target> now;
    if (player && net::client::connected() && holdingClipboard(*player) && !player->isSpectator()) {
        auto const& hit = player->getLevel().getHitResult();
        if (hit.mType == HitResultType::Tile) now = Target{static_cast<int>(player->getDimensionId()), hit.mBlock};
    }
    std::lock_guard lock(s.mutex);
    bool changed = now != s.target;
    s.target     = now;
    if (changed) s.answer = Json();
    if (!now) return;
    if (s.cooldown > 0) --s.cooldown;
    if (!changed && s.cooldown > 0) return;
    s.cooldown = 10;
    net::client::send("display.clipboard_query", {{"pos", posJson(*now)}});
}

/// The answer for the current target: (copy, paste, other clipboard), or nullopt.
std::optional<std::tuple<bool, bool, bool>> actions() {
    auto&           s = st();
    std::lock_guard lock(s.mutex);
    if (!s.target || !s.answer.is_object() || s.answer.value("pos", Json()) != posJson(*s.target)) return std::nullopt;
    bool copy = s.answer.value("copy", false), paste = s.answer.value("paste", false), other = s.answer.value("other", false);
    if (!copy && !paste && !other) return std::nullopt;
    return std::tuple{copy, paste, other};
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

void bar(Tessellator& t, Vec3 lo, Vec3 hi) {
    auto quad = [&](std::array<Vec3, 4> const& v) {
        for (auto const& p : v) t.vertex(p.x, p.y, p.z);
    };
    quad({Vec3{lo.x, hi.y, lo.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, hi.y, lo.z}});
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{lo.x, lo.y, hi.z}});
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, hi.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, lo.y, lo.z}});
    quad({Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}});
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, lo.y, hi.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{lo.x, hi.y, lo.z}});
    quad({Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, lo.y, hi.z}});
}

/// Create's drawCustomBlockSelection: the block's outline, white (instead of the vanilla one).
void renderOutline(BaseActorRenderContext& ctx) {
    auto a = actions();
    if (!a) return;
    std::optional<Target> target;
    {
        std::lock_guard lock(st().mutex);
        target = st().target;
    }
    auto* player = currentPlayer();
    if (!player || !target || static_cast<int>(player->getDimensionId()) != target->dim) return;
    auto&        region = player->getDimensionBlockSource();
    Block const& block  = region.getBlock(target->pos);
    AABB         buffer;
    AABB const&  box = block.getBlockType().getOutline(block, region, target->pos, buffer);
    std::unique_ptr<BaseActorRenderContext::Impl> const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3        cam = impl->mCameraTargetPosition;
    float const g   = 0.003f, w = 1.f / 64;
    Vec3        lo{box.min.x - cam.x - g, box.min.y - cam.y - g, box.min.z - cam.z - g};
    Vec3        hi{box.max.x - cam.x + g, box.max.y - cam.y + g, box.max.z - cam.z + g};
    auto&       tess = ctx.mScreenContext.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (float y : {lo.y, hi.y}) {
        bar(tess, Vec3{lo.x - w, y - w, lo.z - w}, Vec3{hi.x + w, y + w, lo.z + w});
        bar(tess, Vec3{lo.x - w, y - w, hi.z - w}, Vec3{hi.x + w, y + w, hi.z + w});
        bar(tess, Vec3{lo.x - w, y - w, lo.z - w}, Vec3{lo.x + w, y + w, hi.z + w});
        bar(tess, Vec3{hi.x - w, y - w, lo.z - w}, Vec3{hi.x + w, y + w, hi.z + w});
    }
    for (float x : {lo.x, hi.x})
        for (float z : {lo.z, hi.z}) bar(tess, Vec3{x - w, lo.y, z - w}, Vec3{x + w, hi.y, z + w});
    MeshHelpers::renderMeshImmediately(ctx.mScreenContext, tess, material(), OffscreenCaptureDescription{});
}

/// Create's ValueSettingsClient.showHoverTip: the lines next to the crosshair.
void renderTip(MinecraftUIRenderContext& ui) {
    auto a = actions();
    if (!a) return;
    auto [copy, paste, other] = *a;
    std::vector<std::string> lines{ctr("create.clipboard.actions", "Clipboard Actions")};
    auto rmb = "§7" + ctr("lv.display.key.use", "Right-Click") + "§f";
    auto lmb = "§7" + ctr("lv.display.key.attack", "Left-Click") + "§f";
    if (other) lines.push_back(ctr("create.clipboard.copy_other_clipboard", "{} to Copy entries from this Clipboard", rmb));
    else {
        if (copy) lines.push_back(ctr("create.clipboard.to_copy", "{} to Copy settings", rmb));
        if (paste) lines.push_back(ctr("create.clipboard.to_paste", "{} to Paste settings", lmb));
    }
    auto [sw, sh] = screen::guiSize();
    screen::Canvas c(ui);
    c.tooltip(sw / 2 + 10, sh / 2 + 10, lines);
    auto& s = st();
    if (!s.logged) {
        s.logged = true;
        log().info("clipboard: showing the Clipboard Actions tip ({} lines)", lines.size());
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    ClipboardOutlineHook,
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
        renderOutline(renderContext);
    } catch (...) {
    }
}

class ClipboardClientModule {
public:
    void install() {
        net::client::on("display.clipboard_tip", [](Json const& msg) {
            std::lock_guard lock(st().mutex);
            st().answer = msg;
        });
        ClipboardOutlineHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { tick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            if (screen::current() || ev.screenView().getScreenName() != "hud_screen") return;
            renderTip(ev.uiRenderContext());
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(st().mutex);
            st().target.reset();
            st().answer = Json();
        }));
    }
    void uninstall() {
        ClipboardOutlineHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::display

LV_REGISTER_MODULE(lv::client::display::ClipboardClientModule);

#endif // LV_CLIENT
