#ifdef LV_CLIENT

// Client mod parts of the U49 equipment rows (SPEC §3.6.31):
// - Create's BellRenderer: peculiar / haunted bells are meshed without the bell (levitate:part = 1)
//   and the bell is drawn here, swinging after the server's `equipment.bell.ring` (74 ticks,
//   BellRenderer#getSwingAngle about the axis across the hit).
// - Create's CardboardArmorStealthOverlay: while our own player hides in a full cardboard set, a
//   cardboard vignette fades in over the HUD (opacity chases 0 / 1 by a quarter per tick).

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
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "mc/client/gui/screens/ScreenView.h"
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
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/equipment/Bells.h"
#include "core/kinetics/RotorModel.h"
#include "core/kinetics/Solver.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::equipment {

namespace {

namespace k  = core::kinetics;
namespace ce = core::equipment;
namespace net = feature::net;
using namespace lv::client::screen;

constexpr char kModel[] = "geometry.levitate.bell_parts";

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept { return k::GlobalPosHash{}(core::GlobalPos{0, p.x, p.y, p.z}); }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};

struct Info {
    Block const* hidden = nullptr; ///< the levitate:part = 1 variant; nullptr: not a bell
    std::string  texture;
};

struct Swing {
    std::uint64_t start = 0; ///< client tick of the ring
    int           dir   = 3;
};

struct State {
    std::shared_mutex                                   infoMutex;
    std::unordered_map<Block const*, Info>              infos;
    std::mutex                                          mutex;
    std::unordered_set<BlockPos, PosHash, PosEq>        tracked;
    std::unordered_map<BlockPos, Swing, PosHash, PosEq> swings; ///< client thread
    void const*                                         level = nullptr;
    bool                                                logged = false;
    float                                               stealth = 0, stealthPrev = 0;
};
State& state() {
    static State s;
    return s;
}

Info computeInfo(Block const& block) {
    Info        info;
    std::string id = block.getTypeName();
    if (id != "levitate:peculiar_bell" && id != "levitate:haunted_bell") return info;
    if (game::intState(block, "levitate:part").value_or(-1) != 0) return info;
    if (auto const* st = block.getBlockType().getBlockState(HashedString("levitate:part"))) {
        if (auto b = block.setState(*st, 1)) info.hidden = b.as_ptr();
    }
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

std::map<std::string, k::RotorModel> const& models() {
    static std::map<std::string, k::RotorModel> out = [] {
        std::map<std::string, k::RotorModel> m;
        auto          dir = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        std::ifstream in(dir / "bell.geo.json");
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

mce::Mesh* bellMesh(Tessellator& tess, std::string const& texture) {
    auto& cache = meshCache();
    if (auto it = cache.find(texture); it != cache.end()) return it->second.get();
    auto&       slot = cache[texture];
    auto const& all  = models();
    auto        m    = all.find(kModel);
    if (m == all.end()) return nullptr;
    auto b = m->second.bones.find("bell");
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
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_bell", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

void resetLevel(bool all) {
    auto& s = state();
    {
        std::unique_lock lock(s.infoMutex);
        s.infos.clear();
    }
    std::lock_guard lock(s.mutex);
    s.swings.clear();
    s.logged = false;
    if (all) s.tracked.clear();
    meshCache().clear();
}

/// The world vector of a FacingID (rotation axes).
glm::vec3 faceVector(int face) {
    auto o = ce::faceOffset(face);
    return {float(o[0]), float(o[1]), float(o[2])};
}

/// Direction#getCounterClockWise.
int counterClockwise(int face) { return ce::clockwise(ce::clockwise(ce::clockwise(face))); }

void renderBells(BaseActorRenderContext& ctx) {
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
    std::vector<std::pair<BlockPos, Info>> bells;
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
        bells.emplace_back(p, info);
    }
    if (!stale.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : stale) s.tracked.erase(p);
    }
    for (auto const& p : stale) s.swings.erase(p);
    if (bells.empty()) return;

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

    for (auto const& [pos, info] : bells) {
        auto* mesh = bellMesh(tess, info.texture);
        if (!mesh) continue;
        glm::mat4 m{1.f};
        m = glm::translate(m, glm::vec3{pos.x - cam.x + .5f, pos.y - cam.y + .5f, pos.z - cam.z + .5f});
        if (auto it = s.swings.find(pos); it != s.swings.end()) {
            float t = static_cast<float>(tick - it->second.start) + alpha;
            if (tick < it->second.start || t >= static_cast<float>(ce::kBellRingDuration)) s.swings.erase(it);
            else m = glm::rotate(m, ce::bellSwingAngle(t), faceVector(counterClockwise(it->second.dir)));
        }
        m        = glm::translate(m, glm::vec3{-.5f, -.5f, -.5f});
        auto ref = world.push(false);
        ref.mat->_m.get() = ref.mat->_m.get() * m;
        mesh->renderMesh(screen, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    }
    if (!s.logged) {
        s.logged = true;
        log().info("equipment client: drawing {} bells", bells.size());
    }
}

// ---- cardboard stealth overlay -------------------------------------------------------------------------

bool hiding(LocalPlayer& p) {
    using SharedTypes::Legacy::ArmorSlot;
    namespace items = feature::common::items;
    if (!p.isSneaking() || p.isFlying()) return false;
    return items::idOf(p.getArmor(ArmorSlot::Head)) == "levitate:cardboard_helmet" && items::idOf(p.getArmor(ArmorSlot::Torso)) == "levitate:cardboard_chestplate"
        && items::idOf(p.getArmor(ArmorSlot::Legs)) == "levitate:cardboard_leggings" && items::idOf(p.getArmor(ArmorSlot::Feet)) == "levitate:cardboard_boots";
}

void drawStealth(MinecraftUIRenderContext& ctx) {
    auto& s = state();
    float v = s.stealth; // per tick; drawn at the latest value
    if (v <= 0.005f) return;
    auto [sw, sh] = guiSize();
    Canvas c(ctx);
    // a cardboard-brown vignette: the edges dark, a soft fade towards the middle
    constexpr int kBands = 12;
    for (int i = 0; i < kBands; ++i) {
        float    f    = 1.f - static_cast<float>(i) / kBands;
        float    a    = v * (0.12f + 0.55f * f * f);
        unsigned argb = (static_cast<unsigned>(a * 255.f) << 24) | 0x5a4128u;
        float    bx   = sw * .045f, by = sh * .045f;
        float    x = bx * i, y = by * i;
        c.fill({x, y, sw - 2 * x, by}, argb);
        c.fill({x, sh - y - by, sw - 2 * x, by}, argb);
        c.fill({x, y + by, bx, sh - 2 * y - 2 * by}, argb);
        c.fill({sw - x - bx, y + by, bx, sh - 2 * y - 2 * by}, argb);
    }
    // a faint flap seam across the top
    c.fill({0, sh * .02f, sw, 2}, (static_cast<unsigned>(v * 120.f) << 24) | 0x3b2a18u);
}

} // namespace

// Chunk meshes: bells are meshed without the bell; remember where they are.
LL_STATIC_HOOK(
    BellExtractHook,
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
    BellRenderHook,
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
        renderBells(renderContext);
    } catch (...) {
    }
}

class CuriositiesClientModule {
public:
    void install() {
        BellExtractHook::hook();
        BellRenderHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevel(true);
            state().level   = nullptr;
            state().stealth = 0;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) {
            // CardboardArmorStealthOverlay#clientTick: chase 1 while hiding, else 0 (EXP, 0.25)
            auto* p      = currentPlayer();
            auto& s      = state();
            float target = p && hiding(*p) ? 1.f : 0.f;
            s.stealth += (target - s.stealth) * .25f;
            if (std::abs(target - s.stealth) < 1e-3f) s.stealth = target;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            if (ev.screenView().getScreenName() != "hud_screen") return;
            drawStealth(ev.uiRenderContext());
        }));
    }

    void load() {
        net::client::on("equipment.bell.ring", [](nlohmann::json const& msg) {
            auto const& p = msg.value("pos", nlohmann::json::array());
            if (!p.is_array() || p.size() != 3) return;
            auto* ci    = lv::client::currentClient();
            auto* level = ci ? ci->getLevel() : nullptr;
            if (!level) return;
            BlockPos pos{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
            state().swings[pos] = Swing{static_cast<std::uint64_t>(level->getCurrentTick().tickID), msg.value("dir", 3)};
        });
    }

    void uninstall() {
        BellExtractHook::unhook();
        BellRenderHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::equipment

LV_REGISTER_MODULE(lv::client::equipment::CuriositiesClientModule);

#endif // LV_CLIENT
