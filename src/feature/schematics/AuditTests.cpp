// Selftests of the U58 schematics alignments (testPos dx 5870..5899): what a print carries
// (Create's prepareBlockEntityData), the schematicannon's settings on its item and its resting yaw.

#include <cmath>
#include <filesystem>

#include "ll/api/service/Bedrock.h"

#include "mc/world/actor/ActorType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/schematics/Schematics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::schematics {

namespace {

using selftest::Context;
namespace items = common::items;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

void put(Context& ctx, GlobalPos const& p, std::string const& id) {
    ctx.cleanupBlock(p);
    placeId(p, id);
}

struct FileCleanup {
    std::filesystem::path path;
    ~FileCleanup() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

/// Item entities of `id` around a block (and removes them).
std::vector<ItemStack> takeItemsNear(GlobalPos const& p, std::string const& id) {
    std::vector<ItemStack> out;
    auto*                  region = regionOf(p.dim);
    if (!region) return out;
    AABB box{Vec3{p.x - 1.f, p.y - 1.f, p.z - 1.f}, Vec3{p.x + 2.f, p.y + 2.f, p.z + 2.f}};
    for (auto* a : region->getEntities(box, 0.f, ActorType::ItemEntity)) {
        auto& it = static_cast<ItemActor&>(*a);
        if (items::idOf(it.item()) != id) continue;
        out.push_back(it.item());
        a->remove();
    }
    return out;
}

} // namespace

// ---- M3 / M5: what a print carries -----------------------------------------------------------------

LV_REGISTER_SELFTEST("schematics.safe_print", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto o = ctx.testPos(5870);
    o.y += 1;
    auto target = at(o, 6, 0, 0);
    ctx.require(co_await ctx.ensureLoaded(o), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(target), "target area loaded");
    // a redstone link (SAFE_NBT: everything) and a smart observer (filter only) on stone
    for (int x = 0; x < 2; ++x) {
        put(ctx, at(o, x, 0, 0), "minecraft:stone");
        put(ctx, at(target, x, 0, 0), "minecraft:stone");
        ctx.cleanupBlock(at(target, x, 1, 0));
    }
    ctx.require(ctx.setBlock(at(o, 0, 1, 0), "levitate:redstone_link", {{"levitate:facing", 1}}), "link");
    ctx.require(ctx.setBlock(at(o, 1, 1, 0), "levitate:content_observer", {{"levitate:facing", 5}}), "observer");
    co_await ctx.ticks(2);
    common::blockdata::edit(at(o, 0, 1, 0))["redstone"]["first"] = items::toJson(*items::make("minecraft:diamond"));
    common::filterslot::set(at(o, 1, 1, 0), *items::make("minecraft:gold_ingot"));
    common::blockdata::edit(at(o, 1, 1, 0))["redstone"]["turnOff"] = 5; // runtime: never printed
    auto& player = ctx.player();
    auto  dir    = playerDir(player.getRealName());
    auto  file   = saveArea(dir, "lv_selftest_safe_print", true, o.dim, cp(o), cp(at(o, 1, 1, 0)));
    ctx.require(file.has_value(), "saved");
    FileCleanup   cleanup{dir / upath(*file)};
    SchematicData d;
    d.file     = *file;
    d.owner    = player.getRealName();
    d.anchor   = cp(target);
    d.deployed = true;
    d.size     = {2, 2, 1};
    printInstantly(&player, o.dim, d, false);
    co_await ctx.ticks(2);
    auto const* link = common::blockdata::get(at(target, 0, 1, 0));
    ctx.check(
        link && link->contains("redstone") && (*link)["redstone"].contains("first"),
        "the link's frequency is printed (SAFE_NBT)"
    );
    ctx.check(items::idOf(common::filterslot::get(at(target, 1, 1, 0))) == "minecraft:gold_ingot", "the observer's filter is printed");
    auto const* obs = common::blockdata::get(at(target, 1, 1, 0));
    ctx.check(!obs || !obs->contains("redstone"), "the observer's other data is not");
});

// ---- M14 / M15: the schematicannon's item keeps its settings, the barrel rests towards the placer ---

LV_REGISTER_SELFTEST("schematics.cannon_item", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(5880);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    put(ctx, p, std::string(kCannon));
    auto stack = makeItem(kCannon, 1);
    ctx.require(stack.has_value(), "cannon item");
    items::setData(*stack, "schematicannon", Json{{"replace", 3}, {"skipMissing", false}, {"replaceBE", true}});
    cannonPlaced(p, 90.f, false, &*stack); // looking west
    co_await ctx.ticks(1);
    auto const* d = dataIf(p);
    ctx.require(d != nullptr, "the cannon has data");
    ctx.check(d->value("replace", -1) == 3, "replace mode from the item");
    ctx.check(d->value("skipMissing", true) == false && d->value("replaceBE", false) == true, "both options from the item");
    // Create: -floor(90 * 16 / 360 + .5) & 15 = 12 -> 270 degrees
    ctx.check(std::abs(d->value("yaw", 0.f) - 270.f) < .01f, "the barrel rests at 270 degrees, got " + std::to_string(d->value("yaw", 0.f)));
    takeItemsNear(p, std::string(kCannon));
    // broken with drops (as a survival player or an explosion would): the loot table drops it
    ctx.require(ll::service::getLevel()->destroyBlock(*regionOf(p.dim), bp(p), true, BlockChangeContext{}), "cannon broken");
    co_await ctx.ticks(3);
    auto drops = takeItemsNear(p, std::string(kCannon));
    ctx.require(drops.size() == 1, "the cannon dropped itself once, got " + std::to_string(drops.size()));
    auto kept = items::getData(drops[0], "schematicannon");
    ctx.check(kept.is_object() && kept.value("replace", -1) == 3 && kept.value("replaceBE", false), "with its settings");
});

} // namespace lv::feature::schematics
