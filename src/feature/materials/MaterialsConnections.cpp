// Materials unit (U9): states that follow the neighbours (Java updateShape), kept by the server for
// every client (docs/SPEC.md §3.6.9):
// - walls: Bedrock's minecraft:connection trait gives the four connections; this adds Java's
//   WallBlock side heights (levitate:tall_<dir>: the side is covered by the block above) and the
//   post (levitate:up: unless exactly straight, forced by a wall post / torch / sign / banner /
//   pressure plate above or a block above covering the post), and the 24 px collision;
// - layered stone (Create's HorizontalCTBehaviour): levitate:c_<dir> for the 4 horizontal
//   neighbours of the same block;
// - stone pillars (Create's RotatedPillarCTBehaviour): levitate:c_<dir> in world space: along the
//   axis the same pillar on the same axis; across it also needs both columns to end at the same
//   places (pillars join sideways into rectangles only).
//
// Every block change (any cause) marks the affected positions; they are recomputed at the next
// server tick.

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <set>
#include <string>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/GetCollisionShapeInterface.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "core/GlobalPos.h"
#include "feature/materials/Materials.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::materials {

namespace {

using core::GlobalPos;
using Box = std::array<float, 6>;

constexpr int kDx[6]{0, 0, 0, 0, -1, 1};
constexpr int kDy[6]{-1, 1, 0, 0, 0, 0};
constexpr int kDz[6]{0, 0, -1, 1, 0, 0};
constexpr char const* kDirNames[6]{"down", "up", "north", "south", "west", "east"};
constexpr int kMaxColumn = 1000; // Create's ConnectedPillarBlock walks up to 1000 blocks

BlockPos step(BlockPos const& p, int dir, int n = 1) { return {p.x + kDx[dir] * n, p.y + kDy[dir] * n, p.z + kDz[dir] * n}; }
int      axisOfDir(int dir) { return dir < 2 ? 1 : (dir < 4 ? 2 : 0); } // 0 x, 1 y, 2 z

std::set<GlobalPos>& dirty() {
    static std::set<GlobalPos> d;
    return d;
}

bool loaded(BlockSource& region, BlockPos const& p) {
    return p.y >= region.getMinHeight() && p.y < region.getMaxHeight() && region.hasChunksAt(p, 0, false);
}

Shape shapeOf(Block const& b) {
    auto const* i = blockInfo(b);
    return i ? i->shape : Shape::Cube;
}
bool ours(Block const& b) {
    auto const* i = blockInfo(b);
    return i && (i->shape == Shape::Wall || i->shape == Shape::Layered || i->shape == Shape::StonePillar);
}

/// The pillar's axis (0 x, 1 y, 2 z) from the clicked face it was placed on.
int pillarAxis(Block const& b) {
    auto f = stringState(b, "minecraft:block_face");
    if (f == "north" || f == "south") return 2;
    if (f == "east" || f == "west") return 0;
    return 1;
}

Block const* withStates(Block const& block, std::vector<std::pair<std::string, int>> const& states) {
    Block const* b = &block;
    for (auto const& [name, value] : states) {
        auto const* s = b->getBlockType().getBlockState(HashedString(name));
        if (!s) continue;
        if (auto next = b->setState(*s, value)) b = next.as_ptr();
    }
    return b;
}

// ---- walls (Java WallBlock) --------------------------------------------------------------------

enum class Side { None, Low, Tall };

/// The collision boxes of the block at `p` touching its bottom face, in block pixels (x0 z0 x1 z1).
std::vector<std::array<float, 4>> bottomFace(BlockSource& region, BlockPos const& p) {
    std::vector<std::array<float, 4>> out;
    if (!loaded(region, p)) return out;
    Block const& b = region.getBlock(p);
    if (b.isAir()) return out;
    std::vector<AABB> boxes;
    b.getBlockType().addCollisionShapes(b, region, p, nullptr, boxes, {});
    for (auto const& box : boxes) {
        if (box.min.y > p.y + 1e-4f) continue; // Java getFaceShape(DOWN): only what touches the bottom
        out.push_back({(box.min.x - p.x) * 16.f, (box.min.z - p.z) * 16.f, (box.max.x - p.x) * 16.f, (box.max.z - p.z) * 16.f});
    }
    return out;
}

/// Java WallBlock.isCovered: the face shape covers the test rectangle (x0 z0 x1 z1, pixels).
bool covered(std::vector<std::array<float, 4>> const& face, std::array<float, 4> const& test) {
    if (face.empty()) return false;
    for (float x = test[0] + .25f; x < test[2]; x += .5f) {
        for (float z = test[1] + .25f; z < test[3]; z += .5f) {
            bool in = false;
            for (auto const& r : face) {
                if (x >= r[0] - 1e-3f && x <= r[2] + 1e-3f && z >= r[1] - 1e-3f && z <= r[3] + 1e-3f) {
                    in = true;
                    break;
                }
            }
            if (!in) return false;
        }
    }
    return true;
}

// Java WallBlock's test shapes (x0 z0 x1 z1) for north, east, south, west and the post
constexpr std::array<float, 4> kSideTest[4]{{7, 0, 9, 9}, {7, 7, 16, 9}, {7, 7, 9, 16}, {0, 7, 9, 9}};
constexpr std::array<float, 4> kPostTest{7, 7, 9, 9};
constexpr char const*          kSides[4]{"north", "east", "south", "west"};

bool isWall(Block const& b) {
    std::string const& id = b.getTypeName();
    return id.size() > 5 && id.compare(id.size() - 5, 5, "_wall") == 0;
}

/// Java BlockTags.WALL_POST_OVERRIDE: torches, signs, banners, pressure plates.
bool postOverride(Block const& b) {
    std::string const& id = b.getTypeName();
    if (id.find("torch") != std::string::npos && id.find("torchflower") == std::string::npos) return true;
    if (id.find("sign") != std::string::npos || id.find("banner") != std::string::npos) return true;
    return id.ends_with("pressure_plate");
}

/// A wall above with a post (Java: WallBlock with UP).
bool wallPostAbove(Block const& above) {
    if (!isWall(above)) return false;
    if (auto up = game::intState(above, "levitate:up")) return *up != 0;
    auto bit = stringState(above, "wall_post_bit"); // vanilla walls
    return bit == "1" || bit == "true";
}

bool updateWall(BlockSource& region, BlockPos const& pos, Block const& block) {
    BlockPos abovePos = step(pos, 1);
    if (!loaded(region, abovePos)) return false;
    Block const& above = region.getBlock(abovePos);
    auto         face  = bottomFace(region, abovePos);
    Side         side[4];
    for (int i = 0; i < 4; ++i) {
        bool conn = game::intState(block, std::string("minecraft:connection_") + kSides[i]).value_or(0) != 0;
        side[i]   = !conn ? Side::None : (covered(face, kSideTest[i]) ? Side::Tall : Side::Low);
    }
    // Java WallBlock.shouldRaisePost
    bool up = true;
    if (!wallPostAbove(above)) {
        bool n = side[0] == Side::None, e = side[1] == Side::None, s = side[2] == Side::None, w = side[3] == Side::None;
        bool open = (n && s && w && e) || n != s || w != e;
        if (!open) {
            bool tallLine = (side[0] == Side::Tall && side[2] == Side::Tall) || (side[1] == Side::Tall && side[3] == Side::Tall);
            up            = !tallLine && (postOverride(above) || covered(face, kPostTest));
        }
    }
    std::vector<std::pair<std::string, int>> s{{"levitate:up", up ? 1 : 0}};
    for (int i = 0; i < 4; ++i) s.emplace_back(std::string("levitate:tall_") + kSides[i], side[i] == Side::Tall ? 1 : 0);
    Block const* next = withStates(block, s);
    if (!next || next == &block) return false;
    return game::setBlock(region, pos, *next);
}

// ---- connected textures ------------------------------------------------------------------------

/// The pillar column through `p` along `axis`: blocks of the same pillar on the same axis below /
/// above `p` (Create's ConnectedPillarBlock column walk).
/// Column extents of the current batch (a batch only changes states, never ids or axes).
std::map<std::tuple<int, int, int, int>, std::pair<int, int>>& extentCache() {
    static std::map<std::tuple<int, int, int, int>, std::pair<int, int>> c;
    return c;
}

std::pair<int, int> columnExtent(BlockSource& region, BlockPos const& p, std::string const& id, int axis) {
    auto key = std::tuple{p.x, p.y, p.z, axis};
    if (auto it = extentCache().find(key); it != extentCache().end()) return it->second;
    int neg = axis == 0 ? 4 : (axis == 1 ? 0 : 2), pos = neg + 1;
    auto walk = [&](int dir) {
        int n = 0;
        for (int i = 1; i <= kMaxColumn; ++i) {
            BlockPos q = step(p, dir, i);
            if (!loaded(region, q)) break;
            Block const& b = region.getBlock(q);
            if (b.getTypeName() != id || pillarAxis(b) != axis) break;
            ++n;
        }
        return n;
    };
    std::pair<int, int> out{walk(neg), walk(pos)};
    extentCache()[key] = out;
    return out;
}

bool updateCt(BlockSource& region, BlockPos const& pos, Block const& block, Shape shape) {
    std::string const&                       id = block.getTypeName();
    std::vector<std::pair<std::string, int>> s;
    if (shape == Shape::Layered) {
        for (int d = 2; d < 6; ++d) {
            BlockPos n = step(pos, d);
            s.emplace_back(std::string("levitate:c_") + kDirNames[d], loaded(region, n) && region.getBlock(n).getTypeName() == id ? 1 : 0);
        }
    } else {
        int                 axis = pillarAxis(block);
        std::pair<int, int> own{-1, -1};
        for (int d = 0; d < 6; ++d) {
            BlockPos n  = step(pos, d);
            bool     on = false;
            if (loaded(region, n)) {
                Block const& nb = region.getBlock(n);
                on              = nb.getTypeName() == id && pillarAxis(nb) == axis;
                if (on && axisOfDir(d) != axis) {
                    if (own.first < 0) own = columnExtent(region, pos, id, axis);
                    on = columnExtent(region, n, id, axis) == own;
                }
            }
            s.emplace_back(std::string("levitate:c_") + kDirNames[d], on ? 1 : 0);
        }
    }
    Block const* next = withStates(block, s);
    if (!next || next == &block) return false;
    return game::setBlock(region, pos, *next);
}

// ---- dirty tracking ----------------------------------------------------------------------------

void mark(int dim, BlockPos const& p) { dirty().insert(GlobalPos{dim, p.x, p.y, p.z}); }

/// Marks the pillar columns next to `p` (and the columns beside them): their side connections
/// depend on the length of the column through `p`.
void markPillars(BlockSource& region, int dim, BlockPos const& p) {
    std::set<std::pair<int, int>> none;
    for (int d = 0; d < 6; ++d) {
        BlockPos n = step(p, d);
        if (!loaded(region, n)) continue;
        Block const& nb = region.getBlock(n);
        if (shapeOf(nb) != Shape::StonePillar) continue;
        int  axis = pillarAxis(nb);
        auto ext  = columnExtent(region, n, nb.getTypeName(), axis);
        int  neg  = axis == 0 ? 4 : (axis == 1 ? 0 : 2);
        for (int i = -ext.first; i <= ext.second; ++i) {
            BlockPos q = i < 0 ? step(n, neg, -i) : step(n, neg + 1, i);
            mark(dim, q);
            for (int e = 0; e < 6; ++e)
                if (axisOfDir(e) != axis) mark(dim, step(q, e));
        }
    }
}

void onChanged(BlockSource& region, BlockPos const& p, Block const& previous, Block const& now) {
    extentCache().clear();
    int  dim     = static_cast<int>(region.getDimensionId());
    bool wasOurs = ours(previous), isOurs = ours(now);
    if (isOurs) mark(dim, p);
    bool pillar = shapeOf(previous) == Shape::StonePillar || shapeOf(now) == Shape::StonePillar;
    for (int d = 0; d < 6; ++d) {
        BlockPos n = step(p, d);
        if (!loaded(region, n)) continue;
        Block const& nb = region.getBlock(n);
        if (ours(nb)) mark(dim, n);
    }
    if (pillar) markPillars(region, dim, p);
    (void)wasOurs;
}

BlockSource* regionOf(int dim) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto d = level->getDimension(dim).lock();
    return d ? &d->getBlockSourceFromMainChunkSource() : nullptr;
}

void processDirty() {
    if (dirty().empty()) return;
    auto work = std::move(dirty());
    dirty().clear();
    extentCache().clear();
    for (auto const& g : work) {
        auto* region = regionOf(g.dim);
        if (!region) continue;
        BlockPos p{g.x, g.y, g.z};
        if (!loaded(*region, p)) continue;
        try {
            updateConnections(*region, p);
        } catch (std::exception const& e) {
            lv::log().error("materials: updating {} {} {} failed: {}", p.x, p.y, p.z, e.what());
        }
    }
}

std::vector<ll::event::ListenerPtr>& listeners() {
    static std::vector<ll::event::ListenerPtr> l;
    return l;
}

} // namespace

bool updateConnections(BlockSource& region, BlockPos const& pos) {
    Block const& block = region.getBlock(pos);
    switch (shapeOf(block)) {
    case Shape::Wall: return updateWall(region, pos, block);
    case Shape::Layered:
    case Shape::StonePillar: return updateCt(region, pos, block, shapeOf(block));
    default: return false;
    }
}

std::vector<std::array<float, 6>> wallCollision(Block const& block) {
    // Java WallBlock.makeShapes(4, 3, 24, 0, 24, 24): post 4..12, sides 5..11, all 24 px high
    std::vector<Box> out;
    if (game::intState(block, "levitate:up").value_or(1) != 0) out.push_back({4, 0, 4, 12, 24, 12});
    auto conn = [&](char const* d) { return game::intState(block, std::string("minecraft:connection_") + d).value_or(0) != 0; };
    if (conn("north")) out.push_back({5, 0, 0, 11, 24, 11});
    if (conn("south")) out.push_back({5, 0, 5, 11, 24, 16});
    if (conn("west")) out.push_back({0, 0, 5, 11, 24, 11});
    if (conn("east")) out.push_back({5, 0, 5, 16, 24, 11});
    if (out.empty()) out.push_back({4, 0, 4, 12, 24, 12});
    return out;
}

LL_TYPE_INSTANCE_HOOK(
    MaterialsWallCollisionHook,
    ll::memory::HookPriority::Normal,
    BlockType,
    &BlockType::$addCollisionShapes,
    bool,
    ::Block const&                                     block,
    ::IConstBlockSource const&                         region,
    ::BlockPos const&                                  pos,
    ::AABB const*                                      intersectTestBox,
    ::std::vector<::AABB>&                             inoutBoxes,
    ::optional_ref<::GetCollisionShapeInterface const> entity
) {
    auto const* info = blockInfo(block);
    if (!info || info->shape != Shape::Wall) return origin(block, region, pos, intersectTestBox, inoutBoxes, entity);
    bool added = false;
    for (auto const& b : wallCollision(block)) {
        AABB box{
            Vec3{pos.x + b[0] / 16.f, pos.y + b[1] / 16.f, pos.z + b[2] / 16.f},
            Vec3{pos.x + b[3] / 16.f, pos.y + b[4] / 16.f, pos.z + b[5] / 16.f}
        };
        if (intersectTestBox && !intersectTestBox->intersects(box)) continue;
        inoutBoxes.push_back(box);
        added = true;
    }
    return added;
}

void installConnections() {
    MaterialsWallCollisionHook::hook();
    auto& bus = ll::event::EventBus::getInstance();
    listeners().push_back(bus.emplaceListener<ll::event::BlockChangedEvent>([](ll::event::BlockChangedEvent& ev) {
        if (ev.layer() != 0) return;
        onChanged(ev.blockSource(), ev.pos(), ev.previousBlock(), ev.newBlock());
    }));
    listeners().push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { processDirty(); }));
}

void uninstallConnections() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& l : listeners()) bus.removeListener(l);
    listeners().clear();
    dirty().clear();
    MaterialsWallCollisionHook::unhook();
}

} // namespace lv::feature::materials
