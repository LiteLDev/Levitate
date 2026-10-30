#ifdef LV_CLIENT

// Client side of the toolbox lid and drawers (U31 / U45; Create's ToolboxRenderer), client mod only.
//
// The server sets the block state `levitate:lid` to 1 while anyone has the toolbox's screen open
// (vanilla clients show the open model, tools/models/equipment.ps1). The client mod meshes every
// toolbox with `levitate:lid` = 2 (the body only) and draws the lid and the two drawers itself each
// frame, chasing the state like Create's ToolboxBlockEntity (lid linear, drawers exponential, 0.2
// per tick; core/equipment/ToolboxLid.h): the lid turns up to 135° about its back edge, the lower
// drawer slides 0.35 blocks out, the upper one half of that. The block state is the sync: it
// reaches every client (and players joining later) with the chunk.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "glm/gtc/matrix_transform.hpp"
#include "nlohmann/json.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "core/equipment/ToolboxLid.h"
#include "core/kinetics/RotorModel.h"
#include "core/kinetics/Solver.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::equipment {

namespace {

namespace k  = core::kinetics;
namespace ce = core::equipment;

constexpr char kModel[] = "geometry.levitate.toolbox_parts";

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept { return k::GlobalPosHash{}(core::GlobalPos{0, p.x, p.y, p.z}); }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

/// A toolbox block state as the client mod draws it (computed once per Block).
struct Info {
    Block const* hidden = nullptr; ///< the `levitate:lid` = 2 variant (body only); nullptr: not ours
    bool         open   = false;   ///< `levitate:lid` = 1
    int          facing = 0;
    std::string  texture;          ///< terrain texture of the colour
};

/// The animation of one toolbox (client thread).
struct Anim {
    ce::ToolboxLid now, prev;
    std::uint64_t  tick = 0;
};

struct State {
    std::shared_mutex                                    infoMutex; ///< infos (chunk builder threads)
    std::unordered_map<Block const*, Info>               infos;
    std::mutex                                           mutex; ///< tracked (chunk builder threads)
    std::unordered_set<BlockPos, PosHash, PosEq>         tracked;
    std::unordered_map<BlockPos, Anim, PosHash, PosEq>   anims; ///< client thread only
    void const*                                          level  = nullptr;
    bool                                                 logged = false;
};

State& state() {
    static State s;
    return s;
}

Info computeInfo(Block const& block) {
    Info        info;
    std::string id = block.getTypeName();
    if (!id.starts_with("levitate:") || !id.ends_with("_toolbox")) return info;
    int lid = game::intState(block, "levitate:lid").value_or(-1);
    if (lid != 0 && lid != 1) return info;
    if (auto const* st = block.getBlockType().getBlockState(HashedString("levitate:lid"))) {
        if (auto b = block.setState(*st, 2)) info.hidden = b.as_ptr();
    }
    info.open    = lid == 1;
    info.facing  = game::intState(block, "levitate:facing").value_or(0) & 3;
    info.texture = "levitate_" + id.substr(9);
    return info;
}

Info infoOf(Block const& block) {
    auto& s = state();
    {
        std::shared_lock lock(s.infoMutex);
        if (auto it = s.infos.find(&block); it != s.infos.end()) return it->second;
    }
    Info             info = computeInfo(block);
    std::unique_lock lock(s.infoMutex);
    s.infos.emplace(&block, info);
    return info;
}

// ---- meshes (as the kinetics renderer builds its rotor meshes) -------------------------------------------

std::map<std::string, k::RotorModel> const& models() {
    static std::map<std::string, k::RotorModel> out = [] {
        std::map<std::string, k::RotorModel> m;
        auto          dir = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        std::ifstream in(dir / "toolbox.geo.json");
        auto          j = nlohmann::json::parse(in, nullptr, false, true);
        if (!j.is_discarded())
            if (auto g = k::parseGeometry(j)) m.merge(*g);
        return m;
    }();
    return out;
}

std::map<std::string, std::unique_ptr<mce::Mesh>>& meshCache() {
    static std::map<std::string, std::unique_ptr<mce::Mesh>> cache;
    return cache;
}

mce::Mesh* partMesh(Tessellator& tess, std::string const& bone, std::string const& texture) {
    auto  key   = bone + "|" + texture;
    auto& cache = meshCache();
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    auto&       slot = cache[key];
    auto const& all  = models();
    auto        m    = all.find(kModel);
    if (m == all.end()) return nullptr;
    auto b = m->second.bones.find(bone);
    if (b == m->second.bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet(texture, 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_toolbox", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// A new level (block states are registered again) or leaving it (`all`: tracked positions too).
void resetLevel(bool all) {
    auto& s = state();
    {
        std::unique_lock lock(s.infoMutex);
        s.infos.clear();
    }
    std::lock_guard lock(s.mutex);
    s.anims.clear();
    s.logged = false;
    if (all) s.tracked.clear();
    meshCache().clear();
}

/// The animation of the toolbox at `pos` advanced to `tick`, chasing `open`.
Anim& animate(BlockPos const& pos, bool open, std::uint64_t tick) {
    auto& s       = state();
    auto [it, nu] = s.anims.try_emplace(pos);
    auto& a       = it->second;
    if (nu || tick < a.tick || tick - a.tick > 100) {
        // first sight (or a long gap): at rest in the current state
        a.now.lid = a.now.drawers = open ? 1.f : 0.f;
        a.prev                    = a.now;
        a.tick                    = tick;
    }
    while (a.tick < tick) {
        a.prev = a.now;
        a.now.tick(open);
        ++a.tick;
    }
    return a;
}

/// Turn of the model for `levitate:facing` (the block's minecraft:transformation, tools/models/equipment.ps1).
constexpr float kFacingDegrees[4] = {0.f, -90.f, 180.f, 90.f};

void renderToolboxes(BaseActorRenderContext& ctx) {
    auto* ci = lv::client::currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    auto& s      = state();
    if (level != s.level) {
        resetLevel(false);
        s.level = level;
    }
    if (!level || !player) return;
    auto& region = player->getDimensionBlockSource();

    std::vector<BlockPos> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.tracked.empty()) return;
        positions.assign(s.tracked.begin(), s.tracked.end());
    }
    std::vector<std::pair<BlockPos, Info>> items;
    std::vector<BlockPos>                  stale;
    for (auto const& p : positions) {
        if (!region.hasChunksAt(p, 0, false)) {
            stale.push_back(p);
            continue;
        }
        auto info = infoOf(region.getBlock(p));
        if (!info.hidden) {
            stale.push_back(p);
            continue;
        }
        items.emplace_back(p, info);
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.tracked.erase(p);
    }
    for (auto const& p : stale) s.anims.erase(p);
    if (items.empty()) return;

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&      screen = ctx.mScreenContext;
    auto&      tess   = screen.tessellator;
    Vec3 const cam    = ctx.mImpl.get()->mCameraTargetPosition;
    auto       tick   = static_cast<std::uint64_t>(level->getCurrentTick().tickID);
    float      alpha  = ctx.mFrameAlpha;
    auto&      world  = screen.camera.worldMatrixStack.get();

    for (auto const& [pos, info] : items) {
        auto const& a       = animate(pos, info.open, tick);
        float       lid     = a.prev.lid + (a.now.lid - a.prev.lid) * alpha;
        float       drawers = a.prev.drawers + (a.now.drawers - a.prev.drawers) * alpha;
        // the block's place and facing (the model's front is +z for facing 0)
        glm::mat4 base{1.f};
        base = glm::translate(base, glm::vec3{pos.x - cam.x + .5f, pos.y - cam.y, pos.z - cam.z + .5f});
        base = glm::rotate(base, glm::radians(kFacingDegrees[info.facing]), glm::vec3{0.f, 1.f, 0.f});
        base = glm::translate(base, glm::vec3{-.5f, 0.f, -.5f});
        auto draw = [&](std::string const& bone, glm::mat4 const& part) {
            auto* mesh = partMesh(tess, bone, info.texture);
            if (!mesh) return;
            auto ref          = world.push(false);
            ref.mat->_m.get() = ref.mat->_m.get() * base * part;
            mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        };
        // the lid turns back about its back bottom edge (y 8 px, z 4 px from the back of the cell)
        glm::mat4 lidM{1.f};
        lidM = glm::translate(lidM, glm::vec3{0.f, .5f, .25f});
        lidM = glm::rotate(lidM, glm::radians(-ce::lidDegrees(lid)), glm::vec3{1.f, 0.f, 0.f});
        lidM = glm::translate(lidM, glm::vec3{0.f, -.5f, -.25f});
        draw("lid", lidM);
        // the drawers slide out of the front (the upper one 2 px higher)
        draw("drawer", glm::translate(glm::mat4{1.f}, glm::vec3{0.f, 0.f, ce::drawerOut(drawers, false)}));
        draw("drawer", glm::translate(glm::mat4{1.f}, glm::vec3{0.f, 2.f / 16.f, ce::drawerOut(drawers, true)}));
    }
    if (!s.logged) {
        s.logged = true;
        log().info("equipment client: drawing {} toolbox lids", items.size());
    }
}

} // namespace

// Chunk meshes: toolboxes are meshed without lid and drawers; remember where they are.
LL_STATIC_HOOK(
    ToolboxExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block) {
        auto info = infoOf(block);
        if (info.hidden) {
            BlockPos const& rel = iterData.relativePos;
            {
                auto&           s = state();
                std::lock_guard lock(s.mutex);
                s.tracked.insert(BlockPos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
            }
            return origin(*info.hidden, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
        }
    }
    return origin(block, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    ToolboxRenderHook,
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
        renderToolboxes(renderContext);
    } catch (...) {
    }
}

class ToolboxClientModule {
public:
    void install() {
        ToolboxExtractHook::hook();
        ToolboxRenderHook::hook();
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevel(true);
            state().level = nullptr;
        });
    }
    void uninstall() {
        ToolboxExtractHook::unhook();
        ToolboxRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::equipment

LV_REGISTER_MODULE(lv::client::equipment::ToolboxClientModule);

#endif // LV_CLIENT
