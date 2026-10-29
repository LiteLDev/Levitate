#ifdef LV_CLIENT

// World item renderer of the client mod (U33, see WorldItems.h).
//
// Path: ItemInHandRenderer::renderItem(ctx, actor, stack, ..., WorldPass, useMatrixAsIs = true)
// draws the game's own model of any item at the current world matrix, lit like an item held by
// `actor` (the local player). Its model spaces, measured in game (1.26.51):
// - items drawn as blocks (canTessellateAsBlockItem): a cube centred on the origin, block units;
// - item sprites and the other items with a block (torches, ...): pixels, lying in the XZ plane
//   (x 0..16, z 0..16), read from +Y with the icon's right along -X and its top along +Z.
// `drawItem` maps the canonical item (WorldItems.h) onto these.
//
// Display items from the server ("itemrender.batch") are kept per (dimension, owner, slot),
// moved towards their latest transform over `lerp` ticks and drawn in the opaque block entity
// pass. Fallback item entities they name are not drawn (ItemRenderer hook). Blocks with item
// providers seen in chunk meshes are asked for ("itemrender.query"). Whenever the store is cleared
// (new level, other dimension) the client sends "itemrender.reset" so the server sends everything
// in range again.

#include "client/render/WorldItems.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "glm/gtc/matrix_transform.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderData.h"
#include "mc/client/renderer/actor/ItemRenderer.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/minecraft_renderer/game/ItemContextFlags.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"

#include "client/Client.h"
#include "feature/common/Items.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::render {

namespace ci    = core::itemrender;
namespace items = feature::common::items;
using Json      = nlohmann::json;

namespace {

/// How the game's model of a stack is laid out (see the file comment).
enum class Kind { Block, Special, Sprite };

Kind kindOf(ItemStack const& stack) {
    if (ItemInHandRenderer::canTessellateAsBlockItem(stack)) return Kind::Block;
    if (stack.mBlock) return Kind::Special;
    return Kind::Sprite;
}

/// Maps the sprite model (pixels, XZ plane) onto the canonical item (XY plane, read from +Z).
glm::mat4 const kSpriteToCanonical{
    glm::vec4{-1.f / 16, 0, 0, 0},
    glm::vec4{0, 0, 1.f / 16, 0},
    glm::vec4{0, 1.f / 16, 0, 0},
    glm::vec4{.5f, -.5f, 0, 1},
};

// ---- display item store (client thread) ---------------------------------------------------------------

struct Key {
    int          dim = 0;
    int          x = 0, y = 0, z = 0;
    std::int64_t slot = 0;
    friend auto  operator<=>(Key const&, Key const&) = default;
};

struct Shown {
    ItemStack     stack;
    std::string   itemKey; ///< dump of the item json (cheap change test, draw order)
    ci::Transform from, to;
    double        start = 0;
    int           lerp  = 0;
    Style         style = Style::Facing;
    float         spin  = 0;
    bool          pile  = false;
    std::uint64_t hide  = 0;
};

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept {
        return static_cast<std::size_t>(p.x) * 73856093u ^ static_cast<std::size_t>(p.y) * 19349663u ^ static_cast<std::size_t>(p.z) * 83492791u;
    }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

struct State {
    std::mutex                                           mutex;
    std::map<Key, Shown>                                 shown;
    std::mutex                                           hiddenMutex; ///< guards `hidden` only (entity pass)
    std::unordered_set<std::uint64_t>                    hidden;      ///< runtime ids of fallback entities
    bool                                                 needReset = true; ///< tell the server we have nothing
    double                                               now    = 0; ///< level ticks + partial of the last frame
    int                                                  dim    = -1;
    void const*                                          level  = nullptr;
    bool                                                 logged = false;
    // filled by chunk builder threads: blocks that have item providers
    std::unordered_map<Block const*, bool>               providerBlocks;
    std::unordered_set<BlockPos, PosHash, PosEq>         tracked;
    std::unordered_map<BlockPos, double, PosHash, PosEq> queried; ///< pos -> time of the last query
    double                                               lastQuery = -1e9;
};
State& st() {
    static State s;
    return s;
}

void rebuildHidden(State& s) {
    std::lock_guard lock(s.hiddenMutex);
    s.hidden.clear();
    for (auto const& [k, v] : s.shown)
        if (v.hide) s.hidden.insert(v.hide);
}

/// Forgets every display item (the server is told with "itemrender.reset").
void clearShown(State& s) {
    s.shown.clear();
    rebuildHidden(s);
    s.tracked.clear();
    s.queried.clear();
    s.lastQuery = -1e9;
    s.needReset = true;
}

ci::Transform currentOf(Shown const& v, double now) { return ci::lerp(v.from, v.to, ci::progress(now, v.start, v.lerp)); }

void resetLevel(State& s) {
    clearShown(s);
    s.providerBlocks.clear();
    s.logged = false;
    s.dim    = -1;
}

void onBatch(Json const& msg) {
    auto&           s   = st();
    int             dim = msg.value("dim", 0);
    std::lock_guard lock(s.mutex);
    double          now = s.now;
    for (auto const& d : msg.value("del", Json::array()))
        if (auto k = ci::decodeKey(d)) s.shown.erase(Key{dim, k->first[0], k->first[1], k->first[2], k->second});
    auto apply = [&](Json const& j, bool withItem) {
        auto e = ci::decode(j);
        if (!e) return;
        Key  key{dim, e->owner[0], e->owner[1], e->owner[2], e->slot};
        auto it = s.shown.find(key);
        if (it == s.shown.end()) {
            if (!withItem) return; // a move of something we never got
            Shown v;
            v.from = v.to = e->t;
            it            = s.shown.emplace(key, std::move(v)).first;
        } else {
            it->second.from = currentOf(it->second, now);
            it->second.to   = e->t;
        }
        auto& v = it->second;
        v.start = now;
        v.lerp  = e->lerp;
        if (withItem) {
            auto dump = e->item.dump();
            if (dump != v.itemKey) {
                v.itemKey = std::move(dump);
                v.stack   = items::fromJson(e->item);
            }
            v.style = e->style;
            v.spin  = e->spin;
            v.pile  = e->pile;
            v.hide  = static_cast<std::uint64_t>(e->hide);
        }
    };
    for (auto const& j : msg.value("set", Json::array())) apply(j, true);
    for (auto const& j : msg.value("move", Json::array())) apply(j, false);
    rebuildHidden(s);
}

/// With `st().mutex` held.
bool hasProviderLocked(State& s, Block const& block) {
    auto [it, fresh] = s.providerBlocks.try_emplace(&block, false);
    if (fresh) it->second = feature::itemrender::hasProvider(game::blockId(block));
    return it->second;
}

bool hasProviderCached(Block const& block) {
    auto&           s = st();
    std::lock_guard lock(s.mutex);
    return hasProviderLocked(s, block);
}

/// Asks the server about provider blocks near the player (once a second; a block again after 10 s).
void sendQueries(LocalPlayer& player, double now) {
    if (!feature::net::client::connected()) return;
    auto& s    = st();
    Json  list = Json::array();
    {
        std::lock_guard lock(s.mutex);
        if (now - s.lastQuery < 20 && now >= s.lastQuery) return;
        s.lastQuery    = now;
        auto const& at = player.getPosition();
        auto&       region = player.getDimensionBlockSource();
        for (auto it = s.tracked.begin(); it != s.tracked.end();) {
            auto const& p  = *it;
            double      dx = at.x - p.x, dy = at.y - p.y, dz = at.z - p.z;
            double      d2 = dx * dx + dy * dy + dz * dz;
            // far away or no longer such a block: forgotten (seen again when its chunk is meshed)
            bool gone = d2 > 128.0 * 128.0 || (region.hasChunksAt(p, 0, false) && !hasProviderLocked(s, region.getBlock(p)));
            if (gone) {
                s.queried.erase(p);
                it = s.tracked.erase(it);
                continue;
            }
            ++it;
            if (d2 > 64.0 * 64.0 || list.size() >= 256) continue;
            auto [q, fresh] = s.queried.try_emplace(p, now);
            if (!fresh && now - q->second < 200 && now >= q->second) continue;
            q->second = now;
            list.push_back(Json::array({p.x, p.y, p.z}));
        }
    }
    if (!list.empty()) feature::net::client::send("itemrender.query", {{"dim", static_cast<int>(player.getDimensionId())}, {"pos", list}});
}

void drawOne(BaseActorRenderContext& ctx, Shown const& v, double now) {
    auto m = matrixOf(ctx, currentOf(v, now));
    if (v.spin != 0) m = glm::rotate(m, glm::radians(static_cast<float>(std::fmod(now * v.spin, 360.0))), glm::vec3{0, 1, 0});
    drawItem(ctx, v.stack, m, v.style);
    if (!v.pile) return;
    int copies = ci::pileCopies(items::countOf(v.stack));
    for (int i = 1; i <= copies; ++i) {
        auto      o = ci::pileOffset(i);
        glm::mat4 c = glm::translate(m, glm::vec3{o[0], v.style == Style::Lying ? o[1] * .25f : o[1], o[2]});
        c           = glm::rotate(c, glm::radians(o[3]), glm::vec3{0, 1, 0});
        drawItem(ctx, v.stack, c, v.style);
    }
}

void renderFrame(BaseActorRenderContext& ctx) {
    auto* client = currentClient();
    if (!client) return;
    auto* level  = client->getLevel();
    auto* player = client->getLocalPlayer();
    auto& s      = st();
    if (level != s.level) {
        std::lock_guard lock(s.mutex);
        resetLevel(s);
        s.level = level;
    }
    if (!level || !player) return;
    double now = static_cast<double>(level->getCurrentTick().tickID) + ctx.mFrameAlpha;
    int    dim = static_cast<int>(player->getDimensionId());
    bool reset = false;
    {
        std::lock_guard lock(s.mutex);
        s.now = now;
        if (dim != s.dim) {
            if (s.dim != -1) clearShown(s);
            s.dim = dim;
        }
        if (s.needReset && feature::net::client::connected()) {
            s.needReset = false;
            reset       = true;
        }
    }
    if (reset) feature::net::client::send("itemrender.reset", {{"dim", dim}});
    sendQueries(*player, now);

    std::lock_guard          lock(s.mutex); // the store only changes on this thread; held while drawing
    std::vector<Shown const*> todo;
    todo.reserve(s.shown.size());
    for (auto const& [k, v] : s.shown)
        if (k.dim == dim) todo.push_back(&v);
    if (todo.empty()) return;
    // batched by item: consecutive draws reuse the game's cached model of that item
    std::sort(todo.begin(), todo.end(), [](Shown const* a, Shown const* b) { return a->itemKey < b->itemKey; });
    for (auto const* v : todo) drawOne(ctx, *v, now);
    if (!s.logged) {
        s.logged = true;
        log().info("itemrender client: drawing {} display items", todo.size());
    }
}

} // namespace

// ---- public API -------------------------------------------------------------------------------------------

glm::vec3 relative(BaseActorRenderContext& ctx, std::array<double, 3> const& world) {
    Vec3 const cam = ctx.mImpl.get()->mCameraTargetPosition;
    return glm::vec3{static_cast<float>(world[0] - cam.x), static_cast<float>(world[1] - cam.y), static_cast<float>(world[2] - cam.z)};
}

glm::mat4 matrixOf(BaseActorRenderContext& ctx, ci::Transform const& t) {
    glm::mat4 m = glm::translate(glm::mat4{1.f}, relative(ctx, {t.x, t.y, t.z}));
    m           = glm::rotate(m, glm::radians(t.yaw), glm::vec3{0, 1, 0});
    m           = glm::rotate(m, glm::radians(t.pitch), glm::vec3{1, 0, 0});
    m           = glm::rotate(m, glm::radians(t.roll), glm::vec3{0, 0, 1});
    return glm::scale(m, glm::vec3{t.scale});
}

bool isBlockItem(ItemStack const& stack) { return !items::isEmpty(stack) && kindOf(stack) == Kind::Block; }

void drawItem(BaseActorRenderContext& ctx, ItemStack const& stack, glm::mat4 const& transform, Style style) {
    if (items::isEmpty(stack)) return;
    auto* player = currentPlayer();
    if (!player) return;
    glm::mat4 m = transform;
    switch (kindOf(stack)) {
    case Kind::Block: // Lying: stands on the origin plane; Facing: a half size cube in front of it
        if (style == Style::Lying) m = glm::translate(m, glm::vec3{0, .5f, 0});
        else m = glm::scale(glm::translate(m, glm::vec3{0, 0, .25f}), glm::vec3{.5f});
        break;
    case Kind::Special:
    case Kind::Sprite:
        if (style == Style::Lying) {
            m = glm::translate(m, glm::vec3{0, 1.f / 32, 0});
            m = glm::rotate(m, glm::radians(-90.f), glm::vec3{1, 0, 0});
        }
        m = m * kSpriteToCanonical;
        break;
    }
    auto& world       = ctx.mScreenContext.camera.worldMatrixStack.get();
    auto  ref         = world.push(false);
    ref.mat->_m.get() = ref.mat->_m.get() * m;
    ItemInHandRenderer& hand = ctx.mItemInHandRenderer;
    try {
        hand.renderItem(ctx, *player, stack, false, ItemContextFlags::WorldPass, true, true);
    } catch (...) {
    }
}

std::size_t displayItemCount() {
    std::lock_guard lock(st().mutex);
    return st().shown.size();
}

// ---- hooks --------------------------------------------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    WorldItemsRenderHook,
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
        renderFrame(renderContext);
    } catch (...) {
    }
}

// Fallback item entities of display items (belts) are not drawn for us.
LL_TYPE_INSTANCE_HOOK(
    WorldItemsHideHook,
    ll::memory::HookPriority::Normal,
    ItemRenderer,
    &ItemRenderer::$render,
    void,
    BaseActorRenderContext& renderContext,
    ActorRenderData&        entityRenderData
) {
    if (Actor* actor = entityRenderData.mActor) {
        auto&         s  = st();
        std::uint64_t id = actor->getRuntimeID().rawID;
        bool          hide;
        {
            std::lock_guard lock(s.hiddenMutex);
            hide = !s.hidden.empty() && s.hidden.contains(id);
        }
        if (hide) return;
    }
    origin(renderContext, entityRenderData);
}

// Chunk meshes: remember blocks whose items come from a provider (filter slots, placards, ...).
LL_STATIC_HOOK(
    WorldItemsExtractHook,
    ll::memory::HookPriority::Normal,
    &ClientBlockPipeline::WorldExtractorStep::extractCustomBlock,
    std::optional<ClientBlockPipeline::PipelineError>,
    Block const&                                                       block,
    ClientBlockPipeline::WorldExtractorStep::BlockIterationData const& iterData,
    ClientBlockPipeline::WorldExtractorStep::ExtractionData&           extraction,
    ClientBlockPipeline::WorldView::Interface*                         worldView,
    bool                                                               shouldGenerateFaceCulling,
    ClientBlockPipeline::VisualReference                               visualReference,
    ClientBlockPipeline::ITintResolver&                                tintResolver,
    BiomeTintCache*                                                    biomeTintCache,
    BlockPos const&                                                    chunkMinPos,
    bool                                                               checkIgnoreBlockTransforms
) {
    if (visualReference == ClientBlockPipeline::VisualReference::Block && hasProviderCached(block)) {
        BlockPos const& rel = iterData.relativePos;
        BlockPos        pos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z};
        auto&           s = st();
        std::lock_guard lock(s.mutex);
        s.tracked.insert(pos);
    }
    return origin(
        block,
        iterData,
        extraction,
        worldView,
        shouldGenerateFaceCulling,
        visualReference,
        tintResolver,
        biomeTintCache,
        chunkMinPos,
        checkIgnoreBlockTransforms
    );
}

class WorldItemsModule {
public:
    void install() {
        WorldItemsRenderHook::hook();
        WorldItemsHideHook::hook();
        WorldItemsExtractHook::hook();
        feature::net::client::on("itemrender.batch", [](Json const& msg) { onBatch(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto&           s = st();
            std::lock_guard lock(s.mutex);
            resetLevel(s);
            s.level = nullptr;
        });
    }
    void uninstall() {
        WorldItemsRenderHook::unhook();
        WorldItemsHideHook::unhook();
        WorldItemsExtractHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::render

LV_REGISTER_MODULE(lv::client::render::WorldItemsModule);


#endif // LV_CLIENT
