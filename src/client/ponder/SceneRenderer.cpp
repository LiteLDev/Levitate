#ifdef LV_CLIENT

// Draws a Ponder scene state into a screen.
//
// Levitate blocks are built from the pack's own block files and geometry (core/ponder/BlockModel.h:
// permutations, bone visibility, material instances, transformations); other blocks are tessellated
// by a block tessellator of our own (`appendTessellatedBlock`). Both are drawn with the block atlas
// through the UI pass, with the scene camera (core::ponder::View) as the world matrix.
// Blocks are drawn back to front. A block id that is not registered (a unit that has not landed)
// is drawn as a placeholder outline and skipped. Kinetic blocks turn: blocks with a kinetic rotor
// model (U2) draw their static part and their rotor bones at the scene's angle; other kinetic blocks
// turn as a whole about their axis.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <set>
#include <unordered_map>

#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/level/BlockSource.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshContext.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/deps/renderer/ShaderColor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "client/Client.h"
#include "client/ponder/Ponder.h"
#include "client/ponder/PonderConfig.h"
#include "client/screen/Screen.h"
#include "core/kinetics/RotorModel.h"
#include "core/ponder/BlockModel.h"
#include "feature/common/Items.h"
#include "feature/kinetics/KineticsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"

namespace lv::client::ponder {

namespace k  = core::kinetics;
namespace fk = feature::kinetics;
using screen::Align;
using screen::Canvas;
using screen::Rect;

namespace {

// ---- block lookup ------------------------------------------------------------------------------

struct BlockInfo {
    Block const*            block  = nullptr; ///< nullptr: not registered (placeholder)
    Block const*            statics = nullptr; ///< kinetic rotor block: its static part (rotor = 1)
    fk::KineticSpec const*  spec   = nullptr;
    k::BlockState           snap;
};

struct Cache {
    std::map<std::string, BlockInfo>                   blocks;
    std::set<std::string>                              loggedMissing;
    std::map<std::string, std::unique_ptr<mce::Mesh>> rotorMeshes;
    std::map<Block const*, std::unique_ptr<mce::Mesh>> blockMeshes;
    std::map<std::string, std::unique_ptr<mce::Mesh>> modelMeshes; ///< by block key (id + states)
    std::unique_ptr<BlockTessellator>                  tessellator; ///< our own: the game's is shared with item icons
    void const*                                        tessRegion = nullptr;
};
Cache& cache() {
    static Cache c;
    return c;
}

/// Fading blocks (Create's PonderWorld fades sections in and out) are drawn with the blend layer
/// and their vertex alpha, quantised to 16 steps so their meshes can be cached.
int alphaStep(float alpha) { return std::clamp(static_cast<int>(std::lround(alpha * 16.f)), 1, 16); }
float stepAlpha(int step) { return static_cast<float>(step) / 16.f; }

std::string keyOf(cp::BlockDef const& def) {
    std::string key = def.id;
    for (auto const& [k, v] : def.states) key += "|" + k + "=" + std::to_string(v);
    for (auto const& [k, v] : def.strings) key += "|" + k + "=" + v;
    return key;
}

BlockInfo const& infoOf(cp::BlockDef const& def) {
    auto& c          = cache();
    auto [it, fresh] = c.blocks.try_emplace(keyOf(def));
    if (!fresh) return it->second;
    std::vector<std::pair<std::string_view, int>> states;
    for (auto const& [k, v] : def.states) states.emplace_back(k, v);
    auto& info = it->second;
    info.block = game::blockWith(def.id, states);
    if (!info.block) {
        if (c.loggedMissing.insert(def.id).second) log().info("ponder: block {} is not available, drawn as a placeholder", def.id);
        return info;
    }
    info.spec = fk::detail::specOfBlock(*info.block);
    if (info.spec) {
        info.snap = fk::detail::snapshot(*info.block, *info.spec);
        if (!info.spec->rotorState.empty()) info.statics = fk::detail::withState(*info.block, info.spec->rotorState, 1);
    }
    return info;
}

// ---- Levitate block models (the pack's own block files and geometry) ----------------------------

struct Models {
    std::map<std::string, cp::BlockFile> blocks;     ///< by block id
    std::map<std::string, cp::Geometry>  geometries; ///< by geometry identifier
};

Models const& models() {
    static Models m = [] {
        Models out;
        auto   mod = lv::Mod::getInstance().getSelf().getModDir();
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
        log().info("ponder: {} block models, {} geometries", out.blocks.size(), out.geometries.size());
        return out;
    }();
    return m;
}

/// The quads of a Levitate block from its block file and geometry (custom geometry, bone
/// visibility, material instances and transformations as the game resolves them); nullopt for
/// other blocks.
std::optional<std::vector<cp::Quad>> modelQuads(cp::BlockDef const& def, std::vector<std::string> const& hideBones = {}) {
    auto const& m  = models();
    auto        bf = m.blocks.find(def.id);
    if (bf == m.blocks.end()) return std::nullopt;
    auto resolved = cp::resolve(bf->second, def.states, def.strings);
    auto states   = bf->second.defaults;
    for (auto const& [k, v] : def.states) states[k] = v;
    cp::Geometry const* geo = nullptr;
    if (resolved.geometry == "minecraft:geometry.full_block") geo = &cp::fullBlock();
    else if (auto g = m.geometries.find(resolved.geometry); g != m.geometries.end()) geo = &g->second;
    if (!geo) return std::nullopt;
    return cp::buildQuads(resolved, *geo, states, hideBones, def.strings);
}

/// Writes quads into a begun tessellator at `o` (both windings: the UI pass does not know which
/// side faces the camera).
void emitQuads(Tessellator& tess, std::vector<cp::Quad> const& quads, float alpha, Vec3 o = {}) {
    for (auto const& q : quads) {
        auto  tex = BlockGraphics::getTextureUVCoordinateSet(q.texture, 0, 0);
        float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
        auto  uv = [&](cp::V2 const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
        auto  at = [&](cp::V3 const& p) { return Vec3{p[0] + o.x, p[1] + o.y, p[2] + o.z}; };
        tess.color(q.shade, q.shade, q.shade, alpha);
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
}

/// A Levitate block drawn from its block file and geometry; nullptr for other blocks.
mce::Mesh* modelMesh(Tessellator& tess, cp::BlockDef const& def, int step = 16) {
    auto  key = keyOf(def) + "#" + std::to_string(step);
    auto& c   = cache().modelMeshes;
    if (auto it = c.find(key); it != c.end()) return it->second.get();
    auto quads = modelQuads(def);
    if (!quads) return nullptr;
    auto& slot = c[key];
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    emitQuads(tess, *quads, stepAlpha(step));
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_ponder_model", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}


bool isWater(std::string const& id) { return id == "minecraft:water" || id == "minecraft:flowing_water"; }
bool isLava(std::string const& id) { return id == "minecraft:lava" || id == "minecraft:flowing_lava"; }

/// Minecraft's height of a fluid block (LiquidBlockRenderer): 8/9 of a block for a source, one
/// ninth less per level (`liquid_depth` 1..7), a falling fluid (8..) fills the block.
float fluidLevelHeight(cp::BlockDef const& def) {
    auto it    = def.states.find("liquid_depth");
    int  depth = it == def.states.end() ? 0 : it->second;
    if (depth >= 8) return 1.f;
    return static_cast<float>(8 - std::clamp(depth, 0, 7)) / 9.f;
}

/// Corner heights (x0z0, x1z0, x1z1, x0z1) of the fluid at a scene position: the average of the
/// fluid blocks of the same kind around the corner, the full block when one has fluid above it
/// (Minecraft's getHeight / calculateAverageHeight).
using FluidCorners = std::array<float, 4>;

/// Water and lava (the block tessellator draws no liquids on their own): the still texture when
/// level, the flowing one when the surface slopes, tinted, with the surface at its corners' heights.
mce::Mesh* fluidMesh(Tessellator& tess, std::string const& id, int step, FluidCorners corners) {
    bool water = isWater(id), lava = isLava(id);
    if (!water && !lava) return nullptr;
    auto  q   = [](float h) { return std::to_string(static_cast<int>(std::lround(h * 72.f))); };
    auto  key = id + "#" + std::to_string(step) + "|" + q(corners[0]) + "," + q(corners[1]) + "," + q(corners[2]) + "," + q(corners[3]);
    auto& c   = cache().modelMeshes;
    if (auto it = c.find(key); it != c.end()) return it->second.get();
    auto& slot  = c[key];
    float alpha = stepAlpha(step);
    bool  flat  = corners[0] == corners[1] && corners[1] == corners[2] && corners[2] == corners[3];
    auto  tex   = BlockGraphics::getTextureUVCoordinateSet(water ? (flat ? "still_water_grey" : "flowing_water_grey") : (flat ? "still_lava" : "flowing_lava"), 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](float u, float v) { return Vec2{u0 + (u1 - u0) * u, v0 + (v1 - v0) * v}; };
    float r = water ? 0.25f : 1.f, g = water ? 0.46f : 1.f, b = water ? 0.92f : 1.f;
    std::array<Vec3, 4> top{Vec3{0, corners[0], 0}, Vec3{1, corners[1], 0}, Vec3{1, corners[2], 1}, Vec3{0, corners[3], 1}};
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    auto quad = [&](std::array<Vec3, 4> const& v, std::array<Vec2, 4> const& t, float shade) {
        tess.color(r * shade, g * shade, b * shade, alpha);
        tess.simpleQuad(v[0], t[0], v[1], t[1], v[2], t[2], v[3], t[3]);
        tess.simpleQuad(v[3], t[3], v[2], t[2], v[1], t[1], v[0], t[0]);
    };
    // the surface and the bottom
    quad(top, {uv(0, 0), uv(1, 0), uv(1, 1), uv(0, 1)}, 1.f);
    quad({Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 0, 1}, Vec3{0, 0, 1}}, {uv(0, 0), uv(1, 0), uv(1, 1), uv(0, 1)}, 0.5f);
    // the four sides up to the surface
    for (int i = 0; i < 4; ++i) {
        Vec3 a = top[static_cast<std::size_t>(i)], bb = top[static_cast<std::size_t>((i + 1) & 3)];
        quad({Vec3{a.x, 0, a.z}, Vec3{bb.x, 0, bb.z}, bb, a}, {uv(0, 1), uv(1, 1), uv(1, 1 - bb.y), uv(0, 1 - a.y)}, (i & 1) ? 0.6f : 0.8f);
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_ponder_fluid", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

// ---- rotor meshes (U2's rotor geometry, as the kinetics renderer draws it) ---------------------

std::map<std::string, k::RotorModel> const& rotorModels() {
    static std::map<std::string, k::RotorModel> models = [] {
        std::map<std::string, k::RotorModel> out;
        auto dir = lv::Mod::getInstance().getSelf().getModDir() / "resource_packs" / "Levitate_RP" / "models" / "blocks";
        std::error_code ec;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".json") continue;
            std::ifstream in(entry.path());
            auto          j = nlohmann::json::parse(in, nullptr, false, true);
            if (j.is_discarded()) continue;
            if (auto m = k::parseGeometry(j)) out.merge(*m);
        }
        return out;
    }();
    return models;
}

mce::Mesh* rotorMesh(Tessellator& tess, fk::KineticSpec const& spec, std::string const& bone, int step = 16) {
    auto  key = spec.rotorModel + "|" + bone + "|" + spec.rotorTexture + "#" + std::to_string(step);
    auto& c   = cache().rotorMeshes;
    if (auto it = c.find(key); it != c.end()) return it->second.get();
    auto&       slot   = c[key];
    auto const& models = rotorModels();
    auto        model  = models.find(spec.rotorModel);
    if (model == models.end()) return nullptr;
    auto b = model->second.bones.find(bone);
    if (b == model->second.bones.end() || b->second.empty()) return nullptr;
    auto  tex = BlockGraphics::getTextureUVCoordinateSet(spec.rotorTexture, 0, 0);
    float u0 = tex._u0, v0 = tex._v0, u1 = tex._u1, v1 = tex._v1;
    auto  uv = [&](std::array<float, 2> const& t) { return Vec2{u0 + (u1 - u0) * t[0], v0 + (v1 - v0) * t[1]}; };
    auto  at = [](std::array<float, 3> const& p) { return Vec3{p[0], p[1], p[2]}; };
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, stepAlpha(step));
    for (auto const& q : b->second) {
        tess.simpleQuad(at(q.pos[0]), uv(q.uv[0]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[3]), uv(q.uv[3]));
        tess.simpleQuad(at(q.pos[3]), uv(q.uv[3]), at(q.pos[2]), uv(q.uv[2]), at(q.pos[1]), uv(q.uv[1]), at(q.pos[0]), uv(q.uv[0]));
    }
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_ponder_rotor", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// The block's own geometry, tessellated by the game into our tessellator (same vertex format as
/// the rotor meshes, drawn with the block atlas).
mce::Mesh* blockMesh(Tessellator& tess, BlockTessellator& bt, Block const& block) {
    auto& c = cache().blockMeshes;
    if (auto it = c.find(&block); it != c.end()) return it->second.get();
    auto& slot = c[&block];
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    tess.color(1.f, 1.f, 1.f, 1.f);
    bt.appendTessellatedBlock(tess, block);
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_ponder_block", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

/// A block tessellator of our own (tessellating with the game's shared one disturbs the item
/// icons it also draws), bound to the local player's region.
BlockTessellator* ponderTessellator() {
    auto& c      = cache();
    auto* player = currentPlayer();
    if (!player) return nullptr;
    auto& region = player->getDimensionBlockSource();
    if (!c.tessellator || c.tessRegion != &region) {
        c.blockMeshes.clear();
        c.tessellator = std::make_unique<BlockTessellator>(&region);
        c.tessRegion  = &region;
    }
    return c.tessellator.get();
}

// ---- 2D helpers ----------------------------------------------------------------------------------

/// A dotted line (the UI context has no line primitive).
void line(Canvas& c, float x0, float y0, float x1, float y1, unsigned argb, float dot = 1.f, float step = 1.5f) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    int   n   = std::max(1, static_cast<int>(len / step));
    for (int i = 0; i <= n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n);
        c.fill({x0 + dx * t - dot / 2, y0 + dy * t - dot / 2, dot, dot}, argb);
    }
}

void boxOutline(Canvas& c, cp::View const& v, cp::Vec3f lo, cp::Vec3f hi, unsigned argb, float dot = 1.f) {
    cp::Vec3f p[8];
    for (int i = 0; i < 8; ++i) p[i] = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
    static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto const& e : edges) {
        auto a = v.project(p[e[0]]), b = v.project(p[e[1]]);
        line(c, a.x - c.originX(), a.y - c.originY(), b.x - c.originX(), b.y - c.originY(), argb, dot);
    }
}

unsigned withAlpha(unsigned argb, float a) {
    auto alpha = static_cast<unsigned>(std::clamp(a, 0.f, 1.f) * static_cast<float>(argb >> 24));
    return (argb & 0x00FFFFFF) | (alpha << 24);
}

glm::mat4 toGlm(std::array<float, 16> const& m) { return glm::make_mat4(m.data()); }

/// The scene camera for the UI pass. The GUI depth range only keeps depths at or behind 0, where
/// larger is nearer: every depth is pushed behind 0 by the scene's radius (in blocks).
glm::mat4 uiCamera(cp::View const& v, float radius) {
    float d = v.scale * config().depthScale;
    return glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, -(radius * d + 1.f))) * toGlm(v.matrix(d));
}

glm::vec3 axisVec(k::Axis a) {
    return {a == k::Axis::X ? 1.f : 0.f, a == k::Axis::Y ? 1.f : 0.f, a == k::Axis::Z ? 1.f : 0.f};
}

/// Section transform (movement already in BlockView::offset): the rotation about its origin.
glm::mat4 sectionTurn(cp::SceneState const& st, std::string const& section) {
    glm::mat4 m(1.f);
    if (section.empty()) return m;
    auto it = st.sections.find(section);
    if (it == st.sections.end()) return m;
    auto const& s = it->second;
    // each turn about its origin (which moves with the section), later turns applied last
    for (auto const& t : s.turns) {
        if (t.degrees == 0.f) continue;
        glm::vec3 o{t.origin.x + s.offset.x, t.origin.y + s.offset.y, t.origin.z + s.offset.z};
        glm::vec3 ax{t.axis == 'x' ? 1.f : 0.f, t.axis == 'y' ? 1.f : 0.f, t.axis == 'z' ? 1.f : 0.f};
        glm::mat4 r(1.f);
        r = glm::translate(r, o);
        r = glm::rotate(r, glm::radians(t.degrees), ax);
        r = glm::translate(r, -o);
        m = r * m;
    }
    return m;
}
cp::Vec3f transformPoint(glm::mat4 const& m, cp::Vec3f p) {
    glm::vec4 r = m * glm::vec4(p.x, p.y, p.z, 1.f);
    return {r.x, r.y, r.z};
}

} // namespace

mce::Mesh* modelPartMesh(Tessellator& tess, cp::BlockDef const& def, std::vector<std::string> const& hideBones) {
    auto key = keyOf(def) + "#parts";
    for (auto const& b : hideBones) key += "-" + b;
    auto& c = cache().modelMeshes;
    if (auto it = c.find(key); it != c.end()) return it->second.get();
    auto quads = modelQuads(def, hideBones);
    if (!quads) return nullptr;
    auto& slot = c[key];
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    emitQuads(tess, *quads, 1.f);
    slot = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_model_part", SupplementaryFieldAutoGenerationMode::None));
    return slot.get();
}

bool appendBlock(Tessellator& tess, cp::BlockDef const& def, Block const& block, float x, float y, float z) {
    // Levitate blocks: the pack's own geometry
    if (models().blocks.contains(def.id)) {
        auto quads = modelQuads(def);
        if (!quads) return false;
        emitQuads(tess, *quads, 1.f, Vec3{x, y, z});
        return true;
    }
    // other blocks: the game's block tessellator, moved to the block's place
    auto* bt = ponderTessellator();
    if (!bt) return false;
    tess.resetTransform(true);
    tess.addTranslation(Vec3{x, y, z});
    tess.color(1.f, 1.f, 1.f, 1.f);
    bt->appendTessellatedBlock(tess, block);
    tess.resetTransform(true);
    return true;
}

void resetRenderCache() {
    auto& c = cache();
    c.blocks.clear();
    c.rotorMeshes.clear();
    c.blockMeshes.clear();
    c.modelMeshes.clear();
    c.tessellator.reset();
    c.tessRegion = nullptr;
}

std::string itemName(std::string const& id) {
    auto stack = feature::common::items::make(id);
    if (!stack) return {};
    return stack->getName();
}

bool drawIcon(Canvas& c, std::string const& id, float x, float y, float size) {
    auto* client = currentClient();
    if (!client) return false;
    auto const& m = models();
    if (!m.blocks.contains(id)) {
        // items and vanilla blocks: the game's item renderer
        auto stack = feature::common::items::make(id);
        if (!stack) return false;
        c.item(*stack, x + (size - 16) / 2, y + (size - 16) / 2);
        return true;
    }
    cp::BlockDef def{id, {}};
    auto const&  info = infoOf(def);
    if (!info.block) return false;
    auto&                  uiCtx = c.context();
    ScreenContext&         sc    = uiCtx.mScreenContext;
    Tessellator&           tess  = sc.tessellator;
    BaseActorRenderContext rc(sc, *client, client->getMinecraftGame());
    BlockActorRenderDispatcher& dispatcher = rc.mBlockEntityRenderDispatcher;
    auto* moving = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    if (!moving) return false;
    auto* mesh = modelMesh(tess, def);
    if (!mesh) return false;
    // an item-icon view of the block: from the south-west, 30 degrees from above
    cp::View v;
    v.center  = {0.5f, 0.5f, 0.5f};
    v.scale   = size * 0.58f;
    v.screenX = c.originX() + x + size / 2;
    v.screenY = c.originY() + y + size / 2;
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
    auto& worldMat    = sc.camera.worldMatrixStack.get();
    auto  ref         = worldMat.push(false);
    ref.mat->_m.get() = ref.mat->_m.get() * uiCamera(v, 2.f);
    mesh->renderMesh(sc, moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get(), texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
    return true;
}

DrawResult drawScene(
    Canvas&               c,
    cp::Scene const&      scene,
    cp::SceneState const& st,
    cp::View const&       view,
    bool                  identify,
    float                 mx,
    float                 my
) {
    DrawResult res;
    auto*      client = currentClient();
    if (!client) return res;
    auto const& cfg = config();

    // ---- order blocks back to front ----
    struct Item {
        cp::BlockView const* b;
        glm::mat4            place; ///< scene transform of the block's corner (offset, section turn)
        float                depth;
    };
    std::vector<Item> items;
    items.reserve(st.blocks.size());
    for (auto const& b : st.blocks) {
        glm::mat4 turn  = sectionTurn(st, b.section);
        glm::mat4 place = glm::translate(turn, glm::vec3(b.pos.x + b.offset.x, b.pos.y + b.offset.y, b.pos.z + b.offset.z));
        auto      ctr   = transformPoint(place, {0.5f, 0.5f, 0.5f});
        items.push_back({&b, place, view.project(ctr).depth});
    }
    std::stable_sort(items.begin(), items.end(), [](Item const& a, Item const& b) { return a.depth < b.depth; });

    // ---- picking (identify mode): the nearest block whose projected cube contains the mouse ----
    cp::BlockView const* picked = nullptr;
    glm::mat4            pickedPlace(1.f);
    if (identify) {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (it->b->base) continue;
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (int i = 0; i < 8; ++i) {
                auto p = view.project(transformPoint(it->place, {static_cast<float>(i & 1), static_cast<float>((i >> 1) & 1), static_cast<float>((i >> 2) & 1)}));
                minX = std::min(minX, p.x), maxX = std::max(maxX, p.x), minY = std::min(minY, p.y), maxY = std::max(maxY, p.y);
            }
            // shrink to the hexagon's inner box so neighbours don't steal the pick
            float ix = (maxX - minX) * 0.15f, iy = (maxY - minY) * 0.15f;
            if (mx >= minX + ix && mx <= maxX - ix && my >= minY + iy && my <= maxY - iy) {
                picked      = it->b;
                pickedPlace = it->place;
                break;
            }
        }
    }

    // ---- game renderer state ----
    auto&                  uiCtx = c.context();
    ScreenContext&         sc    = uiCtx.mScreenContext;
    Tessellator&           tess  = sc.tessellator;
    BaseActorRenderContext rc(sc, *client, client->getMinecraftGame());
    auto&                  dispatcher = rc.mBlockEntityRenderDispatcher;
    auto* moving = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());
    mce::MaterialPtr const* material = nullptr;
    mce::MaterialPtr const* blend    = nullptr;
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture;
    if (moving) {
        material = &moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
        blend    = &moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerBlend)].get();
        texture  = moving->mAtlasTexture.get();
    }
    auto*     ownTess   = ponderTessellator();
    if (!ownTess) return res;
    auto&     blockTess = *ownTess;
    auto&     worldMat  = sc.camera.worldMatrixStack.get();
    float     radius    = std::sqrt(static_cast<float>(scene.sizeX * scene.sizeX + scene.sizeZ * scene.sizeZ)) + static_cast<float>(scene.height) + 10.f;
    glm::mat4 camera    = uiCamera(view, radius);

    // the alpha of the block being drawn (1: opaque, alpha-tested layer)
    int  step     = 16;
    auto drawMesh = [&](mce::Mesh const& mesh, glm::mat4 const& model) {
        auto ref          = worldMat.push(false);
        ref.mat->_m.get() = ref.mat->_m.get() * camera * model;
        if (step >= 16 || !blend) {
            mesh.renderMesh(sc, *material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
            return;
        }
        // Create's fading blocks: translucent, the alpha in the vertices (our meshes) and in the
        // shader colour (the game's tessellations)
        ShaderColor& shader = sc.currentShaderColor;
        auto  before      = shader.color.get();
        shader.color.get() = mce::Color{1.f, 1.f, 1.f, stepAlpha(step)};
        shader.dirty = true;
        mesh.renderMesh(sc, *blend, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        shader.color.get() = before;
        shader.dirty = true;
    };

    // fluids of the scene by position: (water?, height) for the corner heights
    std::map<std::tuple<int, int, int>, std::pair<bool, float>> fluids;
    for (auto const& b : st.blocks) {
        bool water = isWater(b.block.id);
        if (water || isLava(b.block.id)) fluids[{b.pos.x, b.pos.y, b.pos.z}] = {water, fluidLevelHeight(b.block)};
    }
    auto cornersOf = [&](cp::BlockView const& b) {
        FluidCorners out{};
        bool         water = isWater(b.block.id);
        auto         same  = [&](int x, int y, int z) -> std::optional<float> {
            auto it = fluids.find({x, y, z});
            if (it == fluids.end() || it->second.first != water) return std::nullopt;
            return it->second.second;
        };
        if (same(b.pos.x, b.pos.y + 1, b.pos.z)) return FluidCorners{1.f, 1.f, 1.f, 1.f};
        static constexpr std::array<std::array<int, 2>, 4> kCorner{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (std::size_t i = 0; i < 4; ++i) {
            float sum = 0;
            int   n   = 0;
            bool  full = false;
            for (int dx : {kCorner[i][0] - 1, kCorner[i][0]})
                for (int dz : {kCorner[i][1] - 1, kCorner[i][1]}) {
                    int x = b.pos.x + dx, z = b.pos.z + dz;
                    if (same(x, b.pos.y + 1, z)) full = true;
                    if (auto h = same(x, b.pos.y, z)) {
                        // sources weigh ten times (Minecraft's averaging)
                        int w = *h >= 8.f / 9.f ? 10 : 1;
                        sum += *h * static_cast<float>(w);
                        n += w;
                    }
                }
            out[i] = full ? 1.f : (n > 0 ? sum / static_cast<float>(n) : fluidLevelHeight(b.block));
        }
        return out;
    };
    cp::BlockView const* current = nullptr;
    auto meshOf = [&](cp::BlockDef const& def, Block const& block) -> mce::Mesh* {
        if (auto* m = modelMesh(tess, def, step)) return m;
        if (current && (isWater(def.id) || isLava(def.id))) return fluidMesh(tess, def.id, step, cornersOf(*current));
        return blockMesh(tess, blockTess, block);
    };

    std::vector<k::RotorPart> parts;
    for (auto const& it : items) {
        auto const& b = *it.b;
        current       = it.b;
        // fading blocks are drawn translucent (Create's PonderWorld fade)
        step            = b.alpha >= 1.f ? 16 : alphaStep(b.alpha);
        glm::mat4 model = it.place;
        auto const& info = infoOf(b.block);
        if (!info.block || !material) {
            ++res.missingBlocks;
            if (b.alpha > 0.3f) {
                auto lo = transformPoint(model, {0.1f, 0.1f, 0.1f}), hi = transformPoint(model, {0.9f, 0.9f, 0.9f});
                boxOutline(c, view, lo, hi, cfg.placeholderColour);
                auto ctr = view.project(transformPoint(model, {0.5f, 0.5f, 0.5f}));
                c.text(ctr.x - c.originX(), ctr.y - c.originY() - 4, "?", cfg.placeholderColour, Align::Centre);
            }
            continue;
        }
        ++res.drawnBlocks;
        bool turning = b.rpm != 0.f && info.spec;
        if (turning && info.statics) {
            // static part + rotor bones (as the kinetics renderer)
            auto statics = b.block;
            statics.states[info.spec->rotorState] = 1;
            if (auto* m = meshOf(statics, *info.statics)) drawMesh(*m, model);
            k::RenderView rv{&info.snap, b.rpm, std::nullopt};
            parts.clear();
            info.spec->rotorParts(rv, parts);
            for (auto const& part : parts) {
                auto* mesh = rotorMesh(tess, *info.spec, part.bone, step);
                if (!mesh) continue;
                glm::mat4 m = glm::translate(model, glm::vec3(0.5f));
                m           = glm::rotate(m, glm::radians(b.angle * part.speedFactor + part.angleOffset), axisVec(part.axis));
                if (part.axis == k::Axis::X) m = glm::rotate(m, glm::radians(-90.f), glm::vec3{0.f, 0.f, 1.f});
                else if (part.axis == k::Axis::Z) m = glm::rotate(m, glm::radians(90.f), glm::vec3{1.f, 0.f, 0.f});
                m = glm::translate(m, glm::vec3(-0.5f));
                drawMesh(*mesh, m);
            }
            continue;
        }
        auto axis = turning ? info.spec->axisOf(info.snap) : std::nullopt;
        if (axis) {
            // a kinetic block without a rotor model turns as a whole about its axis
            glm::mat4 m = glm::translate(model, glm::vec3(0.5f));
            m           = glm::rotate(m, glm::radians(b.angle), axisVec(*axis));
            m              = glm::translate(m, glm::vec3(-0.5f));
            if (auto* bm = meshOf(b.block, *info.block)) drawMesh(*bm, m);
            continue;
        }
        if (auto* bm = meshOf(b.block, *info.block)) drawMesh(*bm, model);
    }

    // ---- overlays ----
    for (auto const& o : st.outlines) {
        float a   = std::min({1.f, o.age / 5.f, o.left / 5.f});
        auto  col = withAlpha(cp::paletteColour(o.color), a);
        if (o.area.base) boxOutline(c, view, {0, 0, 0}, {static_cast<float>(scene.sizeX), 1, static_cast<float>(scene.sizeZ)}, col, 1.2f);
        for (auto const& bx : o.area.boxes) {
            boxOutline(c, view, {static_cast<float>(bx.lo.x), static_cast<float>(bx.lo.y), static_cast<float>(bx.lo.z)},
                       {static_cast<float>(bx.hi.x + 1), static_cast<float>(bx.hi.y + 1), static_cast<float>(bx.hi.z + 1)}, col, 1.2f);
        }
    }
    for (auto const& r : st.redstone) {
        for (auto const& bx : r.area.boxes) {
            for (int x = bx.lo.x; x <= bx.hi.x; ++x)
                for (int y = bx.lo.y; y <= bx.hi.y; ++y)
                    for (int z = bx.lo.z; z <= bx.hi.z; ++z) {
                        // a few red sparks that rise and fade (Create's redstone particles)
                        for (int i = 0; i < 4; ++i) {
                            float ph = std::fmod(r.age * 0.05f + i * 0.25f, 1.f);
                            float jx = 0.2f + 0.6f * std::fmod(0.37f * (i + 1) * (x + 3), 1.f);
                            float jz = 0.2f + 0.6f * std::fmod(0.61f * (i + 2) * (z + 5), 1.f);
                            auto  p  = view.project({x + jx, y + 0.3f + ph * 0.6f, z + jz});
                            c.fill({p.x - c.originX() - 1, p.y - c.originY() - 1, 2, 2}, withAlpha(0xFFFF2020, 1.f - ph));
                        }
                    }
        }
    }
    for (auto const& item : st.items) {
        auto p = view.project(item.pos);
        if (!drawIcon(c, item.id, p.x - c.originX() - 8, p.y - c.originY() - 8, 16)) {
            c.text(p.x - c.originX(), p.y - c.originY() - 4, "?", cfg.placeholderColour, Align::Centre);
        }
    }
    for (auto const& in : st.inputs) {
        float a = std::min({1.f, in.age / 4.f, in.left / 4.f});
        if (a <= 0) continue;
        auto  p  = view.project(in.point);
        float px = p.x - c.originX(), py = p.y - c.originY();
        // a pointer from the target to the indicator box (Create's InputWindowElement)
        float bx = px + 14, by = py - 34;
        line(c, px, py, bx, by + 22, withAlpha(0xFFFFFFFF, a * 0.7f));
        std::string label;
        if (in.sneak) label += ctr("ponder.ponder.shared.sneak_and", "Sneak +") + " ";
        if (in.ctrl) label += ctr("ponder.ponder.shared.ctrl_and", "Ctrl +") + " ";
        float lw = label.empty() ? 0 : c.textWidth(label) + 2;
        float w  = lw + 14 + (in.item.empty() ? 0 : 18) + 4;
        c.fill({bx, by, w, 22}, withAlpha(0xE0100010, a));
        c.frame({bx, by, w, 22}, withAlpha(0xFF5000FF, a));
        float x = bx + 3;
        if (!label.empty()) {
            c.text(x, by + 7, label, withAlpha(0xFFFFFFFF, a));
            x += lw;
        }
        // mouse icon: body, the used button lit
        c.fill({x, by + 4, 10, 14}, withAlpha(0xFFDDDDDD, a));
        c.frame({x, by + 4, 10, 14}, withAlpha(0xFF333333, a));
        c.fill({x + 4.5f, by + 4, 1, 6}, withAlpha(0xFF333333, a));
        if (in.action == "left") c.fill({x + 1, by + 5, 3.5f, 5}, withAlpha(0xFFFFC040, a));
        else if (in.action == "right") c.fill({x + 5.5f, by + 5, 3.5f, 5}, withAlpha(0xFFFFC040, a));
        else if (in.action == "scroll") c.fill({x + 4, by + 5, 2, 4}, withAlpha(0xFFFFC040, a));
        x += 14;
        if (!in.item.empty()) {
            if (!drawIcon(c, in.item, x, by + 3, 16)) c.text(x + 8, by + 7, "?", cfg.placeholderColour, Align::Centre);
        }
    }
    if (picked) {
        auto lo = transformPoint(pickedPlace, {0, 0, 0}), hi = transformPoint(pickedPlace, {1, 1, 1});
        boxOutline(c, view, lo, hi, 0xFFFFFFFF, 1.2f);
        res.hoveredBlock = picked->block.id;
    }
    return res;
}

void drawTexts(Canvas& c, cp::Scene const& scene, cp::SceneState const& st, cp::View const& view, float screenW, bool /*slow*/) {
    constexpr float   kMaxWidth = 180, kLine = 10, kPad = 4;
    float             stackY    = 0; // texts not placed near their target stack at the top
    std::vector<Rect> placed;        // bubbles already drawn (older first): newer ones move down
    auto overlaps = [&](Rect const& r) {
        for (auto const& o : placed)
            if (r.x < o.x + o.w && o.x < r.x + r.w && r.y < o.y + o.h && o.y < r.y + r.h) return true;
        return false;
    };
    for (auto const& t : st.texts) {
        float a = std::min({1.f, t.age / 6.f, t.left / 6.f});
        if (a <= 0.02f) continue;
        auto  text  = translateClient(scene.textKey(t.text), scene.textKey(t.text));
        auto  lines = cp::wrapText(text, kMaxWidth, [&](std::string const& s) { return c.textWidth(s); });
        float w     = 0;
        for (auto const& l : lines) w = std::max(w, c.textWidth(l));
        float    h   = static_cast<float>(lines.size()) * kLine + kPad * 2 - 1;
        float    bw  = w + kPad * 2;
        unsigned col = cp::paletteColour(t.color);
        float    bx, by;
        if (t.hasPoint && t.near) {
            // next to the target, on its right (left when it would leave the screen)
            auto  p  = view.project(t.point);
            float px = p.x - c.originX(), py = p.y - c.originY();
            bx       = px + 40;
            if (bx + bw > screenW - 8) bx = px - 40 - bw;
            by = py - h - 10;
        } else {
            bx = (screenW - bw) / 2;
            by = 44 + stackY;
            stackY += h + 4;
        }
        for (int guard = 0; guard < 8 && overlaps({bx, by, bw, h}); ++guard) by += h + 4;
        placed.push_back({bx, by, bw, h});
        if (t.hasPoint) {
            auto  p  = view.project(t.point);
            float px = p.x - c.originX(), py = p.y - c.originY();
            float ex = bx > px ? bx : bx + bw;
            line(c, px, py, ex, by + h / 2, withAlpha(col, a), 1.f, 2.f);
            c.fill({px - 1.5f, py - 1.5f, 3, 3}, withAlpha(col, a));
        }
        c.fill({bx, by, bw, h}, withAlpha(0xE0100010, a));
        c.frame({bx, by, bw, h}, withAlpha(col, a));
        for (std::size_t i = 0; i < lines.size(); ++i) c.text(bx + kPad, by + kPad + static_cast<float>(i) * kLine, lines[i], withAlpha(0xFFFFFFFF, a));
    }
}
} // namespace lv::client::ponder

#endif // LV_CLIENT
