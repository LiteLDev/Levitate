#ifdef LV_CLIENT

#include "client/building/BlockQuads.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>

#include "glm/gtc/matrix_transform.hpp"
#include "nlohmann/json.hpp"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/ActorShaderManager.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/ptexture/LightTexture.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/math/Vec4.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "core/ponder/BlockModel.h"
#include "mod/Log.h"
#include "mod/Main.h"

namespace lv::client::building {

namespace cp = core::ponder;

namespace {

// ---- the pack's block models (Levitate blocks) ---------------------------------------------------

struct Models {
    std::map<std::string, cp::BlockFile> blocks;     ///< by block id
    std::map<std::string, cp::Geometry>  geometries; ///< by geometry identifier
};

Models const& models() {
    static Models m = [] {
        Models out;
        auto   mod  = lv::Mod::getInstance().getSelf().getModDir();
        auto   read = [](std::filesystem::path const& p) {
            std::ifstream in(p);
            return nlohmann::json::parse(in, nullptr, false, true);
        };
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator(mod / "behavior_packs" / "Levitate_BP" / "blocks", ec)) {
            if (auto f = cp::parseBlockFile(read(e.path()))) out.blocks[f->id] = std::move(*f);
        }
        for (auto const& e : std::filesystem::recursive_directory_iterator(mod / "resource_packs" / "Levitate_RP" / "models" / "blocks", ec)) {
            if (e.path().extension() != ".json") continue;
            out.geometries.merge(cp::parseGeometries(read(e.path())));
        }
        return out;
    }();
    return m;
}

struct Cache {
    std::unordered_map<Block const*, std::vector<Quad>> quads;
    std::unordered_map<std::string, glm::vec4>         uvs;
    std::unique_ptr<BlockTessellator>                  tessellator;
    void const*                                        tessRegion = nullptr;
};
Cache& cache() {
    static Cache c;
    return c;
}

/// Integer / string states of a block (its serialization NBT).
void statesOf(Block const& block, std::map<std::string, int>& ints, std::map<std::string, std::string>& strings) {
    auto const& nbt = *block.mSerializationId;
    if (!nbt.contains("states")) return;
    for (auto const& [k, v] : nbt["states"].get<CompoundTag>()) {
        if (v.hold(Tag::Type::Int)) ints[k] = v.get<IntTag>().data;
        else if (v.hold(Tag::Type::Byte)) ints[k] = v.get<ByteTag>().data;
        else if (v.hold(Tag::Type::String)) strings[k] = std::string(v.get<StringTag>());
    }
}

std::vector<Quad> modelQuads(Block const& block) {
    std::vector<Quad> out;
    auto const&       m  = models();
    auto              bf = m.blocks.find(block.getTypeName());
    if (bf == m.blocks.end()) return out;
    std::map<std::string, int>         ints = bf->second.defaults;
    std::map<std::string, std::string> strings;
    statesOf(block, ints, strings);
    auto                resolved = cp::resolve(bf->second, ints, strings);
    cp::Geometry const* geo      = nullptr;
    if (resolved.geometry == "minecraft:geometry.full_block") geo = &cp::fullBlock();
    else if (auto g = m.geometries.find(resolved.geometry); g != m.geometries.end()) geo = &g->second;
    if (!geo) return out;
    for (auto const& q : cp::buildQuads(resolved, *geo, ints, {}, strings)) {
        auto uv = atlasUv(q.texture);
        Quad o;
        for (int i = 0; i < 4; ++i) {
            o.pos[i] = {q.pos[i][0], q.pos[i][1], q.pos[i][2]};
            o.uv[i]  = {uv.x + (uv.z - uv.x) * q.uv[i][0], uv.y + (uv.w - uv.y) * q.uv[i][1]};
        }
        o.color = {q.shade, q.shade, q.shade, 1.f};
        out.push_back(o);
    }
    return out;
}

BlockTessellator* ownTessellator() {
    auto& c      = cache();
    auto* player = currentPlayer();
    if (!player) return nullptr;
    auto& region = player->getDimensionBlockSource();
    if (!c.tessellator || c.tessRegion != &region) {
        c.tessellator = std::make_unique<BlockTessellator>(&region);
        c.tessRegion  = &region;
    }
    return c.tessellator.get();
}

/// A vanilla block tessellated by the game, read back from the tessellator.
std::vector<Quad> gameQuads(Tessellator& tess, Block const& block) {
    std::vector<Quad> out;
    auto*             bt = ownTessellator();
    if (!bt) return out;
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    bt->appendTessellatedBlock(tess, block);
    {
        auto const& md     = *tess.mMeshData;
        auto const& pos    = md.mPositions.get();
        auto const& uvs    = md.mTextureUVs[0].get();
        auto const& colors = md.mColors.get();
        for (std::size_t i = 0; i + 3 < pos.size(); i += 4) {
            Quad q;
            for (int k = 0; k < 4; ++k) {
                q.pos[k] = pos[i + k];
                q.uv[k]  = i + k < uvs.size() ? uvs[i + k] : glm::vec2(0.f);
            }
            if (i < colors.size()) {
                uint c  = colors[i];
                q.color = {(c & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, ((c >> 16) & 0xff) / 255.f, 1.f};
            }
            out.push_back(q);
        }
    }
    tess.clear();
    // the game tessellates around the block's centre: move to the block's corner
    float lo = 1e9f;
    for (auto const& q : out)
        for (auto const& v : q.pos) lo = std::min({lo, v.x, v.y, v.z});
    if (lo < -0.25f)
        for (auto& q : out)
            for (auto& v : q.pos) v += glm::vec3{.5f};
    // Minecraft's directional face shading (the chunk mesh's, which this read-back has not)
    static constexpr float kShade[6]{0.5f, 1.f, 0.8f, 0.8f, 0.6f, 0.6f};
    for (auto& q : out) {
        int face = cubeFace(q);
        if (face >= 0) q.color = glm::vec4{glm::vec3{q.color} * kShade[face], q.color.a};
    }
    return out;
}

} // namespace

int cubeFace(Quad const& q) {
    constexpr float e = 1e-4f;
    for (int a = 0; a < 3; ++a) {
        float v = q.pos[0][a];
        if (std::abs(q.pos[1][a] - v) > e || std::abs(q.pos[2][a] - v) > e || std::abs(q.pos[3][a] - v) > e) continue;
        if (std::abs(v) < e) return a == 0 ? 4 : (a == 1 ? 0 : 2);
        if (std::abs(v - 1.f) < e) return a == 0 ? 5 : (a == 1 ? 1 : 3);
    }
    return -1;
}

std::vector<Quad> const& quadsOf(Tessellator& tess, Block const& block) {
    auto& c = cache().quads;
    if (auto it = c.find(&block); it != c.end()) return it->second;
    auto quads = modelQuads(block);
    if (quads.empty() && block.getTypeName().rfind("levitate:", 0) != 0) quads = gameQuads(tess, block);
    return c.emplace(&block, std::move(quads)).first->second;
}

std::optional<Quad> cropAndMove(Quad const& q, glm::vec3 lo, glm::vec3 hi, glm::vec3 move) {
    // the quad's parameterisation: p = p0 + s * e1 + t * e2, uv the same way
    glm::vec3 e1 = q.pos[1] - q.pos[0], e2 = q.pos[3] - q.pos[0];
    float     l1 = glm::dot(e1, e1), l2 = glm::dot(e2, e2);
    if (l1 < 1e-10f || l2 < 1e-10f) return std::nullopt;
    Quad out = q;
    for (int i = 0; i < 4; ++i) {
        glm::vec3 p = glm::clamp(q.pos[i], lo, hi);
        glm::vec3 d = p - q.pos[0];
        float     s = glm::dot(d, e1) / l1, t = glm::dot(d, e2) / l2;
        out.uv[i]   = q.uv[0] + s * (q.uv[1] - q.uv[0]) + t * (q.uv[3] - q.uv[0]);
        out.pos[i]  = p + move;
    }
    glm::vec3 a = out.pos[1] - out.pos[0], b = out.pos[3] - out.pos[0];
    if (glm::length(glm::cross(a, b)) < 1e-7f) return std::nullopt;
    return out;
}

std::unique_ptr<mce::Mesh> buildMesh(Tessellator& tess, std::vector<Quad> const& quads, bool twoSided) {
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    auto v3 = [](glm::vec3 const& p) { return Vec3{p.x, p.y, p.z}; };
    auto v2 = [](glm::vec2 const& p) { return Vec2{p.x, p.y}; };
    for (auto const& q : quads) {
        tess.color(q.color.r, q.color.g, q.color.b, q.color.a);
        tess.simpleQuad(v3(q.pos[0]), v2(q.uv[0]), v3(q.pos[1]), v2(q.uv[1]), v3(q.pos[2]), v2(q.uv[2]), v3(q.pos[3]), v2(q.uv[3]));
        if (twoSided) tess.simpleQuad(v3(q.pos[3]), v2(q.uv[3]), v3(q.pos[2]), v2(q.uv[2]), v3(q.pos[1]), v2(q.uv[1]), v3(q.pos[0]), v2(q.uv[0]));
    }
    return std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_building", SupplementaryFieldAutoGenerationMode::None));
}

glm::vec4 atlasUv(std::string const& texture) {
    auto& c = cache().uvs;
    if (auto it = c.find(texture); it != c.end()) return it->second;
    auto t = BlockGraphics::getTextureUVCoordinateSet(texture, 0, 0);
    return c[texture] = glm::vec4{t._u0, t._v0, t._u1, t._v1};
}

std::optional<glm::vec4> faceUv(Block const& block, int face) {
    auto const* g = BlockGraphics::getForBlock(block);
    if (!g) return std::nullopt;
    auto const& t = g->getTexture(static_cast<uint64>(std::clamp(face, 0, 5)), 0);
    if (t._u0 == t._u1 || t._v0 == t._v1) return std::nullopt;
    return glm::vec4{t._u0, t._v0, t._u1, t._v1};
}

void resetQuadCache() {
    auto& c = cache();
    c.quads.clear();
    c.uvs.clear();
    c.tessellator.reset();
    c.tessRegion = nullptr;
}

// ---- drawing ---------------------------------------------------------------------------------------

Drawer::Drawer(BaseActorRenderContext& ctx) : mCtx(ctx) {
    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    mMaterial = &moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
    mBlend    = &moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerBlend)].get();
    Vec3 cam  = ctx.mImpl.get()->mCameraTargetPosition;
    mCam      = {cam.x, cam.y, cam.z};
    mOk       = true;
}

Tessellator& Drawer::tessellator() { return mCtx.mScreenContext.tessellator; }

void Drawer::light(BlockSource& region, BlockPos const& pos) {
    auto* ci = currentClient();
    if (!ci) return;
    auto* lt = ci->getLightTexture();
    if (!lt) return;
    ActorShaderManager::setupShaderParameters(mCtx.mScreenContext, region, pos, mCtx.mFrameAlpha, false, *lt, mCtx.mLightVolumeManager, Vec2{1.f, 1.f}, Vec4{0.f, 0.f, 0.f, 0.f});
}

void Drawer::draw(mce::Mesh const& mesh, glm::mat4 const& model) { drawWith(mMaterial, mesh, model); }

void Drawer::drawBlended(mce::Mesh const& mesh, glm::mat4 const& model) { drawWith(mBlend, mesh, model); }

void Drawer::drawWith(void const* material, mce::Mesh const& mesh, glm::mat4 const& model) {
    if (!mOk || !material) return;
    auto& screen     = mCtx.mScreenContext;
    auto& dispatcher = mCtx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return;
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto&     world = screen.camera.worldMatrixStack.get();
    auto      ref   = world.push(false);
    glm::mat4 m     = glm::translate(glm::mat4{1.f}, -mCam) * model;
    ref.mat->_m.get() = ref.mat->_m.get() * m;
    mesh.renderMesh(screen, *static_cast<mce::MaterialPtr const*>(material), texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
}

} // namespace lv::client::building

#endif // LV_CLIENT
