// Handheld worldshaper (Create's WorldshaperItem / TerrainBrushes / TerrainTools / FlattenTool):
// left-click a block to pick the material, sneak + right-click to configure, right-click to shape
// the terrain at the looked-at block (up to 128 blocks away).

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockSupportType.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/phys/HitResult.h"

#include "core/equipment/Shaper.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/equipment/Equipment.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;

namespace {

constexpr char kScreen[] = "equipment.worldshaper";

std::map<std::int64_t, std::uint64_t>& cooldowns() {
    static std::map<std::int64_t, std::uint64_t> m;
    return m;
}

ce::ShaperSettings settingsOf(ItemStack const& s) { return ce::ShaperSettings::fromJson(items::getData(s, kShaperData)); }

void writeSettings(Player& player, int slot, ce::ShaperSettings const& st) {
    auto& inv = player.getInventory();
    auto  s   = inv.getItem(slot);
    if (items::idOf(s) != kWorldshaper) return;
    items::setData(s, kShaperData, st.toJson());
    inv.setItem(slot, s);
    player.sendInventory(false);
}

Block const* materialBlock(ce::ShaperSettings const& s) {
    if (s.material.empty()) return nullptr;
    auto tag = CompoundTag::fromSnbt(s.material);
    if (!tag) return nullptr;
    auto b = Block::tryGetFromRegistry(*tag);
    return b ? b.as_ptr() : nullptr;
}

std::string blockName(Player& player, Block const& b) {
    std::string id = b.getTypeName();
    if (auto item = items::make(id, 1)) return items::displayName(*item);
    (void)player;
    return id;
}

ce::IPos ip(GlobalPos const& p) { return {p.x, p.y, p.z}; }
GlobalPos gp(int dim, ce::IPos const& p) { return {dim, p.x, p.y, p.z}; }
ce::IPos stepFace(ce::IPos p, int face, int n = 1) {
    static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    if (face < 0 || face > 5) return p;
    return {p.x + dx[face] * n, p.y + dy[face] * n, p.z + dz[face] * n};
}
int axisOf(int face) { return face <= 1 ? 1 : face <= 3 ? 2 : 0; }
int coordOn(ce::IPos const& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

bool replaceable(int dim, ce::IPos const& p) { return replaceableAt(gp(dim, p)); }

/// Create's FlattenTool on the positions (heights along the face axis, smoothed, then moved).
void flatten(int dim, std::vector<ce::IPos> const& targets, int face) {
    int axis   = axisOf(face);
    int offset = (face == 1 || face == 3 || face == 5) ? 1 : -1;
    auto coords = [&](ce::IPos const& p) -> std::pair<int, int> {
        if (axis == 0) return {p.z, p.y};
        if (axis == 1) return {p.x, p.z};
        return {p.x, p.y};
    };
    constexpr int kMax = 2147483647;
    std::map<std::pair<int, int>, int> heights;
    std::vector<ce::IPos>              surfaces;
    int minE = kMax, maxE = ce::kNoHeight, minA = kMax, minB = kMax, maxA = ce::kNoHeight, maxB = ce::kNoHeight;
    for (auto p : targets) {
        auto c = coords(p);
        minA = std::min(minA, c.first), minB = std::min(minB, c.second), maxA = std::max(maxA, c.first), maxB = std::max(maxB, c.second);
        if (replaceable(dim, p)) {
            if (!heights.contains(c)) heights[c] = ce::kNoHeight;
            continue;
        }
        p = stepFace(p, face);
        if (!replaceable(dim, p)) {
            if (!heights.contains(c) || heights[c] == ce::kNoHeight) heights[c] = kMax;
            continue;
        }
        surfaces.push_back(p);
        int coord = coordOn(p, axis);
        if (!heights.contains(c) || heights[c] == kMax || heights[c] == ce::kNoHeight || heights[c] * offset < coord * offset) {
            heights[c] = coord;
            maxE       = std::max(maxE, coord);
            minE       = std::min(minE, coord);
        }
    }
    if (surfaces.empty()) return;
    int rows = maxA - minA + 1, cols = maxB - minB + 1;
    std::vector<int> grid(static_cast<std::size_t>(rows * cols), ce::kNoHeight);
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) {
            auto it = heights.find({minA + i, minB + j});
            int  v  = ce::kNoHeight;
            if (it != heights.end()) {
                if (it->second == kMax) v = offset == 1 ? maxE + 2 : minE - 2;
                else if (it->second == ce::kNoHeight) v = offset == 1 ? minE - 2 : maxE + 2;
                else v = it->second;
            }
            grid[static_cast<std::size_t>(i * cols + j)] = v;
        }
    grid = ce::smoothHeights(grid, rows, cols);
    for (auto p : surfaces) {
        auto c       = coords(p);
        int  surface = coordOn(p, axis) * offset;
        int  target  = grid[static_cast<std::size_t>((c.first - minA) * cols + (c.second - minB))] * offset;
        if (surface == target) continue;
        int  opposite  = face ^ 1;
        auto const* bs = blockAt(gp(dim, p));
        if (!bs) continue;
        Block const& surfaceBlock = *bs;
        int timeout = 1000;
        while (surface > target && timeout-- > 0) {
            auto below = stepFace(p, opposite);
            placeBlock(gp(dim, below), surfaceBlock);
            clearAt(gp(dim, p));
            p = below;
            --surface;
        }
        while (surface < target && timeout-- > 0) {
            auto above = stepFace(p, face);
            placeBlock(gp(dim, above), surfaceBlock);
            auto const* under = blockAt(gp(dim, stepFace(p, opposite)));
            if (under) placeBlock(gp(dim, p), *under);
            p = above;
            ++surface;
        }
    }
}

void openForm(Player& player, int slot) {
    auto st = settingsOf(player.getInventory().getItem(slot));
    ll::form::CustomForm form(trFor(player, "item.levitate:handheld_worldshaper.name", "Creative Worldshaper"));
    std::vector<std::string> brushes, tools, placements, patterns;
    for (int b = 0; b < 5; ++b) brushes.push_back(trFor(player, ce::brushKey(ce::Brush(b)), ce::brushFallback(ce::Brush(b))));
    for (int t = 0; t < 6; ++t) tools.push_back(trFor(player, ce::toolKey(ce::ShaperTool(t)), ce::toolFallback(ce::ShaperTool(t))));
    for (int p = 0; p < 3; ++p) placements.push_back(trFor(player, ce::placementKey(ce::Placement(p)), ce::placementFallback(ce::Placement(p))));
    for (int p = 0; p < 6; ++p) patterns.push_back(trFor(player, ce::patternKey(ce::Pattern(p)), ce::patternFallback(ce::Pattern(p))));
    form.appendDropdown("brush", trFor(player, "create.gui.terrainzapper.brush", "Brush"), brushes, static_cast<std::size_t>(st.brush));
    form.appendSlider("p0", tr("lv.equipment.worldshaper.param", "Parameter {}", 1), 0, 32, 1, st.params[0]);
    form.appendSlider("p1", tr("lv.equipment.worldshaper.param", "Parameter {}", 2), 0, 32, 1, st.params[1]);
    form.appendSlider("p2", tr("lv.equipment.worldshaper.param", "Parameter {}", 3), 0, 32, 1, st.params[2]);
    form.appendDropdown("tool", trFor(player, "create.gui.terrainzapper.tool", "Tool"), tools, static_cast<std::size_t>(st.tool));
    form.appendDropdown("placement", trFor(player, "create.gui.terrainzapper.placement", "Placement"), placements, static_cast<std::size_t>(st.placement));
    form.appendDropdown("pattern", trFor(player, "create.gui.terrainzapper.patternSection", "Pattern"), patterns, static_cast<std::size_t>(st.pattern));
    form.sendTo(player, [slot](Player& p, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r) return;
        auto get = [&](char const* k, int def) -> int {
            auto it = r->find(k);
            if (it == r->end()) return def;
            if (auto const* v = std::get_if<uint64>(&it->second)) return static_cast<int>(*v);
            if (auto const* d = std::get_if<double>(&it->second)) return static_cast<int>(std::lround(*d));
            return def;
        };
        auto st       = settingsOf(p.getInventory().getItem(slot));
        st.brush      = ce::Brush(std::clamp(get("brush", 0), 0, 4));
        st.params     = {get("p0", 1), get("p1", 1), get("p2", 1)};
        st.tool       = ce::ShaperTool(std::clamp(get("tool", 0), 0, 5));
        st.placement  = ce::Placement(std::clamp(get("placement", 0), 0, 2));
        st.pattern    = ce::Pattern(std::clamp(get("pattern", 0), 0, 5));
        st.configured = true;
        ce::sanitize(st);
        writeSettings(p, slot, st);
        log().info("equipment: {} configured the worldshaper ({})", p.getRealName(), ce::brushFallback(st.brush));
    });
}

void registerScreen() {
    common::screens::registerType({
        .id   = kScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            int slot = ctx.value("slot", -1);
            if (slot < 0 || items::idOf(p.getInventory().getItem(slot)) != kWorldshaper) return std::nullopt;
            auto st = settingsOf(p.getInventory().getItem(slot));
            Json brushes = Json::array();
            for (int b = 0; b < 5; ++b) {
                Json params = Json::array();
                for (auto const& ps : ce::brushParams(ce::Brush(b)))
                    params.push_back({{"label", trFor(p, ps.key, ps.fallback)}, {"min", ps.min}, {"max", ps.max}, {"toggle", ps.toggle}});
                Json tools = Json::array();
                for (auto t : ce::brushTools(ce::Brush(b))) tools.push_back(static_cast<int>(t));
                brushes.push_back({{"name", trFor(p, ce::brushKey(ce::Brush(b)), ce::brushFallback(ce::Brush(b)))}, {"params", params}, {"tools", tools},
                                   {"placement", ce::hasPlacement(ce::Brush(b))}});
            }
            Json tools = Json::array(), placements = Json::array(), patterns = Json::array();
            for (int t = 0; t < 6; ++t) tools.push_back(trFor(p, ce::toolKey(ce::ShaperTool(t)), ce::toolFallback(ce::ShaperTool(t))));
            for (int q = 0; q < 3; ++q) placements.push_back(trFor(p, ce::placementKey(ce::Placement(q)), ce::placementFallback(ce::Placement(q))));
            for (int q = 0; q < 6; ++q) patterns.push_back(trFor(p, ce::patternKey(ce::Pattern(q)), ce::patternFallback(ce::Pattern(q))));
            return Json{
                {"title", trFor(p, "item.levitate:handheld_worldshaper.name", "Creative Worldshaper")},
                {"settings", st.toJson()},
                {"brushes", brushes},
                {"tools", tools},
                {"placements", placements},
                {"patterns", patterns},
                {"labels",
                 {{"brush", trFor(p, "create.gui.terrainzapper.brush", "Brush")},
                  {"tool", trFor(p, "create.gui.terrainzapper.tool", "Tool")},
                  {"placement", trFor(p, "create.gui.terrainzapper.placement", "Placement")},
                  {"pattern", trFor(p, "create.gui.terrainzapper.patternSection", "Pattern")},
                  {"confirm", trFor(p, "create.action.confirm", "Confirm")}}},
            };
        },
        .action =
            [](Player& p, Json const& ctx, Json const& action) {
                int slot = ctx.value("slot", -1);
                if (slot < 0 || items::idOf(p.getInventory().getItem(slot)) != kWorldshaper) return;
                auto st     = settingsOf(p.getInventory().getItem(slot));
                auto in     = ce::ShaperSettings::fromJson(action.value("settings", Json::object()));
                in.material = st.material;
                in.configured = true;
                ce::sanitize(in);
                writeSettings(p, slot, in);
                log().info("equipment: {} configured the worldshaper ({})", p.getRealName(), ce::brushFallback(in.brush));
            },
        .form = [](Player& p, Json const& ctx) { openForm(p, ctx.value("slot", -1)); },
    });
}

bool cooling(Player& player, int ticks) {
    auto& cd = cooldowns()[uidOf(player)];
    if (currentTick() < cd) return true;
    cd = currentTick() + static_cast<std::uint64_t>(ticks);
    return false;
}

void deny(Player& player, std::string const& text) {
    actionBar(player, text);
    auto p = player.getPosition();
    sound(static_cast<int>(player.getDimensionId()), p.x, p.y, p.z, "note.bass", 1.f, .5f);
}

} // namespace

namespace {
Block const* shaperSelectable(Block const& block) {
    // Create's trySelect: blocks with an item, no block entity (outside safe NBT), no double-block
    // halves, attached or hanging blocks, no bed parts
    auto inst = block.getBlockType().asItemInstance(block, nullptr);
    if (inst.isNull() || inst.getTypeName().empty() || inst.getTypeName() == "minecraft:air") return nullptr;
    if (block.getBlockType().getBlockEntityType() != BlockActorType::Undefined) return nullptr;
    for (auto const* s : {"upper_block_bit", "attached_bit", "hanging", "head_piece_bit"})
        if (game::intState(block, s)) return nullptr;
    // ... and keeps it normalised: leaves PERSISTENT (Bedrock: persistent_bit, no pending decay
    // check); Java's straight stairs shape and waterlogging are no block states on Bedrock
    Block const* out = &block;
    auto set = [&](char const* name, int v) {
        if (auto const* s = out->getBlockType().getBlockState(HashedString(name)))
            if (auto next = out->setState(*s, v)) out = next.as_ptr();
    };
    if (game::intState(block, "persistent_bit")) set("persistent_bit", 1);
    if (game::intState(block, "update_bit")) set("update_bit", 0);
    return out;
}
} // namespace

bool shaperSelect(Player& player, ItemStack& shaper, GlobalPos const& pos) {
    // Create's ZapperItem#canAttackBlock is false: a left click on a block never breaks it,
    // whether or not it picks a material
    (void)pos;
    shaperPick(player, shaper);
    return true;
}

bool shaperPick(Player& player, ItemStack& shaper) {
    if (player.isSneaking()) return false;
    // Create's trySelect: the block looked at within the zapping range (128 blocks)
    auto hit = player.traceRay(128.0f, false, true);
    if (hit.mType != HitResultType::Tile) return false;
    GlobalPos   pos{static_cast<int>(player.getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    auto const* b = blockAt(pos);
    if (!b || common::blockdata::has(pos)) return false;
    auto const* picked = shaperSelectable(*b);
    if (!picked) return false;
    int  slot     = player.getSelectedItemSlot();
    auto st       = settingsOf(shaper);
    auto material = shaperMaterialOf(*picked);
    if (st.material == material) return false; // Create: the same state again changes nothing
    st.material = material;
    writeSettings(player, slot, st);
    shaper = player.getSelectedItem();
    // Create's AllSoundEvents.CONFIRM (note block bell, 0.5, 0.8)
    auto p = player.getPosition();
    sound(static_cast<int>(player.getDimensionId()), p.x, p.y, p.z, "note.bell", .5f, .8f);
    actionBar(player, trFor(player, "create.terrainzapper.usingBlock", "Using: {}", blockName(player, *picked)));
    log().info("equipment: {} picked {} for the worldshaper", player.getRealName(), picked->getTypeName());
    return true;
}

bool shaperUse(Player& player, ItemStack& shaper) {
    int slot = player.getSelectedItemSlot();
    if (player.isSneaking()) {
        if (cooling(player, 10) || screenDebounced(player)) return true;
        common::screens::open(player, kScreen, {{"slot", slot}});
        return true;
    }
    if (cooling(player, 2)) return true;
    auto st = settingsOf(shaper);
    if (!st.configured) {
        deny(player, trFor(player, "create.terrainzapper.shiftRightClickToSet", "Shift-Right-Click to Select a Shape"));
        return true;
    }
    Block const* material = materialBlock(st);
    bool needsMaterial    = st.tool != ce::ShaperTool::Clear && st.tool != ce::ShaperTool::Flatten;
    if (needsMaterial && !material) {
        deny(player, trFor(player, "create.terrainzapper.leftClickToSet", "Left-Click a Block to set Material"));
        return true;
    }
    auto hit = player.traceRay(128.0f, false, true);
    if (hit.mType != HitResultType::Tile) return true;
    GlobalPos target{static_cast<int>(player.getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    shapeAt(player, st, target, hit.mFacing);
    zapBeam(player, hit.mPos.x, hit.mPos.y, hit.mPos.z);
    return true;
}

void zapBeam(Player& player, double x, double y, double z) {
    // Create's ZapperBeamPacket / ZapperRenderHandler: a beam from the barrel to the hit point, 10
    // end rod and 10 firework particles there, and AllSoundEvents.WORLDSHAPER_PLACE (note block
    // basedrum, volume 0.1, pitch 0.1 for the main hand: Java clamps it to 0.5)
    int  dim   = static_cast<int>(player.getDimensionId());
    auto start = gunBarrel(player, true, .35, -.1, 1);
    double dx = x - start.x, dy = y - start.y, dz = z - start.z;
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    int    n   = std::min(96, static_cast<int>(len / 0.75));
    for (int i = 0; i <= n; ++i) {
        double t = n ? double(i) / n : 0;
        particle(dim, start.x + dx * t, start.y + dy * t, start.z + dz * t, "minecraft:basic_crit_particle");
    }
    for (int i = 0; i < 10; ++i) {
        auto off = [] { return (random01() - .5) * .2; };
        particle(dim, x + off(), y + off(), z + off(), "minecraft:endrod");
        particle(dim, x + off(), y + off(), z + off(), "minecraft:sparkler_emitter");
    }
    sound(dim, start.x, start.y, start.z, "note.bd", .1f, .5f);
}

/// Create's Brush#addToGlobalPositions at a block face (before the pattern).
std::vector<ce::IPos> brushPositions(ce::ShaperSettings const& st, int dim, ce::IPos target, int face) {
    std::vector<ce::IPos> positions;
    if (ce::hasPlacement(st.brush)) {
        auto off = ce::placementOffset(st, face);
        for (auto p : ce::brushShape(st)) positions.push_back({target.x + p.x + off.x, target.y + p.y + off.y, target.z + p.z + off.z});
    } else {
        auto const* tb = blockAt(gp(dim, target));
        std::string targetId = tb ? std::string(tb->getTypeName()) : std::string();
        bool fuzzy   = ce::ignoreMaterialBorders(st);
        bool surface = st.brush == ce::Brush::Surface;
        bool replace = st.tool != ce::ShaperTool::Overlay;
        auto start   = replace ? target : stepFace(target, face);
        positions    = ce::floodFill(st, start, face, [&](ce::IPos const& p) {
            if (replace) {
                auto const* b = blockAt(gp(dim, p));
                if (!b || b->getBlockType().getDestroySpeed() < 0) return false;
                if (!fuzzy && std::string(b->getTypeName()) != targetId) return false;
                if (replaceable(dim, p)) return false;
                // Create (DynamicBrush): the surface brush skips a block covered by a block
                // with a sturdy face toward it
                if (surface) {
                    auto const* above = blockAt(gp(dim, stepFace(p, face)));
                    if (above && !above->isAir() && above->canProvideSupport(static_cast<uchar>(face ^ 1), BlockSupportType::Edge)) return false;
                }
                return true;
            }
            auto on = stepFace(p, face ^ 1);
            if (replaceable(dim, on)) return false;
            if (!fuzzy && idAt(gp(dim, on)) != targetId) return false;
            return replaceable(dim, p);
        });
    }
    return positions;
}

int shapeAt(Player& player, ce::ShaperSettings const& st, GlobalPos const& at, int face) {
    Block const* material = materialBlock(st);
    int      dim = at.dim;
    ce::IPos target{at.x, at.y, at.z};
    std::vector<ce::IPos> positions = brushPositions(st, dim, target, face);
    std::erase_if(positions, [&](ce::IPos const& p) { return !ce::patternKeeps(st.pattern, p, [] { return random01() < .5; }); });
    // Create: the dynamic brushes' Overlay places on the found layer
    auto tool = st.tool;
    if (!ce::hasPlacement(st.brush) && tool == ce::ShaperTool::Overlay) tool = ce::ShaperTool::Place;
    int changed = 0;
    if (tool == ce::ShaperTool::Flatten) {
        flatten(dim, positions, face);
        changed = static_cast<int>(positions.size());
    } else if (tool == ce::ShaperTool::Overlay) {
        for (auto p : positions) {
            auto const* b = blockAt(gp(dim, p));
            if (!b || replaceable(dim, p) || (material && *b == *material)) continue;
            auto up = stepFace(p, 1);
            if (!replaceable(dim, up)) continue;
            if (placeBlock(gp(dim, up), *material)) ++changed;
        }
    } else {
        for (auto p : positions) {
            int a = ce::toolAction(tool, replaceable(dim, p));
            if (a == 1 && material && placeBlock(gp(dim, p), *material)) ++changed;
            else if (a == 2 && clearAt(gp(dim, p))) ++changed;
        }
    }
    log().info("equipment: {} used the worldshaper ({} / {}) at {} {} {}: {} blocks", player.getRealName(), ce::brushFallback(st.brush),
               ce::toolFallback(st.tool), target.x, target.y, target.z, changed);
    return changed;
}

Block const* shaperMaterial(ce::ShaperSettings const& st) { return materialBlock(st); }

std::string shaperMaterialOf(Block const& block) { return block.mSerializationId->toSnbt(SnbtFormat::Minimize, 0); }

void tickShaper() {
    // the preview outline for vanilla players (Create's WorldshaperRenderHandler outlines the
    // blocks the brush would change; the client mod draws it, ShaperPreview.cpp): particles along
    // the cluster's edges every 10 ticks
    if (currentTick() % 10 != 0) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([](Player& p) {
        if (net::isCompanion(p) || items::idOf(p.getSelectedItem()) != kWorldshaper) return true;
        auto st = settingsOf(p.getSelectedItem());
        if (!st.configured) return true;
        auto hit = p.traceRay(128.0f, false, true);
        if (hit.mType != HitResultType::Tile) return true;
        int  dim       = static_cast<int>(p.getDimensionId());
        auto edges     = ce::clusterEdges(brushPositions(st, dim, {hit.mBlock.x, hit.mBlock.y, hit.mBlock.z}, hit.mFacing));
        int  budget    = 1500; // particles per outline
        for (auto const& e : edges) {
            double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, dz = e.b.z - e.a.z;
            int    n  = std::max(1, static_cast<int>(std::sqrt(dx * dx + dy * dy + dz * dz) * 2));
            for (int i = 0; i <= n && budget > 0; ++i, --budget) {
                double t = double(i) / n;
                particle(dim, e.a.x + dx * t, e.a.y + dy * t, e.a.z + dz * t, "minecraft:villager_happy");
            }
            if (budget <= 0) break;
        }
        return true;
    });
}

void installShaper() {
    static bool done = false;
    if (done) return;
    done = true;
    registerScreen();
}

} // namespace lv::feature::equipment
