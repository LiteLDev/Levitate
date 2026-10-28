// Super glue: glue regions per dimension (world DB), the two-click selection of the super glue
// item, removal by hitting, and the particle outlines shown to players holding glue.

#include <algorithm>
#include <map>
#include <unordered_map>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "feature/contraption/Internal.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::contraption {

namespace {

constexpr char kDbGlue[] = "lv:contraption:glue";

std::map<int, std::vector<cc::GlueBox>>& regions() {
    static std::map<int, std::vector<cc::GlueBox>> r;
    return r;
}

void persist() {
    if (!game::db::available()) return;
    nlohmann::json j = nlohmann::json::array();
    for (auto const& [dim, boxes] : regions()) {
        for (auto const& b : boxes) {
            j.push_back({
                {"d", dim                                },
                {"a", {b.min.x, b.min.y, b.min.z}},
                {"b", {b.max.x, b.max.y, b.max.z}}
            });
        }
    }
    game::db::save(kDbGlue, j);
}

struct Selection {
    int     dim = 0;
    cc::Pos first;
};
std::unordered_map<std::string, Selection>& selections() {
    static std::unordered_map<std::string, Selection> s;
    return s;
}

struct GlueLifecycle {
    GlueLifecycle() {
        game::db::onWorldReady([] {
            regions().clear();
            selections().clear();
            auto j = game::db::load(kDbGlue);
            if (!j || !j->is_array()) return;
            for (auto const& e : *j) {
                auto a = e.value("a", std::vector<int>{0, 0, 0});
                auto b = e.value("b", std::vector<int>{0, 0, 0});
                if (a.size() < 3 || b.size() < 3) continue;
                regions()[e.value("d", 0)].push_back(cc::GlueBox::of({a[0], a[1], a[2]}, {b[0], b[1], b[2]}));
            }
        });
        game::db::onWorldClosed([] {
            regions().clear();
            selections().clear();
        });
    }
};
GlueLifecycle const gGlueLifecycle;

std::string keyOf(Player const& p) { return p.getRealName(); }

} // namespace

std::vector<cc::GlueBox> glueRegions(int dim) {
    auto it = regions().find(dim);
    return it == regions().end() ? std::vector<cc::GlueBox>{} : it->second;
}

void addGlue(int dim, cc::GlueBox const& box) {
    regions()[dim].push_back(box);
    persist();
}

int removeGlueAt(int dim, cc::Pos const& pos) {
    auto& v = regions()[dim];
    auto  n = std::erase_if(v, [&](cc::GlueBox const& b) { return b.contains(pos); });
    if (n) persist();
    return static_cast<int>(n);
}

int clearGlue(int dim, cc::Pos const& center, int radius) {
    auto& v = regions()[dim];
    auto  area = cc::GlueBox::of(center - cc::Pos{radius, radius, radius}, center + cc::Pos{radius, radius, radius});
    auto  n    = std::erase_if(v, [&](cc::GlueBox const& b) { return area.intersects(b); });
    if (n) persist();
    return static_cast<int>(n);
}

namespace detail {

bool glued(int dim, cc::Pos const& a, cc::Dir d) {
    auto it = regions().find(dim);
    return it != regions().end() && cc::isGlued(it->second, a, d);
}

std::vector<cc::GlueBox> takeGlue(int dim, std::unordered_set<cc::Pos, cc::PosHash> const& positions) {
    std::vector<cc::GlueBox> taken;
    auto it = regions().find(dim);
    if (it == regions().end()) return taken;
    std::erase_if(it->second, [&](cc::GlueBox const& b) {
        bool inside = std::any_of(positions.begin(), positions.end(), [&](cc::Pos const& p) { return b.contains(p); });
        if (inside) taken.push_back(b);
        return inside;
    });
    if (!taken.empty()) persist();
    return taken;
}

void glueClick(Player& player, cc::Pos const& pos, bool sneaking) {
    int  dim = static_cast<int>(player.getDimensionId());
    auto key = keyOf(player);
    auto it  = selections().find(key);
    if (sneaking) {
        if (it != selections().end()) {
            selections().erase(it);
            actionBar(player, trFor(player, "create.super_glue.abort", "Selection discarded"));
        }
        return;
    }
    if (it == selections().end() || it->second.dim != dim) {
        selections()[key] = {dim, pos};
        actionBar(player, trFor(player, "create.super_glue.click_to_confirm", "Click again to confirm"));
        playSound(dim, cc::Vec3d::center(pos), "mob.slime.small", 0.5f, 1.2f);
        return;
    }
    auto first = it->second.first;
    auto* region = regionOf(dim);
    if (!region) return;
    auto check = cc::checkSelection(first, pos, config::get<Config>().maxSuperGlueRange, [&](cc::Pos const& p) {
        return describe(*region, dim, p).necessary;
    });
    if (check == cc::GlueCheck::TooFar) {
        actionBar(player, trFor(player, "create.super_glue.too_far", "Selected area is too big"));
        return;
    }
    if (check == cc::GlueCheck::CannotReach) {
        actionBar(player, trFor(player, "create.super_glue.cannot_reach", "Selected blocks must be connected"));
        return;
    }
    // one use of the glue (Create damages the item per application)
    if (!player.isCreative()) {
        ItemStack item = player.getSelectedItem();
        if (item.isNull() || item.getTypeName() != kSuperGlue) {
            actionBar(player, trFor(player, "create.super_glue.not_enough", "Not enough glue in inventory"));
            return;
        }
        if (item.isDamageableItem()) {
            item.hurtAndBreak(1, &player);
            player.setSelectedItem(item);
            player.refreshInventory();
        }
    }
    selections().erase(key);
    auto box = cc::GlueBox::of(first, pos);
    addGlue(dim, box);
    actionBar(player, trFor(player, "create.super_glue.success", "Applying Glue..."));
    playSound(dim, cc::Vec3d::center(pos), "mob.slime.big", 0.5f, 0.9f);
    log().info(
        "super glue applied by {}: {} {} {} .. {} {} {}",
        player.getRealName(),
        box.min.x,
        box.min.y,
        box.min.z,
        box.max.x,
        box.max.y,
        box.max.z
    );
}

bool glueAttack(Player& player, cc::Pos const& pos) {
    int dim = static_cast<int>(player.getDimensionId());
    int n   = removeGlueAt(dim, pos);
    if (!n) return false;
    playSound(dim, cc::Vec3d::center(pos), "mob.slime.small", 0.5f, 0.6f);
    log().info("super glue removed by {}: {} region(s) at {} {} {}", player.getRealName(), n, pos.x, pos.y, pos.z);
    return true;
}

/// Particle outlines of the regions near a player holding super glue (and of a pending selection).
void glueOutlines(Player& player) {
    int  dim = static_cast<int>(player.getDimensionId());
    auto me  = player.getPosition();
    auto draw = [&](cc::GlueBox const& b, std::string const& effect) {
        cc::Vec3d lo = cc::Vec3d::of(b.min), hi = cc::Vec3d::of(b.max) + cc::Vec3d{1, 1, 1};
        auto      size = hi - lo;
        int       budget = 0;
        for (int axis = 0; axis < 3; ++axis) {
            auto   a   = static_cast<cc::Axis>(axis);
            double len = size.get(a);
            for (int corner = 0; corner < 4; ++corner) {
                cc::Vec3d start = lo;
                int       k     = 0;
                for (int other = 0; other < 3; ++other) {
                    if (other == axis) continue;
                    if (corner >> k & 1) start.set(static_cast<cc::Axis>(other), hi.get(static_cast<cc::Axis>(other)));
                    ++k;
                }
                for (double t = 0; t <= len + 1e-6 && budget < 240; t += 0.5, ++budget) {
                    cc::Vec3d p = start;
                    p.set(a, start.get(a) + t);
                    particle(dim, p, effect);
                }
            }
        }
    };
    for (auto const& b : glueRegions(dim)) {
        cc::Vec3d c = (cc::Vec3d::of(b.min) + cc::Vec3d::of(b.max)) * 0.5;
        if ((c - cc::Vec3d{me.x, me.y, me.z}).lengthSqr() > 32.0 * 32.0) continue;
        draw(b, "minecraft:villager_happy");
    }
    if (auto it = selections().find(keyOf(player)); it != selections().end() && it->second.dim == dim) {
        draw(cc::GlueBox::of(it->second.first, it->second.first), "minecraft:basic_flame_particle");
    }
}

} // namespace detail
} // namespace lv::feature::contraption
