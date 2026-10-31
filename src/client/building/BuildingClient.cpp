#ifdef LV_CLIENT

// Client side of the building unit (client mod only):
//
// - Copycats (Create's CopycatPanelModel / CopycatStepModel): left out of the chunk mesh and drawn
//   every frame from the applied material's own model, cropped and moved as Create does (panel: a
//   1 px slice of the material's back plus its 2 px front, 3 px in all; step: the 4 px border
//   strips of the material put together into the half step), faces against a solid neighbour
//   culled. Without a material the copycat's own model is drawn.
//   A copycat panel with a trapdoor shows the trapdoor as it is turned (Create's prepared
//   material: lying in a floor / ceiling panel, open against a wall panel); with iron / copper /
//   Create bars it shows Create's bars panel (a sheet of the bars' texture 1 px in from the back,
//   2 px edge strips; CopycatBarsModel).
// - Copycat placement helpers (Create's PlacementHelpers ghost): holding a copycat panel / step
//   while looking at one of its kind (not sneaking), the copycat the click would add beside it is
//   drawn as a breathing ghost (feature/building/CopycatHelpers.cpp computes the spot, as the
//   server does when the click arrives).
// - Brackets on shafts and cogwheels: the bracket block's mounted model (facing / axis / cog
//   states) drawn over the shaft.
//   Materials and brackets come from the server ("building.query" / "building.visuals",
//   feature/building/Sync.cpp) for the chunks where the client meshed a copycat or a shaft.
// - Sliding doors (Create's SlidingDoorRenderer): left out of the chunk mesh and drawn every frame
//   with Create's animation: a value chasing open / closed at 0.15 per tick, eased by its square;
//   sliding panels move 13 px sideways (and 0.5 px out), folding doors fold their two leaves (91 /
//   -181 degrees) against the hinge.
// - Connected-texture corners (Create's OMNIDIRECTIONAL CT): where two edges of a face join their
//   neighbours but the block across the corner differs, the frame's corner is drawn over the face
//   (Bedrock picks face textures from the 6 neighbour states only).
// Everything is lit as a block entity at its position. Vanilla clients see the pack's static models.

#include <algorithm>
#include <chrono>
#include <functional>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "glm/gtc/matrix_transform.hpp"
#include "nlohmann/json.hpp"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/PipelineError.h"
#include "mc/client/renderer/block/tessellation_pipeline/client_block_pipeline/WorldExtractorStep.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockSupportType.h"
#include "mc/world/level/block/CachedComponentData.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/building/BlockQuads.h"
#include "feature/building/Building.h"
#include "feature/equipment/Equipment.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::building {

namespace fb = feature::building;
using Json   = nlohmann::json;

namespace {

struct PosHash {
    std::size_t operator()(BlockPos const& p) const noexcept {
        return std::size_t(p.x) * 73856093u ^ std::size_t(p.y) * 19349663u ^ std::size_t(p.z) * 83492791u;
    }
};
struct PosEq {
    bool operator()(BlockPos const& a, BlockPos const& b) const noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
};
using PosSet = std::unordered_set<BlockPos, PosHash, PosEq>;
template <class T>
using PosMap = std::unordered_map<BlockPos, T, PosHash, PosEq>;

constexpr int   kDx[6]{0, 0, 0, 0, -1, 1};
constexpr int   kDy[6]{-1, 1, 0, 0, 0, 0};
constexpr int   kDz[6]{0, 0, -1, 1, 0, 0};
constexpr char const* kDirNames[6]{"down", "up", "north", "south", "west", "east"};
// a face's neighbours behind its top / right / bottom / left edge (seen from outside), as the
// generators' connected textures (tools/gen/building.ps1)
constexpr int kFaceEdges[6][4]{
    {3, 5, 2, 4}, // down:  south east north west
    {2, 5, 3, 4}, // up:    north east south west
    {1, 4, 0, 5}, // north: up west down east
    {1, 5, 0, 4}, // south: up east down west
    {1, 3, 0, 2}, // west:  up south down north
    {1, 2, 0, 3}, // east:  up north down south
};
BlockPos step(BlockPos const& p, int d, int n = 1) { return {p.x + kDx[d] * n, p.y + kDy[d] * n, p.z + kDz[d] * n}; }
glm::vec3 dirVec(int d) { return {float(kDx[d]), float(kDy[d]), float(kDz[d])}; }

struct Visual {
    std::string  materialSnbt;
    Block const* material = nullptr; ///< resolved lazily
    bool         resolved = false;
    Json         bracket;
};

struct DoorAnim {
    float  value  = 0;
    double time   = 0;
    bool   init   = false;
};

struct State {
    std::mutex mutex; ///< the sets below (chunk builder threads)
    PosSet     copycats, doors, ct, ctDirty;
    std::set<std::pair<int, int>> wanted;

    // client thread
    int                                               dim   = -1;
    void const*                                       level = nullptr;
    std::set<std::pair<int, int>>                     asked;
    PosMap<Visual>                                    visuals;
    PosMap<DoorAnim>                                  anims;
    std::map<std::string, std::unique_ptr<mce::Mesh>> meshes;
    PosMap<std::vector<std::pair<int, std::string>>>  notchKeys; ///< ct position -> (face, mesh key)
    bool                                              logged = false;
};
State& state() {
    static State s;
    return s;
}

void resetLevel(bool dropTracked) {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (dropTracked) {
            s.copycats.clear();
            s.doors.clear();
            s.ct.clear();
        }
        s.wanted.clear();
    }
    s.asked.clear();
    s.visuals.clear();
    s.anims.clear();
    s.meshes.clear();
    s.notchKeys.clear();
    s.logged = false;
    resetQuadCache();
}

// ---- server data -----------------------------------------------------------------------------------

void onVisuals(Json const& msg) {
    auto& s = state();
    if (msg.value("dim", -1) != s.dim) return;
    if (auto const& chunks = msg.value("chunks", Json::array()); chunks.is_array() && !chunks.empty()) {
        std::set<std::pair<int, int>> done;
        for (auto const& c : chunks)
            if (c.is_array() && c.size() == 2) done.insert({c[0].get<int>(), c[1].get<int>()});
        for (auto it = s.visuals.begin(); it != s.visuals.end();) {
            if (done.contains({it->first.x >> 4, it->first.z >> 4})) it = s.visuals.erase(it);
            else ++it;
        }
    }
    for (auto const& e : msg.value("entries", Json::array())) {
        BlockPos p{e.value("x", 0), e.value("y", 0), e.value("z", 0)};
        if (!e.contains("copycat") && !e.contains("bracket")) {
            s.visuals.erase(p);
            continue;
        }
        Visual v;
        if (e.contains("copycat") && e["copycat"].is_string()) v.materialSnbt = e["copycat"].get<std::string>();
        if (e.contains("bracket")) v.bracket = e["bracket"];
        s.visuals[p] = std::move(v);
    }
}

Block const* materialOf(Visual& v) {
    if (v.resolved) return v.material;
    v.resolved = true;
    if (v.materialSnbt.empty()) return nullptr;
    if (auto nbt = CompoundTag::fromSnbt(v.materialSnbt)) {
        if (auto b = Block::tryGetFromRegistry(*nbt)) v.material = b.as_ptr();
    }
    return v.material;
}

void askChunks() {
    auto&                         s = state();
    std::set<std::pair<int, int>> wanted;
    {
        std::lock_guard lock(s.mutex);
        wanted.swap(s.wanted);
    }
    Json ask = Json::array();
    for (auto const& c : wanted) {
        if (!s.asked.insert(c).second) continue;
        ask.push_back({c.first, c.second});
        if (ask.size() >= 64) {
            feature::net::client::send(fb::kQueryChannel, {{"chunks", ask}});
            ask = Json::array();
        }
    }
    if (!ask.empty()) feature::net::client::send(fb::kQueryChannel, {{"chunks", ask}});
}

// ---- meshes ----------------------------------------------------------------------------------------

mce::Mesh* cachedMesh(std::string const& key, std::function<std::vector<Quad>()> const& make, Tessellator& tess, bool twoSided = false) {
    auto& m = state().meshes;
    if (auto it = m.find(key); it != m.end()) return it->second.get();
    auto quads = make();
    if (quads.empty()) return m.emplace(key, nullptr).first->second.get();
    return m.emplace(key, buildMesh(tess, quads, twoSided)).first->second.get();
}

std::string ptrKey(void const* p) { return std::to_string(reinterpret_cast<std::uintptr_t>(p)); }

/// The neighbour on `dir` covers that face (a solid full block): Create culls the copycat's faces
/// that may be occluded (vanilla shouldRenderFace).
bool covered(BlockSource& region, BlockPos const& p, int dir) {
    BlockPos     n = step(p, dir);
    Block const& b = region.getBlock(n);
    if (b.isAir() || fb::infoOf(b)) return false;
    return b.mCachedComponentData->mIsSolid && b.canProvideSupport(static_cast<uchar>(dir ^ 1), BlockSupportType::Center);
}

/// Create's CopycatPanelModel: facing `f` (normal n). Front: the 1 px slab opposite the facing,
/// back: the material's 2 px on the facing side moved 13 px back (3 px panel).
std::vector<Quad> panelQuads(std::vector<Quad> const& material, int f, int cullMask) {
    std::vector<Quad> out;
    glm::vec3         n = dirVec(f);
    int               a = f < 2 ? 1 : (f < 4 ? 2 : 0);
    bool              pos = (f & 1) == 1;
    auto slab = [&](float from, float to) {
        glm::vec3 lo{0.f}, hi{1.f};
        lo[a] = from, hi[a] = to;
        return std::pair{lo, hi};
    };
    auto front = pos ? slab(0.f, 1 / 16.f) : slab(15 / 16.f, 1.f);
    auto back  = pos ? slab(14 / 16.f, 1.f) : slab(0.f, 2 / 16.f);
    for (auto const& q : material) {
        int face = cubeFace(q);
        if (face >= 0 && (cullMask >> face & 1)) continue;
        if (face != f)
            if (auto c = cropAndMove(q, front.first, front.second, glm::vec3{0.f})) out.push_back(*c);
        if (face != (f ^ 1))
            if (auto c = cropAndMove(q, back.first, back.second, n * (-13 / 16.f))) out.push_back(*c);
    }
    return out;
}

/// Create's CopycatStepModel: facing `f` (horizontal, normal n), upper half or not: four pieces
/// sampled from the material's 4 px border strips.
std::vector<Quad> stepQuads(std::vector<Quad> const& material, int f, bool upper, int cullMask) {
    std::vector<Quad> out;
    glm::vec3         n = dirVec(f);
    int               a = f < 4 ? 2 : 0;
    bool              pos = (f & 1) == 1;
    for (int top = 0; top < 2; ++top) {
        for (int front = 0; front < 2; ++front) {
            glm::vec3 lo{0.f}, hi{1.f};
            // y: bottom pieces sample 0..4, top pieces 12..16
            lo.y = top ? 12 / 16.f : 0.f;
            hi.y = top ? 1.f : 4 / 16.f;
            // along n: back samples the +n 4 px, front the -n 4 px
            bool plusSide = front ? !pos : pos;
            lo[a]         = plusSide ? 12 / 16.f : 0.f;
            hi[a]         = plusSide ? 1.f : 4 / 16.f;
            glm::vec3 move{0.f};
            if (front) move += n * (8 / 16.f);
            if (top != int(upper)) move.y += upper ? 8 / 16.f : -8 / 16.f;
            for (auto const& q : material) {
                int face = cubeFace(q);
                if (face >= 0 && (cullMask >> face & 1)) continue;
                if (front && face == f) continue;
                if (!front && face == (f ^ 1)) continue;
                if (!top && face == 1) continue;
                if (top && face == 0) continue;
                if (auto c = cropAndMove(q, lo, hi, move)) out.push_back(*c);
            }
        }
    }
    return out;
}

/// Create's copycat_panel/bars(_vertical) models for a panel facing `f`: a sheet of the bars
/// texture 1 px in from the panel's back, and 2 px deep strips along its four edges (the middle
/// 2 px of the texture), both sides.
std::vector<Quad> barsQuads(int f, glm::vec4 uv) {
    std::vector<Quad> out;
    int  a   = f < 2 ? 1 : (f < 4 ? 2 : 0);
    bool pos = (f & 1) == 1;
    // in-plane axes: u horizontal, v up for wall panels; x and z for floor / ceiling panels
    int  ua  = a == 1 ? 0 : (a == 2 ? 0 : 2);
    int  va  = a == 1 ? 2 : 1;
    // depth from the back face (the side opposite the facing)
    auto depth = [&](float w) { return pos ? w : 1.f - w; };
    auto at    = [&](float w, float u, float v) {
        glm::vec3 p{0.f};
        p[a]  = depth(w);
        p[ua] = u;
        p[va] = v;
        return p;
    };
    auto tex = [&](float s, float t) { return glm::vec2{uv.x + (uv.z - uv.x) * s, uv.y + (uv.w - uv.y) * t}; };
    // the sheet (texture upright: v up = texture up)
    Quad sheet;
    sheet.pos = {at(1 / 16.f, 0.f, 0.f), at(1 / 16.f, 1.f, 0.f), at(1 / 16.f, 1.f, 1.f), at(1 / 16.f, 0.f, 1.f)};
    sheet.uv  = {tex(0.f, 1.f), tex(1.f, 1.f), tex(1.f, 0.f), tex(0.f, 0.f)};
    out.push_back(sheet);
    // the edges: across the depth the middle 2 px of the texture, along the edge all of it
    float const s0 = 7 / 16.f, s1 = 9 / 16.f, w1 = 2 / 16.f;
    for (int end = 0; end < 2; ++end) {
        float e = float(end);
        Quad  q; // at u = end, along v
        q.pos = {at(0.f, e, 0.f), at(w1, e, 0.f), at(w1, e, 1.f), at(0.f, e, 1.f)};
        q.uv  = {tex(s0, 1.f), tex(s1, 1.f), tex(s1, 0.f), tex(s0, 0.f)};
        out.push_back(q);
        Quad r; // at v = end, along u
        r.pos = {at(0.f, 0.f, e), at(w1, 0.f, e), at(w1, 1.f, e), at(0.f, 1.f, e)};
        r.uv  = {tex(s0, 1.f), tex(s1, 1.f), tex(s1, 0.f), tex(s0, 0.f)};
        out.push_back(r);
    }
    return out;
}

/// The bars' sprite: a Create bars block's own texture, else the game's (up face for floor /
/// ceiling panels, a side otherwise, as CopycatBarsModel).
glm::vec4 barsUv(Block const& material, bool vertical) {
    std::string const& id = material.getTypeName();
    if (id.starts_with("levitate:")) return atlasUv("levitate_" + id.substr(9));
    if (auto uv = faceUv(material, vertical ? 1 : 2)) return *uv;
    return atlasUv("iron_bars");
}

void drawCopycat(Drawer& d, BlockSource& region, BlockPos const& p, Block const& block, fb::Info const& info) {
    auto&        s    = state();
    auto&        tess = d.tessellator();
    Block const* material = nullptr;
    if (auto it = s.visuals.find(p); it != s.visuals.end()) material = materialOf(it->second);
    mce::Mesh* mesh = nullptr;
    if (!material) {
        mesh = cachedMesh("own|" + ptrKey(&block), [&] { return quadsOf(tess, block); }, tess);
    } else {
        int  f     = std::clamp(game::intState(block, "levitate:facing").value_or(1), 0, 5);
        bool panel = info.kind == fb::Kind::CopycatPanel;
        bool upper = game::intState(block, "levitate:top").value_or(0) != 0;
        if (!panel && f < 2) f = 2;
        // faces that may be occluded (Create's canFaceBeOccluded): the panel's back, the step's own
        // half side and its facing
        int cull = 0;
        if (panel) {
            if (covered(region, p, f ^ 1)) cull |= 1 << (f ^ 1);
        } else {
            int half = upper ? 1 : 0;
            if (covered(region, p, half)) cull |= 1 << half;
            if (covered(region, p, f)) cull |= 1 << f;
        }
        if (panel && fb::isTrapdoorMaterial(*material)) {
            // Create's CopycatPanelModel: a trapdoor material is drawn as it is
            mesh = cachedMesh("td|" + ptrKey(material), [&] { return quadsOf(tess, *material); }, tess);
        } else if (panel && fb::isBarsMaterial(*material)) {
            mesh = cachedMesh("bars|" + ptrKey(material) + "|" + std::to_string(f), [&] { return barsQuads(f, barsUv(*material, f < 2)); }, tess, true);
        } else {
            std::string key = "cc|" + ptrKey(&block) + "|" + ptrKey(material) + "|" + std::to_string(cull);
            mesh            = cachedMesh(key, [&] {
                auto const& mq = quadsOf(tess, *material);
                return panel ? panelQuads(mq, f, cull) : stepQuads(mq, f, upper, cull);
            }, tess);
        }
    }
    if (!mesh) return;
    d.light(region, p);
    d.draw(*mesh, glm::translate(glm::mat4{1.f}, glm::vec3{float(p.x), float(p.y), float(p.z)}));
}

void drawBracket(Drawer& d, BlockSource& region, BlockPos const& p, Json const& br) {
    auto        id     = br.value("id", std::string("levitate:metal_bracket"));
    int         facing = std::clamp(br.value("facing", 1), 0, 5);
    int         axis   = std::clamp(br.value("axis", 1), 0, 2);
    bool        cog    = br.value("cog", false);
    auto const* block  = game::blockWith(id, {{"levitate:mounted", 1}, {"levitate:facing", facing}, {"levitate:axis", axis}, {"levitate:cog", cog ? 1 : 0}});
    if (!block) return;
    auto& tess = d.tessellator();
    auto* mesh = cachedMesh("br|" + ptrKey(block), [&] { return quadsOf(tess, *block); }, tess);
    if (!mesh) return;
    d.light(region, p);
    d.draw(*mesh, glm::translate(glm::mat4{1.f}, glm::vec3{float(p.x), float(p.y), float(p.z)}));
}

// ---- sliding doors ------------------------------------------------------------------------------------

/// Create's SlidingDoorBlockEntity animation: chases open (1) / closed (0) at 0.15 per tick.
float doorValue(BlockPos const& p, bool open, double now) {
    auto& a = state().anims[p];
    if (!a.init) {
        a.init  = true;
        a.value = open ? 1.f : 0.f;
        a.time  = now;
        return a.value;
    }
    float target = open ? 1.f : 0.f;
    float stepv  = float(std::max(0.0, now - a.time)) * 0.15f;
    a.value      = a.value < target ? std::min(target, a.value + stepv) : std::max(target, a.value - stepv);
    a.time       = now;
    return a.value;
}

int quarterOf(int f) { return f == 2 ? 0 : f == 5 ? 1 : f == 3 ? 2 : 3; }

void drawDoor(Drawer& d, BlockSource& region, BlockPos const& p, Block const& block, fb::Info const& info, double now) {
    int  facing = game::intState(block, "levitate:facing").value_or(2);
    if (facing < 2) facing = 2;
    int  hinge  = game::intState(block, "levitate:hinge").value_or(0);
    int  upper  = game::intState(block, "levitate:upper").value_or(0);
    bool open   = game::intState(block, "levitate:open").value_or(0) != 0;
    // the closed panel as seen facing north; turned onto the facing below
    auto const* closed = game::blockWith(info.id, {{"levitate:facing", 2}, {"levitate:upper", upper}, {"levitate:hinge", hinge}});
    if (!closed) return;
    float value = doorValue(p, open, now);
    float eased = value * value;
    auto& tess  = d.tessellator();

    glm::mat4 world = glm::translate(glm::mat4{1.f}, glm::vec3{float(p.x), float(p.y), float(p.z)});
    world           = glm::translate(world, glm::vec3{.5f, 0.f, .5f});
    world           = glm::rotate(world, glm::radians(-90.f * float(quarterOf(facing))), glm::vec3{0.f, 1.f, 0.f});
    world           = glm::translate(world, glm::vec3{-.5f, 0.f, -.5f});
    // out of the frame towards the facing (north = -z here) during the first tenth of the movement
    world = glm::translate(world, glm::vec3{0.f, 0.f, -std::clamp(value * 10.f, 0.f, 1.f) / 32.f});
    d.light(region, p);
    if (!info.folds) {
        auto* mesh = cachedMesh("door|" + ptrKey(closed), [&] { return quadsOf(tess, *closed); }, tess);
        if (!mesh) return;
        float slide = eased * 13.f / 16.f * (hinge ? 1.f : -1.f);
        d.draw(*mesh, glm::translate(world, glm::vec3{slide, 0.f, 0.f}));
        return;
    }
    // folding: two leaves turning about the jamb (hinge left: x = 0) and about each other
    float     sgn   = hinge ? -1.f : 1.f;
    float     jamb  = hinge ? 1.f : 0.f;
    glm::vec3 pivot{jamb, 0.f, 1.f}, mid{.5f, 0.f, 1.f};
    for (int leaf = 0; leaf < 2; ++leaf) {
        bool  nearJamb = leaf == 0;
        float x0 = (nearJamb != bool(hinge)) ? 0.f : .5f;
        auto* mesh = cachedMesh("leaf|" + ptrKey(closed) + "|" + std::to_string(x0), [&] {
            std::vector<Quad> out;
            for (auto const& q : quadsOf(tess, *closed))
                if (auto c = cropAndMove(q, glm::vec3{x0, 0.f, 0.f}, glm::vec3{x0 + .5f, 1.f, 1.f}, glm::vec3{0.f})) out.push_back(*c);
            return out;
        }, tess);
        if (!mesh) continue;
        glm::mat4 m = glm::translate(world, pivot);
        m           = glm::rotate(m, glm::radians(sgn * 91.f * eased), glm::vec3{0.f, 1.f, 0.f});
        m           = glm::translate(m, -pivot);
        if (!nearJamb) {
            m = glm::translate(m, mid);
            m = glm::rotate(m, glm::radians(sgn * -181.f * eased), glm::vec3{0.f, 1.f, 0.f});
            m = glm::translate(m, -mid);
        }
        d.draw(*mesh, m);
    }
}

// ---- connected-texture corners --------------------------------------------------------------------

struct CtSpec {
    int         faces = 0;  ///< bit mask of faces with an omnidirectional sheet
    std::string texture;    ///< the sheet without joins (its corner is the notch)
    float       border = 0; ///< frame width (pixels)
    bool        pane   = false;
};

/// What of a block has Create's OMNIDIRECTIONAL connected textures.
std::optional<CtSpec> ctSpecOf(Block const& b) {
    std::string const& id = b.getTypeName();
    if (auto const* info = fb::infoOf(b)) {
        switch (info->kind) {
        case fb::Kind::Casing: return CtSpec{0x3f, "levitate_" + id.substr(9) + "_0", 2.f, false};
        case fb::Kind::CtGlass:
            if (info->ct == fb::Ct::Omni) return CtSpec{0x3f, "levitate_framed_glass_0", 1.f, false};
            if (info->ct == fb::Ct::Horizontal) return CtSpec{0x03, "levitate_framed_glass_0", 1.f, false};
            return std::nullopt;
        case fb::Kind::Pane:
            if (info->ct == fb::Ct::Omni) return CtSpec{0x3c, "levitate_framed_glass_0", 1.f, true};
            return std::nullopt;
        default: return std::nullopt;
        }
    }
    if (id.rfind("levitate:layered_", 0) == 0) return CtSpec{0x03, "levitate_" + id.substr(17) + "_cap_0", 1.f, false};
    if (id.rfind("levitate:", 0) == 0 && id.ends_with("_pillar") && game::intState(b, "levitate:c_up")) {
        auto const& nbt = *b.mSerializationId;
        auto face = nbt.contains("states") && nbt["states"].contains("minecraft:block_face") ? std::string(nbt["states"]["minecraft:block_face"].get<StringTag>()) : std::string("up");
        int  mask = (face == "north" || face == "south") ? 0x0c : (face == "east" || face == "west") ? 0x30 : 0x03;
        return CtSpec{mask, "levitate_" + id.substr(9, id.size() - 9 - 7) + "_cap_0", 1.f, false};
    }
    return std::nullopt;
}

/// The notch quads of one face (block units).
std::vector<Quad> notchQuads(BlockSource& region, BlockPos const& p, Block const& block, CtSpec const& spec, int face) {
    std::vector<Quad> out;
    int const* e = kFaceEdges[face];
    auto conn = [&](int dir) { return game::intState(block, std::string("levitate:c_") + kDirNames[dir]).value_or(0) != 0; };
    glm::vec3 n = dirVec(face), up = dirVec(e[0]), right = dirVec(e[1]);
    float     plane = .5f + .001f;
    if (spec.pane) plane = 1.f / 16.f + .001f; // the pane's faces are 2 px apart
    glm::vec3 center{.5f, .5f, .5f};
    auto      uv  = atlasUv(spec.texture);
    float     bw  = spec.border / 16.f;
    // corners: (top, right), (right, bottom), (bottom, left), (left, top) -> (r range, t range)
    struct Corner {
        int   a, b;
        float r0, t0;
    };
    Corner const corners[4]{{0, 1, 1 - bw, 1 - bw}, {1, 2, 1 - bw, 0}, {2, 3, 0, 0}, {3, 0, 0, 1 - bw}};
    for (auto const& c : corners) {
        int da = e[c.a], db = e[c.b];
        if (!conn(da) || !conn(db)) continue;
        BlockPos diag = step(step(p, da), db);
        if (region.getBlock(diag).getTypeName() == block.getTypeName()) continue;
        auto at = [&](float r, float t) { return center + (r - .5f) * right + (t - .5f) * up + plane * n; };
        auto tc = [&](float r, float t) { return glm::vec2{uv.x + (uv.z - uv.x) * r, uv.y + (uv.w - uv.y) * (1.f - t)}; };
        Quad q;
        float r0 = c.r0, r1 = c.r0 + bw, t0 = c.t0, t1 = c.t0 + bw;
        q.pos = {at(r0, t1), at(r1, t1), at(r1, t0), at(r0, t0)};
        q.uv  = {tc(r0, t1), tc(r1, t1), tc(r1, t0), tc(r0, t0)};
        out.push_back(q);
    }
    return out;
}

void drawNotches(Drawer& d, BlockSource& region, BlockPos const& p, Block const& block, CtSpec const& spec) {
    auto& s    = state();
    auto& tess = d.tessellator();
    auto  it   = s.notchKeys.find(p);
    if (it == s.notchKeys.end()) {
        std::vector<std::pair<int, std::string>> keys;
        for (int f = 0; f < 6; ++f) {
            if (!(spec.faces >> f & 1)) continue;
            if (!spec.pane) {
                Block const& front = region.getBlock(step(p, f));
                if (front.getTypeName() == block.getTypeName()) continue; // the face is hidden
            }
            auto quads = notchQuads(region, p, block, spec, f);
            if (quads.empty()) continue;
            std::string key = "notch|" + std::to_string(p.x) + "|" + std::to_string(p.y) + "|" + std::to_string(p.z) + "|" + std::to_string(f);
            s.meshes[key]   = buildMesh(tess, quads, true);
            keys.emplace_back(f, key);
        }
        it = s.notchKeys.emplace(p, std::move(keys)).first;
    }
    for (auto const& [f, key] : it->second) {
        auto m = s.meshes.find(key);
        if (m == s.meshes.end() || !m->second) continue;
        d.light(region, spec.pane ? p : step(p, f));
        d.draw(*m->second, glm::translate(glm::mat4{1.f}, glm::vec3{float(p.x), float(p.y), float(p.z)}));
    }
}

// ---- copycat placement helper ghost -----------------------------------------------------------------

/// Create's PlacementClient: the ghost of the copycat a click would place (CreateClient.GHOST_BLOCKS
/// .showGhostState(...).breathingAlpha()).
void renderHelperGhost(BaseActorRenderContext& ctx, LocalPlayer& player, double now) {
    static bool logged = false;
    if (player.isSneaking()) return;
    ItemStack const& held = player.getSelectedItem();
    if (held.isNull()) return;
    std::string heldId = held.getTypeName();
    if (heldId != "levitate:copycat_panel" && heldId != "levitate:copycat_step") return;
    auto const& hit = player.getLevel().getHitResult();
    if (hit.mType != HitResultType::Tile) return;
    auto&        region = player.getDimensionBlockSource();
    Block const& b      = region.getBlock(hit.mBlock);
    if (b.getTypeName() != heldId) return;
    core::GlobalPos at{static_cast<int>(player.getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    // CEquipment.placementAssistRange, +4 with the extendo grip (as the server)
    auto const& off0  = player.getOffhandSlot();
    int         range = feature::equipment::placementAssistRange() + (!off0.isNull() && off0.getTypeName() == "levitate:extendo_grip" ? 4 : 0);
    auto off = fb::copycatHelperOffset(region, at, b, heldId, hit.mPos.x - hit.mBlock.x, hit.mPos.y - hit.mBlock.y, hit.mPos.z - hit.mBlock.z, range);
    if (!off || !off->block) return;
    Drawer d(ctx);
    if (!d.ok()) return;
    auto& tess = d.tessellator();
    // Create's breathing alpha: between 1/4 and 3/4, a breath every 2 s (16 steps, cached meshes)
    int         step = int(std::lround(8 + 8 * std::sin(now / 20.0 * 3.14159265)));
    float       alpha = 0.25f + 0.5f * float(step) / 16.f;
    mce::Mesh*  mesh  = cachedMesh("ghost|" + ptrKey(off->block) + "|" + std::to_string(step), [&] {
        auto quads = quadsOf(tess, *off->block);
        for (auto& q : quads) q.color.a = alpha;
        return quads;
    }, tess);
    if (!mesh) return;
    BlockPos tp{off->pos.x, off->pos.y, off->pos.z};
    d.light(region, tp);
    d.drawBlended(*mesh, glm::translate(glm::mat4{1.f}, glm::vec3{float(tp.x), float(tp.y), float(tp.z)}));
    if (!logged) {
        logged = true;
        log().info("building client: placement helper ghost of {} at {} {} {}", heldId, tp.x, tp.y, tp.z);
    }
}

// ---- frame ------------------------------------------------------------------------------------------

void renderBuilding(BaseActorRenderContext& ctx) {
    auto* ci = currentClient();
    if (!ci) return;
    auto* level  = ci->getLevel();
    auto* player = ci->getLocalPlayer();
    auto& s      = state();
    if (level != s.level) {
        resetLevel(false);
        s.level = level;
    }
    if (!level || !player) return;
    int dim = int(player->getDimensionId());
    if (dim != s.dim) {
        s.visuals.clear();
        s.asked.clear();
        s.anims.clear();
        s.notchKeys.clear();
        s.meshes.clear();
        s.dim = dim;
    }
    if (feature::net::client::connected()) {
        askChunks();
        renderHelperGhost(ctx, *player, double(level->getCurrentTick().tickID) + ctx.mFrameAlpha);
    }

    PosSet copycats, doors, ct, ctDirty;
    {
        std::lock_guard lock(s.mutex);
        copycats = s.copycats;
        doors    = s.doors;
        ct       = s.ct;
        ctDirty.swap(s.ctDirty);
    }
    // re-meshed connected-texture blocks: their corners (and their neighbours') are computed again
    auto forget = [&](BlockPos const& q) {
        auto it = s.notchKeys.find(q);
        if (it == s.notchKeys.end()) return;
        for (auto const& [face, key] : it->second) s.meshes.erase(key);
        s.notchKeys.erase(it);
    };
    for (auto const& p : ctDirty)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) forget(BlockPos{p.x + dx, p.y + dy, p.z + dz});
    if (copycats.empty() && doors.empty() && ct.empty() && s.visuals.empty()) return;
    Drawer d(ctx);
    if (!d.ok()) return;
    auto&                 region = player->getDimensionBlockSource();
    double                now    = double(level->getCurrentTick().tickID) + ctx.mFrameAlpha;
    std::vector<BlockPos> goneCopycats, goneDoors, goneCt;
    int                   drawn = 0;

    for (auto const& p : copycats) {
        if (!region.hasChunksAt(p, 0, false)) continue;
        Block const& b    = region.getBlock(p);
        auto const*  info = fb::infoOf(b);
        if (!info || (info->kind != fb::Kind::CopycatPanel && info->kind != fb::Kind::CopycatStep)) {
            goneCopycats.push_back(p);
            continue;
        }
        drawCopycat(d, region, p, b, *info);
        ++drawn;
    }
    for (auto const& p : doors) {
        if (!region.hasChunksAt(p, 0, false)) continue;
        Block const& b    = region.getBlock(p);
        auto const*  info = fb::infoOf(b);
        if (!info || info->kind != fb::Kind::Door) {
            goneDoors.push_back(p);
            s.anims.erase(p);
            continue;
        }
        drawDoor(d, region, p, b, *info, now);
        ++drawn;
    }
    for (auto const& p : ct) {
        if (!region.hasChunksAt(p, 0, false)) continue;
        Block const& b    = region.getBlock(p);
        auto         spec = ctSpecOf(b);
        if (!spec) {
            goneCt.push_back(p);
            forget(p);
            continue;
        }
        drawNotches(d, region, p, b, *spec);
    }
    for (auto const& [p, v] : s.visuals) {
        if (v.bracket.is_null() || !region.hasChunksAt(p, 0, false)) continue;
        std::string const& id = region.getBlock(p).getTypeName();
        if (id != "levitate:shaft" && id != "levitate:cogwheel" && id != "levitate:large_cogwheel") continue;
        drawBracket(d, region, p, v.bracket);
        ++drawn;
    }
    if (!goneCopycats.empty() || !goneDoors.empty() || !goneCt.empty()) {
        std::lock_guard lock(s.mutex);
        for (auto const& p : goneCopycats) s.copycats.erase(p);
        for (auto const& p : goneDoors) s.doors.erase(p);
        for (auto const& p : goneCt) s.ct.erase(p);
    }
    if (drawn && !s.logged) {
        s.logged = true;
        log().info("building client: drawing {} copycats, doors and brackets", drawn);
    }
}

} // namespace

// Chunk meshes: copycats and sliding doors are drawn by us while the server runs Levitate; the
// positions of connected-texture blocks and of bracketable shafts are noted.
LL_STATIC_HOOK(
    BuildingExtractHook,
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
    if (visualReference == ClientBlockPipeline::VisualReference::Block && feature::net::client::connected()) {
        std::string const& id = block.getTypeName();
        if (id.size() > 9 && id.compare(0, 9, "levitate:") == 0) {
            BlockPos const& rel = iterData.relativePos;
            BlockPos        pos{chunkMinPos.x + rel.x, chunkMinPos.y + rel.y, chunkMinPos.z + rel.z};
            auto&           s    = state();
            auto const*     info = fb::infoOf(block);
            if (info && (info->kind == fb::Kind::CopycatPanel || info->kind == fb::Kind::CopycatStep || info->kind == fb::Kind::Door)) {
                std::lock_guard lock(s.mutex);
                if (info->kind == fb::Kind::Door) s.doors.insert(pos);
                else {
                    s.copycats.insert(pos);
                    s.wanted.insert({pos.x >> 4, pos.z >> 4});
                }
                return std::nullopt;
            }
            if (id == "levitate:shaft" || id == "levitate:cogwheel" || id == "levitate:large_cogwheel") {
                std::lock_guard lock(s.mutex);
                s.wanted.insert({pos.x >> 4, pos.z >> 4});
            } else if (ctSpecOf(block)) {
                std::lock_guard lock(s.mutex);
                s.ct.insert(pos);
                s.ctDirty.insert(pos); // re-meshed: its corners are computed again (client thread)
            }
        }
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

LL_TYPE_INSTANCE_HOOK(
    BuildingRenderHook,
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
        renderBuilding(renderContext);
    } catch (...) {
    }
}

class BuildingClientModule {
public:
    void install() {
        BuildingExtractHook::hook();
        BuildingRenderHook::hook();
        feature::net::client::on(fb::kVisualsChannel, [](Json const& msg) { onVisuals(msg); });
        // Create's CopycatBlock#getDestroyProgress for our own player: the material sent for drawing
        fb::clientCopycatMaterial() = [](BlockPos const& p) -> Block const* {
            auto& v  = state().visuals;
            auto  it = v.find(p);
            return it == v.end() ? nullptr : materialOf(it->second);
        };
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            resetLevel(true);
            state().level = nullptr;
        });
    }
    void uninstall() {
        fb::clientCopycatMaterial() = nullptr;
        BuildingExtractHook::unhook();
        BuildingRenderHook::unhook();
        ll::event::EventBus::getInstance().removeListener(mExit);
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::building

LV_REGISTER_MODULE(lv::client::building::BuildingClientModule);

#endif // LV_CLIENT
