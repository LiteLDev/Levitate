// Wand of symmetry (Create's SymmetryWandItem / SymmetryHandler): a mirror placed in the world;
// blocks placed or broken by the holder are mirrored for every enabled wand in the hotbar.

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/form/CustomForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/util/Mirror.h"
#include "mc/util/Rotation.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/states/VanillaBlockStateTransformUtils.h"

#include "core/equipment/Symmetry.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/equipment/Equipment.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;

namespace {

constexpr char kScreen[] = "equipment.symmetry";

std::map<std::int64_t, std::uint64_t>& cooldowns() {
    static std::map<std::int64_t, std::uint64_t> m;
    return m;
}
/// Re-entrancy guard: mirrored placements must not be mirrored again.
int gMirroring = 0;

ce::Mirror mirrorOf(ItemStack const& s) { return ce::Mirror::fromJson(items::getData(s, kSymmetryData)); }

void writeMirror(Player& player, int slot, ce::Mirror const& m) {
    auto& inv = player.getInventory();
    auto  s   = inv.getItem(slot);
    if (items::idOf(s) != kWand) return;
    items::setData(s, kSymmetryData, m.toJson());
    inv.setItem(slot, s);
    player.sendInventory(false);
}

/// The block transformed by a mirror copy (vanilla blocks through Bedrock's structure transform,
/// Levitate blocks through their `levitate:facing` state).
Block const& transformed(Block const& block, ce::Mat2 const& t) {
    // find (rotation, mirror) with R(rotation) * M(mirror) == t; Bedrock's Mirror::Z negates x (checked by
    // the equipment.symmetry_wand selftest)
    static constexpr ce::Mat2 rots[4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
    static constexpr Rotation rotIds[4] = {Rotation::None, Rotation::Clockwise90, Rotation::Clockwise180, Rotation::CounterClockwise90};
    for (int mi = 0; mi < 2; ++mi) {
        ce::Mat2 m = mi ? ce::Mat2{-1, 0, 0, 1} : ce::Mat2{};
        for (int r = 0; r < 4; ++r) {
            auto const& R = rots[r];
            ce::Mat2    c{R.a * m.a + R.b * m.c, R.a * m.b + R.b * m.d, R.c * m.a + R.d * m.c, R.c * m.b + R.d * m.d};
            if (!(c == t)) continue;
            std::string id = block.getTypeName();
            if (isToolbox(id) || isBacktank(id) || id == kLecternController) {
                // Java 2D facing (0 S, 1 W, 2 N, 3 E) <-> Create's Direction order (0 N, 1 S, 2 W, 3 E)
                static constexpr int toDir[] = {1, 2, 0, 3}, fromDir[] = {2, 0, 1, 3};
                auto f = game::intState(block, kFacing);
                if (!f || *f < 0 || *f > 3) return block;
                int  d    = ce::transformHorizontal(t, toDir[*f]);
                auto const* b = game::blockWith(id, {{kFacing, fromDir[d]}});
                return b ? *b : block;
            }
            if (id.starts_with("levitate:")) {
                auto f = game::intState(block, kFacing);
                if (!f) return block;
                // 6-value facing: 2 north, 3 south, 4 west, 5 east -> Create's 0 N, 1 S, 2 W, 3 E
                if (*f < 2) return block;
                int  d    = ce::transformHorizontal(t, *f - 2);
                auto const* b = game::blockWith(id, {{kFacing, d + 2}});
                return b ? *b : block;
            }
            auto const* out = VanillaBlockStateTransformUtils::transformBlock(block, rotIds[r], mi ? Mirror::Z : Mirror::None);
            return out ? *out : block;
        }
    }
    return block;
}

bool inRange(ce::Mirror const& m, GlobalPos const& p) {
    double dx = p.x - m.x, dy = p.y - m.y, dz = p.z - m.z;
    double r  = config().maxSymmetryWandRange;
    return dx * dx + dy * dy + dz * dz <= r * r;
}

/// Enabled mirrors of the wands in the player's hotbar.
std::vector<ce::Mirror> activeMirrors(Player& player) {
    std::vector<ce::Mirror> out;
    auto&                   inv = player.getInventory();
    for (int s = 0; s < 9; ++s) {
        auto const& st = inv.getItem(s);
        if (items::idOf(st) != kWand) continue;
        auto m = mirrorOf(st);
        if (m.placed && m.enabled && m.dim() == static_cast<int>(player.getDimensionId())) out.push_back(m);
    }
    return out;
}

/// Takes one `item` from a survival player's inventory (Create: each copy needs an item).
bool takeItem(Player& player, std::string const& id) {
    if (player.isCreative()) return true;
    auto& inv = player.getInventory();
    for (int s = 0; s < inv.getContainerSize(); ++s) {
        auto st = inv.getItem(s);
        if (items::idOf(st) != id) continue;
        inv.setItem(s, items::withCount(st, items::countOf(st) - 1));
        return true;
    }
    return false;
}

std::optional<int> slotFrom(Json const& ctx) {
    if (!ctx.contains("slot")) return std::nullopt;
    return ctx["slot"].get<int>();
}

void apply(Player& p, int slot, int type, int orientation) {
    auto& inv = p.getInventory();
    auto  s   = inv.getItem(slot);
    if (items::idOf(s) != kWand) return;
    auto m        = mirrorOf(s);
    m.type        = static_cast<ce::MirrorType>(std::clamp(type, 0, 2));
    m.orientation = std::clamp(orientation, 0, ce::orientationCount(m.type) - 1);
    writeMirror(p, slot, m);
    log().info("equipment: {} set the wand of symmetry to {} / {}", p.getRealName(), ce::mirrorTypeKey(m.type), ce::orientationKey(m.type, m.orientation));
}

void openForm(Player& player, int slot) {
    auto m = mirrorOf(player.getInventory().getItem(slot));
    std::vector<std::string> types, orients;
    for (int t = 0; t < 3; ++t) types.push_back(trFor(player, ce::mirrorTypeKey(static_cast<ce::MirrorType>(t)), t == 0 ? "Mirror Once" : t == 1 ? "Rectangular" : "Octagonal"));
    // every orientation of every type, as Create's second scroll input relabels per type
    for (int o = 0; o < 2; ++o) {
        auto k = ce::orientationKey(m.type, std::min(o, ce::orientationCount(m.type) - 1));
        orients.push_back(trFor(player, k, k));
    }
    ll::form::CustomForm form(trFor(player, "item.levitate:wand_of_symmetry.name", "Wand Of Symmetry"));
    form.appendDropdown("type", trFor(player, "create.gui.symmetryWand.mirrorType", "Mirror"), types, static_cast<std::size_t>(m.type));
    form.appendDropdown("orientation", trFor(player, "create.gui.symmetryWand.orientation", "Orientation"), orients, static_cast<std::size_t>(m.orientation));
    form.sendTo(player, [slot](Player& p, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r) return;
        auto get = [&](char const* k) -> int {
            auto it = r->find(k);
            if (it == r->end()) return 0;
            if (auto const* v = std::get_if<uint64>(&it->second)) return static_cast<int>(*v);
            if (auto const* d = std::get_if<double>(&it->second)) return static_cast<int>(*d);
            return 0;
        };
        apply(p, slot, get("type"), get("orientation"));
    });
}

void registerScreen() {
    common::screens::registerType({
        .id   = kScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            auto slot = slotFrom(ctx);
            if (!slot || items::idOf(p.getInventory().getItem(*slot)) != kWand) return std::nullopt;
            auto m = mirrorOf(p.getInventory().getItem(*slot));
            Json labels = Json::array();
            for (int t = 0; t < 3; ++t) {
                Json o = Json::array();
                for (int i = 0; i < ce::orientationCount(static_cast<ce::MirrorType>(t)); ++i)
                    o.push_back(trFor(p, ce::orientationKey(static_cast<ce::MirrorType>(t), i), ce::orientationKey(static_cast<ce::MirrorType>(t), i)));
                labels.push_back(o);
            }
            Json types = Json::array();
            types.push_back(trFor(p, "create.symmetry.mirror.plane", "Mirror Once"));
            types.push_back(trFor(p, "create.symmetry.mirror.doublePlane", "Rectangular"));
            types.push_back(trFor(p, "create.symmetry.mirror.triplePlane", "Octagonal"));
            return Json{
                {"title", trFor(p, "item.levitate:wand_of_symmetry.name", "Wand Of Symmetry")},
                {"type", static_cast<int>(m.type)},
                {"orientation", m.orientation},
                {"types", types},
                {"orientations", labels},
                {"typeLabel", trFor(p, "create.gui.symmetryWand.mirrorType", "Mirror")},
                {"orientationLabel", trFor(p, "create.gui.symmetryWand.orientation", "Orientation")},
            };
        },
        .action =
            [](Player& p, Json const& ctx, Json const& action) {
                if (auto slot = slotFrom(ctx)) apply(p, *slot, action.value("type", 0), action.value("orientation", 0));
            },
        .form = [](Player& p, Json const& ctx) {
            if (auto slot = slotFrom(ctx)) openForm(p, *slot);
        },
    });
}

bool cooling(Player& player) {
    auto& cd = cooldowns()[uidOf(player)];
    if (currentTick() < cd) return true;
    cd = currentTick() + 5;
    return false;
}

void line(int dim, double x0, double y0, double z0, double x1, double y1, double z1) {
    double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    int    n   = std::min(64, static_cast<int>(len / 0.8));
    for (int i = 0; i <= n; ++i) {
        double t = n ? double(i) / n : 0;
        particle(dim, x0 + dx * t, y0 + dy * t, z0 + dz * t, "minecraft:villager_happy");
    }
    particle(dim, x0, y0, z0, "minecraft:endrod");
    particle(dim, x1, y1, z1, "minecraft:endrod");
}

} // namespace

bool wandUseOn(Player& player, ItemStack& wand, GlobalPos const& clicked, int face) {
    if (player.isSneaking()) return wandUse(player, wand);
    if (cooling(player)) return true;
    static constexpr int dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    GlobalPos pos = clicked;
    if (face >= 0 && face < 6) pos = {clicked.dim, clicked.x + dx[face], clicked.y + dy[face], clicked.z + dz[face]};
    auto  m   = mirrorOf(wand);
    float yaw = player.getRotation().y;
    if (!m.placed) {
        m             = ce::Mirror{};
        m.type        = ce::MirrorType::Plane;
        m.orientation = ce::planeOrientationFor(yaw);
    } else if (m.type == ce::MirrorType::Plane) {
        m.orientation = ce::planeOrientationFor(yaw);
    } else if (m.type == ce::MirrorType::Cross) {
        m.orientation = ce::crossOrientationFor(yaw);
    }
    m.x = pos.x, m.y = pos.y, m.z = pos.z;
    m.setDim(pos.dim);
    m.placed  = true;
    m.enabled = true;
    writeMirror(player, player.getSelectedItemSlot(), m);
    wand = player.getSelectedItem();
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.orb", .5f, 1.5f);
    log().info("equipment: {} placed a symmetry mirror at {} {} {}", player.getRealName(), pos.x, pos.y, pos.z);
    return true;
}

bool wandUse(Player& player, ItemStack& wand) {
    int slot = player.getSelectedItemSlot();
    if (player.isSneaking()) {
        if (!screenDebounced(player)) common::screens::open(player, kScreen, {{"slot", slot}});
        return true;
    }
    if (cooling(player)) return true;
    auto m = mirrorOf(wand);
    if (!m.placed) return true;
    m.enabled = false;
    writeMirror(player, slot, m);
    wand = player.getSelectedItem();
    log().info("equipment: {} disabled the wand of symmetry", player.getRealName());
    return true;
}

void mirrorPlacement(Player& player, GlobalPos const& pos) {
    if (gMirroring) return;
    auto const* placed = blockAt(pos);
    if (!placed) return;
    std::string id = placed->getTypeName();
    if (id == "minecraft:air") return;
    ++gMirroring;
    for (auto const& m : activeMirrors(player)) {
        if (!inRange(m, pos)) continue;
        for (auto const& mp : ce::mirrorPositions(m, pos.x, pos.y, pos.z)) {
            GlobalPos to{pos.dim, mp.x, mp.y, mp.z};
            if (!replaceableAt(to)) continue;
            if (!takeItem(player, id)) continue;
            placeBlock(to, transformed(*placed, mp.transform));
            line(pos.dim, m.x + .5, m.y + .5, m.z + .5, to.x + .5, to.y + .5, to.z + .5);
        }
    }
    player.sendInventory(false);
    --gMirroring;
}

void mirrorBreak(Player& player, GlobalPos const& pos, std::string const& blockId) {
    if (gMirroring) return;
    auto  level  = ll::service::getLevel();
    auto* region = regionOf(pos.dim);
    if (!level || !region) return;
    ++gMirroring;
    for (auto const& m : activeMirrors(player)) {
        if (!inRange(m, pos)) continue;
        for (auto const& mp : ce::mirrorPositions(m, pos.x, pos.y, pos.z)) {
            GlobalPos   to{pos.dim, mp.x, mp.y, mp.z};
            std::string there = idAt(to);
            if (there.empty() || there == "minecraft:air") continue;
            if (player.isCreative()) {
                clearAt(to);
            } else {
                if (there != blockId) continue;
                auto const* b = blockAt(to);
                if (!b || b->getBlockType().getDestroySpeed() < 0) continue;
                BlockChangeContext ctx{};
                level->destroyBlock(*region, BlockPos{to.x, to.y, to.z}, true, ctx);
            }
            line(pos.dim, m.x + .5, m.y + .5, m.z + .5, to.x + .5, to.y + .5, to.z + .5);
        }
    }
    --gMirroring;
}

void tickWands() {
    if (currentTick() % 10 != 0) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([](Player& p) {
        auto const& held = p.getSelectedItem();
        if (items::idOf(held) != kWand) return true;
        auto m = mirrorOf(held);
        if (!m.placed || m.dim() != static_cast<int>(p.getDimensionId())) return true;
        double jx = (random01() - .5) * .3, jz = (random01() - .5) * .3;
        particle(m.dim(), m.x + .5 + jx, m.y + .25, m.z + .5 + jz, m.enabled ? "minecraft:endrod" : "minecraft:basic_smoke_particle");
        return true;
    });
}

void installWand() {
    static bool done = false;
    if (done) return;
    done = true;
    registerScreen();
}

bool mirroringNow() { return gMirroring > 0; }

Block const& mirroredBlock(Block const& block, ce::Mat2 const& t) { return transformed(block, t); }

} // namespace lv::feature::equipment
