// Trains unit: the station's auto-schedule (Create's StationBlockEntity depotBehaviour, which only
// accepts schedules): a one-slot inventory for funnels, hoppers and arms, items thrown onto the
// station, the schedule drawn on top of the station (client mod), and applyAutoSchedule.

#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/phys/AABB.h"

#include "feature/advancements/Advancements.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/itemrender/DisplayItems.h"
#include "feature/trains/Trains.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::trains {

namespace items = common::items;

namespace {

void particle(int dim, ct::Vec3d const& at, char const* effect) {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto dimension = level->getDimension(DimensionType(dim)).lock();
    if (!dimension) return;
    level->spawnParticleEffect(effect, Vec3{float(at.x), float(at.y), float(at.z)}, dimension.get());
}

bool isSchedule(ItemStack const& s) { return !s.isNull() && s.getTypeName() == kSchedule; }

} // namespace

ItemStack autoScheduleOf(Station const& s) {
    return s.autoSchedule.is_null() ? ItemStack{} : items::fromJson(s.autoSchedule);
}

bool applyAutoSchedule(Station& s) {
    auto stack = autoScheduleOf(s);
    if (!isSchedule(stack)) return false;
    auto sch = scheduleOf(stack);
    if (!sch || sch->entries.empty()) return false;
    auto* t = trainAt(s);
    if (!t) return false;
    advancements::awardOwner(s.pos, "conductor");
    t->runtime.setSchedule(*sch, true);
    // Create: AllSoundEvents.CONFIRM, happy villager and end rod particles above the station
    playSound(s.pos.dim, {s.pos.x + .5, s.pos.y + .5, s.pos.z + .5}, "random.orb", 0.5f, 1.4f);
    ct::Vec3d above{s.pos.x + .5, s.pos.y + 1.0, s.pos.z + .5};
    for (int i = 0; i < 8; ++i)
        particle(s.pos.dim, {above.x + (i % 3 - 1) * .3, above.y + .05, above.z + (i / 3 - 1) * .3}, "minecraft:villager_happy");
    particle(s.pos.dim, {above.x, above.y + .25, above.z}, "minecraft:endrod");
    log().info("trains: station '{}' applied its auto-schedule to train #{} '{}'", s.name, t->id, t->name);
    return true;
}

bool setAutoSchedule(Station& s, ItemStack const& stack) {
    if (!s.autoSchedule.is_null() || !isSchedule(stack)) return false;
    ItemStack one = stack;
    one.mCount    = 1;
    s.autoSchedule = items::toJson(one);
    markStationsDirty();
    itemrender::refresh(s.pos);
    log().info("trains: station '{}' holds an auto-schedule", s.name);
    applyAutoSchedule(s); // Create's depot callback: a train standing there gets it at once
    return true;
}

ItemStack takeAutoSchedule(Station& s) {
    auto back = autoScheduleOf(s);
    if (s.autoSchedule.is_null()) return back;
    s.autoSchedule = nullptr;
    markStationsDirty();
    itemrender::refresh(s.pos);
    log().info("trains: auto-schedule taken from station '{}'", s.name);
    return back;
}

void dropAutoSchedule(Station& s) {
    auto back = takeAutoSchedule(s);
    if (back.isNull()) return;
    auto* region = regionOf(s.pos.dim);
    auto  level  = ll::service::getLevel();
    if (!region || !level) return;
    level->getSpawner().spawnItem(*region, back, nullptr, Vec3{float(s.pos.x + .5), float(s.pos.y + .5), float(s.pos.z + .5)}, 10);
}

/// Create's SharedDepotBlockMethods.onLanded: a schedule item entity resting on the station.
void tickAutoSchedules() {
    if (state().tick % 4 != 0) return;
    for (auto& [key, s] : state().stations) {
        if (!s.autoSchedule.is_null()) continue;
        auto* region = regionOf(s.pos.dim);
        if (!region || !region->hasChunksAt(game::blockPos(s.pos), 0, false)) continue;
        AABB box{
            Vec3{float(s.pos.x), float(s.pos.y + 0.75), float(s.pos.z)},
            Vec3{float(s.pos.x + 1), float(s.pos.y + 1.4), float(s.pos.z + 1)}
        };
        for (auto* a : region->getEntities(box, 0.0f, ActorType::ItemEntity)) {
            if (!a || !a->isAlive()) continue;
            auto& item = static_cast<ItemActor*>(a)->mItem.get();
            if (!isSchedule(item)) continue;
            ItemStack rest = item;
            if (!setAutoSchedule(s, item)) break;
            rest.mCount = item.mCount - 1;
            if (rest.mCount <= 0) a->remove();
            else item.mCount = rest.mCount;
            break;
        }
    }
}

void installAutoSchedule() {
    // Create's station item handler: the depot's one slot, schedules only
    items::Handler h;
    h.slots   = [](common::Access const&) { return 1; };
    h.getSlot = [](common::Access const& at, int) -> ItemStack {
        auto* s = stationAt(game::globalPos(at.region, at.pos));
        return s ? autoScheduleOf(*s) : ItemStack{};
    };
    h.insert = [](common::Access const& at, ItemStack const& stack, bool simulate) -> ItemStack {
        auto* s = stationAt(game::globalPos(at.region, at.pos));
        if (!s || !s->autoSchedule.is_null() || !isSchedule(stack)) return stack;
        ItemStack rest = stack;
        rest.mCount    = stack.mCount - 1;
        if (!simulate) setAutoSchedule(*s, stack);
        return rest.mCount > 0 ? rest : ItemStack{};
    };
    h.extract = [](common::Access const& at, items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        auto* s = stationAt(game::globalPos(at.region, at.pos));
        if (!s || s->autoSchedule.is_null() || amount <= 0) return {};
        auto held = autoScheduleOf(*s);
        if (filter && !filter(held)) return {};
        return simulate ? held : takeAutoSchedule(*s);
    };
    items::registerBlock(std::string(kStation), std::move(h));

    // Create's StationRenderer -> DepotRenderer: the schedule lies on top of the station
    itemrender::registerProvider(std::string(kStation), [](GlobalPos const& owner, Block const&, std::vector<std::pair<std::int64_t, itemrender::DisplayItem>>& out) {
        auto* s = stationAt(owner);
        if (!s || s->autoSchedule.is_null()) return;
        itemrender::DisplayItem d;
        d.stack    = autoScheduleOf(*s);
        d.at       = {owner.x + .5, owner.y + 15 / 16.0 + 1 / 64.0, owner.z + .5};
        d.at.scale = .5f;
        d.style    = itemrender::Style::Lying;
        out.emplace_back(0, d);
    });
}

} // namespace lv::feature::trains
