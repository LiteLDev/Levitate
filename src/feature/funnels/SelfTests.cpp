// Selftests of the item logistics unit (U12, testPos dx 1200..1299).

#include <map>
#include <set>
#include <tuple>

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/FunnelsExt.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/funnels/FunnelsInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace fu       = lv::feature::funnels;
namespace fd       = lv::feature::funnels::detail;
namespace cf       = lv::core::funnels;
namespace common   = lv::feature::common;
namespace items    = lv::feature::common::items;
namespace beltlink = lv::feature::common::beltlink;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockPos  bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
BlockSource& region(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

ItemStack item(Context& ctx, char const* id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), std::string("item ") + id + " exists");
    return *s;
}

/// Items of `id` in the inventory at `pos`.
int countIn(BlockSource& r, GlobalPos const& pos, std::string const& id) {
    common::Access a{r, bp(pos), -1};
    auto           h = common::items::find(a);
    if (!h) return -1;
    int n = 0;
    for (int i = 0; i < h->slots(a); ++i) {
        auto s = h->getSlot(a, i);
        if (!items::isEmpty(s) && items::idOf(s) == id) n += items::countOf(s);
    }
    return n;
}

int put(Context& ctx, GlobalPos const& pos, char const* id, int count) {
    auto rest = items::insert(region(ctx), bp(pos), -1, item(ctx, id, count), false);
    return count - items::countOf(rest);
}

/// Item entities of `id` around `pos` (radius r); `remove` deletes them.
int entitiesNear(BlockSource& r, GlobalPos const& pos, std::string const& id, float radius = 2.f, bool remove = false) {
    int n = 0;
    for (auto* e : fd::itemsIn(r, Vec3{pos.x - radius, pos.y - radius, pos.z - radius}, Vec3{pos.x + 1 + radius, pos.y + 1 + radius, pos.z + 1 + radius})) {
        if (items::idOf(e->item()) != id) continue;
        n += items::countOf(e->item());
        if (remove) e->remove();
    }
    return n;
}

void clearEntities(BlockSource& r, GlobalPos const& pos, float radius = 3.f) {
    for (auto* e : fd::itemsIn(r, Vec3{pos.x - radius, pos.y - radius, pos.z - radius}, Vec3{pos.x + 1 + radius, pos.y + 1 + radius, pos.z + 1 + radius})) e->remove();
}

int stateAt(BlockSource& r, GlobalPos const& p, char const* name) {
    auto const* b = fd::loadedBlock(r, bp(p));
    return b ? fd::state(*b, name, -1) : -1;
}

/// A floor of stone under the test area so dropped items stay put.
void floor(Context& ctx, GlobalPos base, int w = 4) {
    for (int dx = -w; dx <= w; ++dx)
        for (int dz = -w; dz <= w; ++dz) ctx.setBlock(at(base, dx, -1, dz), "minecraft:stone");
}

/// A fake belt for the tunnel tests (U11 registers the real one): barrels at the given positions
/// are belt segments moving in the given direction.
struct FakeBelt {
    std::map<std::tuple<int, int, int>, int> segments;
    explicit FakeBelt(std::map<std::tuple<int, int, int>, int> s) : segments(std::move(s)) {
        beltlink::registerSegment("minecraft:barrel", [this](BlockSource&, BlockPos const& p, Block const&) -> std::optional<beltlink::Segment> {
            auto it = segments.find({p.x, p.y, p.z});
            if (it == segments.end()) return std::nullopt;
            return beltlink::Segment{it->second, 32.f, true, true};
        });
    }
    ~FakeBelt() { beltlink::unregisterSegment("minecraft:barrel"); }
};

} // namespace

LV_REGISTER_SELFTEST("funnels.blocks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx.testPos(1200), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    for (char const* id : {fu::kAndesiteFunnel, fu::kBrassFunnel, fu::kAndesiteTunnel, fu::kBrassTunnel, fu::kChute, fu::kSmartChute, fu::kItemVault, fu::kCreativeCrate}) {
        ctx.require(lv::game::blockWith(id) != nullptr, std::string(id) + " is registered");
        ctx.check(items::make(id).has_value(), std::string(id) + " has an item");
    }
    ctx.require(ctx.setBlock(pos, fu::kBrassFunnel, {{fu::kFacing, 3}, {fu::kExtracting, 1}}), "place a brass funnel");
    co_await ctx.ticks(2);
    ctx.check(stateAt(region(ctx), pos, fu::kFacing) == 3, "facing reads back");
    ctx.check(stateAt(region(ctx), pos, fu::kExtracting) == 1, "extracting reads back");
    ctx.check(common::filterslot::hasSlots(fu::kBrassFunnel), "brass funnel has a filter slot");
    ctx.check(!common::filterslot::hasSlots(fu::kAndesiteFunnel), "andesite funnel has no filter slot");
    ctx.check(common::valuebox::boxesOf(fu::kBrassTunnel) != nullptr, "brass tunnel has the distribution box");
});

LV_REGISTER_SELFTEST("funnels.andesite_extracts", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1205), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    floor(ctx, base);
    auto chest  = base;
    auto funnel = at(base, 1, 0, 0);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest");
    ctx.require(ctx.setBlock(funnel, fu::kAndesiteFunnel, {{fu::kFacing, cf::kEast}, {fu::kExtracting, 1}}), "funnel facing east");
    co_await ctx.ticks(1);
    clearEntities(r, base);
    ctx.require(put(ctx, chest, "minecraft:cobblestone", 5) == 5, "fill the chest");
    co_await ctx.ticks(4);
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 4, "andesite funnel extracts single items");
    ctx.check(entitiesNear(r, funnel, "minecraft:cobblestone") == 1, "the item drops in front of the funnel");
    // the dropped item blocks the next extraction until it is gone
    co_await ctx.ticks(12);
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 4, "an item in front blocks the funnel");
    entitiesNear(r, funnel, "minecraft:cobblestone", 2.f, true);
    co_await ctx.ticks(12);
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 3, "extracts again after the cooldown");
    clearEntities(r, base);
});

LV_REGISTER_SELFTEST("funnels.brass_filter_amount", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1215), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    floor(ctx, base);
    auto chest  = base;
    auto funnel = at(base, 0, 0, 1);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest");
    ctx.require(ctx.setBlock(funnel, fu::kBrassFunnel, {{fu::kFacing, cf::kSouth}, {fu::kExtracting, 1}, {fu::kPowered, 1}}), "funnel");
    co_await ctx.ticks(1);
    common::filterslot::set(funnel, item(ctx, "minecraft:cobblestone"));
    fd::setAmount(funnel, {4, false}); // exactly 4
    clearEntities(r, base);
    put(ctx, chest, "minecraft:stone", 10);
    put(ctx, chest, "minecraft:cobblestone", 3);
    co_await ctx.ticks(6);
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 3, "exactly 4: 3 cobblestone are not enough");
    ctx.check(countIn(r, chest, "minecraft:stone") == 10, "the filter keeps stone in");
    put(ctx, chest, "minecraft:cobblestone", 3);
    co_await ctx.ticks(4);
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 2, "exactly 4 cobblestone extracted");
    ctx.check(entitiesNear(r, funnel, "minecraft:cobblestone") == 4, "as one stack of 4");
    clearEntities(r, base);
    // up to 64 without a filter: a whole stack
    common::filterslot::set(funnel, items::empty());
    fd::setAmount(funnel, {64, true});
    co_await ctx.ticks(12);
    ctx.check(countIn(r, chest, "minecraft:stone") == 0 || countIn(r, chest, "minecraft:cobblestone") == 0, "brass funnels extract up to a full stack");
    clearEntities(r, base);
});

LV_REGISTER_SELFTEST("funnels.redstone_pauses", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1225), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    floor(ctx, base);
    auto chest  = base;
    auto funnel = at(base, -1, 0, 0);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest");
    ctx.require(ctx.setBlock(funnel, fu::kAndesiteFunnel, {{fu::kFacing, cf::kWest}, {fu::kExtracting, 1}}), "funnel");
    ctx.require(ctx.setBlock(at(funnel, 0, 0, 1), "minecraft:redstone_block"), "redstone block next to the funnel");
    co_await ctx.ticks(3);
    clearEntities(r, base);
    put(ctx, chest, "minecraft:cobblestone", 5);
    co_await ctx.ticks(10);
    ctx.check(stateAt(r, funnel, fu::kPowered) == 1, "the funnel is powered");
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 5, "a powered funnel does nothing");
    ctx.setBlock(at(funnel, 0, 0, 1), "minecraft:air");
    co_await ctx.ticks(6);
    ctx.check(stateAt(r, funnel, fu::kPowered) == 0, "unpowered again");
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 4, "extracts once unpowered");
    clearEntities(r, base);
});

LV_REGISTER_SELFTEST("funnels.collect_and_insert", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1235), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    floor(ctx, base);
    auto chest  = base;
    auto funnel = at(base, 0, 1, 0);
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest");
    ctx.require(ctx.setBlock(funnel, fu::kBrassFunnel, {{fu::kFacing, cf::kUp}, {fu::kExtracting, 0}}), "inserting funnel on top");
    co_await ctx.ticks(2);
    clearEntities(r, base);
    // an item dropped onto the funnel goes into the chest
    fd::spawnItem(r, item(ctx, "minecraft:cobblestone", 7), Vec3{funnel.x + .5f, funnel.y + .6f, funnel.z + .5f}, Vec3{0, 0, 0});
    co_await ctx.ticks(10);
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 7, "dropped items are collected into the inventory");
    ctx.check(entitiesNear(r, funnel, "minecraft:cobblestone") == 0, "the item entity is gone");
    // belts / chutes hand items to an upward funnel through the capability; the filter applies
    common::filterslot::set(funnel, item(ctx, "minecraft:stone"));
    auto rest = items::insert(r, bp(funnel), cf::kUp, item(ctx, "minecraft:cobblestone", 3), false);
    ctx.check(items::countOf(rest) == 3, "filtered: cobblestone refused");
    rest = items::insert(r, bp(funnel), cf::kUp, item(ctx, "minecraft:stone", 3), false);
    ctx.check(items::isEmpty(rest) && countIn(r, chest, "minecraft:stone") == 3, "matching items inserted");
    common::filterslot::set(funnel, items::empty());
    clearEntities(r, base);
});

LV_REGISTER_SELFTEST("funnels.player_place_and_use", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1245), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    auto& r  = region(ctx);
    floor(ctx, base);
    auto chest = base;
    // placed against stone first (right-clicking a chest would open it), the chest comes after
    ctx.require(ctx.setBlock(chest, "minecraft:stone"), "stone");
    sp.teleport(Vec3{base.x + 3.5f, float(base.y), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(2);
    sp.simulateLookAt(Vec3{chest.x + .5f, chest.y + .5f, chest.z + .5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(3);
    ctx.give(sp, item(ctx, fu::kAndesiteFunnel), 0);
    auto funnel = at(chest, 1, 0, 0);
    ctx.cleanupBlock(funnel);
    sp.simulateUseItemInSlotOnBlock(0, bp(chest), ScriptModuleMinecraft::ScriptFacing::East, Vec3{1.f, .5f, .5f});
    co_await ctx.ticks(3);
    ctx.require(lv::game::blockIdAt(r, bp(funnel)) == fu::kAndesiteFunnel, "the funnel is placed on the chest");
    ctx.check(stateAt(r, funnel, fu::kFacing) == cf::kEast, "it faces away from the chest (towards the player)");
    ctx.check(stateAt(r, funnel, fu::kExtracting) == 1, "placed normally it extracts");
    // switch to inserting and hand-insert the held stack through the front
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest behind the funnel");
    fd::setStates(r, bp(funnel), {{fu::kExtracting, 0}});
    co_await ctx.ticks(10);
    int slot = ctx.give(sp, item(ctx, "minecraft:cobblestone", 12), 1);
    ctx.info("slot " + std::to_string(slot) + " holds " + items::idOf(sp.getInventory().getItem(1)));
    bool used = sp.simulateUseItemInSlotOnBlock(1, bp(funnel), ScriptModuleMinecraft::ScriptFacing::East, Vec3{.55f, .3f, .5f});
    ctx.info(std::string("use returned ") + (used ? "true" : "false"));
    co_await ctx.ticks(2);
    ctx.check(countIn(r, chest, "minecraft:cobblestone") == 12, "right-clicking the front inserts the held stack");
    clearEntities(r, base);
});

LV_REGISTER_SELFTEST("funnels.chute_down", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1255), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r      = region(ctx);
    auto  bottom = base;
    auto  c1 = at(base, 0, 1, 0), c2 = at(base, 0, 2, 0);
    auto  top = at(base, 0, 3, 0);
    ctx.require(ctx.setBlock(bottom, "minecraft:chest"), "bottom chest");
    ctx.require(ctx.setBlock(c1, fu::kChute), "chute 1");
    ctx.require(ctx.setBlock(c2, fu::kChute), "chute 2");
    ctx.require(ctx.setBlock(top, "minecraft:chest"), "top chest");
    co_await ctx.ticks(2);
    put(ctx, top, "minecraft:cobblestone", 20);
    co_await ctx.ticks(40);
    ctx.check(countIn(r, top, "minecraft:cobblestone") == 0, "the chute pulls 16 at a time from the inventory above");
    ctx.check(countIn(r, bottom, "minecraft:cobblestone") == 20, "items fall through the chutes into the chest below");
    // a chute over air drops its items
    auto lone = at(base, 2, 2, 0);
    ctx.require(ctx.setBlock(lone, fu::kChute), "lone chute");
    co_await ctx.ticks(1);
    clearEntities(r, lone, 2.f);
    auto rest = items::insert(r, bp(lone), -1, item(ctx, "minecraft:stone", 5), false);
    ctx.check(items::isEmpty(rest), "a chute takes a stack");
    ctx.check(items::countOf(fd::chuteItem(lone)) == 5, "and holds it");
    co_await ctx.ticks(1);
    ctx.check(fd::shownItems() > 0, "the moving stack is shown to the clients");
    co_await ctx.ticks(8);
    ctx.check(entitiesNear(r, lone, "minecraft:stone", 3.f, true) == 5, "it drops the stack out of the bottom");
    clearEntities(r, base, 4.f);
});

LV_REGISTER_SELFTEST("funnels.smart_chute", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1265), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r      = region(ctx);
    auto  bottom = base, chute = at(base, 0, 1, 0), top = at(base, 0, 2, 0);
    ctx.require(ctx.setBlock(bottom, "minecraft:chest"), "bottom chest");
    ctx.require(ctx.setBlock(chute, fu::kSmartChute, {{fu::kPowered, 0}}), "smart chute");
    ctx.require(ctx.setBlock(top, "minecraft:chest"), "top chest");
    co_await ctx.ticks(2);
    common::filterslot::set(chute, item(ctx, "minecraft:cobblestone"));
    fd::setAmount(chute, {5, false});
    put(ctx, top, "minecraft:stone", 10);
    put(ctx, top, "minecraft:cobblestone", 12);
    co_await ctx.ticks(30);
    ctx.check(countIn(r, bottom, "minecraft:stone") == 0, "the filter keeps other items back");
    ctx.check(countIn(r, bottom, "minecraft:cobblestone") == 10, "exactly 5 at a time: 10 of 12 moved");
    // redstone stops it
    ctx.require(ctx.setBlock(at(chute, 1, 0, 0), "minecraft:redstone_block"), "power");
    put(ctx, top, "minecraft:cobblestone", 8);
    co_await ctx.ticks(20);
    ctx.check(stateAt(r, chute, fu::kPowered) == 1, "powered");
    ctx.check(countIn(r, bottom, "minecraft:cobblestone") == 10, "a powered smart chute stops");
});

LV_REGISTER_SELFTEST("funnels.chute_fans", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1270), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r   = region(ctx);
    auto  fan = base, chute = at(base, 0, 1, 0), top = at(base, 0, 2, 0);
    // a test fan (U13 registers the encased fan): lodestone blowing up at 64 RPM, removed on exit
    struct TestFan {
        TestFan() {
            common::airflow::registerFan("minecraft:lodestone", [](BlockSource&, BlockPos const&, Block const&) {
                return std::optional<common::airflow::Fan>(common::airflow::Fan{cf::kUp, 64.f});
            });
        }
        ~TestFan() { common::airflow::unregisterFan("minecraft:lodestone"); }
    } fanGuard;
    ctx.require(ctx.setBlock(fan, "minecraft:lodestone"), "fan");
    ctx.require(ctx.setBlock(chute, fu::kChute), "chute");
    ctx.require(ctx.setBlock(top, "minecraft:chest"), "chest above");
    co_await ctx.ticks(2);
    auto rest = items::insert(r, bp(chute), -1, item(ctx, "minecraft:cobblestone", 9), false);
    ctx.check(items::isEmpty(rest), "chute takes the stack");
    co_await ctx.ticks(40);
    ctx.check(countIn(r, top, "minecraft:cobblestone") == 9, "a fan below pushes items up into the inventory above");
});

LV_REGISTER_SELFTEST("funnels.vault", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1275), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) ctx.require(ctx.setBlock(at(base, x, y, z), fu::kItemVault, {{fu::kAxis, 0}}), "vault block");
    co_await ctx.ticks(3);
    auto s = fd::vaultStructure(base);
    ctx.require(s.has_value(), "a structure formed");
    ctx.check(s->width == 2 && s->length == 3, "2x2 base, 3 long");
    ctx.check(items::slotCount(r, bp(at(base, 2, 1, 1)), -1) == 12 * fu::config().vaultCapacity, "one inventory of 12 x 20 stacks");
    ctx.check(stateAt(r, base, fu::kLen) == 1 && stateAt(r, at(base, 1, 0, 0), fu::kLen) == 2 && stateAt(r, at(base, 2, 0, 0), fu::kLen) == 3,
        "connected textures along the axis");
    ctx.check(stateAt(r, base, fu::kU) == 1 && stateAt(r, at(base, 0, 1, 1), fu::kV) == 3, "connected textures across");
    // insert through one block, take out through another
    auto rest = items::insert(r, bp(base), cf::kWest, item(ctx, "minecraft:cobblestone", 64), false);
    ctx.check(items::isEmpty(rest), "insert a stack");
    rest = items::insert(r, bp(at(base, 1, 1, 0)), cf::kUp, item(ctx, "minecraft:cobblestone", 30), false);
    auto got = items::extract(r, bp(at(base, 2, 0, 1)), cf::kEast, items::any(), 80, false);
    ctx.check(items::countOf(got) == 80, "extract across the structure");
    ctx.check(countIn(r, base, "minecraft:cobblestone") == 14, "14 left");
    // the comparator signal of the whole vault
    auto sig = fd::vaultSignal(base);
    ctx.check(sig && *sig == 1, "comparator signal 1 for a nearly empty vault");
    // breaking a block splits the vault
    ctx.setBlock(at(base, 2, 1, 1), "minecraft:air");
    co_await ctx.ticks(3);
    auto s2 = fd::vaultStructure(base);
    ctx.check(s2 && s2->size() < 12, "the vault re-forms without the broken block");
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) ctx.setBlock(at(base, x, y, z), "minecraft:air");
    co_await ctx.ticks(2);
    clearEntities(r, base, 4.f);
});

LV_REGISTER_SELFTEST("funnels.vault_comparator", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1285), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    floor(ctx, base, 3);
    ctx.require(ctx.setBlock(base, fu::kItemVault, {{fu::kAxis, 0}}), "vault");
    co_await ctx.ticks(2);
    for (int i = 0; i < 10; ++i) (void)items::insert(r, bp(base), -1, item(ctx, "minecraft:cobblestone", 64), false);
    ctx.check(fd::vaultSignal(base).value_or(0) == 8, "half full: signal 8");
    // a comparator east of the vault, reading it from behind (output to the east)
    auto comp = at(base, 1, 0, 0), wire = at(base, 2, 0, 0);
    auto def  = Block::tryGetFromRegistry(HashedString("minecraft:unpowered_comparator"));
    ctx.require(bool(def), "comparator block");
    CompoundTag nbt = def->mSerializationId.get();
    // Bedrock: the cardinal direction points at the comparator's input
    nbt["states"].get<CompoundTag>().putString("minecraft:cardinal_direction", "west");
    auto cb = Block::tryGetFromRegistry(nbt);
    ctx.require(bool(cb), "comparator reading westwards");
    ctx.cleanupBlock(comp);
    lv::game::setBlock(r, bp(comp), *cb);
    ctx.require(ctx.setBlock(wire, "minecraft:redstone_wire"), "wire");
    co_await ctx.ticks(10);
    auto const* w      = fd::loadedBlock(r, bp(wire));
    int         signal = w ? lv::game::intState(*w, "redstone_signal").value_or(-1) : -1;
    ctx.check(signal == 8, "the comparator outputs the vault's fill level (8), got " + std::to_string(signal));
    // more items: a stronger signal
    for (int i = 0; i < 10; ++i) (void)items::insert(r, bp(base), -1, item(ctx, "minecraft:cobblestone", 64), false);
    co_await ctx.ticks(10);
    w      = fd::loadedBlock(r, bp(wire));
    signal = w ? lv::game::intState(*w, "redstone_signal").value_or(-1) : -1;
    ctx.check(signal == 15, "a full vault gives 15, got " + std::to_string(signal));
    for (auto const& p : {comp, wire}) ctx.setBlock(p, "minecraft:air");
    (void)common::blockdata::take(base); // no drops of the test contents
    ctx.setBlock(base, "minecraft:air");
});

LV_REGISTER_SELFTEST("funnels.creative_crate", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx.testPos(1290), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto& r = region(ctx);
    ctx.require(ctx.setBlock(pos, fu::kCreativeCrate), "crate");
    co_await ctx.ticks(1);
    ctx.check(items::isEmpty(items::extract(r, bp(pos), cf::kUp, items::any(), 64, false)), "empty filter: nothing");
    common::filterslot::set(pos, item(ctx, "minecraft:diamond"));
    for (int i = 0; i < 3; ++i) {
        auto got = items::extract(r, bp(pos), cf::kDown, items::any(), 64, false);
        ctx.check(items::idOf(got) == "minecraft:diamond" && items::countOf(got) == 64, "an endless supply of 64 diamonds");
    }
    auto rest = items::insert(r, bp(pos), cf::kUp, item(ctx, "minecraft:dirt", 64), false);
    ctx.check(items::isEmpty(rest), "inserted items are voided");
});

LV_REGISTER_SELFTEST("funnels.tunnels", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(1293), 0, 1, 4);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    // belt segment (barrel) with outputs north, south and east (barrels = belts / inventories)
    auto belt = base, north = at(base, 0, 0, -1), south = at(base, 0, 0, 1), east = at(base, 1, 0, 0);
    FakeBelt fake({
        {{belt.x, belt.y, belt.z}, cf::kEast},
        {{north.x, north.y, north.z}, cf::kNorth},
        {{south.x, south.y, south.z}, cf::kSouth},
        {{east.x, east.y, east.z}, cf::kEast},
    });
    for (auto const& p : {belt, north, south, east}) ctx.require(ctx.setBlock(p, "minecraft:barrel"), "barrel");
    auto tunnel = at(base, 0, 1, 0);
    ctx.require(ctx.setBlock(tunnel, fu::kBrassTunnel, {{fu::kAxis, 0}}), "brass tunnel");
    co_await ctx.ticks(3);
    ctx.check(stateAt(r, tunnel, fu::kShape) == static_cast<int>(cf::TunnelShape::Cross), "outputs on both sides make a cross");
    // split: 9 items to 3 exits
    auto rest = items::insert(r, bp(tunnel), -1, item(ctx, "minecraft:cobblestone", 9), false);
    ctx.check(items::isEmpty(rest), "items inserted into the tunnel directly");
    co_await ctx.ticks(6);
    ctx.check(countIn(r, north, "minecraft:cobblestone") == 3 && countIn(r, south, "minecraft:cobblestone") == 3
                  && countIn(r, east, "minecraft:cobblestone") == 3,
        "split evenly between the exits");
    // a side filter sorts
    common::filterslot::set(tunnel, item(ctx, "minecraft:stone"), "side_" + std::to_string(cf::kNorth));
    (void)items::insert(r, bp(tunnel), -1, item(ctx, "minecraft:stone", 4), false);
    co_await ctx.ticks(6);
    ctx.check(countIn(r, north, "minecraft:stone") == 4, "filtered exits take matching items first");
    common::filterslot::set(tunnel, items::empty(), "side_" + std::to_string(cf::kNorth));
    // round robin: whole stacks to one exit at a time
    fd::editDoc(tunnel)["mode"] = static_cast<int>(cf::SelectionMode::RoundRobin);
    (void)items::insert(r, bp(tunnel), -1, item(ctx, "minecraft:dirt", 8), false);
    co_await ctx.ticks(6);
    int whole = 0;
    for (auto const& p : {north, south, east}) whole += countIn(r, p, "minecraft:dirt") == 8 ? 1 : 0;
    ctx.check(whole == 1, "round robin keeps the stack whole");
    // andesite tunnel: one item split off to each side output of a passing belt item
    ctx.require(ctx.setBlock(tunnel, fu::kAndesiteTunnel, {{fu::kAxis, 0}}), "andesite tunnel");
    co_await ctx.ticks(3);
    ctx.check(stateAt(r, tunnel, fu::kShape) == static_cast<int>(cf::TunnelShape::Cross), "andesite cross");
    auto stack  = item(ctx, "minecraft:gravel", 5);
    auto result = beltlink::itemEntering(r, bp(belt), stack, cf::kEast);
    ctx.check(result == beltlink::Passing::Continue && items::countOf(stack) == 3, "two items split off, three go on");
    ctx.check(countIn(r, north, "minecraft:gravel") == 1 && countIn(r, south, "minecraft:gravel") == 1, "one to each side");
    for (auto const& p : {belt, north, south, east}) common::blockdata::erase(p);
});

LV_REGISTER_SELFTEST("funnels.hold_filter_for_amount", [](Context& ctx) {
    // Create: holding the filter slot of a brass funnel / smart chute edits its count (U41, R60)
    for (char const* id : {fu::kBrassFunnel, fu::kSmartChute}) {
        auto const* slots = common::filterslot::slotsOf(id);
        ctx.require(slots && !slots->empty(), std::string(id) + " has a filter slot");
        ctx.check(static_cast<bool>(slots->front().onHold), std::string(id) + ": holding the slot opens the amount board");
        ctx.check(common::valuebox::boxesOf(id) != nullptr, std::string(id) + ": the amount box stays for vanilla players");
    }
});
