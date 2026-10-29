// Windmill sails and the sail frame (Create's SailBlock): facing on placement, the placement
// helper (a sail in hand on a sail extends the plane), dyeing and shearing.

#include <deque>
#include <set>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/bearings/Bearings.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::bearings {

namespace {

cc::Dir nearestLooking(Player& player) {
    Vec3    look = player.getViewVector();
    cc::Dir best = cc::Dir::North;
    double  dot  = -2;
    for (auto d : cc::kAllDirs) {
        auto   o = cc::offset(d);
        double v = o.x * look.x + o.y * look.y + o.z * look.z;
        if (v > dot) {
            dot  = v;
            best = d;
        }
    }
    return best;
}

cc::Dir facingAt(BlockSource& region, GP const& p) { return facingOf(region.getBlock(bp(p))); }

bool setSail(BlockSource& region, GP const& p, std::string const& id, cc::Dir facing) {
    return placeBlock(region, p, id, {{"levitate:facing", static_cast<int>(facing)}});
}

/// Create's BlockMovementChecks: a sail is attached to its neighbours in its own plane, so a sail
/// structure moves as a whole without glue.
struct SailRules {
    SailRules() {
        contraption::MovementRules rules;
        rules.attachedTowards = [](cc::States const& s) {
            int f = 1;
            if (auto i = s.find("levitate:facing"); i != s.end())
                if (auto const* v = std::get_if<std::int64_t>(&i->second)) f = static_cast<int>(std::clamp<std::int64_t>(*v, 0, 5));
            cc::DirMask mask = 0;
            for (auto d : cc::kAllDirs)
                if (cc::axisOf(d) != cc::axisOf(static_cast<cc::Dir>(f))) mask |= cc::bit(d);
            return mask;
        };
        contraption::registerMovementRules(std::string(kSailFrame), rules);
        for (auto const& c : dyeColours()) contraption::registerMovementRules(sailIdOf(c), rules);
    }
};
SailRules const sailRules;

} // namespace

void orientSail(BlockSource& region, GP const& pos, Player& player, int /*clickedFace*/) {
    auto look   = nearestLooking(player);
    auto facing = player.isSneaking() ? look : cc::opposite(look);
    setStates(region, pos, {{"levitate:facing", static_cast<int>(facing)}});
}

void applyDye(BlockSource& region, GP const& pos, cc::Vec3d const& hit, std::optional<std::string> const& colour) {
    std::string current = idAt(region, pos);
    if (!isSail(current)) return;
    cc::Dir     facing  = facingAt(region, pos);
    std::string newId   = colour ? sailIdOf(*colour) : std::string(kSailFrame);
    // dye the block itself
    if (current != newId) {
        setSail(region, pos, newId, facing);
        return;
    }
    // dye one adjacent sail (nearest to the click first)
    for (auto d : cb::orderedByDistanceExceptAxis(hit, cc::axisOf(facing))) {
        GP          o  = gp(pos.dim, ccp(pos) + cc::offset(d));
        std::string id = idAt(region, o);
        if (!isSail(id) || isSailFrame(id)) continue;
        if (facingAt(region, o) != facing) continue;
        if (id == current) continue;
        setSail(region, o, newId, facing);
        return;
    }
    // dye all the connected sails of the plane
    std::deque<cc::Pos>  frontier{ccp(pos)};
    std::set<cc::Pos>    visited;
    int                  timeout = 100;
    while (!frontier.empty()) {
        if (timeout-- < 0) break;
        cc::Pos cur = frontier.front();
        frontier.pop_front();
        visited.insert(cur);
        for (auto d : cc::kAllDirs) {
            if (cc::axisOf(d) == cc::axisOf(facing)) continue;
            cc::Pos o = cur + cc::offset(d);
            if (visited.contains(o)) continue;
            GP          og = gp(pos.dim, o);
            std::string id = idAt(region, og);
            if (!isSail(id) || (isSailFrame(id) && colour)) continue;
            if (facingAt(region, og) != facing) continue;
            if (id != current) setSail(region, og, newId, facing);
            frontier.push_back(o);
            visited.insert(o);
        }
    }
}

bool sailUse(Player& player, GP const& pos, int /*face*/, cc::Vec3d const& hit, ItemStack const& held) {
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    std::string target = idAt(*region, pos);
    if (!isSail(target)) return false;
    std::string item = common::items::idOf(held);
    cc::Dir     facing = facingAt(*region, pos);

    // placement helper: a sail or frame in hand extends the plane next to the clicked sail
    if (!player.isSneaking() && player.canUseAbility(AbilitiesIndex::Build) && isSail(item)) {
        for (auto d : cb::orderedByDistanceExceptAxis(hit, cc::axisOf(facing))) {
            GP o = gp(pos.dim, ccp(pos) + cc::offset(d));
            if (!game::replaceable(*region, bp(o))) continue;
            if (!setSail(*region, o, item, facing)) return false;
            consumeSelected(player);
            playSound(pos.dim, {o.x + .5, o.y + .5, o.z + .5}, isSailFrame(item) ? "dig.wood" : "dig.cloth");
            log().info("sail placed at {} by the placement helper", o.key());
            return true;
        }
        return false;
    }
    if (item == "minecraft:shears") {
        playSound(pos.dim, {pos.x + .5, pos.y + .5, pos.z + .5}, "mob.sheep.shear");
        applyDye(*region, pos, hit, std::nullopt);
        log().info("sail at {} sheared by {}", pos.key(), player.getRealName());
        return true;
    }
    if (isSailFrame(target)) return false;
    if (auto colour = dyeColourOf(item)) {
        playSound(pos.dim, {pos.x + .5, pos.y + .5, pos.z + .5}, "dye.use");
        applyDye(*region, pos, hit, colour);
        log().info("sail at {} dyed {} by {}", pos.key(), *colour, player.getRealName());
        return true;
    }
    return false;
}

} // namespace lv::feature::bearings
