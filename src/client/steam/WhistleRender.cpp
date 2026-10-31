// Client (steam unit): the mouth of steam whistles (Create's WhistleRenderer). Chunk meshes draw
// whistles without their mouth (levitate:mouth 1); the mouth is drawn here, moved up to 4/16 by
// the whistle's animation (Create: chases 1 while it sounds, EXP .5, back to 0 LINEAR .4) and,
// once more than half open, wiggled by sin(ticks / 8 · 2π · (4 − size)) / 16. The server sends the
// sounding whistles ("steam.whistles"); vanilla clients see the mouth still.

#ifdef LV_CLIENT

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

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
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
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
#include "core/kinetics/RotorModel.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::steam {

namespace {

using Json = nlohmann::json;
using Key  = std::array<int, 4>; // dim, x, y, z

constexpr char const* kWhistle = "levitate:steam_whistle";

struct Mouth {
    float         value = 0, prev = 0; ///< Create's LerpedFloat animation (0 shut .. 1 open)
    std::uint64_t tick  = 0;           ///< client tick of `value`
};

struct State {
    std::mutex                                     mutex; ///< chunk builder threads
    std::unordered_map<Block const*, Block const*> mouthless;
    std::set<std::array<int, 3>>                   seen;
    std::set<Key>                                  sounding; ///< client thread
    std::map<Key, Mouth>                           mouths;   ///< client thread
    std::map<std::string, std::unique_ptr<mce::Mesh>> meshes;
    bool                                           logged = false;
};

State& state() {
    static State s;
    return s;
}

std::uint64_t clientTick() {
    auto* ci = client::currentClient();
    auto* l  = ci ? ci->getLevel() : nullptr;
    return l ? static_cast<std::uint64_t>(l->getCurrentTick().tickID) : 0;
}

void onWhistles(Json const& msg) {
    try {
        auto& s = state();
        s.sounding.clear();
        for (auto const& w : msg.value("whistles", Json::array())) {
            auto p = w.at("p");
            s.sounding.insert(Key{w.value("d", 0), p.at(0).get<int>(), p.at(1).get<int>(), p.at(2).get<int>()});
        }
    } catch (...) {
    }
}

std::string strState(Block const& block, char const* name) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return {};
    auto const& st = nbt["states"].get<CompoundTag>();
    if (!st.contains(name)) return {};
    return std::string(st[name].get<StringTag>());
}

/// The y rotation of the block's model (the permutations of steam_whistle.json).
float yRotation(Block const& block) {
    auto face = strState(block, "minecraft:block_face");
    if (face == "north") return 180;
    if (face == "east") return 90;
    if (face == "west") return -90;
    if (face == "up" || face == "down" || face.empty()) {
        auto dir = strState(block, "minecraft:cardinal_direction");
        if (dir == "south") return 180;
        if (dir == "east") return -90;
        if (dir == "west") return 90;
    }
    return 0;
}

mce::Mesh* mouthMesh(Tessellator& tess, std::string const& bone) {
    auto& s = state();
    if (auto it = s.meshes.find(bone); it != s.meshes.end()) return it->second.get();
    auto& slot = s.meshes[bone];
    static std::optional<core::kinetics::RotorModel> model = []() -> std::optional<core::kinetics::RotorModel> {
        auto file = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks" / "steam_whistle.geo.json";
        std::ifstream in(file);
        auto          j = Json::parse(in, nullptr, false, true);
        if (j.is_discarded()) return std::nullopt;
        auto parsed = core::kinetics::parseGeometry(j);
        if (!parsed || !parsed->contains("geometry.levitate.steam_whistle")) return std::nullopt;
        return parsed->at("geometry.levitate.steam_whistle");
    }();
    if (!model) return nullptr;
    auto b = model->bones.find(bone);
    if (b == model->bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet("levitate_steam_whistle", 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_whistle_mouth", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// Advances Create's chaser to `now` (whole ticks): EXP .5 towards 1, LINEAR .4 towards 0.
void tickMouth(Mouth& m, bool sounding, std::uint64_t now) {
    if (m.tick == 0 || now < m.tick) m.tick = now;
    for (int i = 0; m.tick < now && i < 40; ++i, ++m.tick) {
        m.prev = m.value;
        if (sounding) {
            m.value += (1.f - m.value) * .5f;
            if (1.f - m.value < 1e-3f) m.value = 1.f;
        } else {
            m.value = std::max(0.f, m.value - .4f);
        }
    }
    m.tick = now;
}

void renderMouths(BaseActorRenderContext& rc) {
    auto& s = state();
    std::vector<std::array<int, 3>> positions;
    {
        std::lock_guard lock(s.mutex);
        if (s.seen.empty()) return;
        positions.assign(s.seen.begin(), s.seen.end());
    }
    auto* player = client::currentPlayer();
    if (!player) return;
    int   dim    = static_cast<int>(player->getDimensionId());
    auto& region = player->getDimensionBlockSource();
    auto* moving = static_cast<MovingBlockActorRenderer*>(rc.mBlockEntityRenderDispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture = moving->mAtlasTexture.get();
    Vec3          cam   = rc.mImpl.get()->mCameraTargetPosition;
    auto&         tess  = rc.mScreenContext.tessellator;
    std::uint64_t now   = clientTick();
    float         alpha = rc.mFrameAlpha;
    std::vector<std::array<int, 3>> stale;
    int                             open = 0;
    for (auto const& p : positions) {
        BlockPos bp{p[0], p[1], p[2]};
        if (!region.hasChunksAt(bp, 0, false) || region.getBlock(bp).getTypeName() != kWhistle) {
            stale.push_back(p);
            continue;
        }
        auto const& block = region.getBlock(bp);
        int         size  = game::intState(block, "levitate:size").value_or(0); // 0 medium, 1 small, 2 large
        int         ord   = size == 1 ? 0 : size == 0 ? 1 : 2;                   // Create's WhistleSize ordinal
        Key         key{dim, p[0], p[1], p[2]};
        bool        on = s.sounding.contains(key);
        auto&       m  = s.mouths[key];
        tickMouth(m, on, now);
        float offset = m.prev + (m.value - m.prev) * alpha;
        if (on && m.value > .5f) {
            float wiggle = (static_cast<float>(now) + alpha) / 8.f;
            offset -= std::sin(wiggle * 2 * std::numbers::pi_v<float> * static_cast<float>(4 - ord)) / 16.f;
        }
        if (offset > .01f) ++open;
        static constexpr char const* bones[]{"mouth_small", "mouth_medium", "mouth_large"};
        auto* mesh = mouthMesh(tess, bones[ord]);
        if (!mesh) continue;
        auto&     screen = rc.mScreenContext;
        auto      ref    = screen.camera.worldMatrixStack.get().push(false);
        glm::mat4 mat{1.f};
        mat = glm::translate(mat, glm::vec3{bp.x - cam.x + .5f, bp.y - cam.y + offset * 4.f / 16.f, bp.z - cam.z + .5f});
        mat = glm::rotate(mat, glm::radians(yRotation(block)), glm::vec3{0, 1, 0});
        mat = glm::translate(mat, glm::vec3{-.5f, 0.f, -.5f});
        ref.mat->_m.get() = ref.mat->_m.get() * mat;
        mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.seen.erase(p);
    }
    if (open > 0 && !s.logged) {
        s.logged = true;
        log().info("steam client: a whistle mouth opens");
    }
}

/// The whistle state without its mouth (levitate:mouth 1), cached per state.
Block const* withoutMouth(Block const& block) {
    auto&           s = state();
    std::lock_guard lock(s.mutex);
    auto [it, fresh] = s.mouthless.try_emplace(&block, nullptr);
    if (fresh && game::intState(block, "levitate:mouth").value_or(1) == 0) {
        if (auto const* type = block.getBlockType().getBlockState(HashedString("levitate:mouth")))
            if (auto b = block.setState(*type, 1)) it->second = b.as_ptr();
    }
    return it->second;
}

void reset() {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.mouthless.clear();
        s.seen.clear();
    }
    s.sounding.clear();
    s.mouths.clear();
    s.meshes.clear();
    s.logged = false;
}

} // namespace

// Chunk meshes: whistles lose their mouth (drawn and moved above).
LL_STATIC_HOOK(
    WhistleExtractHook,
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
    Block const* use = &block;
    if (visualReference == ClientBlockPipeline::VisualReference::Block && block.getTypeName() == kWhistle) {
        if (auto const* hidden = withoutMouth(block)) {
            use                 = hidden;
            BlockPos const& rel = iterData.relativePos;
            auto&           s   = state();
            std::lock_guard lock(s.mutex);
            s.seen.insert({chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z});
        }
    }
    return origin(*use, iterData, extraction, worldView, shouldGenerateFaceCulling, visualReference, tintResolver, biomeTintCache, chunkMinPos, checkIgnoreBlockTransforms);
}

LL_TYPE_INSTANCE_HOOK(
    WhistleRenderHook,
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
        renderMouths(renderContext);
    } catch (...) {
    }
}

class WhistleClientModule {
public:
    void install() {
        WhistleExtractHook::hook();
        WhistleRenderHook::hook();
        feature::net::client::on("steam.whistles", [](Json const& msg) { onWhistles(msg); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { reset(); });
    }
    void uninstall() {
        WhistleExtractHook::unhook();
        WhistleRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
        reset();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::steam

LV_REGISTER_MODULE(lv::client::steam::WhistleClientModule);

#endif // LV_CLIENT
