// Create's WorldshaperRenderHandler in the client mod (U45): while a configured handheld
// worldshaper is held, the blocks its brush would change at the looked-at block (up to 128
// blocks away) are outlined (Outliner#showCluster, 0xbfbfbf, 1/32 wide).

#ifdef LV_CLIENT

#include <mutex>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/render/Outline.h"
#include "core/equipment/Shaper.h"
#include "feature/common/Items.h"
#include "game/Blocks.h"
#include "mod/Module.h"

namespace lv::client::equipment {

namespace {

namespace ce    = lv::core::equipment;
namespace items = lv::feature::common::items;

std::mutex& previewMutex() {
    static std::mutex m;
    return m;
}
std::vector<ce::Edge>& previewEdges() {
    static std::vector<ce::Edge> e;
    return e;
}

ce::IPos stepFace(ce::IPos p, int face) {
    static constexpr int d[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    return {p.x + d[face][0], p.y + d[face][1], p.z + d[face][2]};
}

/// The settings of the held worldshaper (main hand first, as Create), when its brush was set.
std::optional<ce::ShaperSettings> heldSettings(LocalPlayer& p) {
    for (ItemStack const* s : {&p.getSelectedItem(), &p.getOffhandSlot()}) {
        if (items::isEmpty(*s) || items::idOf(*s) != "levitate:handheld_worldshaper") continue;
        auto st = ce::ShaperSettings::fromJson(items::getData(*s, "lv:shaper"));
        if (!st.configured) return std::nullopt;
        return st;
    }
    return std::nullopt;
}

/// Create's Brush#addToGlobalPositions for the looked-at block (the same rules as the server's
/// Worldshaper.cpp, read from the client's world).
std::vector<ce::IPos> brushPositions(LocalPlayer& p, ce::ShaperSettings const& st) {
    auto hit = p.traceRay(128.f, false, true);
    if (hit.mType != HitResultType::Tile) return {};
    int         face   = static_cast<int>(hit.mFacing);
    ce::IPos    target{hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    auto&       region = p.getDimensionBlockSource();
    auto        bp     = [](ce::IPos const& q) { return BlockPos{q.x, q.y, q.z}; };
    auto        repl   = [&](ce::IPos const& q) { return game::replaceable(region, bp(q)); };
    std::vector<ce::IPos> out;
    if (ce::hasPlacement(st.brush)) {
        auto off = ce::placementOffset(st, face);
        for (auto q : ce::brushShape(st)) out.push_back({target.x + q.x + off.x, target.y + q.y + off.y, target.z + q.z + off.z});
        return out;
    }
    std::string targetId = game::blockIdAt(region, bp(target));
    bool        fuzzy    = ce::ignoreMaterialBorders(st);
    bool        surface  = st.brush == ce::Brush::Surface;
    bool        replace  = st.tool != ce::ShaperTool::Overlay;
    auto        start    = replace ? target : stepFace(target, face);
    return ce::floodFill(st, start, face, [&](ce::IPos const& q) {
        if (replace) {
            auto const& b = region.getBlock(bp(q));
            if (b.getBlockType().getDestroySpeed() < 0) return false;
            if (!fuzzy && game::blockId(b) != targetId) return false;
            if (repl(q)) return false;
            if (surface && !repl(stepFace(q, face))) return false;
            return true;
        }
        auto on = stepFace(q, face ^ 1);
        if (repl(on)) return false;
        if (!fuzzy && game::blockIdAt(region, bp(on)) != targetId) return false;
        return repl(q);
    });
}

void tickPreview() {
    static std::string    lastKey;
    static std::vector<ce::Edge> lastEdges;
    std::vector<ce::Edge> edges;
    if (auto* lp = currentPlayer(); lp && inLevel()) {
        if (auto st = heldSettings(*lp)) {
            // the outline of a big brush costs: rebuild it only when the aim or the settings change
            auto        hit = lp->traceRay(128.f, false, true);
            std::string key = st->toJson().dump() + "@" + std::to_string(hit.mBlock.x) + "," + std::to_string(hit.mBlock.y) + ","
                            + std::to_string(hit.mBlock.z) + "," + std::to_string(static_cast<int>(hit.mFacing)) + ","
                            + std::to_string(static_cast<int>(hit.mType));
            if (key != lastKey) {
                lastKey   = key;
                lastEdges = ce::clusterEdges(brushPositions(*lp, *st));
            }
            edges = lastEdges;
        }
    }
    if (edges.empty()) lastKey.clear();
    std::lock_guard lock(previewMutex());
    previewEdges() = std::move(edges);
}

void renderPreview(BaseActorRenderContext& ctx) {
    std::vector<ce::Edge> edges;
    {
        std::lock_guard lock(previewMutex());
        edges = previewEdges();
    }
    if (edges.empty()) return;
    render::Lines lines(ctx);
    for (auto const& e : edges)
        lines.line({double(e.a.x), double(e.a.y), double(e.a.z)}, {double(e.b.x), double(e.b.y), double(e.b.z)}, render::Rgb::hex(0xbfbfbf), 1.0 / 32);
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    ShaperPreviewRenderHook,
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

class ShaperPreviewModule {
public:
    void install() {}
    void load() {
        ShaperPreviewRenderHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { tickPreview(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(previewMutex());
            previewEdges().clear();
        }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        ShaperPreviewRenderHook::unhook();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::equipment

LV_REGISTER_MODULE(lv::client::equipment::ShaperPreviewModule);

#endif // LV_CLIENT
