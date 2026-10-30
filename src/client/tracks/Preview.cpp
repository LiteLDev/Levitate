// Client: the track placement preview (Create's TrackPlacement.clientTick) and the Ctrl key for a
// maximised turn.
//
// The server tells the client mod which track end the player selected with the track item
// (channel "tracks.selection", src/feature/tracks/TrackSync.cpp). While the player holds the track
// item with a selection and looks at a block, this file runs the same placement rules as the
// server (core/tracks/Placement.h) every frame: it draws both rails of the planned straight runs
// and of the curve, coloured from Create's red (0xEA5C2B) to green (0x95CD41) as the connection
// becomes valid, and shows Create's message in the action bar ("Can Connect ✔" or why not).
// Holding Ctrl (Create's sprint key mapping) plans the maximised turn; the key state is sent to
// the server ("tracks.ctrl"), which uses it for the click (vanilla clients: sprinting).

#ifdef LV_CLIENT

#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "core/tracks/Placement.h"
#include "feature/net/Net.h"
#include "feature/tracks/Tracks.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::tracks {

namespace ct = core::tracks;

namespace {

constexpr int kVkControl = 0x11, kVkLControl = 0xA2, kVkRControl = 0xA3;

struct Selection {
    int          dim = 0;
    ct::TrackEnd end;
    int          maxLength = 32;
};

struct State {
    std::mutex               mutex;
    std::optional<Selection> selection;
    bool                     ctrl = false; ///< Ctrl held (window thread writes, client thread reads)
    // client thread
    float                                  animation = 0; ///< Create's LerpedFloat chasing valid (1) / invalid (0)
    std::chrono::steady_clock::time_point  last{};
    std::string                            message;
    std::chrono::steady_clock::time_point  messageAt{};
    bool                                   loggedPreview = false;
};
State& st() {
    static State s;
    return s;
}

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

float lin(int c) { return static_cast<float>(std::pow(c / 255.0, 2.2)); }

/// Create's Color.mixColors(0xEA5C2B, 0x95CD41, t), as linear vertex colours.
std::array<float, 3> previewColour(float t) {
    auto mix = [t](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
    return {lin(mix(0xEA, 0x95)), lin(mix(0x5C, 0xCD)), lin(mix(0x2B, 0x41))};
}

/// A bar of square cross-section `w` from `a` to `b` (relative to the camera), Create's outliner line.
void bar(Tessellator& t, ct::Vec3d a, ct::Vec3d b, double w, std::array<float, 3> const& c) {
    ct::Vec3d d = b - a;
    if (d.length() < 1e-6) return;
    d              = d.normalize();
    ct::Vec3d side = d.cross({0, 1, 0});
    if (side.length() < 1e-6) side = d.cross({1, 0, 0});
    side         = side.normalize() * (w / 2);
    ct::Vec3d up = side.cross(d).normalize() * (w / 2);
    std::array<ct::Vec3d, 4> ring{side + up, side * -1 + up, side * -1 - up, side - up};
    auto v = [&](ct::Vec3d const& p) { t.vertex(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)); };
    for (int i = 0; i < 4; ++i) {
        int   j     = (i + 1) & 3;
        float shade = (i & 1) ? 0.8f : 1.f;
        t.color(c[0] * shade, c[1] * shade, c[2] * shade, 1.f);
        v(a + ring[i]);
        v(b + ring[i]);
        v(b + ring[j]);
        v(a + ring[j]);
    }
    t.color(c[0], c[1], c[2], 1.f);
    for (auto const& e : {a, b}) {
        v(e + ring[0]);
        v(e + ring[1]);
        v(e + ring[2]);
        v(e + ring[3]);
    }
}

bool holdingTrack(LocalPlayer& p) {
    auto const& held = p.getSelectedItem();
    return !held.isNull() && held.getTypeName() == feature::tracks::kTrackId;
}

int tracksInInventory(LocalPlayer& p) {
    int   n   = 0;
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& s = inv.getItem(i);
        if (!s.isNull() && s.getTypeName() == feature::tracks::kTrackId) n += s.mCount;
    }
    auto const& off = p.getOffhandSlot();
    if (!off.isNull() && off.getTypeName() == feature::tracks::kTrackId) n += off.mCount;
    return n;
}

void showMessage(std::string const& text) {
    auto& s   = st();
    auto  now = std::chrono::steady_clock::now();
    // Create sends it every tick; the action bar keeps a message for a while: resend each second
    if (text == s.message && now - s.messageAt < std::chrono::seconds(1)) return;
    s.message   = text;
    s.messageAt = now;
    if (auto* c = currentClient()) c->getGuiData()->setActionBarMessage(text, std::nullopt);
}

/// The plan for the block the player looks at (Create's tryConnect from the client), or nullopt
/// when there is nothing to preview.
std::optional<ct::PlacementPlan> planFor(LocalPlayer& player, Selection const& sel, bool ctrl) {
    auto const& hit = player.getLevel().getHitResult();
    if (hit.mType != HitResultType::Tile) return std::nullopt;
    auto&    region  = player.getDimensionBlockSource();
    BlockPos clicked = hit.mBlock;
    auto     toCore  = [](BlockPos const& p) { return ct::BlockPos3{p.x, p.y, p.z}; };
    auto     toGame  = [](ct::BlockPos3 const& p) { return BlockPos{p.x, p.y, p.z}; };

    ct::PlacementRequest req;
    ct::BlockPos3        pos2 = toCore(clicked);
    if (auto shape = feature::tracks::shapeAt(region, clicked)) {
        req.shape2 = *shape;
    } else {
        if (!game::replaceable(region, clicked)) {
            static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
            int              face = hit.mFacing;
            if (face < 6) pos2 = pos2.offset(dx[face], dy[face], dz[face]);
        }
        if (!game::replaceable(region, toGame(pos2))) return std::nullopt;
        Vec3 look   = player.getViewVector();
        auto sturdy = [&](int ddx, int ddz) {
            BlockPos side = toGame(pos2.offset(ddx, 0, ddz));
            BlockPos top  = toGame(pos2.offset(ddx, 1, ddz));
            return region.getBlock(side).canProvideFullSupport(1) && !region.getBlock(top).canProvideFullSupport(0);
        };
        req.shape2 = ct::placementShape({look.x, look.y, look.z}, player.getRotation().y, sturdy);
    }
    if (auto existing = feature::tracks::shapeAt(region, toGame(pos2))) req.shape2 = *existing;
    Vec3 look        = player.getViewVector();
    req.pos2         = pos2;
    req.look         = {look.x, look.y, look.z};
    req.maxLength    = sel.maxLength;
    req.maximiseTurn = ctrl;
    auto const& off  = player.getOffhandSlot();
    req.girder       = !off.isNull() && off.getTypeName() == "levitate:metal_girder";
    req.from         = sel.end;
    auto s1          = feature::tracks::shapeAt(region, toGame(sel.end.pos));
    req.fromIsTrack  = s1.has_value();
    auto plan        = ct::planConnection(req, s1.value_or(ct::Shape::ZO));
    if (plan.valid && !player.isCreative()) {
        int needed = plan.curve ? plan.curve->trackItemCost() : 0;
        for (auto const& piece : plan.straight)
            if (game::replaceable(region, toGame(piece.pos))) ++needed;
        if (tracksInInventory(player) < needed) {
            plan.valid   = false;
            plan.message = "create.track.not_enough_tracks";
        }
    }
    return plan;
}

void renderPreview(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    std::optional<Selection> sel;
    bool                     ctrl;
    {
        std::lock_guard lock(st().mutex);
        sel  = st().selection;
        ctrl = st().ctrl;
    }
    if (!sel || sel->dim != static_cast<int>(player->getDimensionId()) || !holdingTrack(*player)) {
        st().animation = 0;
        return;
    }
    auto plan = planFor(*player, *sel, ctrl);
    if (!plan) return;
    auto& s = st();

    // messages (Create: green when valid, white for the second point, red otherwise)
    if (plan->valid) showMessage("§a" + ctr("create.track.valid_connection", "Can Connect ✔"));
    else if (!plan->message.empty())
        showMessage((plan->message == "create.track.second_point" ? "§f" : "§c") + translateClient(plan->message, plan->message));

    // Create's animation.chase(valid ? 1 : 0, 0.25, EXP) per tick
    auto  now = std::chrono::steady_clock::now();
    float dt  = s.last.time_since_epoch().count() == 0 ? 0.f : std::chrono::duration<float>(now - s.last).count();
    s.last    = now;
    float target = plan->valid ? 1.f : 0.f;
    s.animation += (target - s.animation) * (1.f - std::pow(0.75f, std::min(dt, 0.5f) * 20.f));
    if (!plan->valid) plan->end1Extent = plan->end2Extent = 0;
    auto colour = previewColour(s.animation);

    std::unique_ptr<BaseActorRenderContext::Impl> const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3      camera = impl->mCameraTargetPosition;
    ct::Vec3d o{camera.x, camera.y, camera.z};
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
    ct::Vec3d const up{0, 4 / 16.0, 0};
    constexpr double kWidth = 1 / 8.0; // Create's lineWidth(1 / 8f)
    // the straight runs at both ends (Create: from the curve start, both rails)
    for (int e = 0; e < 2; ++e) {
        auto const& pos    = e == 0 ? plan->pos1 : plan->pos2;
        auto const& axis   = e == 0 ? plan->axis1 : plan->axis2;
        int         extent = e == 0 ? plan->end1Extent : plan->end2Extent;
        if (axis.length() < 1e-6) continue;
        ct::Vec3d v  = ct::curveStart(pos, axis);
        ct::Vec3d a  = axis.normalize();
        ct::Vec3d n  = ct::Vec3d{0, 1, 0}.cross(a).normalize() * (15 / 16.0);
        ct::Vec3d of = a * 0.125;
        ct::Vec3d ex = a * ((extent - (!plan->curve && extent > 0 ? 2 : 0)) * axis.length());
        for (int side : {1, -1}) {
            ct::Vec3d base = v + n * side + up - o;
            bar(tess, base - of, base + ex, kWidth, colour);
        }
    }
    // the curve's rails
    if (plan->curve) {
        auto segs = plan->curve->segments();
        for (std::size_t i = 1; i < segs.size(); ++i) {
            auto const& p = segs[i - 1];
            auto const& q = segs[i];
            for (int side : {1, -1}) {
                ct::Vec3d a = p.position + p.normal * (side * 15 / 16.0) + up - o;
                ct::Vec3d b = q.position + q.normal * (side * 15 / 16.0) + up - o;
                bar(tess, a, b, kWidth, colour);
            }
        }
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!s.loggedPreview) {
        s.loggedPreview = true;
        log().info("tracks: previewing a track placement ({})", plan->valid ? "valid" : plan->message);
    }
}

void onSelection(nlohmann::json const& msg) {
    std::optional<Selection> sel;
    if (msg.is_object() && msg.contains("end")) {
        if (auto end = ct::TrackEnd::fromJson(msg["end"])) sel = Selection{msg.value("dim", 0), *end, msg.value("maxLength", 32)};
    }
    std::lock_guard lock(st().mutex);
    st().selection     = sel;
    st().loggedPreview = false;
    log().info("tracks: {}", sel ? "a track end is selected" : "the track selection is gone");
}

void setCtrl(bool down) {
    {
        std::lock_guard lock(st().mutex);
        if (st().ctrl == down) return;
        st().ctrl = down;
    }
    if (feature::net::client::connected()) feature::net::client::send("tracks.ctrl", {{"down", down}});
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TrackPreviewRenderHook,
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
        renderPreview(renderContext);
    } catch (...) {
    }
}

class TrackPreviewModule {
public:
    void install() {
        feature::net::client::on("tracks.selection", [](nlohmann::json const& msg) { onSelection(msg); });
        TrackPreviewRenderHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(st().mutex);
            st().selection.reset();
            st().ctrl = false;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>([](ll::event::KeyInputEvent& ev) {
            int vk = ev.keyCode();
            if (vk == kVkControl || vk == kVkLControl || vk == kVkRControl) setCtrl(ev.isDown());
        }));
    }
    void uninstall() {
        TrackPreviewRenderHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

/// Whether the client knows a track selection (selftests / debugging).
bool hasSelection() {
    std::lock_guard lock(st().mutex);
    return st().selection.has_value();
}

} // namespace lv::client::tracks

LV_REGISTER_MODULE(lv::client::tracks::TrackPreviewModule);

#endif // LV_CLIENT
