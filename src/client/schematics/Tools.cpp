// Client mod: the schematic tools (Create's SchematicHandler + Deploy / Move XZ / Move Y / Rotate /
// Mirror / Print tools: Alt + scroll picks a tool, Ctrl + scroll uses it, right-click deploys or
// prints), the ghost preview of the held schematic, the schematic and quill (Ctrl for mid-air
// selection, Ctrl + scroll for the range and to push / pull a face of the box, Create's blue
// outline) and the schematicannon's launched blocks flying to their target.

#ifdef LV_CLIENT

#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "glm/gtc/matrix_transform.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/BlockActorRenderer.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/schematics/Schematic.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "feature/schematics/Schematics.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::schematics {

namespace cs  = core::schematics;
namespace fsc = feature::schematics;
namespace net = feature::net;
using Json    = nlohmann::json;
using Clock   = std::chrono::steady_clock;

namespace {

constexpr int kKeyCtrl = 17;
constexpr int kKeyAlt  = 18;

enum Tool { Deploy = 0, Move, MoveY, Rotate, Flip, Print, ToolCount };
constexpr std::array<char const*, ToolCount> kToolKeys{"deploy", "move", "move_y", "rotate", "flip", "print"};
constexpr std::array<char const*, ToolCount> kToolNames{"Position", "Move XZ", "Move Y", "Rotate", "Mirror", "Print"};

struct Preview {
    cs::Pos                   size;
    std::vector<Block const*> palette;
    std::vector<std::int32_t> blocks;
};

struct Launch {
    int               dim = 0;
    cs::Pos           from, to;
    int               ticks = 10;
    Block const*      block = nullptr;
    Clock::time_point start;
};

struct State {
    std::mutex mutex; // previews / launches / quill (network thread vs render)
    // previews: "owner/file" -> data
    std::map<std::string, Preview> previews;
    std::set<std::string>          requested;
    // ghost mesh cache
    std::string                  meshKey;
    std::unique_ptr<mce::Mesh>   mesh;
    std::map<Block const*, std::unique_ptr<mce::Mesh>> blockMeshes;
    // tools
    int  tool  = Deploy;
    bool ctrl  = false;
    bool alt   = false;
    int  range = 10;
    /// local edits not yet seen in the item (anchor / rotation / mirror / deployed)
    std::optional<fsc::SchematicData> pending;
    Clock::time_point                 pendingAt;
    // quill
    std::optional<cs::Pos> first, second;
    int                    quillRange = 10;
    // launches
    std::vector<Launch> launches;
    bool                loggedGhost = false;
};

State& st() {
    static State s;
    return s;
}

std::string keyOf(fsc::SchematicData const& d) { return d.owner + "/" + d.file; }

/// The written schematic in the local player's hand (with local edits applied).
std::optional<fsc::SchematicData> heldSchematic() {
    auto* p = currentPlayer();
    if (!p) return std::nullopt;
    auto d = fsc::schematicData(p->getSelectedItem());
    if (!d) return std::nullopt;
    auto& s = st();
    if (s.pending) {
        bool same = s.pending->file == d->file && s.pending->anchor == d->anchor && s.pending->transform.rotation == d->transform.rotation
                 && s.pending->transform.mirror == d->transform.mirror && s.pending->deployed == d->deployed;
        if (same || Clock::now() - s.pendingAt > std::chrono::seconds(3)) s.pending.reset();
        else if (s.pending->file == d->file) return s.pending;
    }
    return d;
}

bool holdingQuill() {
    auto* p = currentPlayer();
    return p && feature::common::items::idOf(p->getSelectedItem()) == fsc::kQuill;
}

void sync(fsc::SchematicData const& d) {
    auto* p = currentPlayer();
    if (!p) return;
    auto& s     = st();
    s.pending   = d;
    s.pendingAt = Clock::now();
    net::client::send("schematics.sync", {
        {"slot", p->getSelectedItemSlot()}, {"anchor", {d.anchor.x, d.anchor.y, d.anchor.z}}, {"rotation", d.transform.rotation},
        {"mirror", static_cast<int>(d.transform.mirror)}, {"deployed", d.deployed}
    });
}

Vec3 eye() { return currentPlayer()->getEyePos(); }
Vec3 look() { return currentPlayer()->getViewVector(1.f); }

cs::Pos floorPos(double x, double y, double z) { return {int(std::floor(x)), int(std::floor(y)), int(std::floor(z))}; }

/// Block the crosshair points at (+ face), or nullopt.
std::optional<std::pair<cs::Pos, int>> hitBlock() {
    auto* ci = currentClient();
    if (!ci) return std::nullopt;
    auto* level = ci->getLevel();
    if (!level) return std::nullopt;
    auto& hit = level->getHitResult();
    if (hit.mType != HitResultType::Tile) return std::nullopt;
    return std::pair{cs::Pos{hit.mBlock.x, hit.mBlock.y, hit.mBlock.z}, static_cast<int>(hit.mFacing)};
}

constexpr std::array<cs::Pos, 6> kDirs{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

/// Where the deploy tool would put the schematic (Create: the bottom centre at the target).
std::optional<cs::Pos> deployTarget() {
    auto& s = st();
    if (s.ctrl) {
        auto e = eye();
        auto l = look();
        return floorPos(e.x + l.x * s.range, e.y + l.y * s.range, e.z + l.z * s.range);
    }
    auto h = hitBlock();
    if (!h) return std::nullopt;
    return h->first + kDirs[static_cast<std::size_t>(std::clamp(h->second, 0, 5))];
}

cs::Pos anchorFor(fsc::SchematicData const& d, cs::Pos target) {
    auto    b    = cs::bounds(d.transform, d.size);
    cs::Pos span = b[1] - b[0];
    cs::Pos min{target.x - span.x / 2, target.y, target.z - span.z / 2};
    return min - b[0];
}

/// Dominant horizontal look axis: x (true) or z, and its sign.
std::pair<bool, int> horizontalLook() {
    auto l = look();
    if (std::abs(l.x) > std::abs(l.z)) return {true, l.x > 0 ? 1 : -1};
    return {false, l.z > 0 ? 1 : -1};
}

void applyTool(int delta) {
    auto d = heldSchematic();
    if (!d) return;
    auto& s = st();
    switch (s.tool) {
    case Deploy:
        s.range = std::clamp(s.range + delta, 1, 100);
        return;
    case Move: {
        auto [xAxis, sign] = horizontalLook();
        if (xAxis) d->anchor.x += delta * sign;
        else d->anchor.z += delta * sign;
        break;
    }
    case MoveY:
        d->anchor.y += delta;
        break;
    case Rotate: {
        auto to     = d->transform;
        to.rotation = ((to.rotation + (delta < 0 ? 1 : 3)) % 4);
        d->anchor   = cs::recenterAnchor(d->anchor, d->size, d->transform, to);
        d->transform = to;
        break;
    }
    case Flip: {
        auto [xAxis, sign] = horizontalLook();
        (void)sign;
        bool       odd  = d->transform.rotation % 2 == 1;
        cs::Mirror flip = (xAxis != odd) ? cs::Mirror::FrontBack : cs::Mirror::LeftRight;
        auto       to   = d->transform;
        if (to.mirror == cs::Mirror::None) to.mirror = flip;
        else if (to.mirror == flip) to.mirror = cs::Mirror::None;
        else {
            to.mirror   = cs::Mirror::None;
            to.rotation = (to.rotation + 2) % 4;
        }
        d->anchor    = cs::recenterAnchor(d->anchor, d->size, d->transform, to);
        d->transform = to;
        break;
    }
    default:
        return;
    }
    sync(*d);
}

// ---- quill helpers ------------------------------------------------------------------------------------

/// The face of the selection box the player looks at (inverted from inside), Create's selectedFace.
std::optional<std::pair<int, bool>> selectedFace() {
    auto& s = st();
    if (!s.first || !s.second) return std::nullopt;
    cs::Pos lo{std::min(s.first->x, s.second->x), std::min(s.first->y, s.second->y), std::min(s.first->z, s.second->z)};
    cs::Pos hi{std::max(s.first->x, s.second->x) + 1, std::max(s.first->y, s.second->y) + 1, std::max(s.first->z, s.second->z) + 1};
    auto    e = eye();
    auto    l = look();
    double  o[3]{e.x, e.y, e.z}, dv[3]{l.x, l.y, l.z};
    double  mn[3]{lo.x - .45, lo.y - .45, lo.z - .45}, mx[3]{hi.x + .45, hi.y + .45, hi.z + .45};
    bool    inside = o[0] > mn[0] && o[0] < mx[0] && o[1] > mn[1] && o[1] < mx[1] && o[2] > mn[2] && o[2] < mx[2];
    // slab method: entering face (outside) or exiting face (inside)
    double tEnter = -1e9, tExit = 1e9;
    int    fEnter = -1, fExit = -1;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(dv[a]) < 1e-9) {
            if (o[a] < mn[a] || o[a] > mx[a]) return std::nullopt;
            continue;
        }
        double t1 = (mn[a] - o[a]) / dv[a], t2 = (mx[a] - o[a]) / dv[a];
        int    f1 = a == 0 ? 4 : a == 1 ? 0 : 2; // min faces: west, down, north
        int    f2 = f1 + 1;
        if (t1 > t2) {
            std::swap(t1, t2);
            std::swap(f1, f2);
        }
        if (t1 > tEnter) tEnter = t1, fEnter = f1;
        if (t2 < tExit) tExit = t2, fExit = f2;
    }
    if (tEnter > tExit || tExit < 0) return std::nullopt;
    if (inside) return std::pair{fExit ^ 1, true};
    if (tEnter > 70) return std::nullopt;
    return std::pair{fEnter, false};
}

// ---- rendering ------------------------------------------------------------------------------------------

struct Uv {
    float u0, v0, u1, v1;
};

Uv uvOf(Block const& b, int face) {
    auto const* g = BlockGraphics::getForBlock(b);
    if (!g) return {0, 0, 0, 0};
    auto const& t = g->getTexture(static_cast<uint64>(face), 0);
    return {t._u0, t._v0, t._u1, t._v1};
}

/// Appends the faces of a unit cube at (x, y, z) (not those in `skip`).
void cube(Tessellator& t, Block const& b, float x, float y, float z, float s, std::array<bool, 6> const& skip) {
    static constexpr std::array<float, 6> kShade{0.5f, 1.f, 0.8f, 0.8f, 0.6f, 0.6f};
    float x1 = x + s, y1 = y + s, z1 = z + s;
    for (int f = 0; f < 6; ++f) {
        if (skip[static_cast<std::size_t>(f)]) continue;
        auto uv = uvOf(b, f);
        float c = kShade[static_cast<std::size_t>(f)];
        t.color(c, c, c, 1.f);
        Vec2 a{uv.u0, uv.v1}, bb{uv.u1, uv.v1}, cc{uv.u1, uv.v0}, d{uv.u0, uv.v0};
        switch (f) {
        case 0:
            t.simpleQuad(Vec3{x, y, z}, d, Vec3{x1, y, z}, cc, Vec3{x1, y, z1}, bb, Vec3{x, y, z1}, a);
            break;
        case 1:
            t.simpleQuad(Vec3{x, y1, z1}, a, Vec3{x1, y1, z1}, bb, Vec3{x1, y1, z}, cc, Vec3{x, y1, z}, d);
            break;
        case 2:
            t.simpleQuad(Vec3{x1, y, z}, a, Vec3{x, y, z}, bb, Vec3{x, y1, z}, cc, Vec3{x1, y1, z}, d);
            break;
        case 3:
            t.simpleQuad(Vec3{x, y, z1}, a, Vec3{x1, y, z1}, bb, Vec3{x1, y1, z1}, cc, Vec3{x, y1, z1}, d);
            break;
        case 4:
            t.simpleQuad(Vec3{x, y, z}, a, Vec3{x, y, z1}, bb, Vec3{x, y1, z1}, cc, Vec3{x, y1, z}, d);
            break;
        default:
            t.simpleQuad(Vec3{x1, y, z1}, a, Vec3{x1, y, z}, bb, Vec3{x1, y1, z}, cc, Vec3{x1, y1, z1}, d);
            break;
        }
    }
}

std::uint64_t packPos(cs::Pos p) {
    return (static_cast<std::uint64_t>(p.x + (1 << 20)) << 42) | (static_cast<std::uint64_t>(p.y + (1 << 20)) << 21) | static_cast<std::uint64_t>(p.z + (1 << 20));
}

/// Builds the ghost mesh of a schematic in anchor-relative coordinates (transform applied).
std::unique_ptr<mce::Mesh> buildGhost(Tessellator& tess, Preview const& p, cs::Transform const& t) {
    // transformed blocks per palette index
    std::vector<Block const*> tblocks(p.palette.size(), nullptr);
    for (std::size_t i = 0; i < p.palette.size(); ++i) {
        if (!p.palette[i]) continue;
        auto state = fsc::stateOf(*p.palette[i]);
        cs::transformStates(state, t);
        tblocks[i] = fsc::resolve(state);
        if (!tblocks[i]) tblocks[i] = p.palette[i];
    }
    cs::Schematic shape;
    shape.size = p.size;
    std::unordered_map<std::uint64_t, Block const*> occ;
    for (std::size_t i = 0; i < p.blocks.size(); ++i) {
        auto v = p.blocks[i];
        if (v < 0 || static_cast<std::size_t>(v) >= tblocks.size() || !tblocks[static_cast<std::size_t>(v)]) continue;
        occ[packPos(cs::apply(t, shape.posOf(i)))] = tblocks[static_cast<std::size_t>(v)];
    }
    if (occ.empty()) return nullptr;
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    std::size_t faces = 0;
    for (std::size_t i = 0; i < p.blocks.size() && faces < 400000; ++i) {
        auto v = p.blocks[i];
        if (v < 0 || static_cast<std::size_t>(v) >= tblocks.size() || !tblocks[static_cast<std::size_t>(v)]) continue;
        auto const&         b   = *tblocks[static_cast<std::size_t>(v)];
        cs::Pos             off = cs::apply(t, shape.posOf(i));
        std::array<bool, 6> skip{};
        for (int f = 0; f < 6; ++f) {
            auto it = occ.find(packPos(off + kDirs[static_cast<std::size_t>(f)]));
            skip[static_cast<std::size_t>(f)] = it != occ.end() && BlockGraphics::isFullAndOpaque(*it->second);
        }
        cube(tess, b, float(off.x), float(off.y), float(off.z), 1.f, skip);
        faces += 6;
    }
    return std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_schematic_ghost", SupplementaryFieldAutoGenerationMode::None));
}

mce::MaterialPtr& lineMaterial() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

void box(Tessellator& t, Vec3 lo, Vec3 hi, float r, float g, float b) {
    auto quad = [&](std::array<Vec3, 4> const& v, float shade) {
        t.color(r * shade, g * shade, b * shade, 1.f);
        for (auto const& p : v) t.vertex(p.x, p.y, p.z);
    };
    quad({Vec3{lo.x, hi.y, lo.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, hi.y, lo.z}}, 1.f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{lo.x, lo.y, hi.z}}, 0.5f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, hi.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, lo.y, lo.z}}, 0.8f);
    quad({Vec3{lo.x, lo.y, hi.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}}, 0.8f);
    quad({Vec3{lo.x, lo.y, lo.z}, Vec3{lo.x, lo.y, hi.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{lo.x, hi.y, lo.z}}, 0.6f);
    quad({Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{hi.x, lo.y, hi.z}}, 0.6f);
}

/// The 12 edges of a box as thin bars (relative to the camera).
void outline(Tessellator& t, Vec3 lo, Vec3 hi, float r, float g, float b, float w = 1.f / 32) {
    for (float y : {lo.y, hi.y}) {
        box(t, Vec3{lo.x - w, y - w, lo.z - w}, Vec3{hi.x + w, y + w, lo.z + w}, r, g, b);
        box(t, Vec3{lo.x - w, y - w, hi.z - w}, Vec3{hi.x + w, y + w, hi.z + w}, r, g, b);
        box(t, Vec3{lo.x - w, y - w, lo.z - w}, Vec3{lo.x + w, y + w, hi.z + w}, r, g, b);
        box(t, Vec3{hi.x - w, y - w, lo.z - w}, Vec3{hi.x + w, y + w, hi.z + w}, r, g, b);
    }
    for (float x : {lo.x, hi.x})
        for (float z : {lo.z, hi.z}) box(t, Vec3{x - w, lo.y, z - w}, Vec3{x + w, hi.y, z + w}, r, g, b);
}

void renderSchematics(BaseActorRenderContext& ctx) {
    auto* ci     = currentClient();
    auto* player = currentPlayer();
    if (!ci || !player) return;
    auto& s   = st();
    Vec3  cam = ctx.mImpl.get()->mCameraTargetPosition;
    auto& sc  = ctx.mScreenContext;
    auto& tess = sc.tessellator;
    int   dim  = static_cast<int>(player->getDimensionId());

    auto& dispatcher = ctx.mBlockEntityRenderDispatcher;
    auto* moving     = static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers->operator[](BlockActorRendererId::MovingBlock).get());

    // ---- ghost of the held schematic ----
    if (auto d = heldSchematic(); d && moving) {
        std::lock_guard lock(s.mutex);
        auto            key = keyOf(*d);
        if (!s.previews.contains(key) && !s.requested.contains(key)) {
            s.requested.insert(key);
            net::client::send("schematics.preview.request", {{"owner", d->owner}, {"file", d->file}});
        }
        auto it = s.previews.find(key);
        std::optional<cs::Pos> anchor;
        if (d->deployed) anchor = d->anchor;
        else if (s.tool == Deploy) {
            if (auto t = deployTarget()) anchor = anchorFor(*d, *t);
        }
        if (it != s.previews.end() && anchor) {
            std::string meshKey = key + "|" + std::to_string(d->transform.rotation) + "|" + std::to_string(static_cast<int>(d->transform.mirror));
            if (meshKey != s.meshKey) {
                s.mesh    = buildGhost(tess, it->second, d->transform);
                s.meshKey = meshKey;
            }
            if (s.mesh) {
                auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
                std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
                auto&     world = sc.camera.worldMatrixStack.get();
                auto      ref   = world.push(false);
                glm::mat4 m{1.f};
                m = glm::translate(m, glm::vec3{anchor->x - cam.x, anchor->y - cam.y, anchor->z - cam.z});
                ref.mat->_m.get() = ref.mat->_m.get() * m;
                s.mesh->renderMesh(sc, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
                if (!s.loggedGhost) {
                    s.loggedGhost = true;
                    log().info("schematics client: drawing the ghost of {} at {} {} {}", d->file, anchor->x, anchor->y, anchor->z);
                }
            }
            // bounds outline
            auto b = cs::bounds(d->transform, d->size);
            tess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
            outline(tess, Vec3{float(anchor->x + b[0].x - cam.x), float(anchor->y + b[0].y - cam.y), float(anchor->z + b[0].z - cam.z)},
                    Vec3{float(anchor->x + b[1].x + 1 - cam.x), float(anchor->y + b[1].y + 1 - cam.y), float(anchor->z + b[1].z + 1 - cam.z)}, 0.4f, 0.55f, 0.85f);
            MeshHelpers::renderMeshImmediately(sc, tess, lineMaterial(), OffscreenCaptureDescription{});
        }
    }

    // ---- quill selection (Create's 0x6886c5 outline, the face being resized highlighted) ----
    if (holdingQuill()) {
        std::optional<cs::Pos> a, b;
        {
            std::lock_guard lock(s.mutex);
            a = s.first;
            b = s.second;
        }
        std::optional<cs::Pos> hovered;
        if (!b) {
            if (s.ctrl) {
                auto e = eye(), l = look();
                hovered = floorPos(e.x + l.x * s.quillRange, e.y + l.y * s.quillRange, e.z + l.z * s.quillRange);
            } else if (auto h = hitBlock()) {
                hovered = h->first;
                if ((h->second == 0 || h->second == 1)) hovered = h->first + kDirs[static_cast<std::size_t>(h->second)];
            }
        }
        std::optional<std::pair<cs::Pos, cs::Pos>> sel;
        if (a && b) sel = std::pair{*a, *b};
        else if (a) sel = std::pair{*a, hovered.value_or(*a)};
        else if (hovered) sel = std::pair{*hovered, *hovered};
        if (sel) {
            cs::Pos lo{std::min(sel->first.x, sel->second.x), std::min(sel->first.y, sel->second.y), std::min(sel->first.z, sel->second.z)};
            cs::Pos hi{std::max(sel->first.x, sel->second.x) + 1, std::max(sel->first.y, sel->second.y) + 1, std::max(sel->first.z, sel->second.z) + 1};
            Vec3    l{float(lo.x - cam.x), float(lo.y - cam.y), float(lo.z - cam.z)}, h{float(hi.x - cam.x), float(hi.y - cam.y), float(hi.z - cam.z)};
            tess.begin({}, mce::PrimitiveMode::QuadList, 0, false);
            outline(tess, l, h, 0x68 / 255.f, 0x86 / 255.f, 0xc5 / 255.f, 1.f / 32);
            if (a && b) {
                if (auto f = selectedFace()) {
                    // a thin plate on the selected face
                    Vec3 fl = l, fh = h;
                    float e  = 1.f / 64;
                    switch (f->first) {
                    case 0: fh.y = l.y + e; break;
                    case 1: fl.y = h.y - e; break;
                    case 2: fh.z = l.z + e; break;
                    case 3: fl.z = h.z - e; break;
                    case 4: fh.x = l.x + e; break;
                    default: fl.x = h.x - e; break;
                    }
                    box(tess, fl, fh, 0.55f, 0.7f, 1.f);
                }
            }
            MeshHelpers::renderMeshImmediately(sc, tess, lineMaterial(), OffscreenCaptureDescription{});
        }
    }

    // ---- launched blocks ----
    std::vector<Launch> flying;
    {
        std::lock_guard lock(s.mutex);
        auto            now = Clock::now();
        std::erase_if(s.launches, [&](Launch const& l) { return now - l.start > std::chrono::milliseconds(50 * (l.ticks + 2)); });
        for (auto const& l : s.launches)
            if (l.dim == dim) flying.push_back(l);
    }
    if (!flying.empty() && moving) {
        auto const& material = moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get();
        std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> texture{moving->mAtlasTexture.get()};
        auto& world = sc.camera.worldMatrixStack.get();
        for (auto const& l : flying) {
            if (!l.block) continue;
            auto& mesh = s.blockMeshes[l.block];
            if (!mesh) {
                tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
                cube(tess, *l.block, -0.5f, -0.5f, -0.5f, 1.f, {});
                mesh = std::make_unique<mce::Mesh>(tess.end(Tessellator::UploadMode::Buffered, "levitate_launched_block", SupplementaryFieldAutoGenerationMode::None));
            }
            float t  = std::clamp(std::chrono::duration<float>(Clock::now() - l.start).count() * 20.f / std::max(1, l.ticks), 0.f, 1.f);
            float fx = l.from.x + 0.5f, fy = l.from.y + 1.2f, fz = l.from.z + 0.5f;
            float tx = l.to.x + 0.5f, ty = l.to.y + 0.5f, tz = l.to.z + 0.5f;
            float dist = std::sqrt((tx - fx) * (tx - fx) + (tz - fz) * (tz - fz));
            // Create's arc: a parabola above the straight line, the block grows as it flies
            float x = fx + (tx - fx) * t, z = fz + (tz - fz) * t;
            float y = fy + (ty - fy) * t + std::sin(t * 3.14159f) * (0.5f + dist * 0.3f);
            float scale = 0.3f + 0.7f * t;
            auto  ref   = world.push(false);
            glm::mat4 m{1.f};
            m = glm::translate(m, glm::vec3{x - cam.x, y - cam.y, z - cam.z});
            m = glm::rotate(m, t * 6.28f, glm::vec3{0.3f, 1.f, 0.2f});
            m = glm::scale(m, glm::vec3{scale});
            ref.mat->_m.get() = ref.mat->_m.get() * m;
            mesh->renderMesh(sc, material, texture, 0, 0, OffscreenCaptureDescription{}, nullptr);
        }
    }
}

// ---- HUD --------------------------------------------------------------------------------------------------

void hud(screen::Canvas& c) {
    auto d = heldSchematic();
    if (!d) return;
    auto& s = st();
    auto [sw, sh] = screen::guiSize();
    (void)sw;
    float x = 8, y = sh / 2 - 40;
    if (s.alt) {
        // Create's tool selection: every tool, the selected one highlighted
        for (int i = 0; i < ToolCount; ++i) {
            if (i == Print && !currentPlayer()->isCreative()) continue;
            std::string key = std::string("create.schematic.tool.") + kToolKeys[static_cast<std::size_t>(i)];
            c.text(x, y, (i == s.tool ? "> " : "  ") + ctr(key, kToolNames[static_cast<std::size_t>(i)]), i == s.tool ? screen::color::kTextGold : screen::color::kText);
            y += 11;
        }
        return;
    }
    std::string key = std::string("create.schematic.tool.") + kToolKeys[static_cast<std::size_t>(s.tool)];
    c.text(x, y, ctr(key, kToolNames[static_cast<std::size_t>(s.tool)]), screen::color::kTextGold);
    for (int i = 0; i < 2; ++i) {
        y += 11;
        auto line = ctr(key + ".description." + std::to_string(i), "");
        if (!line.empty()) c.text(x, y, line, screen::color::kTextGray);
    }
    y += 13;
    c.text(x, y, ctr("lv.schematics.tools.hint", "[Alt]+Scroll: select tool"), screen::color::kTextGray);
    if (d->deployed) c.text(x, y + 11, ctr("lv.schematics.tools.deployed", "Schematic deployed"), screen::color::kTextGray);
}

// ---- messages -----------------------------------------------------------------------------------------------

Block const* blockFrom(std::string const& snbt) {
    if (snbt.empty()) return nullptr;
    auto tag = CompoundTag::fromSnbt(snbt);
    if (!tag) return nullptr;
    auto b = Block::tryGetFromRegistry(*tag);
    return b ? b.as_ptr() : nullptr;
}

void onPreview(Json const& msg) {
    Preview p;
    auto    size = msg.value("size", Json::array({0, 0, 0}));
    p.size       = {size[0].get<int>(), size[1].get<int>(), size[2].get<int>()};
    for (auto const& e : msg.value("palette", Json::array())) p.palette.push_back(blockFrom(e.value("snbt", std::string{})));
    for (auto const& r : msg.value("runs", Json::array())) {
        if (!r.is_array() || r.size() != 2) continue;
        int v = r[0].get<int>(), n = r[1].get<int>();
        if (n < 0 || p.blocks.size() + static_cast<std::size_t>(n) > (64u << 20)) break;
        p.blocks.insert(p.blocks.end(), static_cast<std::size_t>(n), v);
    }
    auto&           s = st();
    std::lock_guard lock(s.mutex);
    auto            key = msg.value("owner", std::string{}) + "/" + msg.value("file", std::string{});
    s.previews[key]     = std::move(p);
    s.meshKey.clear();
    log().info("schematics client: received the preview of {}", key);
}

void onLaunch(Json const& msg) {
    Launch l;
    l.dim    = msg.value("dim", 0);
    auto f   = msg.value("from", Json::array({0, 0, 0}));
    auto t   = msg.value("to", Json::array({0, 0, 0}));
    l.from   = {f[0].get<int>(), f[1].get<int>(), f[2].get<int>()};
    l.to     = {t[0].get<int>(), t[1].get<int>(), t[2].get<int>()};
    l.ticks  = msg.value("ticks", 10);
    l.block  = blockFrom(msg.value("snbt", std::string{}));
    l.start  = Clock::now();
    auto&           s = st();
    std::lock_guard lock(s.mutex);
    if (s.launches.size() < 256) s.launches.push_back(l);
}

void onQuill(Json const& msg) {
    auto&           s = st();
    std::lock_guard lock(s.mutex);
    auto            vec = [&](char const* k) -> std::optional<cs::Pos> {
        auto a = msg.value(k, Json());
        if (!a.is_array() || a.size() != 3) return std::nullopt;
        return cs::Pos{a[0].get<int>(), a[1].get<int>(), a[2].get<int>()};
    };
    s.first      = vec("first");
    s.second     = vec("second");
    s.quillRange = msg.value("range", s.quillRange);
}

bool inGameWorld() { return currentPlayer() && !screen::current(); }

} // namespace

LL_TYPE_INSTANCE_HOOK(
    SchematicsRenderHook,
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
        renderSchematics(renderContext);
    } catch (...) {
    }
}

class SchematicsToolsModule {
public:
    void install() {
        net::client::on("schematics.preview", [](Json const& msg) { onPreview(msg); });
        net::client::on("schematics.launch", [](Json const& msg) { onLaunch(msg); });
        net::client::on("schematics.quill", [](Json const& msg) { onQuill(msg); });
        SchematicsRenderHook::hook();
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>([](ll::event::KeyInputEvent& ev) {
            auto& s = st();
            if (ev.keyCode() == kKeyCtrl && s.ctrl != ev.isDown()) {
                s.ctrl = ev.isDown();
                if (holdingQuill()) net::client::send("schematics.quill.ctrl", {{"down", s.ctrl}, {"range", s.quillRange}});
            }
            if (ev.keyCode() == kKeyAlt) {
                s.alt = ev.isDown();
                if (s.alt && heldSchematic() && inGameWorld()) ev.cancel();
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::MouseInputEvent>([](ll::event::MouseInputEvent& ev) {
            if (!inGameWorld()) return;
            auto& s     = st();
            char  id    = ev.actionButtonId();
            auto  data  = ev.buttonData();
            if (id == 4 && data != 0) {
                int delta = data > 0 ? 1 : -1;
                if (holdingQuill() && s.ctrl) {
                    std::lock_guard lock(s.mutex);
                    if (!s.second) {
                        s.quillRange = std::clamp(s.quillRange + delta, 1, 100);
                        net::client::send("schematics.quill.ctrl", {{"down", true}, {"range", s.quillRange}});
                    } else if (auto f = selectedFace()) {
                        net::client::send("schematics.quill.resize", {{"face", f->first}, {"delta", delta}, {"inside", f->second}});
                    }
                    ev.cancel();
                    return;
                }
                if (heldSchematic()) {
                    if (s.alt) {
                        bool creative = currentPlayer()->isCreative();
                        int  count    = creative ? ToolCount : ToolCount - 1;
                        auto d        = heldSchematic();
                        if (d && !d->deployed) s.tool = Deploy; // Create: only the deploy tool until it is placed
                        else s.tool = ((s.tool - delta) % count + count) % count;
                        ev.cancel();
                    } else if (s.ctrl) {
                        applyTool(delta);
                        ev.cancel();
                    }
                }
                return;
            }
            if (id == 2 && data == 1) {
                auto d = heldSchematic();
                if (!d || currentPlayer()->isSneaking()) return;
                if (s.tool == Deploy) {
                    auto t = deployTarget();
                    if (!t) return;
                    d->anchor   = anchorFor(*d, *t);
                    d->deployed = true;
                    sync(*d);
                    log().info("schematics client: deployed {} at {} {} {}", d->file, d->anchor.x, d->anchor.y, d->anchor.z);
                    s.tool = Move;
                } else if (s.tool == Print && d->deployed && currentPlayer()->isCreative()) {
                    net::client::send("schematics.print", {{"slot", currentPlayer()->getSelectedItemSlot()}});
                }
            }
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            if (screen::current() || ev.screenView().getScreenName() != "hud_screen") return;
            screen::Canvas c(ev.uiRenderContext());
            hud(c);
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            auto&           s = st();
            std::lock_guard lock(s.mutex);
            s.previews.clear();
            s.requested.clear();
            s.mesh.reset();
            s.meshKey.clear();
            s.blockMeshes.clear();
            s.launches.clear();
            s.first.reset();
            s.second.reset();
            s.pending.reset();
            s.loggedGhost = false;
        }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        SchematicsRenderHook::unhook();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::schematics

LV_REGISTER_MODULE(lv::client::schematics::SchematicsToolsModule);

#endif // LV_CLIENT
