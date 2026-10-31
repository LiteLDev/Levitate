// Items in the basin for vanilla clients (SPEC §8): Create's BasinRenderer draws the stacks in a ring
// around the centre (the client mod does that, U33); vanilla clients get one client-only item entity
// per stack on the basin's floor, in the same ring (feature/common/VanillaItems.h).

#include <cmath>
#include <numbers>

#include "mc/world/level/BlockSource.h"

#include "feature/basin/BasinInternal.h"
#include "feature/common/Items.h"
#include "feature/common/VanillaItems.h"
#include "game/Blocks.h"

namespace lv::feature::basin {

namespace {

namespace vi = common::vanillaitems;

void basinItems(std::vector<vi::Shown>& out) {
    for (auto const& [pos, kind] : tracked()) {
        if (kind != Kind::Basin) continue;
        auto* region = regionOf(pos.dim);
        if (!region || !region->hasChunksAt(game::blockPos(pos), 0, false)) continue;
        auto* b = basinops::get(*region, pos);
        if (!b) continue;
        std::vector<ItemStack const*> stacks;
        for (int i = 0; i < 2 * cb::kSlotCount; ++i) {
            auto const& s = basinops::slot(*b, i);
            if (!common::items::isEmpty(s)) stacks.push_back(&s);
        }
        double radius = stacks.size() > 1 ? .22 : 0;
        for (std::size_t i = 0; i < stacks.size(); ++i) {
            double a = 2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(stacks.size());
            out.push_back({
                pos,
                static_cast<int>(i),
                *stacks[i],
                Vec3{static_cast<float>(pos.x + .5 + radius * std::cos(a)), pos.y + .15f, static_cast<float>(pos.z + .5 + radius * std::sin(a))},
            });
        }
    }
}

struct Registration {
    Registration() { vi::registerSource(basinItems); }
} const gRegistration;

} // namespace

} // namespace lv::feature::basin
