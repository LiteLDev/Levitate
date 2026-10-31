// Crafter slot items for vanilla clients (SPEC §8): Create's MechanicalCrafterRenderer draws the
// item on the crafter's face (the client mod does that, U33); vanilla clients get a client-only item
// entity just in front of the face (feature/common/VanillaItems.h). Covered crafters show nothing.

#include <algorithm>

#include "mc/world/level/BlockSource.h"

#include "feature/common/Items.h"
#include "feature/common/VanillaItems.h"
#include "feature/crafter/Internal.h"
#include "game/Blocks.h"

namespace lv::feature::crafter {

namespace {

namespace vi = common::vanillaitems;

void crafterItems(std::vector<vi::Shown>& out) {
    for (auto const& pos : crafterlogic::tracked()) {
        auto* region = world::loadedRegion(pos);
        if (!region) continue;
        auto* s = crafterlogic::at(pos);
        if (!s || s->covered || common::items::isEmpty(s->slot)) continue;
        auto const* block = world::blockAt(pos);
        if (!block) continue;
        int   facing = std::clamp(world::intState(*block, "levitate:facing", 2), 2, 5);
        float dx     = facing == 4 ? -1.f : facing == 5 ? 1.f : 0.f;
        float dz     = facing == 2 ? -1.f : facing == 3 ? 1.f : 0.f;
        out.push_back({pos, 0, s->slot, Vec3{pos.x + .5f + dx * .6f, pos.y + .3f, pos.z + .5f + dz * .6f}});
    }
}

struct Registration {
    Registration() { vi::registerSource(crafterItems); }
} const gRegistration;

} // namespace

} // namespace lv::feature::crafter
